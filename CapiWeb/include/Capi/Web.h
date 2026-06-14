#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Json.h>

#include <any>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Capi::Web {

inline constexpr std::string_view Version = "0.1.0";

enum class LogLevel {
    Trace,
    Debug,
    Information,
    Warning,
    Error,
    Critical,
};

struct LogMessage {
    LogLevel Level = LogLevel::Information;
    std::string Message;
    unsigned long ErrorCode = 0;
};

using Logger = std::function<void(const LogMessage&)>;

class HttpSysException : public std::runtime_error {
public:
    HttpSysException(std::string operation, unsigned long errorCode);

    const std::string& Operation() const noexcept;
    unsigned long ErrorCode() const noexcept;

private:
    std::string operation_;
    unsigned long errorCode_;
};

class WindowsServiceException : public std::runtime_error {
public:
    WindowsServiceException(std::string operation, unsigned long errorCode);

    const std::string& Operation() const noexcept;
    unsigned long ErrorCode() const noexcept;

private:
    std::string operation_;
    unsigned long errorCode_;
};

class HeaderCollection {
public:
    using Entry = std::pair<std::string, std::string>;

    void Add(std::string name, std::string value);
    void Set(std::string name, std::string value);
    bool Remove(std::string_view name);
    void Clear();

    bool Contains(std::string_view name) const;
    std::optional<std::string> TryGet(std::string_view name) const;
    std::string Get(std::string_view name, std::string defaultValue = {}) const;
    std::vector<std::string> GetValues(std::string_view name) const;
    const std::vector<Entry>& Items() const noexcept;

private:
    std::vector<Entry> entries_;
};

using ValueMap = std::unordered_map<std::string, std::string>;
using ValueListMap = std::unordered_map<std::string, std::vector<std::string>>;

struct MultipartFile {
    std::string Name;
    std::string FileName;
    std::string ContentType;
    HeaderCollection Headers;
    std::string Body;
};

struct FormOptions {
    std::size_t ValueCountLimit = 1024;
    std::size_t KeyLengthLimit = 2048;
    std::size_t ValueLengthLimit = 4 * 1024 * 1024;
    std::size_t MultipartBoundaryLengthLimit = 128;
    std::size_t MultipartFileCountLimit = 128;
    std::size_t MultipartHeadersCountLimit = 16;
    std::size_t MultipartHeadersLengthLimit = 16 * 1024;
    std::uint64_t MultipartBodyLengthLimit = 128ull * 1024ull * 1024ull;
};

enum class CookieSameSiteMode {
    Unspecified,
    Lax,
    Strict,
    None,
};

struct CookieOptions {
    std::string Path;
    std::string Domain;
    std::optional<std::chrono::system_clock::time_point> Expires;
    std::optional<std::chrono::seconds> MaxAge;
    bool HttpOnly = false;
    bool Secure = false;
    CookieSameSiteMode SameSite = CookieSameSiteMode::Unspecified;
};

class ClaimsPrincipal {
public:
    static ClaimsPrincipal Anonymous();
    static ClaimsPrincipal Authenticated(std::string name = {}, std::string authenticationType = {});

    bool IsAuthenticated() const noexcept;
    const std::string& Name() const noexcept;
    const std::string& AuthenticationType() const noexcept;
    const std::vector<std::string>& Roles() const noexcept;
    const std::unordered_map<std::string, std::vector<std::string>>& Claims() const noexcept;

    ClaimsPrincipal& IsAuthenticated(bool authenticated);
    ClaimsPrincipal& Name(std::string name);
    ClaimsPrincipal& AuthenticationType(std::string authenticationType);
    ClaimsPrincipal& AddRole(std::string role);
    ClaimsPrincipal& AddClaim(std::string type, std::string value);

    bool IsInRole(std::string_view role) const;
    bool HasClaim(std::string_view type) const;
    bool HasClaim(std::string_view type, std::string_view value) const;
    std::string Claim(std::string_view type, std::string defaultValue = {}) const;

private:
    bool isAuthenticated_ = false;
    std::string name_;
    std::string authenticationType_;
    std::vector<std::string> roles_;
    std::unordered_map<std::string, std::vector<std::string>> claims_;
};

struct EndpointMatch {
    std::string Pattern;
    std::string Name;
    std::vector<std::string> Methods;
    bool IsFallback = false;
};

class HttpRequest {
public:
    const std::string& Method() const noexcept;
    const std::string& Path() const noexcept;
    const std::string& QueryString() const noexcept;
    const std::string& RawUrl() const noexcept;
    const std::string& Body() const noexcept;
    const std::string& RemoteAddress() const noexcept;
    const std::string& Scheme() const noexcept;
    const std::string& Host() const noexcept;

    HttpRequest& Method(std::string method);
    HttpRequest& Path(std::string path);
    HttpRequest& QueryString(std::string queryString);
    HttpRequest& Body(std::string body);
    HttpRequest& RemoteAddress(std::string remoteAddress);
    HttpRequest& Scheme(std::string scheme);
    HttpRequest& Host(std::string host);
    HttpRequest& Header(std::string name, std::string value);
    HttpRequest& AddHeader(std::string name, std::string value);
    HttpRequest& RemoveHeader(std::string_view name);

    const HeaderCollection& Headers() const noexcept;
    const ValueMap& Query() const noexcept;
    const ValueListMap& QueryValues() const noexcept;
    const ValueMap& RouteValues() const noexcept;
    const ValueMap& Cookies() const;
    const ValueMap& Form() const;
    const ValueListMap& FormValues() const;
    const std::vector<MultipartFile>& Files() const;
    const MultipartFile* File(std::string_view name) const;

    std::string Header(std::string_view name, std::string defaultValue = {}) const;
    std::string Cookie(std::string_view name, std::string defaultValue = {}) const;
    std::string FormValue(std::string_view name, std::string defaultValue = {}) const;
    std::vector<std::string> FormValues(std::string_view name) const;
    std::string QueryValue(std::string_view name, std::string defaultValue = {}) const;
    std::vector<std::string> QueryValues(std::string_view name) const;
    std::string RouteValue(std::string_view name, std::string defaultValue = {}) const;
    std::string Value(std::string_view name, std::string defaultValue = {}) const;
    bool HasJsonContentType() const;
    bool HasFormUrlEncodedContentType() const;
    bool HasMultipartFormDataContentType() const;
    bool HasFormContentType() const;

    System::Text::Json::JsonElement Json() const;

private:
    std::string method_;
    std::string path_;
    std::string queryString_;
    std::string rawUrl_;
    std::string body_;
    std::string remoteAddress_;
    std::string scheme_ = "http";
    std::string host_;
    HeaderCollection headers_;
    ValueMap query_;
    ValueListMap queryValues_;
    ValueMap routeValues_;
    mutable std::optional<ValueMap> cookies_;
    mutable std::optional<ValueMap> form_;
    mutable std::optional<ValueListMap> formValues_;
    mutable std::optional<std::vector<MultipartFile>> files_;
    mutable FormOptions formOptions_;
    mutable std::shared_ptr<System::Text::Json::JsonDocument> jsonDocument_;

    friend class WebApplication;
    friend class HttpResponse;
    friend class HttpResult;
    friend class HttpSysServer;
};

class HttpResponse {
public:
    int StatusCode() const noexcept;
    HttpResponse& Status(int statusCode);

    HeaderCollection& Headers() noexcept;
    const HeaderCollection& Headers() const noexcept;

    const std::vector<std::uint8_t>& Body() const noexcept;
    std::string BodyText() const;
    std::string ContentType() const;

    HttpResponse& Header(std::string name, std::string value);
    HttpResponse& ContentType(std::string contentType);
    HttpResponse& Write(std::string_view text);
    HttpResponse& WriteBytes(const void* data, std::size_t size);
    HttpResponse& WriteBytes(std::vector<std::uint8_t> bytes);
    HttpResponse& ClearBody();
    HttpResponse& AppendCookie(std::string name, std::string value, CookieOptions options = {});
    HttpResponse& DeleteCookie(std::string name, CookieOptions options = {});

private:
    int statusCode_ = 200;
    HeaderCollection headers_;
    std::vector<std::uint8_t> body_;
};

