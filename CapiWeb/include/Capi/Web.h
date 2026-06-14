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

    const HeaderCollection& Headers() const noexcept;
    const ValueMap& Query() const noexcept;
    const ValueMap& RouteValues() const noexcept;

    std::string Header(std::string_view name, std::string defaultValue = {}) const;
    std::string QueryValue(std::string_view name, std::string defaultValue = {}) const;
    std::string RouteValue(std::string_view name, std::string defaultValue = {}) const;
    std::string Value(std::string_view name, std::string defaultValue = {}) const;
    bool HasJsonContentType() const;

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
    ValueMap routeValues_;
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

private:
    HttpRequest request_;
    HttpResponse response_;
    std::string traceIdentifier_;
    ClaimsPrincipal user_;
    std::unordered_map<std::string, std::any> items_;

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
    static HttpResult Created(std::string location, const System::Text::Json::JsonNode& value);
    static HttpResult NoContent();
    static HttpResult BadRequest(std::string detail = {});
    static HttpResult Unauthorized(std::string detail = {});
    static HttpResult Forbidden(std::string detail = {});
    static HttpResult NotFound(std::string detail = {});
    static HttpResult TooManyRequests(std::string detail = {});
    static HttpResult Problem(std::string title, std::string detail = {}, int statusCode = 500);
    static HttpResult Redirect(std::string location, bool permanent = false);
};

using EndpointHandler = std::function<HttpResult(HttpContext&)>;
using RequestDelegate = std::function<HttpResult(HttpContext&)>;
using Middleware = std::function<HttpResult(HttpContext&, RequestDelegate)>;
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
    Body,
    JsonProperty,
};

class ParameterBindingException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
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

struct HttpSysOptions {
    std::vector<std::string> UrlPrefixes = { "http://localhost:8080/" };
    std::uint32_t WorkerCount = 0;
    std::uint32_t RequestBufferSize = 64 * 1024;
    std::uint64_t MaxRequestBodyBytes = 16ull * 1024ull * 1024ull;
    std::uint32_t MaxConcurrentRequests = 0;
    std::chrono::milliseconds StopTimeout = std::chrono::seconds(5);
    bool AddServerHeader = true;
    std::string ServerHeader = "CapiWeb/" + std::string(Version);
    UrlReservationOptions UrlReservation;
    std::vector<SslCertificateBinding> SslCertificateBindings;
    HttpSysTimeoutOptions Timeouts;
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

struct RequestLoggingOptions {
    bool IncludeQueryString = false;
    bool IncludeRemoteAddress = true;
    bool IncludeTraceIdentifier = true;
    LogLevel SuccessLevel = LogLevel::Information;
    LogLevel FailureLevel = LogLevel::Warning;
};

struct RequestIdOptions {
    std::string HeaderName = "X-Request-ID";
    std::string ResponseHeaderName = "X-Request-ID";
    bool TrustIncomingHeader = true;
    bool AddResponseHeader = true;
    std::string ItemKey = "RequestId";
};

struct RateLimitOptions {
    std::uint32_t PermitLimit = 100;
    std::chrono::seconds Window = std::chrono::seconds(60);
    RateLimitKeySelector KeySelector;
    int RejectedStatusCode = 429;
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

struct HealthCheckOptions {
    std::string Path = "/health";
    bool IncludeDetails = false;
    bool TreatDegradedAsFailure = false;
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
};

struct WebApplicationOptions {
    HttpSysOptions HttpSys;
    Logger Log;
    bool DetailedErrors = false;
    ProblemDetailsOptions ProblemDetails;
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
    WebApplication& UseResponseCompression(ResponseCompressionOptions options = {});
    WebApplication& UseAuthentication(AuthenticationOptions options);
    WebApplication& UseAuthorization(AuthorizationOptions options = {});
    WebApplication& UseCors(CorsOptions options = {});
    WebApplication& UseSecurityHeaders(SecurityHeadersOptions options = {});
    WebApplication& UseForwardedHeaders(ForwardedHeadersOptions options = {});
    WebApplication& UseRequestId(RequestIdOptions options = {});
    WebApplication& UseRateLimiter(RateLimitOptions options = {});
    WebApplication& UseRequestLogging(RequestLoggingOptions options = {});
    WebApplication& UseMetrics(MetricsOptions options = {});
    WebApplication& MapMetrics(MetricsEndpointOptions options = {});
    WebApplication& AddHealthCheck(std::string name, HealthCheck check);
    WebApplication& MapHealthChecks(HealthCheckOptions options = {});
    WebApplication& UseHealthChecks(HealthCheckOptions options = {});
    WebApplication& MapOpenApi(OpenApiOptions options = {});
    WebApplication& MapSwaggerUi(SwaggerUiOptions options = {});
    WebApplication& OnStarted(ApplicationCallback callback);
    WebApplication& OnStopping(ApplicationCallback callback);
    WebApplication& OnStopped(ApplicationCallback callback);
    WebApplication& AddHostedService(HostedServiceOptions service);
    WebApplication& AddBackgroundService(BackgroundServiceOptions service);
    WebApplication& MapMethods(std::initializer_list<std::string> methods, std::string pattern, EndpointHandler handler);
    WebApplication& MapMethods(std::vector<std::string> methods, std::string pattern, EndpointHandler handler);
    WebApplication& MapFallback(EndpointHandler handler);
    WebApplication& RequireAuthorization(std::string policy = {});
    WebApplication& RequireAuthorization(std::initializer_list<std::string> policies);
    WebApplication& AllowAnonymous();
    WebApplication& ExcludeFromDescription();
    WebApplication& WithName(std::string name);
    WebApplication& WithTags(std::initializer_list<std::string> tags);
    WebApplication& WithTags(std::vector<std::string> tags);
    WebApplication& WithSummary(std::string summary);
    WebApplication& WithDescription(std::string description);
    WebApplication& WithParameter(OpenApiParameter parameter);
    WebApplication& WithQueryParameter(std::string name, std::string schemaType = "string", bool required = false, std::string description = {});
    WebApplication& WithHeaderParameter(std::string name, std::string schemaType = "string", bool required = false, std::string description = {});
    WebApplication& Produces(int statusCode = 200, std::string contentType = "application/json", std::string description = {});
    WebApplication& Accepts(std::string contentType = "application/json", bool required = true, std::string description = {});

