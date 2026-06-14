# CapiWeb

CapiWeb is a small C++20 WebAPI framework hosted directly on Windows HTTP.sys.
It is shaped after ASP.NET Core minimal APIs: build an app, map endpoints,
return `Results`, and optionally serve a frontend from the same listener.

## Minimal API

```cpp
#include <Capi/Web.h>

using namespace Capi::Web;
using namespace System::Text::Json;

int main() {
    auto app = WebApplication::Create();

    app.AddHealthCheck("self", [] {
        return HealthCheckResult::Healthy();
    });
    app.MapHealthChecks({ .Path = "/api/health" });

    app.UseStaticFiles({ .Root = "wwwroot" });
    app.Run();
}
```

Default URL: `http://localhost:8080/`.

## Detailed Configuration

```cpp
auto builder = WebApplication::CreateBuilder();
builder
    .ConfigureFromJsonFile("appsettings.json", true)
    .ConfigureFromEnvironment()
    .UseUrls({ "http://localhost:8080/", "http://127.0.0.1:8081/" })
    .UseDetailedErrors()
    .ConfigureLogging([](const LogMessage& message) {
        std::cout << message.Message << std::endl;
    })
    .ConfigureHttpSys([](HttpSysOptions& http) {
        http.WorkerCount = 8;
        http.RequestBufferSize = 128 * 1024;
        http.MaxRequestBodyBytes = 32ull * 1024ull * 1024ull;
        http.MaxConcurrentRequests = 4096;
        http.StopTimeout = std::chrono::seconds(10);
        http.Timeouts.IdleConnection = std::chrono::seconds(90);
        http.Timeouts.HeaderWait = std::chrono::seconds(30);
        http.UrlReservation.Mode = UrlAclMode::Refresh;
        http.UrlReservation.SecurityDescriptor = L"D:(A;;GX;;;WD)";
    });

auto app = builder.Build();

app.UseRequestLogging();
app.UseSecurityHeaders();
app.UseCors();
```

## JSON Configuration

`ConfigureFromJsonFile` loads deployment settings without forcing them into
source code. The file is optional by default, and code configuration can still
override or refine anything after it loads.

```cpp
auto builder = WebApplication::CreateBuilder();
builder
    .ConfigureFromJsonFile("appsettings.json", true)
    .ConfigureFromEnvironment(); // CAPIWEB_ by default
```

Example `appsettings.json`:

```json
{
  "DetailedErrors": false,
  "Urls": [ "http://localhost:8080/" ],
  "ProblemDetails": {
    "DefaultType": "about:blank",
    "IncludeType": true,
    "IncludeInstance": true,
    "IncludeTraceIdentifier": true,
    "TraceIdentifierExtensionName": "traceId"
  },
  "HttpSys": {
    "WorkerCount": 8,
    "RequestBufferSize": 131072,
    "MaxRequestBodyBytes": 33554432,
    "MaxConcurrentRequests": 4096,
    "StopTimeoutMs": 10000,
    "UrlReservation": {
      "Mode": "Refresh",
      "SecurityDescriptor": "D:(A;;GX;;;WD)",
      "IgnoreAccessDenied": false
    },
    "Timeouts": {
      "EntityBodySeconds": 120,
      "DrainEntityBodySeconds": 120,
      "RequestQueueSeconds": 120,
      "IdleConnectionSeconds": 90,
      "HeaderWaitSeconds": 30,
      "MinSendRateBytesPerSecond": 150
    },
    "SslCertificateBindings": [
      {
        "Ip": "0.0.0.0",
        "Port": 8443,
        "CertificateHashHex": "00112233445566778899aabbccddeeff00112233",
        "CertificateStoreName": "MY",
        "Mode": "Ensure"
      }
    ]
  }
}
```

Configuration property names are case-insensitive. JSON comments and trailing
commas are accepted. Arrays replace the current list values when present.

## Environment Configuration

`ConfigureFromEnvironment` applies deployment overrides from process
environment variables. Values loaded later win, so put it after
`ConfigureFromJsonFile` when environment variables should override
`appsettings.json`.