class HttpContext {
public:
    HttpRequest& Request() noexcept;
    const HttpRequest& Request() const noexcept;
    HttpResponse& Response() noexcept;
    const HttpResponse& Response() const noexcept;

    const std::string& TraceIdentifier() const noexcept;
    HttpContext& TraceIdentifier(std::string traceIdentifier);

    std::unordered_map<std::string, std::any>& Items() noexcept;
    const std::unordered_map<std::string, std::any>& Items() const noexcept;

    ClaimsPrincipal& User() noexcept;
    const ClaimsPrincipal& User() const noexcept;
    HttpContext& User(ClaimsPrincipal user);

    const std::optional<EndpointMatch>& MatchedEndpoint() const noexcept;
    std::string EndpointName(std::string defaultValue = {}) const;
    std::string EndpointPattern(std::string defaultValue = {}) const;

private:
    HttpRequest request_;
    HttpResponse response_;
    std::string traceIdentifier_;
    ClaimsPrincipal user_;
    std::unordered_map<std::string, std::any> items_;
    std::optional<EndpointMatch> matchedEndpoint_;

    friend class WebApplication;
    friend class HttpSysServer;
};

struct ProblemDetails {
    std::string Type = "about:blank";
    std::string Title;
    int Status = 500;
    std::string Detail;
    std::string Instance;
    std::unordered_map<std::string, System::Text::Json::JsonNode> Extensions;
};

using ProblemDetailsCustomizer = std::function<void(HttpContext&, ProblemDetails&)>;

struct ProblemDetailsOptions {
    std::string DefaultType = "about:blank";
    bool IncludeType = true;
    bool IncludeInstance = true;
    bool IncludeTraceIdentifier = true;
    std::string TraceIdentifierExtensionName = "traceId";
    ProblemDetailsCustomizer Customize;
};

class HttpResult {
public:
    HttpResult() = default;

    bool HasResponse() const noexcept;
    int StatusCode() const noexcept;
    HeaderCollection& Headers() noexcept;
    const HeaderCollection& Headers() const noexcept;
    const std::vector<std::uint8_t>& Body() const noexcept;

    HttpResult& Header(std::string name, std::string value);
    HttpResult& AppendCookie(std::string name, std::string value, CookieOptions options = {});
    HttpResult& DeleteCookie(std::string name, CookieOptions options = {});
    HttpResult& WriteBytes(std::vector<std::uint8_t> bytes);
    void Apply(HttpResponse& response) const;

private:
    bool hasResponse_ = false;
    int statusCode_ = 200;
    HeaderCollection headers_;
    std::vector<std::uint8_t> body_;

    static HttpResult Create(int statusCode, std::string contentType, std::vector<std::uint8_t> body);

    friend class Results;
};

class Results final {
public:
    Results() = delete;

    static HttpResult Empty();
    static HttpResult StatusCode(int statusCode);
    static HttpResult Text(std::string_view text, int statusCode = 200, std::string contentType = "text/plain; charset=utf-8");
    static HttpResult Content(std::string_view content, std::string contentType, int statusCode = 200);
    static HttpResult Bytes(const void* data, std::size_t size, std::string contentType = "application/octet-stream", int statusCode = 200);
    static HttpResult Bytes(const std::vector<std::uint8_t>& bytes, std::string contentType = "application/octet-stream", int statusCode = 200);
    static HttpResult File(std::vector<std::uint8_t> bytes, std::string contentType = "application/octet-stream", std::string downloadName = {});
    static HttpResult File(const std::filesystem::path& path, std::string contentType = {}, std::string downloadName = {});

    static HttpResult Json(const System::Text::Json::JsonElement& value, int statusCode = 200);
    static HttpResult Json(const System::Text::Json::JsonNode& value, int statusCode = 200);
    static HttpResult Json(std::nullptr_t, int statusCode = 200);
    static HttpResult Json(bool value, int statusCode = 200);
    static HttpResult Json(int value, int statusCode = 200);
    static HttpResult Json(long long value, int statusCode = 200);
    static HttpResult Json(double value, int statusCode = 200);
    static HttpResult Json(std::string_view value, int statusCode = 200);

    static HttpResult Ok();
    static HttpResult Ok(std::string_view text);
    static HttpResult Ok(const System::Text::Json::JsonNode& value);
    static HttpResult Created(std::string location);
    static HttpResult Created(std::string location, const System::Text::Json::JsonElement& value);
    static HttpResult Created(std::string location, const System::Text::Json::JsonNode& value);
    static HttpResult Accepted(std::string location = {});
    static HttpResult Accepted(std::string location, const System::Text::Json::JsonElement& value);
    static HttpResult Accepted(std::string location, const System::Text::Json::JsonNode& value);
    static HttpResult NoContent();
    static HttpResult BadRequest(std::string detail = {});
    static HttpResult Unauthorized(std::string detail = {});
    static HttpResult Forbidden(std::string detail = {});
    static HttpResult NotFound(std::string detail = {});
    static HttpResult Conflict(std::string detail = {});
    static HttpResult TooManyRequests(std::string detail = {});
    static HttpResult Problem(std::string title, std::string detail = {}, int statusCode = 500);
    static HttpResult Redirect(std::string location, bool permanent = false);
};

using EndpointHandler = std::function<HttpResult(HttpContext&)>;
using RequestDelegate = std::function<HttpResult(HttpContext&)>;
using Middleware = std::function<HttpResult(HttpContext&, RequestDelegate)>;
using EndpointFilter = Middleware;
using ApplicationCallback = std::function<void()>;
using RateLimitKeySelector = std::function<std::string(HttpContext&)>;
class AuthenticationResult;
using AuthenticationHandler = std::function<AuthenticationResult(HttpContext&)>;
using AuthorizationHandler = std::function<bool(HttpContext&, const ClaimsPrincipal&)>;
using HostedServiceCallback = std::function<void(class WebApplication&)>;
using BackgroundServiceCallback = std::function<void(class WebApplication&, std::stop_token)>;

enum class BindingSource {
    Route,
    Query,
    Header,
    Cookie,
    Form,
    Body,
    JsonProperty,
};

class ParameterBindingException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UnsupportedMediaTypeBindingException : public ParameterBindingException {
public:
    using ParameterBindingException::ParameterBindingException;
};

template <class T>
class BindingParameter {
public:
    using ValueType = T;

    BindingParameter(BindingSource source, std::string name = {})
        : source_(source), name_(std::move(name)) {
    }

    BindingSource Source() const noexcept { return source_; }
    const std::string& Name() const noexcept { return name_; }
    bool HasDefault() const noexcept { return defaultValue_.has_value(); }
    const T& DefaultValue() const { return *defaultValue_; }

    BindingParameter& Default(T value) & {
        defaultValue_ = std::move(value);
        return *this;
    }

    BindingParameter Default(T value) && {
        defaultValue_ = std::move(value);
        return std::move(*this);
    }

private:
    BindingSource source_;
    std::string name_;
    std::optional<T> defaultValue_;
};

template <class T>
BindingParameter<T> Route(std::string name) {
    return BindingParameter<T>(BindingSource::Route, std::move(name));
}

template <class T>
BindingParameter<T> Query(std::string name) {
    return BindingParameter<T>(BindingSource::Query, std::move(name));
}

template <class T>
BindingParameter<T> Header(std::string name) {
    return BindingParameter<T>(BindingSource::Header, std::move(name));
}

template <class T>
BindingParameter<T> Cookie(std::string name) {
    return BindingParameter<T>(BindingSource::Cookie, std::move(name));
}

template <class T>
BindingParameter<T> Form(std::string name) {
    return BindingParameter<T>(BindingSource::Form, std::move(name));
}

