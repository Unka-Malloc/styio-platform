#include "PlatformService/BeastServer.hpp"

#if __has_include(<boost/asio.hpp>) && __has_include(<boost/beast.hpp>)
#define STYIO_PLATFORM_HAS_BOOST_BEAST 1
#else
#define STYIO_PLATFORM_HAS_BOOST_BEAST 0
#endif

#include "PlatformService/Http.hpp"
#include "PlatformService/Identity.hpp"
#include "PlatformService/Router.hpp"

#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <unistd.h>

#if STYIO_PLATFORM_HAS_BOOST_BEAST
#ifndef BOOST_ERROR_CODE_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#endif
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace spio::platform
{

namespace
{

constexpr std::string_view kPlatformApiBase = "/api/styio-platform/v1";
constexpr std::string_view kIdentityHeader = "x-styio-mtls-uri-san";

std::string StripQuery(std::string target)
{
  const size_t query = target.find('?');
  if (query != std::string::npos)
  {
    target.resize(query);
  }
  return target.empty() ? "/" : target;
}

std::string NormalizePlatformPath(std::string target)
{
  target = StripQuery(std::move(target));
  if (target == kPlatformApiBase)
  {
    return "/";
  }
  if (target.starts_with(std::string(kPlatformApiBase) + "/"))
  {
    return target.substr(kPlatformApiBase.size());
  }
  return target;
}

std::string LowerAscii(std::string value)
{
  for (char &ch : value)
  {
    if (ch >= 'A' && ch <= 'Z')
    {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

std::optional<MtlsIdentity> IdentityFromHeaders(
    const std::map<std::string, std::string> &headers,
    const PlatformConfig &config)
{
  if (!config.mtls.trust_proxy_identity_headers)
  {
    return std::nullopt;
  }
  const auto found = headers.find(std::string(kIdentityHeader));
  if (found == headers.end())
  {
    return std::nullopt;
  }
  return ParseMtlsUriSan(found->second);
}

std::optional<MtlsIdentity> IdentityFromPeerCertificate(SSL *ssl)
{
  X509 *certificate = SSL_get_peer_certificate(ssl);
  if (certificate == nullptr)
  {
    return std::nullopt;
  }
  std::optional<MtlsIdentity> identity;
  GENERAL_NAMES *names = static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
  if (names != nullptr)
  {
    const int count = sk_GENERAL_NAME_num(names);
    for (int index = 0; index < count && !identity.has_value(); ++index)
    {
      const GENERAL_NAME *name = sk_GENERAL_NAME_value(names, index);
      if (name == nullptr || name->type != GEN_URI)
      {
        continue;
      }
      const ASN1_IA5STRING *uri = name->d.uniformResourceIdentifier;
      const unsigned char *data = ASN1_STRING_get0_data(uri);
      const int length = ASN1_STRING_length(uri);
      if (data != nullptr && length > 0)
      {
        identity = ParseMtlsUriSan(std::string_view(reinterpret_cast<const char *>(data), static_cast<size_t>(length)));
      }
    }
    GENERAL_NAMES_free(names);
  }
  X509_free(certificate);
  return identity;
}

std::optional<MtlsIdentity> IdentityForRequest(
    const std::map<std::string, std::string> &headers,
    const PlatformConfig &config,
    const std::optional<MtlsIdentity> &peer_identity = std::nullopt)
{
  if (peer_identity.has_value())
  {
    return peer_identity;
  }
  return IdentityFromHeaders(headers, config);
}

nlohmann::json BadRequestBody(std::string detail)
{
  return FailureEnvelope(
      "malformed HTTP request",
      std::move(detail),
      "UsageError",
      "httpAdapter",
      2);
}

struct RawHttpRequest
{
  std::string method;
  std::string target;
  std::map<std::string, std::string> headers;
  std::string body;
};

std::string TrimAscii(std::string value)
{
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
  {
    value.erase(value.begin());
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
  {
    value.pop_back();
  }
  return value;
}

std::string HttpReason(int status_code)
{
  switch (status_code)
  {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 409:
      return "Conflict";
    case 422:
      return "Unprocessable Entity";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    default:
      return "Status";
  }
}

std::optional<size_t> ContentLength(const std::map<std::string, std::string> &headers)
{
  const auto found = headers.find("content-length");
  if (found == headers.end())
  {
    return 0;
  }
  try
  {
    return static_cast<size_t>(std::stoull(found->second));
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::optional<RawHttpRequest> ParseRawHttpRequest(const std::string &raw, std::string &error)
{
  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos)
  {
    error = "HTTP request headers are incomplete";
    return std::nullopt;
  }

  RawHttpRequest parsed;
  std::istringstream stream(raw.substr(0, header_end));
  std::string line;
  if (!std::getline(stream, line))
  {
    error = "HTTP request line is missing";
    return std::nullopt;
  }
  line = TrimAscii(std::move(line));
  std::istringstream request_line(line);
  std::string version;
  if (!(request_line >> parsed.method >> parsed.target >> version))
  {
    error = "HTTP request line must include method, target, and version";
    return std::nullopt;
  }

  while (std::getline(stream, line))
  {
    line = TrimAscii(std::move(line));
    if (line.empty())
    {
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
    {
      error = "HTTP header line is missing ':'";
      return std::nullopt;
    }
    parsed.headers[LowerAscii(line.substr(0, colon))] = TrimAscii(line.substr(colon + 1));
  }

  const std::optional<size_t> content_length = ContentLength(parsed.headers);
  if (!content_length.has_value())
  {
    error = "Content-Length must be an integer";
    return std::nullopt;
  }
  const size_t body_start = header_end + 4;
  if (raw.size() < body_start + *content_length)
  {
    error = "HTTP request body is incomplete";
    return std::nullopt;
  }
  parsed.body = raw.substr(body_start, *content_length);
  return parsed;
}

std::string BuildRawHttpResponse(int status_code, const nlohmann::json &body)
{
  const std::string payload = body.dump(2) + "\n";
  std::ostringstream out;
  out << "HTTP/1.1 " << status_code << " " << HttpReason(status_code) << "\r\n";
  out << "Content-Type: application/json; charset=utf-8\r\n";
  out << "Content-Length: " << payload.size() << "\r\n";
  out << "Connection: close\r\n";
  out << "\r\n";
  out << payload;
  return out.str();
}

HttpResponse DispatchRawRequest(
    PlatformRouter &router,
    const RawHttpRequest &request,
    const std::optional<MtlsIdentity> &peer_identity = std::nullopt)
{
  const std::optional<HttpMethod> method = ParseHttpMethod(request.method);
  if (!method.has_value())
  {
    return {
        .status_code = 405,
        .body = BadRequestBody("method must be GET or POST"),
    };
  }

  nlohmann::json body = nlohmann::json::object();
  if (!request.body.empty())
  {
    try
    {
      body = nlohmann::json::parse(request.body);
    }
    catch (const std::exception &error)
    {
      return {
          .status_code = 400,
          .body = BadRequestBody(error.what()),
      };
    }
  }

  return router.Dispatch({
      .method = *method,
      .path = NormalizePlatformPath(request.target),
      .headers = request.headers,
      .body = std::move(body),
      .identity = IdentityForRequest(request.headers, router.config(), peer_identity),
  });
}

std::string OpenSslErrorText()
{
  const unsigned long code = ERR_get_error();
  if (code == 0)
  {
    return "unknown OpenSSL error";
  }
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return buffer.data();
}

std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> BuildTlsContext(const PlatformConfig &config)
{
  if (config.mtls.cert_path.empty() || config.mtls.key_path.empty())
  {
    throw std::runtime_error("TLS mode requires STYIO_PLATFORM_MTLS_CERT and STYIO_PLATFORM_MTLS_KEY");
  }
  if (config.mtls.required && config.mtls.ca_path.empty())
  {
    throw std::runtime_error("mTLS mode requires STYIO_PLATFORM_MTLS_CA");
  }

  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_ssl_algorithms();

  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
  if (!context)
  {
    throw std::runtime_error("failed to create TLS context: " + OpenSslErrorText());
  }
  SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
  if (SSL_CTX_use_certificate_chain_file(context.get(), config.mtls.cert_path.c_str()) != 1)
  {
    throw std::runtime_error("failed to load TLS certificate: " + OpenSslErrorText());
  }
  if (SSL_CTX_use_PrivateKey_file(context.get(), config.mtls.key_path.c_str(), SSL_FILETYPE_PEM) != 1)
  {
    throw std::runtime_error("failed to load TLS private key: " + OpenSslErrorText());
  }
  if (!config.mtls.ca_path.empty())
  {
    if (SSL_CTX_load_verify_locations(context.get(), config.mtls.ca_path.c_str(), nullptr) != 1)
    {
      throw std::runtime_error("failed to load mTLS CA: " + OpenSslErrorText());
    }
  }
  SSL_CTX_set_verify(
      context.get(),
      config.mtls.required ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT) : SSL_VERIFY_PEER,
      nullptr);
  return context;
}

int CreateTcpListener(const PlatformConfig &config)
{
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
  {
    throw std::runtime_error("failed to create listener socket: " + std::string(std::strerror(errno)));
  }
  int enable = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(config.bind_port));
  if (inet_pton(AF_INET, config.bind_host.c_str(), &address.sin_addr) != 1)
  {
    close(fd);
    throw std::runtime_error("TLS listener supports only IPv4 bind addresses");
  }
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
  {
    close(fd);
    throw std::runtime_error("failed to bind listener socket: " + std::string(std::strerror(errno)));
  }
  if (listen(fd, 16) != 0)
  {
    close(fd);
    throw std::runtime_error("failed to listen on socket: " + std::string(std::strerror(errno)));
  }
  return fd;
}

bool ReadSslHttpRequest(SSL *ssl, std::string &raw, std::string &error)
{
  constexpr size_t kMaxRequestBytes = 1024 * 1024;
  char buffer[4096];
  while (raw.find("\r\n\r\n") == std::string::npos)
  {
    const int rc = SSL_read(ssl, buffer, sizeof(buffer));
    if (rc <= 0)
    {
      error = "failed to read TLS HTTP request headers";
      return false;
    }
    raw.append(buffer, static_cast<size_t>(rc));
    if (raw.size() > kMaxRequestBytes)
    {
      error = "HTTP request exceeds maximum size";
      return false;
    }
  }

  std::string parse_error;
  const std::optional<RawHttpRequest> header_only = ParseRawHttpRequest(raw, parse_error);
  size_t content_length = 0;
  if (!header_only.has_value())
  {
    const size_t header_end = raw.find("\r\n\r\n");
    std::map<std::string, std::string> headers;
    std::istringstream stream(raw.substr(0, header_end));
    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line))
    {
      line = TrimAscii(std::move(line));
      const size_t colon = line.find(':');
      if (colon != std::string::npos)
      {
        headers[LowerAscii(line.substr(0, colon))] = TrimAscii(line.substr(colon + 1));
      }
    }
    const std::optional<size_t> parsed_length = ContentLength(headers);
    if (!parsed_length.has_value())
    {
      error = parse_error;
      return false;
    }
    content_length = *parsed_length;
  }
  else
  {
    content_length = header_only->body.size();
  }

  const size_t body_start = raw.find("\r\n\r\n") + 4;
  while (raw.size() < body_start + content_length)
  {
    const int rc = SSL_read(ssl, buffer, sizeof(buffer));
    if (rc <= 0)
    {
      error = "failed to read TLS HTTP request body";
      return false;
    }
    raw.append(buffer, static_cast<size_t>(rc));
    if (raw.size() > kMaxRequestBytes)
    {
      error = "HTTP request exceeds maximum size";
      return false;
    }
  }
  return true;
}

void SslWriteAll(SSL *ssl, const std::string &payload)
{
  size_t written = 0;
  while (written < payload.size())
  {
    const int rc = SSL_write(ssl, payload.data() + written, static_cast<int>(payload.size() - written));
    if (rc <= 0)
    {
      throw std::runtime_error("failed writing TLS response: " + OpenSslErrorText());
    }
    written += static_cast<size_t>(rc);
  }
}

int RunOpenSslServer(const PlatformConfig &config, BeastServerOptions options)
{
  try
  {
    auto context = BuildTlsContext(config);
    const int listener = CreateTcpListener(config);
    PlatformRouter router(config);
    std::cerr << "styio-platformd TLS listening on " << config.bind_host << ":" << config.bind_port << "\n";
    do
    {
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0)
      {
        close(listener);
        throw std::runtime_error("failed accepting TLS client: " + std::string(std::strerror(errno)));
      }
      std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context.get()), SSL_free);
      SSL_set_fd(ssl.get(), client);
      if (SSL_accept(ssl.get()) != 1)
      {
        close(client);
        continue;
      }
      std::string raw;
      std::string read_error;
      HttpResponse response;
      if (ReadSslHttpRequest(ssl.get(), raw, read_error))
      {
        std::string parse_error;
        const std::optional<RawHttpRequest> parsed = ParseRawHttpRequest(raw, parse_error);
        if (parsed.has_value())
        {
          response = DispatchRawRequest(router, *parsed, IdentityFromPeerCertificate(ssl.get()));
        }
        else
        {
          response = {.status_code = 400, .body = BadRequestBody(parse_error)};
        }
      }
      else
      {
        response = {.status_code = 400, .body = BadRequestBody(read_error)};
      }
      SslWriteAll(ssl.get(), BuildRawHttpResponse(response.status_code, response.body));
      SSL_shutdown(ssl.get());
      close(client);
    } while (!options.once);
    close(listener);
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "styio-platformd TLS server failed: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace

nlohmann::json DescribeBeastServerCapability()
{
  return {
      {"target", "Boost.Beast/Asio"},
      {"headers_available", STYIO_PLATFORM_HAS_BOOST_BEAST == 1},
      {"role", "production HTTP adapter for PlatformRouter"},
      {"fallback_listener_available", true},
      {"fallback", "POSIX synchronous HTTP listener is used when Boost.Beast/Asio headers are unavailable"},
  };
}

#if STYIO_PLATFORM_HAS_BOOST_BEAST

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

std::map<std::string, std::string> RequestHeaders(const http::request<http::string_body> &request)
{
  std::map<std::string, std::string> headers;
  for (const auto &field : request.base())
  {
    headers[LowerAscii(std::string(field.name_string()))] = std::string(field.value());
  }
  return headers;
}

http::response<http::string_body> BuildHttpResponse(
    const http::request<http::string_body> &request,
    int status_code,
    const nlohmann::json &body)
{
  http::response<http::string_body> response{http::int_to_status(status_code), request.version()};
  response.set(http::field::server, BOOST_BEAST_VERSION_STRING);
  response.set(http::field::content_type, "application/json; charset=utf-8");
  response.keep_alive(false);
  response.body() = body.dump(2) + "\n";
  response.prepare_payload();
  return response;
}

http::response<http::string_body> DispatchHttpRequest(
    PlatformRouter &router,
    const http::request<http::string_body> &request)
{
  const std::optional<HttpMethod> method = ParseHttpMethod(std::string(request.method_string()));
  if (!method.has_value())
  {
    return BuildHttpResponse(request, 405, BadRequestBody("method must be GET or POST"));
  }

  nlohmann::json body = nlohmann::json::object();
  if (!request.body().empty())
  {
    try
    {
      body = nlohmann::json::parse(request.body());
    }
    catch (const std::exception &error)
    {
      return BuildHttpResponse(request, 400, BadRequestBody(error.what()));
    }
  }

  const std::map<std::string, std::string> headers = RequestHeaders(request);
  const HttpResponse platform_response = router.Dispatch({
      .method = *method,
      .path = NormalizePlatformPath(std::string(request.target())),
      .headers = headers,
      .body = std::move(body),
      .identity = IdentityForRequest(headers, router.config()),
  });
  return BuildHttpResponse(request, platform_response.status_code, platform_response.body);
}

void HandleConnection(tcp::socket socket, PlatformRouter &router)
{
  beast::flat_buffer buffer;
  http::request<http::string_body> request;
  beast::error_code error;
  http::read(socket, buffer, request, error);
  if (error == http::error::end_of_stream)
  {
    socket.shutdown(tcp::socket::shutdown_send, error);
    return;
  }
  if (error)
  {
    throw beast::system_error{error};
  }

  http::response<http::string_body> response = DispatchHttpRequest(router, request);
  http::write(socket, response, error);
  if (error)
  {
    throw beast::system_error{error};
  }
  socket.shutdown(tcp::socket::shutdown_send, error);
}

}  // namespace

int RunBeastServer(const PlatformConfig &config, BeastServerOptions options)
{
  if (config.mtls.tls_enabled)
  {
    return RunOpenSslServer(config, options);
  }
  try
  {
    net::io_context io_context{1};
    const auto address = net::ip::make_address(config.bind_host);
    tcp::acceptor acceptor{io_context, {address, static_cast<unsigned short>(config.bind_port)}};
    PlatformRouter router(config);

    std::cerr << "styio-platformd listening on " << config.bind_host << ":" << config.bind_port << "\n";
    do
    {
      tcp::socket socket{io_context};
      acceptor.accept(socket);
      HandleConnection(std::move(socket), router);
    } while (!options.once);
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "styio-platformd server failed: " << error.what() << "\n";
    return 1;
  }
}

#else

namespace
{

bool SendAll(int fd, const std::string &payload)
{
  size_t written = 0;
  while (written < payload.size())
  {
    const ssize_t rc = ::send(fd, payload.data() + written, payload.size() - written, 0);
    if (rc <= 0)
    {
      return false;
    }
    written += static_cast<size_t>(rc);
  }
  return true;
}

bool ReadHttpRequest(int fd, std::string &raw, std::string &error)
{
  constexpr size_t kMaxRequestBytes = 1024 * 1024;
  char buffer[4096];
  while (raw.find("\r\n\r\n") == std::string::npos)
  {
    const ssize_t rc = ::recv(fd, buffer, sizeof(buffer), 0);
    if (rc <= 0)
    {
      error = "failed to read HTTP request headers";
      return false;
    }
    raw.append(buffer, static_cast<size_t>(rc));
    if (raw.size() > kMaxRequestBytes)
    {
      error = "HTTP request exceeds maximum size";
      return false;
    }
  }

  std::string parse_error;
  const std::optional<RawHttpRequest> header_only = ParseRawHttpRequest(raw, parse_error);
  size_t content_length = 0;
  if (!header_only.has_value())
  {
    const size_t header_end = raw.find("\r\n\r\n");
    std::map<std::string, std::string> headers;
    std::istringstream stream(raw.substr(0, header_end));
    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line))
    {
      line = TrimAscii(std::move(line));
      const size_t colon = line.find(':');
      if (colon != std::string::npos)
      {
        headers[LowerAscii(line.substr(0, colon))] = TrimAscii(line.substr(colon + 1));
      }
    }
    const std::optional<size_t> parsed_length = ContentLength(headers);
    if (!parsed_length.has_value())
    {
      error = parse_error;
      return false;
    }
    content_length = *parsed_length;
  }
  else
  {
    content_length = header_only->body.size();
  }

  const size_t body_start = raw.find("\r\n\r\n") + 4;
  while (raw.size() < body_start + content_length)
  {
    const ssize_t rc = ::recv(fd, buffer, sizeof(buffer), 0);
    if (rc <= 0)
    {
      error = "failed to read HTTP request body";
      return false;
    }
    raw.append(buffer, static_cast<size_t>(rc));
    if (raw.size() > kMaxRequestBytes)
    {
      error = "HTTP request exceeds maximum size";
      return false;
    }
  }
  return true;
}

int CreateListener(const PlatformConfig &config)
{
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0)
  {
    throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
  }

  int reuse = 1;
  (void) ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(config.bind_port));
  if (::inet_pton(AF_INET, config.bind_host.c_str(), &address.sin_addr) != 1)
  {
    ::close(listener);
    throw std::runtime_error("POSIX fallback only supports IPv4 bind hosts");
  }

  if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
  {
    ::close(listener);
    throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
  }
  if (::listen(listener, 16) < 0)
  {
    ::close(listener);
    throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
  }
  return listener;
}