```powershell
$env:CAPIWEB_URLS = "http://localhost:8080/;http://127.0.0.1:8081/"
$env:CAPIWEB_DETAILED_ERRORS = "false"
$env:CAPIWEB_HTTP_SYS__MAX_CONCURRENT_REQUESTS = "4096"
$env:CAPIWEB_HTTP_SYS__STOP_TIMEOUT_MS = "10000"
$env:CAPIWEB_HTTP_SYS__URL_RESERVATION__MODE = "Refresh"
$env:CAPIWEB_PROBLEM_DETAILS__INCLUDE_TRACE_IDENTIFIER = "true"
```

Common variables:

- `CAPIWEB_URLS`: semicolon or comma separated URL prefixes.
- `CAPIWEB_DETAILED_ERRORS`: `true/false`, `1/0`, `yes/no`, or `on/off`.
- `CAPIWEB_HTTP_SYS__WORKER_COUNT`
- `CAPIWEB_HTTP_SYS__REQUEST_BUFFER_SIZE`
- `CAPIWEB_HTTP_SYS__MAX_REQUEST_BODY_BYTES`
- `CAPIWEB_HTTP_SYS__MAX_CONCURRENT_REQUESTS`
- `CAPIWEB_HTTP_SYS__STOP_TIMEOUT_MS`
- `CAPIWEB_HTTP_SYS__ADD_SERVER_HEADER`
- `CAPIWEB_HTTP_SYS__SERVER_HEADER`
- `CAPIWEB_HTTP_SYS__URL_RESERVATION__MODE`
- `CAPIWEB_HTTP_SYS__URL_RESERVATION__SECURITY_DESCRIPTOR`
- `CAPIWEB_HTTP_SYS__URL_RESERVATION__REMOVE_ON_STOP`
- `CAPIWEB_HTTP_SYS__URL_RESERVATION__IGNORE_ACCESS_DENIED`
- `CAPIWEB_HTTP_SYS__TIMEOUTS__ENTITY_BODY_SECONDS`
- `CAPIWEB_HTTP_SYS__TIMEOUTS__DRAIN_ENTITY_BODY_SECONDS`
- `CAPIWEB_HTTP_SYS__TIMEOUTS__REQUEST_QUEUE_SECONDS`
- `CAPIWEB_HTTP_SYS__TIMEOUTS__IDLE_CONNECTION_SECONDS`
- `CAPIWEB_HTTP_SYS__TIMEOUTS__HEADER_WAIT_SECONDS`
- `CAPIWEB_HTTP_SYS__TIMEOUTS__MIN_SEND_RATE_BYTES_PER_SECOND`
- `CAPIWEB_PROBLEM_DETAILS__DEFAULT_TYPE`
- `CAPIWEB_PROBLEM_DETAILS__INCLUDE_TYPE`
- `CAPIWEB_PROBLEM_DETAILS__INCLUDE_INSTANCE`
- `CAPIWEB_PROBLEM_DETAILS__INCLUDE_TRACE_IDENTIFIER`
- `CAPIWEB_PROBLEM_DETAILS__TRACE_IDENTIFIER_EXTENSION_NAME`

A single HTTP.sys SSL certificate binding can also be supplied with
`CAPIWEB_HTTP_SYS__SSL_CERTIFICATE_BINDING__*` variables such as
`PORT`, `IP`, `CERTIFICATE_HASH_HEX`, `CERTIFICATE_STORE_NAME`, `APP_ID`, and
`MODE`.

## API + Frontend Hosting

```cpp
StaticFileOptions files;
files.Root = "wwwroot";
files.EnableSpaFallback = true;
files.SpaFallbackExcludedPrefixes = { "/api" };
files.CacheControl = "no-cache";
files.EnableConditionalRequests = true;
files.EnableRangeProcessing = true;

app.UseResponseCompression();
app.UseStaticFiles(files);
app.MapGet("/api/echo/{name}", [](HttpContext& ctx) {
    JsonObject result;
    result.Add("hello", JsonNode::Create(ctx.Request().RouteValue("name")));
    return Results::Ok(result);
});
```