template <class T = System::Text::Json::JsonElement>
BindingParameter<T> Body() {
    return BindingParameter<T>(BindingSource::Body);
}

template <class T>
BindingParameter<T> JsonProperty(std::string name) {
    return BindingParameter<T>(BindingSource::JsonProperty, std::move(name));
}

enum class MachineConfigMode {
    Disabled,
    Ensure,
    Refresh,
    Delete,
};

using UrlAclMode = MachineConfigMode;

struct UrlReservationOptions {
    UrlAclMode Mode = UrlAclMode::Disabled;
    std::wstring SecurityDescriptor = L"D:(A;;GX;;;WD)";
    bool RemoveOnStop = false;
    bool IgnoreAccessDenied = false;
};

struct SslCertificateBinding {
    std::string Ip = "0.0.0.0";
    std::uint16_t Port = 443;
    std::string CertificateHashHex;
    std::wstring CertificateStoreName = L"MY";
    std::string AppId = "{7ca4c94e-7491-4de9-ba51-0bb02fb2dd60}";
    MachineConfigMode Mode = MachineConfigMode::Ensure;
    bool RemoveOnStop = false;
    bool IgnoreAccessDenied = false;
    bool DisableHttp2 = false;
    bool DisableQuic = false;
    bool DisableTls13 = false;
    bool DisableLegacyTls = true;
    bool NegotiateClientCertificate = false;
};

struct HttpSysTimeoutOptions {
    std::chrono::seconds EntityBody = std::chrono::seconds(120);
    std::chrono::seconds DrainEntityBody = std::chrono::seconds(120);
    std::chrono::seconds RequestQueue = std::chrono::seconds(120);
    std::chrono::seconds IdleConnection = std::chrono::seconds(120);
    std::chrono::seconds HeaderWait = std::chrono::seconds(120);
    std::uint32_t MinSendRateBytesPerSecond = 150;
};

struct HttpSysBackpressureOptions {
    int StoppingStatusCode = 503;
    std::string StoppingDetail = "Server is stopping.";
    int StoppingRetryAfterSeconds = 5;
    int OverloadedStatusCode = 503;
    std::string OverloadedDetail = "Too many concurrent requests.";
    int OverloadedRetryAfterSeconds = 1;
};

struct HttpSysOptions {
    std::vector<std::string> UrlPrefixes = { "http://localhost:8080/" };
    std::uint32_t WorkerCount = 0;
    std::uint32_t RequestBufferSize = 64 * 1024;
    std::uint64_t MaxRequestBodyBytes = 16ull * 1024ull * 1024ull;
    std::uint64_t MaxRequestUrlBytes = 16ull * 1024ull;
    std::uint64_t MaxRequestHeaderBytes = 64ull * 1024ull;
    std::uint32_t MaxConcurrentRequests = 0;
    std::chrono::milliseconds StopTimeout = std::chrono::seconds(5);
    bool AddServerHeader = true;
    std::string ServerHeader = "CapiWeb/" + std::string(Version);
    UrlReservationOptions UrlReservation;
    std::vector<SslCertificateBinding> SslCertificateBindings;
    HttpSysTimeoutOptions Timeouts;
    HttpSysBackpressureOptions Backpressure;
};

struct StaticFileOptions {
    std::filesystem::path Root = "wwwroot";
    std::string RequestPath = "/";
    std::vector<std::string> DefaultFiles = { "index.html", "default.html" };
    bool EnableSpaFallback = true;
    std::string SpaFallbackFile = "index.html";
    std::vector<std::string> SpaFallbackExcludedPrefixes = { "/api" };
    bool ServeUnknownFileTypes = false;
    std::string UnknownFileContentType = "application/octet-stream";
    bool ServeHiddenFiles = false;
    std::vector<std::string> BlockedFileNames = {
        ".env",
        ".env.*",
        "appsettings.json",
        "appsettings.*.json",
        "secrets.json",
        "web.config",
        "package.json",
        "package-lock.json",
        "pnpm-lock.yaml",
        "yarn.lock"
    };
    std::vector<std::string> BlockedPathSegments = {
        ".git",
        ".hg",
        ".svn",
        ".vs",
        "node_modules"
    };
    std::uint64_t MaximumFileSizeBytes = 64ull * 1024ull * 1024ull;
    std::string CacheControl;
    bool EnableConditionalRequests = true;
    bool EnableRangeProcessing = true;
    bool AddLastModifiedHeader = true;
    bool AddETagHeader = true;
};

struct ResponseCompressionOptions {
    bool Enable = true;
    bool EnableForHttps = false;
    std::size_t MinimumBodySize = 1024;
    int GzipLevel = 5;
    bool SkipWhenNotSmaller = true;
    std::vector<std::string> MimeTypes = {
        "text/",
        "application/json",
        "application/problem+json",
        "application/xml",
        "application/problem+xml",
        "application/javascript",
        "image/svg+xml"
    };
    std::vector<std::string> ExcludedMimeTypes = {
        "image/png",
        "image/jpeg",
        "image/gif",
        "image/webp",
        "image/x-icon",
        "audio/",
        "video/",
        "application/zip",
        "application/gzip",
        "application/x-gzip",
        "application/octet-stream",
        "application/pdf",
        "application/wasm",
        "font/"
    };
};

struct RequestDecompressionOptions {
    bool Enable = true;
    bool EnableGzip = true;
    bool EnableDeflate = true;
    std::uint64_t MaxDecompressedBodyBytes = 0;
    bool RemoveContentEncodingHeader = true;
    bool UpdateContentLengthHeader = true;
};

struct ResponseCachingOptions {
    bool Enable = true;
    bool AddETagHeader = true;
    bool EnableConditionalRequests = true;
    bool UseWeakETags = true;
    std::size_t MaximumBodySize = 1024 * 1024;
    std::string CacheControl;
    bool CacheAuthenticatedResponses = false;
    bool SkipSetCookieResponses = true;
};

class AuthenticationResult {
public:
    static AuthenticationResult Success(ClaimsPrincipal principal);
    static AuthenticationResult NoResult();
    static AuthenticationResult Fail(std::string failure, std::string challenge = {});

    bool Succeeded() const noexcept;
    bool None() const noexcept;
    bool Failed() const noexcept;
    const ClaimsPrincipal& Principal() const noexcept;
    const std::string& Failure() const noexcept;
    const std::string& Challenge() const noexcept;

private:
    bool succeeded_ = false;
    bool none_ = true;
    ClaimsPrincipal principal_;
    std::string failure_;
    std::string challenge_;
};

struct AuthenticationScheme {
    std::string Name = "Default";
    AuthenticationHandler Handler;
    std::string Challenge;
};

struct AuthenticationOptions {
    std::vector<AuthenticationScheme> Schemes;
    std::string DefaultScheme;
    std::string UserItemKey = "User";
    std::string FailureItemKey = "AuthenticationFailure";
    std::string ChallengeItemKey = "AuthenticationChallenge";
};

struct AuthorizationClaimRequirement {
    std::string Type;
    std::string Value;
};

struct AuthorizationPolicy {
    std::string Name;
    bool RequireAuthenticatedUser = true;
    std::vector<std::string> RequiredRoles;
    std::vector<AuthorizationClaimRequirement> RequiredClaims;
    AuthorizationHandler Assertion;

    AuthorizationPolicy& RequireRole(std::string role);
    AuthorizationPolicy& RequireRoles(std::initializer_list<std::string> roles);
    AuthorizationPolicy& RequireRoles(std::vector<std::string> roles);
    AuthorizationPolicy& RequireClaim(std::string type);
    AuthorizationPolicy& RequireClaim(std::string type, std::string value);
};

struct AuthorizationOptions {
    bool RequireAuthenticatedUserByDefault = false;
    AuthorizationPolicy DefaultPolicy;
    std::vector<AuthorizationPolicy> Policies;
    std::string Challenge;
    std::string UserItemKey = "User";
    std::string FailureItemKey = "AuthorizationFailure";
    std::string AuthenticationChallengeItemKey = "AuthenticationChallenge";
    bool IncludeFailureDetails = false;
};

