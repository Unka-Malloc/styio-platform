#include "PlatformCore/SourceFetch/SourceFetch.hpp"

#include "PlatformCore/Core/Paths.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{

bool StartsWith(std::string_view value, std::string_view prefix)
{
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

fs::path CanonicalAbsolutePath(const fs::path &path)
{
  return fs::absolute(path).lexically_normal();
}

uint64_t Fnv1a64(std::string_view value)
{
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char ch : value)
  {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string Hex64(const uint64_t value, const bool padded)
{
  std::ostringstream out;
  out << std::hex;
  if (padded)
  {
    out.width(16);
    out.fill('0');
  }
  out << value;
  return out.str();
}

bool HasControlCharacter(std::string_view value)
{
  for (const unsigned char ch : value)
  {
    if (ch < 0x20 || ch == 0x7f)
    {
      return true;
    }
  }
  return false;
}

bool ResultFailed(const pafio::ProcessResult &result)
{
  return result.exit_code != 0 || result.timed_out || result.terminated_by_signal;
}

std::string FailureMessage(const std::string &operation, const pafio::ProcessResult &result)
{
  return operation + " failed: " + pafio::DescribeProcessFailure(result);
}

std::optional<fs::path> FindVendoredSnapshot(
    const std::optional<fs::path> &vendor_root,
    const std::string &repo_hash,
    const std::string &revision)
{
  if (!vendor_root.has_value())
  {
    return std::nullopt;
  }

  const fs::path snapshot_root = CanonicalAbsolutePath(*vendor_root) / "git" / repo_hash / revision;
  if (!fs::exists(snapshot_root))
  {
    return std::nullopt;
  }

  const fs::path ready_marker = snapshot_root / ".pafio-snapshot-ready";
  if (fs::exists(ready_marker) || fs::exists(snapshot_root / "pafio.toml"))
  {
    return CanonicalAbsolutePath(snapshot_root);
  }
  return std::nullopt;
}

}  // namespace

namespace pafio
{

GitSourcePolicy PublicGitSourcePolicy()
{
  return {
      .allow_https = true,
      .allow_http = true,
      .allow_ssh = true,
      .allow_file_url = true,
      .allow_git_protocol = true,
      .allow_local_path = true,
  };
}

GitSourcePolicy TrustedGitSourcePolicy()
{
  return {
      .allow_https = true,
      .allow_http = true,
      .allow_ssh = true,
      .allow_file_url = true,
      .allow_git_protocol = true,
      .allow_local_path = true,
  };
}

bool LooksLikeRemoteGitSource(std::string_view source)
{
  return source.find("://") != std::string_view::npos || StartsWith(source, "git@");
}

std::optional<std::string> GitSourcePolicyViolation(std::string_view source, const GitSourcePolicy &policy)
{
  if (source.empty())
  {
    return "git source origin is required";
  }
  if (HasControlCharacter(source))
  {
    return "git source origin contains control characters";
  }
  if (StartsWith(source, "https://"))
  {
    return policy.allow_https ? std::nullopt : std::optional<std::string>("https git sources are not allowed");
  }
  if (StartsWith(source, "http://"))
  {
    return policy.allow_http ? std::nullopt : std::optional<std::string>("http git sources are not allowed");
  }
  if (StartsWith(source, "ssh://") || StartsWith(source, "git@"))
  {
    return policy.allow_ssh ? std::nullopt : std::optional<std::string>("ssh git sources are not allowed");
  }
  if (StartsWith(source, "file://"))
  {
    return policy.allow_file_url ? std::nullopt : std::optional<std::string>("file git sources are not allowed");
  }
  if (StartsWith(source, "git://"))
  {
    return policy.allow_git_protocol ? std::nullopt : std::optional<std::string>("git protocol sources are not allowed");
  }
  if (source.find("://") != std::string_view::npos)
  {
    return "unsupported git source protocol";
  }
  return policy.allow_local_path ? std::nullopt : std::optional<std::string>("local git source paths are not allowed");
}

std::string NormalizeGitSource(const std::string &source, const fs::path &base_dir)
{
  if (LooksLikeRemoteGitSource(source))
  {
    return source;
  }

  const fs::path source_path(source);
  if (source_path.is_absolute())
  {
    return CanonicalAbsolutePath(source_path).generic_string();
  }
  return CanonicalAbsolutePath(base_dir / source_path).generic_string();
}

std::string SourceFetchIdentityHash(std::string_view value)
{
  return Hex64(Fnv1a64(value), false);
}

std::string SourceFetchSlug(std::string_view value)
{
  std::string slug;
  slug.reserve(value.size() + 17U);
  for (const unsigned char ch : value)
  {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')
    {
      slug.push_back(static_cast<char>(ch));
    }
    else
    {
      slug.push_back('_');
    }
  }
  slug += "-";
  slug += Hex64(Fnv1a64(value), true);
  return slug;
}

SourceFetchError::SourceFetchError(std::string operation, ProcessResult result, std::string message)
    : FetchError(std::move(message)),
      operation_(std::move(operation)),
      result_(std::move(result))
{
}

GitSourceFetcher::GitSourceFetcher(const platform::OperatingSystemAdapter &os) : os_(os) {}

void GitSourceFetcher::ValidateOrigin(std::string_view origin, const GitSourcePolicy &policy) const
{
  if (const std::optional<std::string> violation = GitSourcePolicyViolation(origin, policy); violation.has_value())
  {
    throw FetchError(*violation + ": " + std::string(origin));
  }
}

ProcessResult GitSourceFetcher::RunGitChecked(
    std::vector<std::string> args,
    const std::optional<fs::path> &working_directory,
    const std::string &operation,
    const std::string &error_context) const
{
  ProcessResult result;
  try
  {
    result = os_.RunProcess({
        .program = "git",
        .args = std::move(args),
        .working_directory = working_directory,
        .timeout = kExternalProcessStepTimeout,
        .error_context = error_context,
    });
  }
  catch (const ProcessFailure &error)
  {
    throw SourceFetchError(operation, {}, operation + " failed: " + error.what());
  }

  if (ResultFailed(result))
  {
    throw SourceFetchError(operation, result, FailureMessage(operation, result));
  }
  return result;
}

std::string GitSourceFetcher::ResolveWorktreeRevision(const fs::path &checkout_root) const
{
  ProcessResult result;
  try
  {
    result = os_.RunProcess({
        .program = "git",
        .args = {"rev-parse", "HEAD"},
        .working_directory = checkout_root,
        .timeout = kExternalProcessProbeTimeout,
        .error_context = "source revision probe",
    });
  }
  catch (const ProcessFailure &error)
  {
    throw SourceFetchError("git rev-parse", {}, std::string("git rev-parse failed: ") + error.what());
  }
  if (ResultFailed(result))
  {
    throw SourceFetchError("git rev-parse", result, FailureMessage("git rev-parse", result));
  }
  return TrimTrailingNewline(result.stdout_text);
}

GitWorktreeResult GitSourceFetcher::EnsureWorktree(const GitWorktreeRequest &request) const
{
  ValidateOrigin(request.origin, request.policy);
  if (request.checkout_root.empty())
  {
    throw FetchError("git checkout root is required");
  }
  if (request.revision.has_value() && request.revision->empty())
  {
    throw FetchError("git revision must be non-empty when present");
  }

  const fs::path checkout_root = CanonicalAbsolutePath(request.checkout_root);
  GitWorktreeResult result{
      .origin = request.origin,
      .checkout_root = checkout_root,
      .requested_revision = request.revision,
  };

  auto add_depth = [&](std::vector<std::string> &args) {
    if (request.shallow)
    {
      args.push_back("--depth");
      args.push_back(std::to_string(std::max(request.depth, 1)));
    }
  };

  const bool checkout_exists = fs::exists(checkout_root);
  if (!checkout_exists)
  {
    os_.CreateDirectories(checkout_root.parent_path());
    std::vector<std::string> clone_args = {"clone"};
    add_depth(clone_args);
    if (request.revision.has_value() && request.clone_revision_as_branch)
    {
      clone_args.push_back("--branch");
      clone_args.push_back(*request.revision);
    }
    else if (request.revision.has_value())
    {
      clone_args.push_back("--no-checkout");
    }
    clone_args.push_back(request.origin);
    clone_args.push_back(checkout_root.string());
    RunGitChecked(std::move(clone_args), std::nullopt, "git clone", request.error_context);
    result.cloned = true;
    if (request.revision.has_value() && request.clone_revision_as_branch)
    {
      result.checked_out = true;
    }
  }
  else if (!fs::is_directory(checkout_root))
  {
    throw FetchError("git checkout root exists but is not a directory: " + checkout_root.string());
  }

  if (request.revision.has_value() && (!request.clone_revision_as_branch || checkout_exists) && request.update_existing)
  {
    std::vector<std::string> fetch_args = {"fetch"};
    add_depth(fetch_args);
    fetch_args.push_back("origin");
    fetch_args.push_back(*request.revision);
    RunGitChecked(std::move(fetch_args), checkout_root, "git fetch", request.error_context);
    result.fetched = true;
    RunGitChecked({"checkout", "--force", "FETCH_HEAD"}, checkout_root, "git checkout", request.error_context);
    result.checked_out = true;
  }

  result.resolved_revision = ResolveWorktreeRevision(checkout_root);
  return result;
}

GitSnapshotResult GitSourceFetcher::MaterializeSnapshot(const GitSnapshotRequest &request) const
{
  ValidateOrigin(request.origin, request.policy);
  if (request.revision.empty())
  {
    throw FetchError("git snapshot revision is required");
  }
  if (request.pafio_home.empty())
  {
    throw FetchError("git snapshot cache root is required");
  }

  const fs::path pafio_home = CanonicalAbsolutePath(request.pafio_home);
  const std::string repo_hash = SourceFetchIdentityHash(request.origin);
  if (const std::optional<fs::path> vendored_snapshot =
          FindVendoredSnapshot(request.vendor_root, repo_hash, request.revision);
      vendored_snapshot.has_value())
  {
    return {
        .origin = request.origin,
        .revision = request.revision,
        .repo_hash = repo_hash,
        .snapshot_root = *vendored_snapshot,
        .used_vendor = true,
    };
  }

  fs::create_directories(pafio_home / "git" / "repos");
  fs::create_directories(pafio_home / "git" / "checkouts");
  const fs::path repo_dir = pafio_home / "git" / "repos" / (repo_hash + ".git");
  GitSnapshotResult snapshot{
      .origin = request.origin,
      .revision = request.revision,
      .repo_hash = repo_hash,
      .snapshot_root = pafio_home / "git" / "checkouts" / repo_hash / request.revision,
  };

  if (!fs::exists(repo_dir))
  {
    if (request.offline)
    {
      throw FetchError("offline mode requires a vendored snapshot or cached git mirror for '" + request.origin + "'");
    }
    fs::create_directories(repo_dir.parent_path());
    RunGitChecked({"clone", "--mirror", request.origin, repo_dir.string()}, std::nullopt, "git clone", request.error_context);
    snapshot.cloned_mirror = true;
  }

  auto has_revision = [&]() {
    try
    {
      RunGitChecked(
          {"--git-dir", repo_dir.string(), "cat-file", "-e", request.revision + "^{commit}"},
          std::nullopt,
          "git rev check",
          request.error_context);
      return true;
    }
    catch (const SourceFetchError &error)
    {
      if (error.result().timed_out)
      {
        throw;
      }
      return false;
    }
  };

  if (!has_revision())
  {
    if (request.offline)
    {
      throw FetchError("offline mode is missing git rev '" + request.revision + "' in the local cache for '" + request.origin + "'");
    }
    RunGitChecked({"--git-dir", repo_dir.string(), "fetch", "--prune", "origin"}, std::nullopt, "git fetch", request.error_context);
    snapshot.fetched = true;
    if (!has_revision())
    {
      throw FetchError("git source does not contain requested rev '" + request.revision + "': " + request.origin);
    }
  }

  const fs::path ready_marker = snapshot.snapshot_root / ".pafio-snapshot-ready";
  if (fs::exists(ready_marker))
  {
    return snapshot;
  }

  std::error_code ignored;
  fs::remove_all(snapshot.snapshot_root, ignored);
  fs::create_directories(snapshot.snapshot_root);
  const fs::path archive_path = snapshot.snapshot_root.parent_path() / (SourceFetchIdentityHash(request.revision) + ".tar");
  try
  {
    RunGitChecked(
        {"--git-dir", repo_dir.string(), "archive", "--format=tar", "--output", archive_path.string(), request.revision},
        std::nullopt,
        "git archive",
        request.error_context);
    const ProcessResult extract = os_.RunProcess({
        .program = "tar",
        .args = {"-xf", archive_path.string(), "-C", snapshot.snapshot_root.string()},
        .timeout = kExternalProcessStepTimeout,
        .error_context = request.error_context,
    });
    fs::remove(archive_path, ignored);
    if (ResultFailed(extract))
    {
      throw CacheError("failed to extract git snapshot '" + snapshot.snapshot_root.string() + "': " + DescribeProcessFailure(extract));
    }
  }
  catch (...)
  {
    fs::remove(archive_path, ignored);
    throw;
  }

  std::ofstream marker(ready_marker);
  marker << "ready\n";
  snapshot.created_snapshot = true;
  return snapshot;
}

}  // namespace pafio