Static file hosting rejects path traversal, serves default documents, maps common
content types, and can fall back to `index.html` for SPA routes. The default SPA
fallback excludes `/api`, so browser navigation to a backend path does not get
mistaken for a frontend route.

Static responses include `ETag`, `Last-Modified`, and `Accept-Ranges` by
default. CapiWeb handles `If-None-Match` and `If-Modified-Since` without reading
the file body when the client cache is fresh, returning `304 Not Modified`.
Single byte ranges such as `Range: bytes=0-1023` or suffix ranges such as
`Range: bytes=-1024` return `206 Partial Content`; invalid or unsatisfiable
ranges return `416 Range Not Satisfiable`. Multi-range requests fall back to the
full file response.

## Parameter Binding

For simple endpoints, bind route values, query string values, headers, raw body,
or JSON body properties into strongly typed handler parameters:

```cpp
app.MapGet("/api/products/{id}", Bind([](int id, int page) {
    JsonObject result;
    result.Add("id", JsonNode::Create(id));
    result.Add("page", JsonNode::Create(page));
    return Results::Ok(result);
}, Route<int>("id"), Query<int>("page").Default(1)));

app.MapPost("/api/sum", Bind([](int a, int b) {
    return a + b;
}, JsonProperty<int>("a"), JsonProperty<int>("b")));
```

Available binding descriptors are `Route<T>("name")`, `Query<T>("name")`,
`Header<T>("name")`, `Body<T>()`, and `JsonProperty<T>("name")`. Supported
value types include `std::string`, integral types, floating-point types, `bool`,
`std::optional<T>`, and `System::Text::Json::JsonElement`. Use `.Default(value)`
for optional route/query/header/JSON values with a concrete fallback.

Binding failures return `400 Bad Request` with a problem-details JSON body. This
includes missing required parameters, conversion failures, invalid JSON, and
JSON bodies that are not objects when `JsonProperty<T>` is used.

## OpenAPI and Docs

CapiWeb can publish an OpenAPI 3 document and a small built-in docs page from
the same HTTP.sys listener:

```cpp
app.MapGet("/api/products/{id}", Bind([](int id, int page) {
    return Results::Ok();
}, Route<int>("id"), Query<int>("page").Default(1)))
    .WithName("GetProduct")
    .WithTags({ "Products" })
    .WithSummary("Gets a product.")
    .WithQueryParameter("page", "integer", false, "Page number.")
    .Produces(200);

OpenApiOptions openApi;
openApi.Info.Title = "My API";
openApi.Info.Version = "v1";
app.MapOpenApi(openApi);

SwaggerUiOptions docs;
docs.Path = "/docs";
docs.OpenApiPath = openApi.Path;
app.MapSwaggerUi(docs);
```

Endpoint metadata helpers include `WithName`, `WithTags`, `WithSummary`,
`WithDescription`, `WithParameter`, `WithQueryParameter`, `WithHeaderParameter`,
`Produces`, `Accepts`, and `ExcludeFromDescription`. Route parameters are
discovered from route patterns automatically. Protected endpoints marked with
`RequireAuthorization` get `401`/`403` responses in the OpenAPI document; add
`OpenApiSecurityScheme` entries to `OpenApiOptions::SecuritySchemes` to describe
API keys or bearer tokens.

## Built-In Middleware

```cpp
app.UseForwardedHeaders();
app.UseRequestId();
app.UseProblemDetails();
app.UseAuthentication(authentication);
app.UseAuthorization(authorization);
app.UseMetrics();
app.UseRequestLogging();
app.UseRateLimiter();
app.UseResponseCompression();
app.UseSecurityHeaders();
app.UseCors();
```

- `UseForwardedHeaders`: applies trusted proxy headers such as
  `X-Forwarded-For`, `X-Forwarded-Proto`, and `X-Forwarded-Host`.
- `UseRequestId`: assigns `HttpContext::TraceIdentifier()` from `X-Request-ID`
  or a generated GUID and echoes it on the response.