struct CorsOptions {
    std::vector<std::string> AllowedOrigins = { "*" };
    std::vector<std::string> AllowedMethods = { "GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
    std::vector<std::string> AllowedHeaders = { "*" };
    std::vector<std::string> ExposedHeaders;
    bool AllowCredentials = false;
    int MaxAgeSeconds = 600;
    bool RejectInvalidPreflightRequests = false;
    int PreflightFailureStatusCode = 403;
};

struct SecurityHeadersOptions {
    bool AddContentTypeOptions = true;
    bool AddFrameOptions = true;
    bool AddReferrerPolicy = true;
    bool AddCrossOriginOpenerPolicy = true;
    std::string FrameOptions = "DENY";
    std::string ReferrerPolicy = "no-referrer";
    std::string CrossOriginOpenerPolicy = "same-origin";
    std::string ContentSecurityPolicy;
};

struct HstsOptions {
    bool Enable = true;
    std::chrono::seconds MaxAge = std::chrono::seconds(31536000);
    bool IncludeSubDomains = false;
    bool Preload = false;
    std::vector<std::string> ExcludedHosts = { "localhost", "127.0.0.1", "::1" };
};

struct HttpsRedirectionOptions {
    int RedirectStatusCode = 307;
    std::uint16_t HttpsPort = 0;
    bool AllowEmptyHost = false;
    std::string FailureDetail = "The request host is required for HTTPS redirection.";
};

struct HostFilteringOptions {
    std::vector<std::string> AllowedHosts;
    bool AllowEmptyHost = false;
    int RejectedStatusCode = 400;
    std::string FailureDetail = "The requested host is not allowed.";
    std::string ItemKey = "HostFilteringRejected";
};

struct RequestLoggingOptions {
    bool IncludeQueryString = false;
    bool IncludeRemoteAddress = true;
    bool IncludeTraceIdentifier = true;
    bool IncludeEndpoint = false;
    LogLevel SuccessLevel = LogLevel::Information;
    LogLevel FailureLevel = LogLevel::Warning;
    std::vector<std::string> RedactedQueryParameters = {
        "access_token",
        "api_key",
        "apikey",
        "authorization",
        "client_secret",
        "code",
        "password",
        "refresh_token",
        "secret",
        "token"
    };
    std::string RedactedQueryValue = "[redacted]";
};

struct RequestIdOptions {
    std::string HeaderName = "X-Request-ID";
    std::string ResponseHeaderName = "X-Request-ID";
    bool TrustIncomingHeader = true;
    bool AddResponseHeader = true;
    std::size_t MaxLength = 128;
    std::string ItemKey = "RequestId";
};

struct ServerTimingOptions {
    bool Enable = true;
    bool AddServerTimingHeader = true;
    bool AddResponseTimeHeader = false;
    std::string ServerTimingHeaderName = "Server-Timing";
    std::string ResponseTimeHeaderName = "X-Response-Time";
    std::string MetricName = "app";
    std::string MetricDescription = "Application";
    std::string ItemKey = "ServerTimingElapsedMilliseconds";
    int DecimalPlaces = 3;
};

struct RateLimitOptions {
    std::uint32_t PermitLimit = 100;
    std::chrono::seconds Window = std::chrono::seconds(60);
    RateLimitKeySelector KeySelector;
    int RejectedStatusCode = 429;
    std::size_t MaxTrackedKeys = 10000;
    std::size_t MaxKeyLength = 256;
    bool AddRateLimitHeaders = true;
    std::string RetryAfterHeaderName = "Retry-After";
    std::string LimitHeaderName = "X-RateLimit-Limit";
    std::string RemainingHeaderName = "X-RateLimit-Remaining";
    std::string ResetHeaderName = "X-RateLimit-Reset";
    std::string ItemKey = "RateLimitKey";
};

struct ForwardedHeadersOptions {
    bool ForwardedFor = true;
    bool ForwardedProto = true;
    bool ForwardedHost = true;
    std::string ForwardedForHeaderName = "X-Forwarded-For";
    std::string ForwardedProtoHeaderName = "X-Forwarded-Proto";
    std::string ForwardedHostHeaderName = "X-Forwarded-Host";
    std::vector<std::string> KnownProxies = { "127.0.0.1", "::1" };
    bool RequireKnownProxy = true;
    std::uint32_t ForwardLimit = 1;
    std::string AppliedItemKey = "ForwardedHeadersApplied";
};

struct MetricsOptions {
    bool IncludeMethodCounters = true;
    bool IncludeStatusCodeCounters = true;
    bool IncludePathCounters = false;
    bool UseMatchedEndpointPatternForPathCounters = true;
};

struct MetricsEndpointOptions {
    std::string Path = "/metrics";
    bool IncludeDetails = true;
};

enum class HealthStatus {
    Healthy,
    Degraded,
    Unhealthy,
};

struct HealthCheckResult {
    HealthStatus Status = HealthStatus::Healthy;
    std::string Description;

    static HealthCheckResult Healthy(std::string description = {});
    static HealthCheckResult Degraded(std::string description = {});
    static HealthCheckResult Unhealthy(std::string description = {});
};

using HealthCheck = std::function<HealthCheckResult()>;

struct HealthCheckRegistration {
    std::string Name;
    HealthCheck Check;
    std::vector<std::string> Tags;
};

using HealthCheckPredicate = std::function<bool(const HealthCheckRegistration&)>;

struct HealthCheckOptions {
    std::string Path = "/health";
    bool IncludeDetails = false;
    bool IncludeApplicationState = true;
    bool TreatDegradedAsFailure = false;
    bool TreatStoppingAsFailure = true;
    int FailureStatusCode = 503;
    std::vector<std::string> Tags;
    HealthCheckPredicate Predicate;
};

struct OpenApiInfo {
    std::string Title = "CapiWeb API";
    std::string Version = "v1";
    std::string Description;
};

struct OpenApiParameter {
    std::string Name;
    std::string In = "query";
    std::string SchemaType = "string";
    std::string SchemaFormat;
    bool IsArray = false;
    std::string ItemsSchemaType;
    std::string ItemsSchemaFormat;
    bool Required = false;
    std::string Description;
};

struct OpenApiResponse {
    int StatusCode = 200;
    std::string ContentType = "application/json";
    std::string Description;
};

struct OpenApiRequestBody {
    std::string ContentType = "application/json";
    bool Required = true;
    std::string Description;
};

struct OpenApiSecurityScheme {
    std::string Name;
    std::string Type = "apiKey";
    std::string In = "header";
    std::string ParameterName = "Authorization";
    std::string Scheme;
    std::string BearerFormat;
    std::string Description;
};

struct OpenApiOptions {
    std::string Path = "/openapi.json";
    OpenApiInfo Info;
    std::vector<std::string> ServerUrls;
    std::vector<OpenApiSecurityScheme> SecuritySchemes;
    std::string DefaultSecurityScheme;
};

struct SwaggerUiOptions {
    std::string Path = "/docs";
    std::string OpenApiPath = "/openapi.json";
    std::string Title = "CapiWeb API";
};

struct HostedServiceOptions {
    std::string Name;
    HostedServiceCallback Start;
    HostedServiceCallback Stop;
};

struct BackgroundServiceOptions {
    std::string Name;
    BackgroundServiceCallback Execute;
    bool StopApplicationOnException = true;
    std::chrono::milliseconds StopTimeout = std::chrono::seconds(30);
    bool DetachOnStopTimeout = false;
};

struct WebApplicationOptions {
    HttpSysOptions HttpSys;
    Logger Log;
    bool DetailedErrors = false;
    ProblemDetailsOptions ProblemDetails;
    FormOptions Forms;
};

class UrlReservationManager final {
public:
    UrlReservationManager() = delete;

