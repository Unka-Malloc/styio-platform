#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "PlatformCore/Core/Errors.hpp"
#include "PlatformCore/Core/Process.hpp"
#include "PlatformCore/System/OperatingSystemAdapter.hpp"

namespace pafio
{

// Defines which Git transports are acceptable for a caller. Workspace and
// resolver paths choose different policies rather than hard-coding scheme
// checks at each call site.
struct GitSourcePolicy
{
  bool allow_https = true;
  bool allow_http = false;
  bool allow_ssh = false;
  bool allow_file_url = false;
  bool allow_git_protocol = false;
  bool allow_local_path = false;
};

GitSourcePolicy PublicGitSourcePolicy();
GitSourcePolicy TrustedGitSourcePolicy();

bool LooksLikeRemoteGitSource(std::string_view source);
std::optional<std::string> GitSourcePolicyViolation(std::string_view source, const GitSourcePolicy &policy);
std::string NormalizeGitSource(const std::string &source, const std::filesystem::path &base_dir);
std::string SourceFetchIdentityHash(std::string_view value);
std::string SourceFetchSlug(std::string_view value);

class SourceFetchError : public FetchError
{
public:
  SourceFetchError(std::string operation, ProcessResult result, std::string message);

  const std::string &operation() const {
    return operation_;
  }

  const ProcessResult &result() const {
    return result_;
  }

private:
  std::string operation_;
  ProcessResult result_;
};

// Describes an editable Git worktree checkout. This path is used by developer
// workspace jobs that need a real repository on disk.
struct GitWorktreeRequest
{
  std::string origin;
  std::filesystem::path checkout_root;
  std::optional<std::string> revision;
  bool update_existing = true;
  bool shallow = false;
  int depth = 1;
  bool clone_revision_as_branch = false;
  GitSourcePolicy policy = PublicGitSourcePolicy();
  std::string error_context = "source fetch";
};

struct GitWorktreeResult
{
  std::string origin;
  std::filesystem::path checkout_root;
  std::optional<std::string> requested_revision;
  std::string resolved_revision;
  bool cloned = false;
  bool fetched = false;
  bool checked_out = false;
};

// Describes a content snapshot materialized from Git or a trusted vendor tree.
// Resolver code consumes snapshots so dependency resolution is repeatable.
struct GitSnapshotRequest
{
  std::filesystem::path pafio_home;
  std::optional<std::filesystem::path> vendor_root;
  std::string origin;
  std::string revision;
  bool offline = false;
  GitSourcePolicy policy = TrustedGitSourcePolicy();
  std::string error_context = "source resolver";
};

struct GitSnapshotResult
{
  std::string origin;
  std::string revision;
  std::string repo_hash;
  std::filesystem::path snapshot_root;
  bool used_vendor = false;
  bool cloned_mirror = false;
  bool fetched = false;
  bool created_snapshot = false;
};

// Fetches source through Git while delegating all filesystem, process, and
// timing effects to OperatingSystemAdapter.
class GitSourceFetcher
{
public:
  explicit GitSourceFetcher(
    const platform::OperatingSystemAdapter &os = platform::DefaultOperatingSystemAdapter()
  );

  GitWorktreeResult EnsureWorktree(const GitWorktreeRequest &request) const;
  GitSnapshotResult MaterializeSnapshot(const GitSnapshotRequest &request) const;
  std::string ResolveWorktreeRevision(const std::filesystem::path &checkout_root) const;

private:
  ProcessResult RunGitChecked(
    std::vector<std::string> args,
    const std::optional<std::filesystem::path> &working_directory,
    const std::string &operation,
    const std::string &error_context
  ) const;
  void ValidateOrigin(std::string_view origin, const GitSourcePolicy &policy) const;

  const platform::OperatingSystemAdapter &os_;
};

}  // namespace pafio