- `UseAuthentication`: runs configured authentication schemes and stores the
  authenticated `ClaimsPrincipal` on `HttpContext::User()`.
- `UseAuthorization`: enforces endpoint authorization metadata added with
  `RequireAuthorization()` and returns `401` or `403` problem responses.
- `UseMetrics`: records active, completed, failed, duration, method, and status
  counters for operational diagnostics.
- `UseRequestLogging`: logs method, path, status code, elapsed time, and remote
  address through the configured `Logger`, including trace id by default.
- `UseRateLimiter`: applies fixed-window in-process rate limiting and returns
  `429 Too Many Requests` with `Retry-After` when the key is over quota.
- `UseResponseCompression`: applies gzip compression for eligible downstream
  API and static responses when the client sends `Accept-Encoding: gzip`.
- `UseSecurityHeaders`: adds conservative response headers such as
  `X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy`, and
  `Cross-Origin-Opener-Policy`.
- `UseCors`: handles simple CORS and preflight `OPTIONS` requests. Defaults to
  wildcard origins/methods/headers for development, with `CorsOptions` available
  for production tightening.

## Response Compression

Response compression is a middleware, so place it before endpoints or static
file middleware that should be compressed:

```cpp
ResponseCompressionOptions compression;
compression.MinimumBodySize = 1024;
compression.GzipLevel = 5;
compression.EnableForHttps = false;

app.UseResponseCompression(compression);
app.UseStaticFiles();
```

By default, CapiWeb compresses text, JSON, XML, JavaScript, and SVG responses
only when the request allows `gzip`. It skips `HEAD`, `204`, `206`, `304`, range
responses, responses with an existing `Content-Encoding`, `Cache-Control:
no-transform`, and already-compressed or binary media types such as images,
video, zip, PDF, WASM, and fonts.

The middleware sets `Content-Encoding: gzip`, appends `Vary: Accept-Encoding`,
updates `Content-Length`, and leaves the response uncompressed if gzip would not
make it smaller. HTTPS compression is opt-in through `EnableForHttps` because
compressed encrypted responses can be inappropriate for pages or APIs that mix
secrets with attacker-controlled input.

## Authentication and Authorization

Authentication is scheme-based and pluggable. A handler returns
`AuthenticationResult::Success`, `NoResult`, or `Fail`; authorization then
checks endpoint metadata and policies:

```cpp
AuthenticationOptions authentication;
authentication.Schemes.push_back(AuthenticationScheme{
    "ApiKey",
    [](HttpContext& ctx) {
        std::string key = ctx.Request().Header("X-API-Key");
        if (key.empty()) {
            return AuthenticationResult::NoResult();
        }
        if (key != "demo-secret") {
            return AuthenticationResult::Fail("Invalid API key.", "ApiKey");
        }
        return AuthenticationResult::Success(
            ClaimsPrincipal::Authenticated("sample-client", "ApiKey")
                .AddRole("admin"));
    },
    "ApiKey"
});
app.UseAuthentication(authentication);

AuthorizationPolicy admin;
admin.Name = "admin";
admin.RequiredRoles = { "admin" };

AuthorizationOptions authorization;
authorization.Policies.push_back(admin);
app.UseAuthorization(authorization);

app.MapGet("/api/secure", [](HttpContext& ctx) {
    return Results::Ok(ctx.User().Name());
}).RequireAuthorization("admin");
```

`RequireAuthorization()` without a policy uses `DefaultPolicy`, which requires an
authenticated user by default. `RequireAuthorization("name")` applies a named
policy, and `AllowAnonymous()` lets a route bypass global endpoint authorization.
Policies can require roles, claims, or a custom assertion callback. Missing or
invalid credentials return `401 Unauthorized`; authenticated users that fail a
policy return `403 Forbidden`.

## Reverse Proxies and Load Balancers

When CapiWeb runs behind IIS ARR, Nginx, a cloud load balancer, or an API
gateway, use forwarded headers before logging, metrics, rate limiting, or
authentication middleware:

```cpp
ForwardedHeadersOptions forwarded;
forwarded.KnownProxies = { "127.0.0.1", "::1", "10.0.0.5" };
forwarded.ForwardLimit = 1;

app.UseForwardedHeaders(forwarded);
app.UseRequestId();
app.UseAuthentication(authentication);
app.UseAuthorization(authorization);
app.UseMetrics();
app.UseRequestLogging();
app.UseRateLimiter();
```

By default, CapiWeb only trusts forwarded headers from loopback proxies
(`127.0.0.1` and `::1`). Set `KnownProxies` to the addresses of your real
gateway nodes. Keep `RequireKnownProxy = true` for internet-facing listeners so
clients cannot spoof `X-Forwarded-For` and poison logs, metrics, or rate-limit
keys.

`HttpRequest::RemoteAddress()`, `Scheme()`, and `Host()` reflect the forwarded
values after the middleware applies them. `ForwardLimit = 1` accepts only the
nearest trusted forwarded value; set it higher only when you control the full
proxy chain.

## Observability

Request IDs and process-local metrics are built in:

```cpp
RequestIdOptions requestIds;
requestIds.HeaderName = "X-Request-ID";
requestIds.ResponseHeaderName = "X-Request-ID";
requestIds.TrustIncomingHeader = true;
app.UseRequestId(requestIds);

MetricsOptions metrics;
metrics.IncludePathCounters = false; // avoid high-cardinality counters by default
app.UseMetrics(metrics);

app.MapMetrics({ .Path = "/api/metrics" });

app.MapGet("/api/echo/{name}", [](HttpContext& ctx) {
    JsonObject result;
    result.Add("hello", JsonNode::Create(ctx.Request().RouteValue("name")));
    result.Add("traceId", JsonNode::Create(ctx.TraceIdentifier()));
    return Results::Ok(result);
});
```

The metrics endpoint returns JSON with started/completed/failed/active request
counts, uptime, total/max/average duration, and optional method/status/path
breakdowns. Path counters are opt-in because route or URL cardinality can grow
without bound in public services.

## Rate Limiting

`UseRateLimiter` provides a lightweight fixed-window limiter for single-process
deployments:

```cpp
RateLimitOptions limit;
limit.PermitLimit = 120;
limit.Window = std::chrono::seconds(60);

app.UseRateLimiter(limit);
```

The default key is the remote address. Set `KeySelector` to rate limit by API
key, tenant, authenticated user, or any application-specific value:

```cpp
RateLimitOptions limit;
limit.PermitLimit = 1000;
limit.Window = std::chrono::minutes(1);
limit.KeySelector = [](HttpContext& ctx) {
    return ctx.Request().Header("X-Api-Key", ctx.Request().RemoteAddress());
};

app.UseRateLimiter(limit);
```

Allowed responses include `X-RateLimit-Limit`, `X-RateLimit-Remaining`, and
`X-RateLimit-Reset`. Rejected responses also include `Retry-After` and a
problem-details JSON body. Set `PermitLimit = 0` or `Window = 0s` to disable the
limiter while keeping the call in place.

## Health Checks

Health checks are first-class endpoints, so load balancers and service monitors
do not need app-specific probe code:

```cpp
app.AddHealthCheck("self", [] {
    return HealthCheckResult::Healthy("Process is running.");
});

app.AddHealthCheck("static-files", [] {
    return std::filesystem::exists("wwwroot/index.html")
        ? HealthCheckResult::Healthy("Frontend assets are available.")
        : HealthCheckResult::Unhealthy("wwwroot/index.html is missing.");
});

HealthCheckOptions health;
health.Path = "/api/health";
health.IncludeDetails = true;
app.MapHealthChecks(health);
```

The aggregate response is `200` for `Healthy`, `503` for `Unhealthy`, and `200`
for `Degraded` unless `TreatDegradedAsFailure` is enabled. Thrown exceptions are
captured as unhealthy check results instead of escaping as unhandled endpoint
errors.

## Graceful Shutdown and Backpressure

HTTP.sys request processing has bounded shutdown and overload behavior:

```cpp
builder.ConfigureHttpSys([](HttpSysOptions& http) {
    http.MaxConcurrentRequests = 4096;       // 0 means unlimited
    http.StopTimeout = std::chrono::seconds(10);
});

app.OnStarted([] {
    std::cout << "started" << std::endl;
});

app.OnStopping([] {
    std::cout << "draining" << std::endl;
});

app.OnStopped([] {
    std::cout << "stopped" << std::endl;
});
```

When stopping, CapiWeb marks the HTTP.sys URL group inactive, stops accepting new
requests, waits up to `StopTimeout` for active handlers to drain, then shuts down
the request queue so worker threads unblock cleanly. Requests that arrive after
draining starts receive `503 Service Unavailable`.

When `MaxConcurrentRequests` is non-zero and all request slots are busy, CapiWeb
returns `503 Service Unavailable` with `Retry-After` instead of running user
middleware. `ActiveRequestCount()` and `IsStopping()` are available for health
checks and diagnostics.

## Error Handling

`UseDetailedErrors(false)` is the default. Unhandled exceptions are converted to
problem-details JSON without leaking exception text. Enable detailed errors only
for development:

```cpp
builder.UseDetailedErrors();
```

`Results::Problem` writes `application/problem+json` with the standard
`type`, `title`, `status`, `detail`, and `instance` shape. Add
`UseProblemDetails` near the start of the middleware pipeline to enrich empty
`4xx`/`5xx` responses and existing problem responses with request context:

```cpp
ProblemDetailsOptions problems;
problems.Customize = [](HttpContext& ctx, ProblemDetails& problem) {
    problem.Extensions["service"] = System::Text::Json::JsonNode::Create("Orders");
};

app.UseProblemDetails(problems);
```

Internal framework errors such as request body limit failures, invalid JSON, and
unhandled exceptions use the same options. `IncludeTraceIdentifier` adds the
current `HttpContext::TraceIdentifier()` as an extension, and `IncludeInstance`
defaults the problem `instance` to the request URL.

Request bodies larger than `HttpSysOptions::MaxRequestBodyBytes` return `413`;
invalid JSON parsing returns `400`; unhandled application exceptions return
`500`.

## Routing Behavior

Routes support literals, parameters such as `/api/users/{id}`, and catch-all
parameters such as `/files/{*path}`. `HEAD` can match `GET` endpoints. If a path
matches an endpoint but the method does not, CapiWeb returns `405 Method Not
Allowed` with an `Allow` header.

## Machine-Level HTTP.sys Configuration

HTTP.sys URL reservations are machine-level state. They are not automatically
removed when the process exits, so CapiWeb makes that lifecycle explicit:

- `UrlAclMode::Disabled`: do not touch machine configuration. The process must
  already have permission to bind the URL, or it must run elevated.
- `UrlAclMode::Ensure`: add the URL ACL if it is missing. Existing entries are
  kept as-is.
- `UrlAclMode::Refresh`: delete then add configured URL ACLs on startup. This is
  the safest choice when the app's prefixes or security descriptor may change
  between runs.
- `UrlAclMode::Delete`: remove configured URL ACLs and do not start listening.
- `RemoveOnStop`: optional cleanup on graceful stop. Default is `false` because
  URL ACLs are usually deployment configuration, not process-local state.
- `IgnoreAccessDenied`: useful for apps that try to ensure the reservation when
  elevated, but can also run normally after deployment has already created it.

CapiWeb uses `HttpSetServiceConfiguration` and
`HttpDeleteServiceConfiguration` directly; it does not shell out to `netsh`.

## HTTPS Certificate Bindings

HTTP.sys TLS certificate bindings are also machine-level state. CapiWeb can
manage ordinary IP:port SSL bindings directly:

```cpp
SslCertificateBinding https;
https.Ip = "0.0.0.0";
https.Port = 8443;
https.CertificateHashHex = "00112233445566778899aabbccddeeff00112233";
https.CertificateStoreName = L"MY";
https.Mode = MachineConfigMode::Refresh;

builder.UseHttps(https);
```