    static void Apply(const std::vector<std::string>& prefixes, const UrlReservationOptions& options);
    static void Ensure(const std::vector<std::string>& prefixes, const std::wstring& securityDescriptor);
    static void Refresh(const std::vector<std::string>& prefixes, const std::wstring& securityDescriptor);
    static void Remove(const std::vector<std::string>& prefixes, bool ignoreMissing = true);
};

class SslBindingManager final {
public:
    SslBindingManager() = delete;

    static void Apply(const std::vector<SslCertificateBinding>& bindings);
    static void Ensure(const SslCertificateBinding& binding);
    static void Refresh(const SslCertificateBinding& binding);
    static void Remove(const SslCertificateBinding& binding, bool ignoreMissing = true);
};

class WebApplication;
class RouteGroupBuilder;

enum class WindowsServiceStartMode {
    Manual,
    Automatic,
    AutomaticDelayed,
};

struct WindowsServiceOptions {
    std::wstring ServiceName;
    std::wstring DisplayName;
    std::wstring Description;
    WindowsServiceStartMode StartMode = WindowsServiceStartMode::Automatic;
    std::vector<std::wstring> Dependencies;
    std::wstring AccountName;
    std::wstring Password;
    std::vector<std::wstring> Arguments;
    bool FallbackToConsole = true;
    std::chrono::seconds StopTimeout = std::chrono::seconds(30);
};

class WindowsServiceHost final {
public:
    WindowsServiceHost() = delete;

    static bool IsRunningAsService();
    static int Run(WebApplication& app, WindowsServiceOptions options);
    static void Install(const WindowsServiceOptions& options, const std::filesystem::path& executablePath = {});
    static void Uninstall(std::wstring serviceName, std::chrono::seconds stopTimeout = std::chrono::seconds(30));
    static void Start(std::wstring serviceName);
    static void Stop(std::wstring serviceName, std::chrono::seconds timeout = std::chrono::seconds(30));
};

class WebApplicationBuilder {
public:
    WebApplicationBuilder();
    explicit WebApplicationBuilder(WebApplicationOptions options);

    WebApplicationBuilder& UseUrls(std::initializer_list<std::string> prefixes);
    WebApplicationBuilder& UseUrls(std::vector<std::string> prefixes);
    WebApplicationBuilder& ConfigureFromJson(std::string_view json);
    WebApplicationBuilder& ConfigureFromJsonFile(const std::filesystem::path& path = "appsettings.json", bool optional = true);
    WebApplicationBuilder& ConfigureFromEnvironment(std::string prefix = "CAPIWEB_");
    WebApplicationBuilder& ConfigureHttpSys(std::function<void(HttpSysOptions&)> configure);
    WebApplicationBuilder& ConfigureProblemDetails(std::function<void(ProblemDetailsOptions&)> configure);
    WebApplicationBuilder& ConfigureFormOptions(std::function<void(FormOptions&)> configure);
    WebApplicationBuilder& ConfigureLogging(Logger logger);
    WebApplicationBuilder& UseDetailedErrors(bool enabled = true);
    WebApplicationBuilder& UseUrlAcl(UrlAclMode mode, std::wstring securityDescriptor = L"D:(A;;GX;;;WD)");
    WebApplicationBuilder& UseHttps(SslCertificateBinding binding);

    WebApplication Build() const;
    const WebApplicationOptions& Options() const noexcept;

private:
    WebApplicationOptions options_;
};

class WebApplication {
public:
    WebApplication();
    explicit WebApplication(WebApplicationOptions options);
    ~WebApplication();

    WebApplication(WebApplication&& other) noexcept;
    WebApplication& operator=(WebApplication&& other) noexcept;

    WebApplication(const WebApplication&) = delete;
    WebApplication& operator=(const WebApplication&) = delete;

    static WebApplicationBuilder CreateBuilder();
    static WebApplicationBuilder CreateBuilder(WebApplicationOptions options);
    static WebApplication Create();
    static WebApplication Create(WebApplicationOptions options);

    WebApplication& Use(Middleware middleware);
    WebApplication& UseProblemDetails();
    WebApplication& UseProblemDetails(ProblemDetailsOptions options);
    WebApplication& UseStaticFiles(StaticFileOptions options = {});
    WebApplication& UseRequestDecompression(RequestDecompressionOptions options = {});
    WebApplication& UseResponseCompression(ResponseCompressionOptions options = {});
    WebApplication& UseResponseCaching(ResponseCachingOptions options = {});
    WebApplication& UseAuthentication(AuthenticationOptions options);
    WebApplication& UseAuthorization(AuthorizationOptions options = {});
    WebApplication& UseCors(CorsOptions options = {});
    WebApplication& UseSecurityHeaders(SecurityHeadersOptions options = {});
    WebApplication& UseHsts(HstsOptions options = {});
    WebApplication& UseHttpsRedirection(HttpsRedirectionOptions options = {});
    WebApplication& UseForwardedHeaders(ForwardedHeadersOptions options = {});
    WebApplication& UseHostFiltering(HostFilteringOptions options);
    WebApplication& UseRequestId(RequestIdOptions options = {});
    WebApplication& UseServerTiming(ServerTimingOptions options = {});
    WebApplication& UseRateLimiter(RateLimitOptions options = {});
    WebApplication& UseRequestLogging(RequestLoggingOptions options = {});
    WebApplication& UseMetrics(MetricsOptions options = {});
    WebApplication& MapMetrics(MetricsEndpointOptions options = {});
    WebApplication& AddHealthCheck(std::string name, HealthCheck check, std::vector<std::string> tags = {});
    WebApplication& MapHealthChecks(HealthCheckOptions options = {});
    WebApplication& UseHealthChecks(HealthCheckOptions options = {});
    WebApplication& MapOpenApi(OpenApiOptions options = {});
    WebApplication& MapSwaggerUi(SwaggerUiOptions options = {});
    RouteGroupBuilder MapGroup(std::string prefix);
    WebApplication& OnStarted(ApplicationCallback callback);
    WebApplication& OnStopping(ApplicationCallback callback);
    WebApplication& OnStopped(ApplicationCallback callback);
    WebApplication& AddHostedService(HostedServiceOptions service);
    WebApplication& AddBackgroundService(BackgroundServiceOptions service);
    WebApplication& MapMethods(std::initializer_list<std::string> methods, std::string pattern, EndpointHandler handler);
    WebApplication& MapMethods(std::vector<std::string> methods, std::string pattern, EndpointHandler handler);
    WebApplication& MapFallback(EndpointHandler handler);
    std::optional<std::string> TryPathByName(std::string_view name, const ValueMap& routeValues = {}, const ValueMap& queryValues = {}) const;
    std::string PathByName(std::string_view name, const ValueMap& routeValues = {}, const ValueMap& queryValues = {}) const;
    std::optional<std::string> TryUrlByName(const HttpContext& context, std::string_view name, const ValueMap& routeValues = {}, const ValueMap& queryValues = {}) const;
    std::optional<std::string> TryUrlByName(const HttpRequest& request, std::string_view name, const ValueMap& routeValues = {}, const ValueMap& queryValues = {}) const;
    std::string UrlByName(const HttpContext& context, std::string_view name, const ValueMap& routeValues = {}, const ValueMap& queryValues = {}) const;
    std::string UrlByName(const HttpRequest& request, std::string_view name, const ValueMap& routeValues = {}, const ValueMap& queryValues = {}) const;
    WebApplication& RequireAuthorization(std::string policy = {});
    WebApplication& RequireAuthorization(std::initializer_list<std::string> policies);
    WebApplication& RequireAuthorization(AuthorizationPolicy policy);
    WebApplication& RequireRole(std::string role);
    WebApplication& RequireRoles(std::initializer_list<std::string> roles);
    WebApplication& RequireRoles(std::vector<std::string> roles);
    WebApplication& RequireClaim(std::string type);
    WebApplication& RequireClaim(std::string type, std::string value);
    WebApplication& AllowAnonymous();
    WebApplication& ExcludeFromDescription();
    WebApplication& AddEndpointFilter(EndpointFilter filter);
    WebApplication& WithName(std::string name);
    WebApplication& WithTags(std::initializer_list<std::string> tags);
    WebApplication& WithTags(std::vector<std::string> tags);
    WebApplication& WithSummary(std::string summary);
    WebApplication& WithDescription(std::string description);
    WebApplication& WithParameter(OpenApiParameter parameter);
    WebApplication& WithQueryParameter(std::string name, std::string schemaType = "string", bool required = false, std::string description = {});
    WebApplication& WithHeaderParameter(std::string name, std::string schemaType = "string", bool required = false, std::string description = {});
    WebApplication& WithQueryArrayParameter(std::string name, std::string itemSchemaType = "string", bool required = false, std::string description = {}, std::string itemSchemaFormat = {});
    WebApplication& WithHeaderArrayParameter(std::string name, std::string itemSchemaType = "string", bool required = false, std::string description = {}, std::string itemSchemaFormat = {});
    WebApplication& Produces(int statusCode = 200, std::string contentType = "application/json", std::string description = {});
    WebApplication& Accepts(std::string contentType = "application/json", bool required = true, std::string description = {});