void HandlePosixConnection(int client, PlatformRouter &router)
{
  std::string raw;
  std::string error;
  if (!ReadHttpRequest(client, raw, error))
  {
    const std::string response = BuildRawHttpResponse(400, BadRequestBody(error));
    (void) SendAll(client, response);
    return;
  }

  const std::optional<RawHttpRequest> request = ParseRawHttpRequest(raw, error);
  if (!request.has_value())
  {
    const std::string response = BuildRawHttpResponse(400, BadRequestBody(error));
    (void) SendAll(client, response);
    return;
  }
  const HttpResponse platform_response = DispatchRawRequest(router, *request);
  const std::string response = BuildRawHttpResponse(platform_response.status_code, platform_response.body);
  (void) SendAll(client, response);
}

}  // namespace

int RunBeastServer(const PlatformConfig &config, BeastServerOptions options)
{
  if (config.mtls.tls_enabled)
  {
    return RunOpenSslServer(config, options);
  }
  try
  {
    const int listener = CreateListener(config);
    PlatformRouter router(config);
    std::cerr << "styio-platformd POSIX fallback listening on " << config.bind_host << ":" << config.bind_port << "\n";
    do
    {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client < 0)
      {
        ::close(listener);
        throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
      }
      HandlePosixConnection(client, router);
      ::close(client);
    } while (!options.once);
    ::close(listener);
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "styio-platformd server failed: " << error.what() << "\n";
    return 1;
  }
}

#endif

}  // namespace spio::platform