`UseHttps` adds the binding and ensures an `https://+:port/` URL prefix exists.
Use `Mode = Ensure` to add a missing binding, `Refresh` to delete and recreate
it when deployment configuration changes, `Delete` to remove it, or `Disabled`
to leave the machine untouched. `RemoveOnStop` is available but defaults to
`false`, because certificate bindings are normally deployment state.

## HTTP.sys Timeouts

`HttpSysOptions::Timeouts` applies URL-group timeout limits with
`HttpSetUrlGroupProperty(HttpServerTimeoutsProperty)`. The values map to
HTTP.sys `EntityBody`, `DrainEntityBody`, `RequestQueue`, `IdleConnection`,
`HeaderWait`, and `MinSendRate`, giving the app bounded behavior for slow or
stalled clients without hand-rolled socket code.

## Hosted Services

Hosted services run with the application lifecycle and are useful for queue
consumers, cache refreshers, maintenance loops, and heartbeat workers. Register
them before `Start()` or `Run()`:

```cpp
HostedServiceOptions warmup;
warmup.Name = "warmup";
warmup.Start = [](WebApplication& app) {
    // Open connections, warm caches, or start external subscriptions.
};
warmup.Stop = [](WebApplication& app) {
    // Flush state and release resources.
};
app.AddHostedService(std::move(warmup));

BackgroundServiceOptions worker;
worker.Name = "heartbeat";
worker.Execute = [](WebApplication& app, std::stop_token stop) {
    while (!stop.stop_requested()) {
        // Do cooperative background work.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
};
app.AddBackgroundService(std::move(worker));
```

Services start in registration order and stop in reverse order. If a service
fails during startup, CapiWeb logs the failure, stops already-started services,
stops HTTP.sys, and rethrows the exception. Stop callbacks are best-effort: each
failure is logged and the remaining services still get a stop signal.

`AddBackgroundService` owns a `std::jthread` and passes a cooperative
`std::stop_token` to the worker. Unhandled worker exceptions are logged and stop
the application by default; set `StopApplicationOnException = false` for
supervised workers that should fail independently.

## Windows Service Hosting

The same executable can run as a console process during development and as a
Windows Service under the Service Control Manager in production:

```cpp
WindowsServiceOptions service;
service.ServiceName = L"CapiSample";
service.DisplayName = L"Capi Sample Web Service";
service.Description = L"Sample CapiWeb HTTP.sys web service.";
service.StartMode = WindowsServiceStartMode::Automatic;

return WindowsServiceHost::Run(app, service);
```

`Run` first tries `StartServiceCtrlDispatcherW`. When the executable is launched
from a console, it falls back to `app.Run()` by default; when SCM launches it,
stop and shutdown controls call `app.Stop()` and report service state changes
back to SCM.

Deployment helpers call SCM APIs directly and are safe to repeat:

```cpp
WindowsServiceHost::Install(service);   // create or refresh service config
WindowsServiceHost::Start(service.ServiceName);
WindowsServiceHost::Stop(service.ServiceName);
WindowsServiceHost::Uninstall(service.ServiceName);
```

Installing, updating, starting, stopping, and deleting services normally require
an elevated process. Service entries are machine-level configuration and are not
removed automatically when the app exits; call `Uninstall` when the deployment
should remove them.

## Sample

Build `Capi.Sample` and open `http://localhost:8080/`. The sample hosts:

- `GET /api/health`
- `GET /api/metrics`
- `GET /openapi.json`
- `GET /docs`
- `GET /api/echo/{name}`
- `GET /api/bind/{id}?multiplier=6`
- `GET /api/secure` with `X-API-Key: demo-secret`
- `POST /api/sum`
- `wwwroot/index.html` from the same HTTP.sys listener
- gzip response compression for eligible API and static responses
- RFC-style problem-details JSON for errors
- a hosted heartbeat background service reported by `/api/health`

The sample also supports service lifecycle commands:

```powershell
Capi.Sample.exe --install-service
Capi.Sample.exe --start-service
Capi.Sample.exe --stop-service
Capi.Sample.exe --uninstall-service
```
