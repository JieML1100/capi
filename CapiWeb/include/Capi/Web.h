#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Json.h>

#include <any>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

class HttpRequest {
public:
    const std::string& Method() const noexcept;
    const std::string& Path() const noexcept;
    const std::string& QueryString() const noexcept;
    const std::string& RawUrl() const noexcept;
    const std::string& Body() const noexcept;
    const std::string& RemoteAddress() const noexcept;

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

    std::unordered_map<std::string, std::any>& Items() noexcept;
    const std::unordered_map<std::string, std::any>& Items() const noexcept;

private:
    HttpRequest request_;
    HttpResponse response_;
    std::unordered_map<std::string, std::any> items_;

    friend class WebApplication;
    friend class HttpSysServer;
};

class HttpResult {
public:
    HttpResult() = default;

    bool HasResponse() const noexcept;
    int StatusCode() const noexcept;
    const HeaderCollection& Headers() const noexcept;
    const std::vector<std::uint8_t>& Body() const noexcept;

    HttpResult& Header(std::string name, std::string value);
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
    static HttpResult NotFound(std::string detail = {});
    static HttpResult Problem(std::string title, std::string detail = {}, int statusCode = 500);
    static HttpResult Redirect(std::string location, bool permanent = false);
};

using EndpointHandler = std::function<HttpResult(HttpContext&)>;
using RequestDelegate = std::function<HttpResult(HttpContext&)>;
using Middleware = std::function<HttpResult(HttpContext&, RequestDelegate)>;

enum class UrlAclMode {
    Disabled,
    Ensure,
    Refresh,
    Delete,
};

struct UrlReservationOptions {
    UrlAclMode Mode = UrlAclMode::Disabled;
    std::wstring SecurityDescriptor = L"D:(A;;GX;;;WD)";
    bool RemoveOnStop = false;
    bool IgnoreAccessDenied = false;
};

struct HttpSysOptions {
    std::vector<std::string> UrlPrefixes = { "http://localhost:8080/" };
    std::uint32_t WorkerCount = 0;
    std::uint32_t RequestBufferSize = 64 * 1024;
    std::uint64_t MaxRequestBodyBytes = 16ull * 1024ull * 1024ull;
    std::chrono::milliseconds StopTimeout = std::chrono::seconds(5);
    bool AddServerHeader = true;
    std::string ServerHeader = "CapiWeb/" + std::string(Version);
    UrlReservationOptions UrlReservation;
};

struct StaticFileOptions {
    std::filesystem::path Root = "wwwroot";
    std::string RequestPath = "/";
    std::vector<std::string> DefaultFiles = { "index.html", "default.html" };
    bool EnableSpaFallback = true;
    std::string SpaFallbackFile = "index.html";
    bool ServeUnknownFileTypes = false;
    std::string UnknownFileContentType = "application/octet-stream";
    std::string CacheControl;
};

struct WebApplicationOptions {
    HttpSysOptions HttpSys;
    Logger Log;
    bool DetailedErrors = false;
};

class UrlReservationManager final {
public:
    UrlReservationManager() = delete;

    static void Apply(const std::vector<std::string>& prefixes, const UrlReservationOptions& options);
    static void Ensure(const std::vector<std::string>& prefixes, const std::wstring& securityDescriptor);
    static void Refresh(const std::vector<std::string>& prefixes, const std::wstring& securityDescriptor);
    static void Remove(const std::vector<std::string>& prefixes, bool ignoreMissing = true);
};

class WebApplication;

class WebApplicationBuilder {
public:
    WebApplicationBuilder();
    explicit WebApplicationBuilder(WebApplicationOptions options);

    WebApplicationBuilder& UseUrls(std::initializer_list<std::string> prefixes);
    WebApplicationBuilder& UseUrls(std::vector<std::string> prefixes);
    WebApplicationBuilder& ConfigureHttpSys(std::function<void(HttpSysOptions&)> configure);
    WebApplicationBuilder& ConfigureLogging(Logger logger);
    WebApplicationBuilder& UseDetailedErrors(bool enabled = true);
    WebApplicationBuilder& UseUrlAcl(UrlAclMode mode, std::wstring securityDescriptor = L"D:(A;;GX;;;WD)");

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
    WebApplication& UseStaticFiles(StaticFileOptions options = {});
    WebApplication& MapMethods(std::initializer_list<std::string> methods, std::string pattern, EndpointHandler handler);
    WebApplication& MapMethods(std::vector<std::string> methods, std::string pattern, EndpointHandler handler);
    WebApplication& MapFallback(EndpointHandler handler);

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
    bool IsRunning() const noexcept;
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

}  // namespace detail

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