    template <class Handler>
    WebApplication& MapGet(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapHead(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapOptions(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapPost(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapPut(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapPatch(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapDelete(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapAny(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapFallback(Handler&& handler);

    void Start();
    void Stop();
    void WaitForShutdown();
    void Run();
    HttpResponse Handle(HttpContext& context);
    HttpResponse Handle(HttpRequest request);
    bool IsRunning() const noexcept;
    bool IsStopping() const noexcept;
    std::uint64_t ActiveRequestCount() const noexcept;
    const WebApplicationOptions& Options() const noexcept;

private:
    friend class RouteGroupBuilder;

    void EnsureCanConfigure(std::string_view operation) const;
    void MarkLastEndpointAuthorizationRequirementsAsGroup();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class RouteGroupBuilder {
public:
    RouteGroupBuilder(WebApplication& app, std::string prefix);

    const std::string& Prefix() const noexcept;
    RouteGroupBuilder MapGroup(std::string prefix) const;

    RouteGroupBuilder& RequireAuthorization(std::string policy = {});
    RouteGroupBuilder& RequireAuthorization(std::initializer_list<std::string> policies);
    RouteGroupBuilder& RequireAuthorization(AuthorizationPolicy policy);
    RouteGroupBuilder& RequireRole(std::string role);
    RouteGroupBuilder& RequireRoles(std::initializer_list<std::string> roles);
    RouteGroupBuilder& RequireRoles(std::vector<std::string> roles);
    RouteGroupBuilder& RequireClaim(std::string type);
    RouteGroupBuilder& RequireClaim(std::string type, std::string value);
    RouteGroupBuilder& AllowAnonymous();
    RouteGroupBuilder& AddEndpointFilter(EndpointFilter filter);
    RouteGroupBuilder& WithTags(std::initializer_list<std::string> tags);
    RouteGroupBuilder& WithTags(std::vector<std::string> tags);

    WebApplication& MapMethods(std::initializer_list<std::string> methods, std::string pattern, EndpointHandler handler);
    WebApplication& MapMethods(std::vector<std::string> methods, std::string pattern, EndpointHandler handler);

    template <class Handler>
    WebApplication& MapGet(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapHead(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapOptions(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapPost(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapPut(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapPatch(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapDelete(std::string pattern, Handler&& handler);

    template <class Handler>
    WebApplication& MapAny(std::string pattern, Handler&& handler);

private:
    WebApplication& App() const;
    std::string FullPattern(std::string pattern) const;
    WebApplication& ApplyGroupMetadata(WebApplication& app) const;
    AuthorizationPolicy& LocalAuthorizationRequirement();

    WebApplication* app_ = nullptr;
    std::string prefix_;
    bool requireAuthorization_ = false;
    bool allowAnonymous_ = false;
    std::vector<std::string> authorizationPolicies_;
    std::vector<AuthorizationPolicy> authorizationRequirements_;
    std::size_t inheritedAuthorizationRequirementCount_ = 0;
    std::vector<EndpointFilter> endpointFilters_;
    std::vector<std::string> tags_;
};

namespace detail {

template <class>
inline constexpr bool AlwaysFalse = false;

template <class T>
HttpResult ToHttpResult(T&& value) {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, HttpResult>) {
        return std::forward<T>(value);
    } else if constexpr (std::is_same_v<Decayed, std::string>) {
        return Results::Text(value);
    } else if constexpr (std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*>) {
        return Results::Text(value ? std::string_view(value) : std::string_view());
    } else if constexpr (std::is_same_v<Decayed, System::Text::Json::JsonElement>) {
        return Results::Json(value);
    } else if constexpr (std::is_base_of_v<System::Text::Json::JsonNode, Decayed>) {
        return Results::Json(static_cast<const System::Text::Json::JsonNode&>(value));
    } else if constexpr (std::is_same_v<Decayed, std::nullptr_t>) {
        return Results::Json(nullptr);
    } else if constexpr (std::is_same_v<Decayed, bool>) {
        return Results::Json(value);
    } else if constexpr (std::is_integral_v<Decayed> && !std::is_same_v<Decayed, bool>) {
        return Results::Json(static_cast<long long>(value));
    } else if constexpr (std::is_floating_point_v<Decayed>) {
        return Results::Json(static_cast<double>(value));
    } else {
        static_assert(AlwaysFalse<Decayed>, "Handler return type cannot be converted to Capi::Web::HttpResult.");
    }
}

inline std::string BindingTrim(std::string value) {
    auto isSpace = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

inline std::string BindingToLower(std::string_view value) {
    std::string result(value);
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

inline bool BindingEqualsIgnoreCase(std::string_view left, std::string_view right) {
    return left.size() == right.size() && BindingToLower(left) == BindingToLower(right);
}

inline std::string JsonBindingContentTypeMessage(const HttpRequest& request, std::string_view label) {
    std::string contentType = request.Header("Content-Type");
    if (BindingTrim(contentType).empty()) {
        return "Request Content-Type is required to bind " + std::string(label) +
            ". Supported content types: application/json, application/*+json.";
    }
    return "Unsupported request Content-Type '" + contentType + "' while binding " + std::string(label) +
        ". Supported content types: application/json, application/*+json.";
}

inline void RequireJsonContentType(const HttpRequest& request, std::string_view label) {
    if (!request.HasJsonContentType()) {
        throw UnsupportedMediaTypeBindingException(JsonBindingContentTypeMessage(request, label));
    }
}

template <class T>
struct IsOptional : std::false_type {
};

template <class T>
struct IsOptional<std::optional<T>> : std::true_type {
    using ValueType = T;
};

template <class T>
inline constexpr bool IsOptionalV = IsOptional<std::decay_t<T>>::value;

template <class T>
using OptionalValueType = typename IsOptional<std::decay_t<T>>::ValueType;

template <class T>
struct IsVector : std::false_type {
};

template <class T, class Allocator>
struct IsVector<std::vector<T, Allocator>> : std::true_type {
    using ValueType = T;
};

template <class T>
inline constexpr bool IsVectorV = IsVector<std::decay_t<T>>::value;

template <class T>
using VectorValueType = typename IsVector<std::decay_t<T>>::ValueType;

template <class T>
struct IsTextBindable
    : std::bool_constant<
        std::is_same_v<std::decay_t<T>, std::string> ||
        std::is_same_v<std::decay_t<T>, bool> ||
        (std::is_integral_v<std::decay_t<T>> && !std::is_same_v<std::decay_t<T>, bool>) ||
        std::is_floating_point_v<std::decay_t<T>>> {
};

template <class T>
struct IsTextBindable<std::optional<T>> : IsTextBindable<T> {
};

template <class T>
inline constexpr bool IsTextBindableV = IsTextBindable<std::decay_t<T>>::value;

template <class T>
T MissingBindingValue(const BindingParameter<T>& binding, std::string_view label) {
    if (binding.HasDefault()) {
        return binding.DefaultValue();
    }
    if constexpr (IsOptionalV<T>) {
        return std::nullopt;
    } else {
        throw ParameterBindingException("Missing required " + std::string(label) + " parameter '" + binding.Name() + "'.");
    }
}

template <class T>
T MissingVectorBindingValue(const BindingParameter<T>& binding) {
    if (binding.HasDefault()) {
        return binding.DefaultValue();
    }
    return {};
}

template <class T>
T ConvertTextValue(std::string_view text, std::string_view label) {
    using Decayed = std::decay_t<T>;
    if constexpr (IsOptionalV<Decayed>) {
        using Inner = OptionalValueType<Decayed>;
        return ConvertTextValue<Inner>(text, label);
    } else if constexpr (std::is_same_v<Decayed, std::string>) {
        return std::string(text);
    } else if constexpr (std::is_same_v<Decayed, bool>) {
        std::string normalized = BindingToLower(BindingTrim(std::string(text)));
        if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
            return true;
        }
        if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
            return false;
        }
        throw ParameterBindingException("Parameter '" + std::string(label) + "' must be a boolean value.");
    } else if constexpr (std::is_integral_v<Decayed>) {
        Decayed value{};
        std::string normalized = BindingTrim(std::string(text));
        auto [ptr, ec] = std::from_chars(normalized.data(), normalized.data() + normalized.size(), value);
        if (ec != std::errc{} || ptr != normalized.data() + normalized.size()) {
            throw ParameterBindingException("Parameter '" + std::string(label) + "' must be an integer value.");
        }
        return value;
    } else if constexpr (std::is_floating_point_v<Decayed>) {
        Decayed value{};
        std::string normalized = BindingTrim(std::string(text));
        auto [ptr, ec] = std::from_chars(normalized.data(), normalized.data() + normalized.size(), value);
        if (ec != std::errc{} || ptr != normalized.data() + normalized.size()) {
            throw ParameterBindingException("Parameter '" + std::string(label) + "' must be a number.");
        }
        return value;
    } else {
        static_assert(AlwaysFalse<Decayed>, "Unsupported text binding parameter type.");
    }
}

template <class T>
T ConvertTextVectorBindingValue(const std::vector<std::string>& values, std::string_view name, std::string_view source) {
    if constexpr (IsVectorV<T>) {
        using Element = VectorValueType<T>;
        if constexpr (IsTextBindableV<Element>) {
            T result;
            result.reserve(values.size());
            for (const std::string& value : values) {
                result.push_back(ConvertTextValue<Element>(value, name));
            }
            return result;
        } else {
            throw ParameterBindingException(
                "Parameter '" + std::string(name) + "' cannot be bound from " + std::string(source) + ".");
        }
    } else {
        static_assert(AlwaysFalse<T>, "ConvertTextVectorBindingValue requires std::vector<T>.");
    }
}

template <class T>
T ConvertJsonValue(const System::Text::Json::JsonElement& element, std::string_view label) {
    using Decayed = std::decay_t<T>;
    if constexpr (IsOptionalV<Decayed>) {
        using Inner = OptionalValueType<Decayed>;
        if (element.ValueKind() == System::Text::Json::JsonValueKind::Null ||
            element.ValueKind() == System::Text::Json::JsonValueKind::Undefined) {
            return std::nullopt;
        }
        return ConvertJsonValue<Inner>(element, label);
    } else if constexpr (IsVectorV<Decayed>) {
        using Element = VectorValueType<Decayed>;
        if (element.ValueKind() != System::Text::Json::JsonValueKind::Array) {
            throw ParameterBindingException("JSON parameter '" + std::string(label) + "' must be an array.");
        }
        Decayed result;
        for (const auto& item : element.EnumerateArray()) {
            result.push_back(ConvertJsonValue<Element>(item, label));
        }
        return result;
    } else if constexpr (std::is_same_v<Decayed, System::Text::Json::JsonElement>) {
        return element;
    } else if constexpr (std::is_same_v<Decayed, std::string>) {
        if (element.ValueKind() != System::Text::Json::JsonValueKind::String) {
            throw ParameterBindingException("JSON parameter '" + std::string(label) + "' must be a string.");
        }
        return element.GetString();
    } else if constexpr (std::is_same_v<Decayed, bool>) {
        if (element.ValueKind() != System::Text::Json::JsonValueKind::True &&
            element.ValueKind() != System::Text::Json::JsonValueKind::False) {
            throw ParameterBindingException("JSON parameter '" + std::string(label) + "' must be a boolean value.");
        }
        return element.GetBoolean();
    } else if constexpr (std::is_same_v<Decayed, int>) {
        int value = 0;
        if (!element.TryGetInt32(value)) {
            throw ParameterBindingException("JSON parameter '" + std::string(label) + "' must be an Int32 value.");
        }
        return value;
    } else if constexpr (std::is_integral_v<Decayed>) {
        long long value = 0;
        if (!element.TryGetInt64(value) ||
            value < static_cast<long long>((std::numeric_limits<Decayed>::min)()) ||
            value > static_cast<long long>((std::numeric_limits<Decayed>::max)())) {
            throw ParameterBindingException("JSON parameter '" + std::string(label) + "' must be an integer value.");
        }
        return static_cast<Decayed>(value);
    } else if constexpr (std::is_floating_point_v<Decayed>) {
        double value = 0;
        if (!element.TryGetDouble(value)) {
            throw ParameterBindingException("JSON parameter '" + std::string(label) + "' must be a number.");
        }
        return static_cast<Decayed>(value);
    } else {
        static_assert(AlwaysFalse<Decayed>, "Unsupported JSON binding parameter type.");
    }
}

template <class T>
T ConvertTextBindingValue(std::string_view text, std::string_view name, std::string_view source) {
    if constexpr (IsTextBindableV<T>) {
        return ConvertTextValue<T>(text, name);
    } else {
        throw ParameterBindingException(
            "Parameter '" + std::string(name) + "' cannot be bound from " + std::string(source) + ".");
    }
}

template <class T>
T ResolveBindingValue(const BindingParameter<T>& binding, HttpContext& context) {
    const std::string& name = binding.Name();
    switch (binding.Source()) {
    case BindingSource::Route: {
        if constexpr (IsVectorV<T>) {
            auto it = context.Request().RouteValues().find(name);
            if (it == context.Request().RouteValues().end()) {
                return MissingVectorBindingValue(binding);
            }
            std::vector<std::string> values = { it->second };
            return ConvertTextVectorBindingValue<T>(values, name, "route");
        } else {
            auto it = context.Request().RouteValues().find(name);
            if (it == context.Request().RouteValues().end()) {
                return MissingBindingValue(binding, "route");
            }
            return ConvertTextBindingValue<T>(it->second, name, "route");
        }
    }
    case BindingSource::Query: {
        if constexpr (IsVectorV<T>) {
            const ValueListMap& values = context.Request().QueryValues();
            auto it = values.find(name);
            if (it == values.end()) {
                return MissingVectorBindingValue(binding);
            }
            return ConvertTextVectorBindingValue<T>(it->second, name, "query");
        } else {
            auto it = context.Request().Query().find(name);
            if (it == context.Request().Query().end()) {
                return MissingBindingValue(binding, "query");
            }
            return ConvertTextBindingValue<T>(it->second, name, "query");
        }
    }
    case BindingSource::Header: {
        if constexpr (IsVectorV<T>) {
            std::vector<std::string> values = context.Request().Headers().GetValues(name);
            if (values.empty()) {
                return MissingVectorBindingValue(binding);
            }
            return ConvertTextVectorBindingValue<T>(values, name, "header");
        } else {
            auto value = context.Request().Headers().TryGet(name);
            if (!value) {
                return MissingBindingValue(binding, "header");
            }
            return ConvertTextBindingValue<T>(*value, name, "header");
        }
    }
    case BindingSource::Cookie: {
        const ValueMap& cookies = context.Request().Cookies();
        if constexpr (IsVectorV<T>) {
            auto it = cookies.find(name);
            if (it == cookies.end()) {
                return MissingVectorBindingValue(binding);
            }
            std::vector<std::string> values = { it->second };
            return ConvertTextVectorBindingValue<T>(values, name, "cookie");
        } else {
            auto it = cookies.find(name);
            if (it == cookies.end()) {
                return MissingBindingValue(binding, "cookie");
            }
            return ConvertTextBindingValue<T>(it->second, name, "cookie");
        }
    }
    case BindingSource::Form: {
        if (!context.Request().HasFormContentType()) {
            throw UnsupportedMediaTypeBindingException(
                "Request Content-Type must be application/x-www-form-urlencoded or multipart/form-data to bind form parameter '" + name + "'.");
        }
        if constexpr (IsVectorV<T>) {
            const ValueListMap& values = context.Request().FormValues();
            auto it = values.find(name);
            if (it == values.end()) {
                return MissingVectorBindingValue(binding);
            }
            return ConvertTextVectorBindingValue<T>(it->second, name, "form");
        } else {
            const ValueMap& form = context.Request().Form();
            auto it = form.find(name);
            if (it == form.end()) {
                return MissingBindingValue(binding, "form");
            }
            return ConvertTextBindingValue<T>(it->second, name, "form");
        }
    }
    case BindingSource::Body: {
        if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
            return context.Request().Body();
        } else {
            RequireJsonContentType(context.Request(), "JSON request body");
            return ConvertJsonValue<T>(context.Request().Json(), "body");
        }
    }
    case BindingSource::JsonProperty: {
        RequireJsonContentType(context.Request(), "JSON property '" + name + "'");
        System::Text::Json::JsonElement root = context.Request().Json();
        if (root.ValueKind() != System::Text::Json::JsonValueKind::Object) {
            throw ParameterBindingException("JSON request body must be an object to bind property '" + name + "'.");
        }
        System::Text::Json::JsonElement value;
        bool found = root.TryGetProperty(name, value);
        if (!found) {
            for (const auto& [propertyName, propertyValue] : root.EnumerateObject()) {
                if (BindingEqualsIgnoreCase(propertyName, name)) {
                    value = propertyValue;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            return MissingBindingValue(binding, "JSON body");
        }
        return ConvertJsonValue<T>(value, name);
    }
    default:
        throw ParameterBindingException("Unsupported binding source.");
    }
}

template <class>
struct IsBindingParameter : std::false_type {
};

template <class T>
struct IsBindingParameter<BindingParameter<T>> : std::true_type {
};

template <class T>
inline constexpr bool IsBindingParameterV = IsBindingParameter<std::decay_t<T>>::value;

template <class Handler>
EndpointHandler MakeEndpointHandler(Handler&& handler) {
    using Fn = std::decay_t<Handler>;
    return [fn = Fn(std::forward<Handler>(handler))](HttpContext& context) mutable -> HttpResult {
        if constexpr (std::is_invocable_v<Fn&, HttpContext&>) {
            using ReturnType = std::invoke_result_t<Fn&, HttpContext&>;
            if constexpr (std::is_void_v<ReturnType>) {
                fn(context);
                return Results::Empty();
            } else {
                return ToHttpResult(fn(context));
            }
        } else if constexpr (std::is_invocable_v<Fn&, HttpRequest&>) {
            using ReturnType = std::invoke_result_t<Fn&, HttpRequest&>;
            if constexpr (std::is_void_v<ReturnType>) {
                fn(context.Request());
                return Results::Empty();
            } else {
                return ToHttpResult(fn(context.Request()));
            }
        } else if constexpr (std::is_invocable_v<Fn&, const HttpRequest&>) {
            using ReturnType = std::invoke_result_t<Fn&, const HttpRequest&>;
            if constexpr (std::is_void_v<ReturnType>) {
                fn(context.Request());
                return Results::Empty();
            } else {
                return ToHttpResult(fn(context.Request()));
            }
        } else if constexpr (std::is_invocable_v<Fn&>) {
            using ReturnType = std::invoke_result_t<Fn&>;
            if constexpr (std::is_void_v<ReturnType>) {
                fn();
                return Results::Empty();
            } else {
                return ToHttpResult(fn());
            }
        } else {
            static_assert(AlwaysFalse<Fn>, "Endpoint handler must be invocable with HttpContext&, HttpRequest&, const HttpRequest&, or no arguments.");
        }
    };
}

template <class Handler, class... Bindings>
EndpointHandler MakeBoundEndpointHandler(Handler&& handler, Bindings&&... bindings) {
    static_assert((IsBindingParameterV<Bindings> && ...), "Bind arguments after handler must be Capi::Web binding parameters.");

    using Fn = std::decay_t<Handler>;
    using BindingTuple = std::tuple<std::decay_t<Bindings>...>;
    return [fn = Fn(std::forward<Handler>(handler)),
        bindingTuple = BindingTuple(std::forward<Bindings>(bindings)...)](HttpContext& context) mutable -> HttpResult {
        try {
            return std::apply([&](auto&... currentBindings) -> HttpResult {
                using ReturnType = std::invoke_result_t<Fn&, typename std::decay_t<decltype(currentBindings)>::ValueType...>;
                if constexpr (std::is_void_v<ReturnType>) {
                    fn(ResolveBindingValue(currentBindings, context)...);
                    return Results::Empty();
                } else {
                    return ToHttpResult(fn(ResolveBindingValue(currentBindings, context)...));
                }
            }, bindingTuple);
        } catch (const UnsupportedMediaTypeBindingException& ex) {
            return Results::Problem("Unsupported Media Type", ex.what(), 415);
        } catch (const ParameterBindingException& ex) {
            return Results::BadRequest(ex.what());
        } catch (const System::Text::Json::JsonException& ex) {
            return Results::BadRequest(std::string("Invalid JSON: ") + ex.what());
        }
    };
}

}  // namespace detail

template <class Handler, class... Bindings>
EndpointHandler Bind(Handler&& handler, Bindings&&... bindings) {
    return detail::MakeBoundEndpointHandler(std::forward<Handler>(handler), std::forward<Bindings>(bindings)...);
}

template <class Handler>
WebApplication& WebApplication::MapGet(std::string pattern, Handler&& handler) {
    return MapMethods({ "GET" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapGet(std::string pattern, Handler&& handler) {
    return MapMethods({ "GET" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapHead(std::string pattern, Handler&& handler) {
    return MapMethods({ "HEAD" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapHead(std::string pattern, Handler&& handler) {
    return MapMethods({ "HEAD" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapOptions(std::string pattern, Handler&& handler) {
    return MapMethods({ "OPTIONS" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapOptions(std::string pattern, Handler&& handler) {
    return MapMethods({ "OPTIONS" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapPost(std::string pattern, Handler&& handler) {
    return MapMethods({ "POST" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapPost(std::string pattern, Handler&& handler) {
    return MapMethods({ "POST" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapPut(std::string pattern, Handler&& handler) {
    return MapMethods({ "PUT" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapPut(std::string pattern, Handler&& handler) {
    return MapMethods({ "PUT" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapPatch(std::string pattern, Handler&& handler) {
    return MapMethods({ "PATCH" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapPatch(std::string pattern, Handler&& handler) {
    return MapMethods({ "PATCH" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapDelete(std::string pattern, Handler&& handler) {
    return MapMethods({ "DELETE" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapDelete(std::string pattern, Handler&& handler) {
    return MapMethods({ "DELETE" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapAny(std::string pattern, Handler&& handler) {
    return MapMethods({}, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& RouteGroupBuilder::MapAny(std::string pattern, Handler&& handler) {
    return MapMethods({}, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapFallback(Handler&& handler) {
    return MapFallback(detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

}  // namespace Capi::Web