    template <class Handler>
    WebApplication& MapGet(std::string pattern, Handler&& handler);

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
T ConvertJsonValue(const System::Text::Json::JsonElement& element, std::string_view label) {
    using Decayed = std::decay_t<T>;
    if constexpr (IsOptionalV<Decayed>) {
        using Inner = OptionalValueType<Decayed>;
        if (element.ValueKind() == System::Text::Json::JsonValueKind::Null ||
            element.ValueKind() == System::Text::Json::JsonValueKind::Undefined) {
            return std::nullopt;
        }
        return ConvertJsonValue<Inner>(element, label);
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
T ResolveBindingValue(const BindingParameter<T>& binding, HttpContext& context) {
    const std::string& name = binding.Name();
    switch (binding.Source()) {
    case BindingSource::Route: {
        auto it = context.Request().RouteValues().find(name);
        if (it == context.Request().RouteValues().end()) {
            return MissingBindingValue(binding, "route");
        }
        return ConvertTextValue<T>(it->second, name);
    }
    case BindingSource::Query: {
        auto it = context.Request().Query().find(name);
        if (it == context.Request().Query().end()) {
            return MissingBindingValue(binding, "query");
        }
        return ConvertTextValue<T>(it->second, name);
    }
    case BindingSource::Header: {
        auto value = context.Request().Headers().TryGet(name);
        if (!value) {
            return MissingBindingValue(binding, "header");
        }
        return ConvertTextValue<T>(*value, name);
    }
    case BindingSource::Body: {
        if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
            return context.Request().Body();
        } else {
            return ConvertJsonValue<T>(context.Request().Json(), "body");
        }
    }
    case BindingSource::JsonProperty: {
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
WebApplication& WebApplication::MapPost(std::string pattern, Handler&& handler) {
    return MapMethods({ "POST" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapPut(std::string pattern, Handler&& handler) {
    return MapMethods({ "PUT" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapPatch(std::string pattern, Handler&& handler) {
    return MapMethods({ "PATCH" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapDelete(std::string pattern, Handler&& handler) {
    return MapMethods({ "DELETE" }, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapAny(std::string pattern, Handler&& handler) {
    return MapMethods({}, std::move(pattern), detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

template <class Handler>
WebApplication& WebApplication::MapFallback(Handler&& handler) {
    return MapFallback(detail::MakeEndpointHandler(std::forward<Handler>(handler)));
}

}  // namespace Capi::Web
