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
    .ConfigureFormOptions([](FormOptions& forms) {
        forms.ValueCountLimit = 1024;
        forms.MultipartFileCountLimit = 128;
        forms.MultipartBodyLengthLimit = 128ull * 1024ull * 1024ull;
    })
    .ConfigureHttpSys([](HttpSysOptions& http) {
        http.WorkerCount = 8;
        http.RequestBufferSize = 128 * 1024;
        http.MaxRequestBodyBytes = 32ull * 1024ull * 1024ull;
        http.MaxRequestUrlBytes = 16ull * 1024ull;
        http.MaxRequestHeaderBytes = 64ull * 1024ull;
        http.MaxConcurrentRequests = 4096;
        http.StopTimeout = std::chrono::seconds(10);
        http.Backpressure.OverloadedStatusCode = 429;
        http.Backpressure.OverloadedRetryAfterSeconds = 3;
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
  "FormOptions": {
    "ValueCountLimit": 1024,
    "KeyLengthLimit": 2048,
    "ValueLengthLimit": 4194304,
    "MultipartBoundaryLengthLimit": 128,
    "MultipartFileCountLimit": 128,
    "MultipartHeadersCountLimit": 16,
    "MultipartHeadersLengthLimit": 16384,
    "MultipartBodyLengthLimit": 134217728
  },
  "HttpSys": {
    "WorkerCount": 8,
    "RequestBufferSize": 131072,
    "MaxRequestBodyBytes": 33554432,
    "MaxRequestUrlBytes": 16384,
    "MaxRequestHeaderBytes": 65536,
    "MaxConcurrentRequests": 4096,
    "StopTimeoutMs": 10000,
    "Backpressure": {
      "StoppingStatusCode": 503,
      "StoppingDetail": "Server is stopping.",
      "StoppingRetryAfterSeconds": 5,
      "OverloadedStatusCode": 429,
      "OverloadedDetail": "Too many concurrent requests.",
      "OverloadedRetryAfterSeconds": 3
    },
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
Known JSON settings are strict: invalid boolean, integer, duration, and list
values fail during configuration instead of being silently ignored.
`builder.Build()` validates HTTP.sys options up front, including URL prefix
shape, duplicate prefixes, server header safety, non-negative timeouts, URL ACL
security descriptors, and SSL binding IP/port/certificate fields. Invalid
configuration fails before the listener or any machine-level HTTP.sys state is
touched.

## Environment Configuration

`ConfigureFromEnvironment` applies deployment overrides from process
environment variables. Values loaded later win, so put it after
`ConfigureFromJsonFile` when environment variables should override
`appsettings.json`.

```powershell
$env:CAPIWEB_URLS = "http://localhost:8080/;http://127.0.0.1:8081/"
$env:CAPIWEB_DETAILED_ERRORS = "false"
$env:CAPIWEB_HTTP_SYS__MAX_REQUEST_URL_BYTES = "16384"
$env:CAPIWEB_HTTP_SYS__MAX_REQUEST_HEADER_BYTES = "65536"
$env:CAPIWEB_HTTP_SYS__MAX_CONCURRENT_REQUESTS = "4096"
$env:CAPIWEB_HTTP_SYS__STOP_TIMEOUT_MS = "10000"
$env:CAPIWEB_FORM_OPTIONS__VALUE_COUNT_LIMIT = "1024"
$env:CAPIWEB_FORM_OPTIONS__MULTIPART_FILE_COUNT_LIMIT = "128"
$env:CAPIWEB_HTTP_SYS__URL_RESERVATION__MODE = "Refresh"
$env:CAPIWEB_PROBLEM_DETAILS__INCLUDE_TRACE_IDENTIFIER = "true"
```

Common variables:

- `CAPIWEB_URLS`: semicolon or comma separated URL prefixes.
- `CAPIWEB_DETAILED_ERRORS`: `true/false`, `1/0`, `yes/no`, or `on/off`.
- `CAPIWEB_HTTP_SYS__WORKER_COUNT`
- `CAPIWEB_HTTP_SYS__REQUEST_BUFFER_SIZE`
- `CAPIWEB_HTTP_SYS__MAX_REQUEST_BODY_BYTES`
- `CAPIWEB_HTTP_SYS__MAX_REQUEST_URL_BYTES`
- `CAPIWEB_HTTP_SYS__MAX_REQUEST_HEADER_BYTES`
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
- `CAPIWEB_FORM_OPTIONS__VALUE_COUNT_LIMIT`
- `CAPIWEB_FORM_OPTIONS__KEY_LENGTH_LIMIT`
- `CAPIWEB_FORM_OPTIONS__VALUE_LENGTH_LIMIT`
- `CAPIWEB_FORM_OPTIONS__MULTIPART_BOUNDARY_LENGTH_LIMIT`
- `CAPIWEB_FORM_OPTIONS__MULTIPART_FILE_COUNT_LIMIT`
- `CAPIWEB_FORM_OPTIONS__MULTIPART_HEADERS_COUNT_LIMIT`
- `CAPIWEB_FORM_OPTIONS__MULTIPART_HEADERS_LENGTH_LIMIT`
- `CAPIWEB_FORM_OPTIONS__MULTIPART_BODY_LENGTH_LIMIT`

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
files.ServeHiddenFiles = false;
files.MaximumFileSizeBytes = 64ull * 1024ull * 1024ull;

app.UseResponseCompression();
app.UseStaticFiles(files);
app.MapGet("/api/echo/{name}", [](HttpContext& ctx) {
    JsonObject result;
    result.Add("hello", JsonNode::Create(ctx.Request().RouteValue("name")));
    return Results::Ok(result);
});
```

Static file hosting rejects path traversal, serves default documents, maps common
content types, and can fall back to `index.html` for SPA routes. By default it
also rejects hidden files, dot-prefixed paths, common source-control directories,
and deployment-sensitive names such as `appsettings*.json`, `.env*`, and
`web.config`. Clear or replace `BlockedFileNames`/`BlockedPathSegments`, or set
`ServeHiddenFiles = true`, only for files that are intentionally public. The
default SPA fallback excludes `/api`, so browser navigation to a backend path
does not get mistaken for a frontend route. Prefix matching is path-segment
aware: `/api` excludes `/api` and `/api/health`, but not `/apix`. Static file
options are validated when `UseStaticFiles` is called, including cache headers,
unknown content type, request mount path, fallback/default file paths, SPA
fallback exclusions, and blocked name/path patterns.

Static responses include `ETag`, `Last-Modified`, and `Accept-Ranges` by
default. CapiWeb handles `If-None-Match` and `If-Modified-Since` without reading
the file body when the client cache is fresh, returning `304 Not Modified`.
`HEAD` requests return the same metadata and `Content-Length` as `GET` while
omitting the body, matching the HTTP.sys send path and the in-memory test path.
Single byte ranges such as `Range: bytes=0-1023` or suffix ranges such as
`Range: bytes=-1024` return `206 Partial Content`; invalid or unsatisfiable
ranges return `416 Range Not Satisfiable`. Multi-range requests fall back to the
full file response. Because this lightweight static-file middleware buffers the
response body before sending it, `MaximumFileSizeBytes` bounds full-body and
range buffering by default; set it to `0` only for trusted deployments that
intentionally serve larger files. `HEAD` and fresh conditional requests do not
read the file body.

## Route Groups

Use route groups to share a URL prefix and endpoint metadata across related
APIs, similar to ASP.NET Core minimal API groups:

```cpp
auto api = app.MapGroup("/api")
    .WithTags({ "Products" })
    .RequireAuthorization("admin");

api.MapGet("/products/{id}", [](HttpContext& ctx) {
    return Results::Ok(ctx.Request().RouteValue("id"));
});

api.MapGet("/status", [] {
    return Results::Ok("public");
}).AllowAnonymous();
```

Group metadata is applied when each endpoint is mapped. Endpoint-level calls can
still refine or override the result, so `AllowAnonymous()` can open a single
route inside an authorized group, and endpoint `WithTags(...)` can replace group
tags for that route. Nested groups compose prefixes and inherit metadata.

## Endpoint Filters

Endpoint filters wrap a single endpoint, or every endpoint mapped through a
route group. They run after routing, authorization, and content negotiation, but
before the endpoint handler:

```cpp
auto api = app.MapGroup("/api")
    .AddEndpointFilter([](HttpContext& ctx, RequestDelegate next) {
        HttpResult result = next(ctx);
        result.Header("X-API-Filter", "products");
        return result;
    });

api.MapPost("/products", [](HttpContext&) {
    return Results::Created("/api/products/42");
}).AddEndpointFilter([](HttpContext& ctx, RequestDelegate next) {
    if (ctx.Request().Header("Idempotency-Key").empty()) {
        return Results::BadRequest("Idempotency-Key is required.");
    }
    return next(ctx);
});
```

Filters can inspect route values, headers, authenticated users, and items;
short-circuit by returning a `HttpResult` without calling `next`; or call `next`
and then adjust the response. Parent group filters wrap child group filters and
endpoint filters in registration order.

## Parameter Binding

For simple endpoints, bind route values, query string values, headers, cookies,
form fields, raw body, or JSON body properties into strongly typed handler
parameters:

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

app.MapPost("/api/profile", Bind([](JsonElement body) {
    return Results::Ok(body.GetProperty("name").GetString());
}, Body<JsonElement>()));

app.MapPost("/api/login", Bind([](std::string user, bool remember) {
    return Results::Ok();
}, Form<std::string>("user"), Form<bool>("remember")));
```

Available binding descriptors are `Route<T>("name")`, `Query<T>("name")`,
`Header<T>("name")`, `Cookie<T>("name")`, `Form<T>("name")`, `Body<T>()`, and
`JsonProperty<T>("name")`. Supported value types include `std::string`,
integral types, floating-point types, `bool`, `std::optional<T>`,
`std::vector<T>` for repeated query/header/form values or JSON arrays, and
`System::Text::Json::JsonElement`. Use `.Default(value)` for optional
route/query/header/cookie/form/JSON values with a concrete fallback. Missing
`std::vector<T>` query/header/form bindings produce an empty vector.

Binding failures return problem-details JSON bodies. Missing required
parameters, conversion failures, invalid JSON, and JSON bodies that are not
objects when `JsonProperty<T>` is used return `400 Bad Request`. JSON
`Body<T>`/`JsonProperty<T>` binding requires `application/json` or
`application/*+json`, and `Form<T>` requires
`application/x-www-form-urlencoded` or `multipart/form-data`; wrong or missing
media types return `415 Unsupported Media Type`. URL-encoded forms use normal
`+` and percent decoding.

Direct request accessors keep repeated values available:
`QueryValue("tag")` and `FormValue("tag")` return the last value for compatibility,
while `QueryValues("tag")` and `FormValues("tag")` return every value in request
order.

Multipart file uploads are available from the request object:

```cpp
app.MapPost("/api/upload", [](HttpContext& ctx) {
    const HttpRequest& request = ctx.Request();
    const MultipartFile* file = request.File("upload");
    if (file == nullptr) {
        return Results::BadRequest("upload is required.");
    }

    JsonObject result;
    result.Add("title", JsonNode::Create(request.FormValue("title")));
    result.Add("fileName", JsonNode::Create(file->FileName));
    result.Add("contentType", JsonNode::Create(file->ContentType));
    result.Add("bytes", JsonNode::Create(static_cast<int>(file->Body.size())));
    return Results::Ok(result);
}).Accepts("multipart/form-data");
```

`HttpRequest::Form()` contains multipart text fields, and
`HttpRequest::Files()` exposes uploaded file parts with name, file name, content
type, part headers, and body bytes stored as `std::string`.
`FormOptions` bounds URL-encoded and multipart parsing before handlers see the
data: value count, key/value length, boundary length, multipart file count,
per-part header count/bytes, and per-file body bytes. Defaults are intentionally
finite for public endpoints, and `0` disables an individual limit.

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
    .WithQueryArrayParameter("tag", "string", false, "Filter tags.")
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
`WithQueryArrayParameter`, `WithHeaderArrayParameter`, `Produces`, `Accepts`,
`AddEndpointFilter`, and `ExcludeFromDescription`. Route parameters are
discovered from route patterns automatically. Protected endpoints marked with
`RequireAuthorization` get `401`/`403` responses in the OpenAPI document; add
`OpenApiSecurityScheme` entries to `OpenApiOptions::SecuritySchemes` to describe
API keys or bearer tokens. For custom parameters, `OpenApiParameter::SchemaFormat`
can add OpenAPI formats such as `uuid`, `int64`, or `date-time`. Use
`WithQueryArrayParameter` or `WithHeaderArrayParameter` for repeated parameters,
or set `OpenApiParameter::IsArray = true` directly when you need full control;
`SchemaType` and `SchemaFormat` describe the item type unless `ItemsSchemaType`
or `ItemsSchemaFormat` are set explicitly.
Explicit `MapHead` and `MapOptions` endpoints are emitted as `head` and
`options` operations, and `MapAny` expands to the common HTTP verbs including
`HEAD`.

Built-in endpoint paths and OpenAPI metadata are validated when they are
registered. Metrics, health, OpenAPI, and Swagger UI paths must be
application-relative route paths; OpenAPI titles, versions, security schemes,
header parameters, and declared media types are rejected early when they would
produce an invalid or misleading API document.

`Accepts(contentType, required)` also validates requests at runtime. Required
request bodies without a matching `Content-Type` return `415 Unsupported Media
Type`; when `required` is `false`, a missing `Content-Type` is allowed but an
unsupported one is still rejected. Parameters such as `charset=utf-8` are
ignored for matching, and media-type wildcards such as `application/*` are
supported.

`Produces(statusCode, contentType)` participates in simple `Accept` negotiation
for declared success responses. Requests with no `Accept` header, `*/*`, or a
matching wildcard are allowed; requests that explicitly exclude all declared
success content types return `406 Not Acceptable`. No-body responses such as
`204` and `304` are ignored for response body negotiation.

OpenAPI generation follows the same metadata: endpoints with `Accepts` include a
`415` problem response, and endpoints with negotiated success content include a
`406` problem response unless those statuses were explicitly documented.

## Built-In Middleware

```cpp
app.UseForwardedHeaders();
app.UseRequestId();
app.UseProblemDetails();
app.UseAuthentication(authentication);
app.UseAuthorization(authorization);
app.UseMetrics();
app.UseRequestLogging();
app.UseServerTiming();
app.UseRateLimiter();
app.UseRequestDecompression();
app.UseResponseCompression();
app.UseResponseCaching();
app.UseSecurityHeaders();
app.UseHttpsRedirection();
app.UseHsts();
app.UseCors();
```

- `UseForwardedHeaders`: applies trusted proxy headers such as
  `X-Forwarded-For`, `X-Forwarded-Proto`, and `X-Forwarded-Host`.
- `UseRequestId`: assigns `HttpContext::TraceIdentifier()` from `X-Request-ID`
  or a generated GUID and echoes it on the response. Unsafe or overlong incoming
  IDs are ignored.
- `UseAuthentication`: runs configured authentication schemes and stores the
  authenticated `ClaimsPrincipal` on `HttpContext::User()`.
- `UseAuthorization`: enforces endpoint authorization metadata added with
  `RequireAuthorization()` and returns `401` or `403` problem responses.
- `UseMetrics`: records active, completed, failed, duration, method, and status
  counters for operational diagnostics.
- `UseRequestLogging`: logs method, path, status code, elapsed time, and remote
  address through the configured `Logger`, including trace id by default. When
  query-string logging is enabled, sensitive parameters such as `token`,
  `api_key`, `password`, and OAuth `code` are redacted by default.
- `UseServerTiming`: adds elapsed request time to `Server-Timing`, optionally
  emits `X-Response-Time`, and stores the elapsed milliseconds in request items.
- `UseRateLimiter`: applies fixed-window in-process rate limiting and returns
  `429 Too Many Requests` with `Retry-After` when the key is over quota.
- `UseRequestDecompression`: decodes supported compressed request bodies before
  parameter binding or endpoint handlers read them.
- `UseResponseCompression`: applies gzip compression for eligible downstream
  API and static responses when the client sends `Accept-Encoding: gzip`.
- `UseResponseCaching`: adds conservative ETag and conditional request handling
  for eligible `GET`/`HEAD` API responses.
- `UseSecurityHeaders`: adds conservative response headers such as
  `X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy`, and
  `Cross-Origin-Opener-Policy`. Configured header values are validated when the
  middleware is registered, so CR/LF mistakes fail before serving traffic.
- `UseHttpsRedirection`: redirects HTTP requests to HTTPS using `301`, `302`,
  `307`, or `308`, preserving path and query string.
- `UseHsts`: adds `Strict-Transport-Security` on HTTPS responses, skipping
  localhost-style hosts by default.
- `UseCors`: handles simple CORS and preflight `OPTIONS` requests. Defaults to
  wildcard origins/methods/headers for development, with `CorsOptions` available
  for production tightening.

Custom middleware registered with `Use(...)` is validated at registration time;
empty middleware callbacks are rejected before the app can start serving
requests. Builder configuration callbacks such as `ConfigureHttpSys(...)` and
`ConfigureProblemDetails(...)` follow the same early-failure rule.

## Request Logging

`UseRequestLogging` writes one log entry per request through the configured
`Logger`. Query-string logging is opt-in, and common secret-bearing parameter
names are redacted before the message is written:

```cpp
RequestLoggingOptions logging;
logging.IncludeQueryString = true;
logging.IncludeEndpoint = true;
logging.RedactedQueryParameters.push_back("session_id");

app.UseRequestId();
app.UseRequestLogging(logging);
```

Set `RedactedQueryParameters` to the decoded parameter names your application
uses for credentials or one-time codes. Dynamic fields are sanitized before they
enter the log message, and logging options are validated when
`UseRequestLogging` is called. When `IncludeEndpoint` is enabled, log messages
append the matched endpoint name if one was configured with `WithName`, falling
back to the route pattern.

## CORS

`UseCors` supports simple requests and browser preflight requests. The defaults
are permissive for local development; tighten them for production APIs:

```cpp
CorsOptions cors;
cors.AllowedOrigins = { "https://app.example.com" };
cors.AllowedMethods = { "GET", "POST" };
cors.AllowedHeaders = { "X-Api-Key", "Content-Type" };
cors.AllowCredentials = true;
cors.RejectInvalidPreflightRequests = true;

app.UseCors(cors);
```

When `AllowCredentials` is enabled, wildcard origins are echoed back as the
request origin instead of `*`, and `Vary: Origin` is appended. With
`RejectInvalidPreflightRequests`, disallowed origins, methods, or requested
headers return problem-details JSON instead of a permissive preflight response.
CORS options are validated when `UseCors` is called: header lists must contain
valid header names, methods must be token-like values or `*`, configured header
values cannot contain control characters, and `MaxAgeSeconds` cannot be
negative.

## Results Helpers

Handlers can return `HttpResult` directly or any value supported by automatic
conversion. Common helpers cover API responses, redirects, problem details, and
small files:

```cpp
app.MapGet("/api/items/{id:int}", [](HttpContext& ctx) {
    return Results::Ok(ctx.Request().RouteValue("id"));
}).WithName("GetItem");

app.MapPost("/api/items", [&app](HttpContext&) {
    std::string location = app.PathByName("GetItem", { { "id", "42" } });
    return Results::Created(location);
});

app.MapPost("/api/items/absolute", [&app](HttpContext& ctx) {
    std::string location = app.UrlByName(ctx, "GetItem", { { "id", "42" } });
    return Results::Created(location);
});

app.MapPost("/api/jobs", [](HttpContext&) {
    JsonObject payload;
    payload.Add("status", JsonNode::Create("queued"));
    return Results::Accepted("/api/jobs/42", payload);
});

app.MapGet("/reports/latest", [] {
    return Results::File("reports/latest.txt", {}, "latest.txt");
});
```

Useful helpers include `Ok`, `Created`, `Accepted`, `NoContent`, `BadRequest`,
`Unauthorized`, `Forbidden`, `NotFound`, `Conflict`, `TooManyRequests`,
`Problem`, `Redirect`, `Text`, `Content`, `Json`, `Bytes`, and `File`.
Header helpers validate names and reject CR/LF in values, including generated
`Location` and `Content-Disposition` headers, so accidental response-splitting
inputs fail before they reach HTTP.sys. Status helpers validate HTTP status
codes and reject values outside `100`-`599` before the response is sent.
Use `WithName` with `PathByName` or `TryPathByName` to generate route paths for
`Location` headers and links without hand-building URL templates. Route values
are URL-encoded and checked against route constraints such as `{id:int}`;
`PathByName` throws when a named endpoint or required route value is missing.
Use `UrlByName` or `TryUrlByName` when an absolute URL is required; it uses the
request scheme and host, so place `UseForwardedHeaders` before handlers when the
service is behind a trusted proxy. Invalid or missing host values are rejected
instead of reflected into `Location` headers.

`MapGet` endpoints also answer `HEAD` requests. The handler runs once so headers
match the `GET` response, `Content-Length` reflects the body that would have
been sent, and the final body is omitted. Use `MapHead` when a route needs
HEAD-specific headers or work; explicit HEAD endpoints win over the implicit
GET-derived HEAD behavior. `MapOptions` is available for OPTIONS endpoints.
CapiWeb also suppresses bodies for `1xx`, `204`, and `304` responses and omits
generated `Content-Length` values for those statuses.

## Cookies

Cookie helpers avoid hand-building `Cookie` and `Set-Cookie` headers:

```cpp
app.MapGet("/api/session", [](HttpContext& ctx) {
    std::string theme = ctx.Request().Cookie("theme", "light");

    CookieOptions session;
    session.Path = "/";
    session.HttpOnly = true;
    session.Secure = true;
    session.SameSite = CookieSameSiteMode::Lax;
    session.MaxAge = std::chrono::hours(1);

    HttpResult result = Results::Ok(theme);
    result.AppendCookie("session", "abc 123", session);
    return result;
});

app.MapPost("/api/logout", [](HttpContext&) {
    CookieOptions session;
    session.Path = "/";
    HttpResult result = Results::NoContent();
    result.DeleteCookie("session", session);
    return result;
});
```

Request cookies are parsed lazily and percent-decoded. Response cookie values are
percent-encoded as needed, and multiple `Set-Cookie` headers are preserved
through both the in-memory `app.Handle(...)` path and the HTTP.sys send path.
Cookie names, `Path`/`Domain` attributes, and `SameSite` values are validated
before the header is added.

## Request Decompression

`UseRequestDecompression` decodes compressed request bodies before endpoint
handlers and parameter binding read `HttpRequest::Body()`:

```cpp
RequestDecompressionOptions decompression;
decompression.MaxDecompressedBodyBytes = 8ull * 1024ull * 1024ull;

app.UseRequestDecompression(decompression);
app.MapPost("/api/sum", Bind([](int a, int b) {
    return a + b;
}, JsonProperty<int>("a"), JsonProperty<int>("b")));
```

The middleware supports `Content-Encoding: gzip` and `deflate`, including
multiple content-codings decoded in reverse order. `identity` is accepted as a
no-op. Unsupported encodings return `415 Unsupported Media Type`, invalid
compressed bodies return `400 Bad Request`, and decompressed bodies larger than
`MaxDecompressedBodyBytes` return `413 Payload Too Large`. When
`MaxDecompressedBodyBytes` is `0`, CapiWeb uses
`HttpSysOptions::MaxRequestBodyBytes` as the decompressed-size limit. After a
successful decode, `Content-Encoding` is removed and `Content-Length` is updated
by default.

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
secrets with attacker-controlled input. Compression options are validated when
`UseResponseCompression` is called: `GzipLevel` must be `-1` through `9`, and
MIME type lists must contain single media type or prefix patterns without
control characters.

## Response Caching

`UseResponseCaching` provides API response validators without keeping a shared
in-process response cache. It can generate ETags for successful `GET` and
`HEAD` responses, apply an optional `Cache-Control` value, and turn matching
`If-None-Match` requests into `304 Not Modified` responses:

```cpp
ResponseCachingOptions caching;
caching.CacheControl = "public, max-age=60";
caching.MaximumBodySize = 1024 * 1024; // 0 means unlimited

app.UseResponseCompression();
app.UseResponseCaching(caching);
```

By default, it skips non-200 responses, non-GET/HEAD methods, large bodies,
responses with `Content-Encoding`, range/content-digest headers, `no-store`,
`Set-Cookie`, and requests with `Authorization`. Set
`CacheAuthenticatedResponses = true` only when the endpoint's cache policy and
ETag are safe for authenticated callers. `CacheControl` is validated when
`UseResponseCaching` is called so invalid header values fail before traffic is
served.

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
admin.RequireRole("admin");

AuthorizationOptions authorization;
authorization.Policies.push_back(admin);
app.UseAuthorization(authorization);

app.MapGet("/api/secure", [](HttpContext& ctx) {
    return Results::Ok(ctx.User().Name());
}).RequireAuthorization("admin");

auto adminApi = app.MapGroup("/api/admin")
    .RequireRoles({ "admin", "ops" })
    .RequireClaim("tenant", "blue");

adminApi.MapGet("/reports", [] {
    return Results::Ok("reports");
}).RequireClaim("scope", "reports.read");
```

`RequireAuthorization()` without a policy uses `DefaultPolicy`, which requires an
authenticated user by default. `RequireAuthorization("name")` applies a named
policy, and `AllowAnonymous()` lets a route bypass global endpoint authorization.
Policies can require roles, claims, or a custom assertion callback. Use named
policies for shared or assertion-based rules; use endpoint and route-group
`RequireRole`, `RequireRoles`, and `RequireClaim` for local rules without first
registering a policy name. Multiple roles in one call mean any one matching role
is enough, while multiple claim requirements must all pass. Parent route-group
requirements and endpoint requirements are both enforced. Missing or invalid
credentials return `401 Unauthorized`; authenticated users that fail a policy
return `403 Forbidden`.

Authorization options are validated when `UseAuthorization` is called. Policy
names, required roles, and required claim type/value strings cannot contain
control characters, and required role names and claim types cannot be empty.
Endpoint and route-group authorization helpers run the same validation when they
are called.

Authentication options are validated when `UseAuthentication` is called. Scheme
names and the default scheme cannot be empty, contain control characters, or use
leading/trailing whitespace; scheme names are matched case-insensitively and
duplicates are rejected. Authentication challenges are also validated as HTTP
header values before the server starts accepting requests. Authentication and
authorization `HttpContext::Items()` key names are validated at registration
time; set them to an empty string to disable storing that item.

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
proxy chain. `ForwardLimit` must be at least `1`, `KnownProxies` entries are
validated when `UseForwardedHeaders` is called, and malformed
`X-Forwarded-Host` values are ignored instead of replacing the request host. The
applied-state `HttpContext::Items()` key is also validated at registration time.

Host filtering protects public services from unexpected or spoofed `Host`
values. Put it after forwarded headers when a trusted proxy supplies
`X-Forwarded-Host`:

```cpp
HostFilteringOptions hosts;
hosts.AllowedHosts = { "api.example.com", "*.example.net", "127.0.0.1:8080" };

app.UseForwardedHeaders(forwarded);
app.UseHostFiltering(hosts);
```

An empty `AllowedHosts` list disables filtering. Exact hosts match any port
unless the pattern includes a port; wildcard entries such as `*.example.net`
match subdomains but not the apex domain. Use `*` only for local development or
when a front proxy already enforces host policy. Rejected requests return
problem-details JSON with `400 Bad Request` by default. Host patterns, rejected
status code, item key, and failure detail text are validated when
`UseHostFiltering` is called.

HTTPS redirection and HSTS are explicit middleware so development and proxy
topologies stay under application control:

```cpp
HttpsRedirectionOptions redirects;
redirects.HttpsPort = 443;      // set when HTTP and HTTPS listen on different ports
redirects.RedirectStatusCode = 307;

HstsOptions hsts;
hsts.MaxAge = std::chrono::days(365);
hsts.IncludeSubDomains = true;
hsts.Preload = false;

app.UseForwardedHeaders(forwarded);
app.UseHostFiltering(hosts);
app.UseHttpsRedirection(redirects);
app.UseHsts(hsts);
```

`UseHttpsRedirection` validates the request host before building the `Location`
header to avoid reflecting malformed host values. Its redirect status code and
failure detail text are validated when the middleware is registered. `UseHsts`
only writes `Strict-Transport-Security` for HTTPS requests and skips
`localhost`, `127.0.0.1`, and `::1` by default. HSTS options are validated when
`UseHsts` is called: `MaxAge` cannot be negative, and excluded host patterns
must be valid host filters.

## Observability

Request IDs and process-local metrics are built in:

```cpp
RequestIdOptions requestIds;
requestIds.HeaderName = "X-Request-ID";
requestIds.ResponseHeaderName = "X-Request-ID";
requestIds.TrustIncomingHeader = true;
requestIds.MaxLength = 128;
app.UseRequestId(requestIds);

ServerTimingOptions timing;
timing.AddResponseTimeHeader = true;
timing.MetricName = "app";
timing.MetricDescription = "Application";
app.UseServerTiming(timing);

MetricsOptions metrics;
metrics.IncludePathCounters = false; // avoid high-cardinality counters by default
app.UseMetrics(metrics);

app.MapMetrics({ .Path = "/api/metrics" });

app.MapGet("/api/echo/{name}", [](HttpContext& ctx) {
    JsonObject result;
    result.Add("hello", JsonNode::Create(ctx.Request().RouteValue("name")));
    result.Add("traceId", JsonNode::Create(ctx.TraceIdentifier()));
    result.Add("endpoint", JsonNode::Create(ctx.EndpointPattern()));
    return Results::Ok(result);
});
```

The metrics endpoint returns JSON with started/completed/failed/active request
counts, `clientErrors` for 4xx responses, `serverErrors` for 5xx responses,
uptime, total/max/average duration, total/max/average application response body
bytes, and optional method/status/path breakdowns. The response byte counters use
the response body that flows through the middleware pipeline; `HEAD`, `204`, and
`304` responses count as `0` body bytes. The legacy `failed` counter tracks
server errors. Path counters are opt-in because route or URL cardinality can grow
without bound in public services. When path counters are enabled, matched route
patterns such as `/api/products/{id}`
are used by default instead of raw URL paths; set
`MetricsOptions::UseMatchedEndpointPatternForPathCounters = false` to count raw
paths. `HttpContext::MatchedEndpoint()`, `EndpointName()`, and
`EndpointPattern()` expose the same endpoint metadata to middleware, endpoint
filters, and handlers.
`UseRequestId` trims incoming IDs, ignores values containing control characters,
and ignores values longer than `RequestIdOptions::MaxLength` before echoing the
chosen trace id to the response. `UseServerTiming` appends a `Server-Timing`
metric such as `app;dur=2.137;desc="Application"` and can add an
`X-Response-Time` compatibility header. Header names, metric names,
descriptions, and `HttpContext::Items()` keys are validated when the middleware
is registered.

## Rate Limiting

`UseRateLimiter` provides a lightweight fixed-window limiter for single-process
deployments:

```cpp
RateLimitOptions limit;
limit.PermitLimit = 120;
limit.Window = std::chrono::seconds(60);
limit.MaxTrackedKeys = 10000; // 0 means unlimited tracked keys
limit.MaxKeyLength = 256;     // 0 means store full keys

app.UseRateLimiter(limit);
```

The default key is the remote address. Set `KeySelector` to rate limit by API
key, tenant, authenticated user, or any application-specific value:

```cpp
RateLimitOptions limit;
limit.PermitLimit = 1000;
limit.Window = std::chrono::minutes(1);
limit.KeySelector = [](HttpContext& ctx) {
    const HttpRequest& request = ctx.Request();
    return request.Header("X-Api-Key", request.RemoteAddress());
};

app.UseRateLimiter(limit);
```

Allowed responses include `X-RateLimit-Limit`, `X-RateLimit-Remaining`, and
`X-RateLimit-Reset` for both `Results::*` handlers and handlers that write to
`HttpContext::Response()`. Rejected responses also include `Retry-After` and a
problem-details JSON body. Set `PermitLimit = 0` or `Window = 0s` to disable the
limiter while keeping the call in place. `MaxTrackedKeys` bounds the in-memory
key table under high-cardinality traffic; when the table is full, new keys are
rejected with the configured status instead of growing memory without bound.
`MaxKeyLength` bounds each stored key. When a selected key is longer than the
limit, CapiWeb keeps a readable prefix and appends a stable hash so repeated
requests share the same quota without storing unbounded user input.

## Health Checks

Health checks are first-class endpoints, so load balancers and service monitors
do not need app-specific probe code:

```cpp
app.AddHealthCheck("self", [] {
    return HealthCheckResult::Healthy("Process is running.");
}, { "live" });

app.AddHealthCheck("static-files", [] {
    return std::filesystem::exists("wwwroot/index.html")
        ? HealthCheckResult::Healthy("Frontend assets are available.")
        : HealthCheckResult::Unhealthy("wwwroot/index.html is missing.");
}, { "ready" });

HealthCheckOptions health;
health.Path = "/api/health";
health.IncludeDetails = true;
app.MapHealthChecks(health);
```

Use tags to expose separate liveness and readiness probes from the same
registration list:

```cpp
HealthCheckOptions live;
live.Path = "/health/live";
live.Tags = { "live" };
app.MapHealthChecks(live);

HealthCheckOptions ready;
ready.Path = "/health/ready";
ready.Tags = { "ready" };
ready.IncludeDetails = true;
ready.FailureStatusCode = 503;
app.MapHealthChecks(ready);
```

The aggregate response is `200` for `Healthy`,
`HealthCheckOptions::FailureStatusCode` for `Unhealthy`, and `200` for
`Degraded` unless `TreatDegradedAsFailure` is enabled. The default failure
status is `503 Service Unavailable`. Thrown exceptions are captured as unhealthy
check results instead of escaping as unhandled endpoint errors. Health responses
include `isRunning`, `isStopping`, and `activeRequests` by default; set
`IncludeApplicationState = false` to omit them. During graceful shutdown,
`TreatStoppingAsFailure = true` makes health endpoints return the configured
failure status so readiness probes can remove the instance before the process
exits. `HealthCheckOptions::Predicate` can apply custom filtering when tags are
not expressive enough. Health check names, tags, endpoint paths, and failure
status codes are validated when registered.

## Graceful Shutdown and Backpressure

HTTP.sys request processing has bounded shutdown and overload behavior:

```cpp
builder.ConfigureHttpSys([](HttpSysOptions& http) {
    http.MaxConcurrentRequests = 4096;       // 0 means unlimited
    http.StopTimeout = std::chrono::seconds(10);
    http.Backpressure.OverloadedStatusCode = 429;
    http.Backpressure.OverloadedRetryAfterSeconds = 3;
    http.Backpressure.StoppingRetryAfterSeconds = 10;
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

Complete middleware, endpoint, health check, hosted service, and lifecycle
callback registration before calling `Start()` or `Run()`. The application
configuration is frozen on first start so request-processing threads never race
against late pipeline or route mutations; late `Use...`, `Map...`, `Add...`,
and endpoint metadata calls throw `std::logic_error`.

When stopping, CapiWeb marks the HTTP.sys URL group inactive, stops accepting new
requests, waits up to `StopTimeout` for active handlers to drain, then shuts down
the request queue so worker threads unblock cleanly. Requests that arrive after
draining starts receive `HttpSysOptions::Backpressure.StoppingStatusCode`
(`503` by default) with an optional `Retry-After` header.

When `MaxConcurrentRequests` is non-zero and all request slots are busy, CapiWeb
returns `HttpSysOptions::Backpressure.OverloadedStatusCode` (`503` by default,
often set to `429`) with `Retry-After` instead of running user middleware.
Backpressure status codes, details, and retry delays are configurable from code,
JSON, or environment variables, and are validated before the listener starts.
`ActiveRequestCount()` and `IsStopping()` are available for health checks and
diagnostics.

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
Problem details options are validated during application build and when
`UseProblemDetails(options)` is called: `DefaultType` and extension names cannot
contain control characters, and the trace-id extension name must be non-empty
and must not be one of the reserved `type`, `title`, `status`, `detail`, or
`instance` members.

Request bodies larger than `HttpSysOptions::MaxRequestBodyBytes` return `413`;
the default limit is 16 MiB, and `0` disables the application-level body limit.
Request URLs larger than `MaxRequestUrlBytes` return `414`, and aggregate
request headers larger than `MaxRequestHeaderBytes` return `431`; their defaults
are 16 KiB and 64 KiB, and `0` disables each limit. Invalid JSON parsing returns
`400`; unhandled application exceptions return `500`. The in-memory
`app.Handle(...)` test path uses the same conversions and request size limits as
the HTTP.sys listener, so integration tests can assert the same problem-details
responses that production clients receive.

## Routing Behavior

Routes support literals, parameters such as `/api/users/{id}`, and catch-all
parameters such as `/files/{*path}`. `HEAD` can match `GET` endpoints. If a path
matches an endpoint but the method does not, CapiWeb returns `405 Method Not
Allowed` with an `Allow` header.

Route patterns and route-group prefixes are validated when endpoints are
registered. They must be application-relative paths and cannot include query
strings, fragments, absolute URLs, or backslashes. `MapMethods` also validates
HTTP method tokens, and endpoint/fallback handlers cannot be empty. CapiWeb also
rejects duplicate fallback endpoints, duplicate endpoint names, and exact
route/method registration conflicts early. A `MapGet` endpoint can still be
paired with an explicit `MapHead` endpoint for the same route; the explicit
`HEAD` handler wins over the implicit `GET`-derived HEAD behavior.

Route parameters can use common constraints: `{id:int}`, `{id:long}`,
`{price:float}`, `{ratio:double}`, `{enabled:bool}`, `{id:guid}`, and
`{name:alpha}`. Constraints participate in route matching, so
`/api/orders/{id:int}` will not match `/api/orders/abc`; the route value name
remains `id` for `Route<int>("id")`. Parameter names must be identifier-like
(`id`, `order_id`) and unique within a route, and catch-all parameters such as
`{*path}` must be the final segment. OpenAPI output removes constraint syntax
from the path template and maps obvious types such as `int`/`long` to
`integer`, `float`/`double` to `number`, and `bool` to `boolean`; it also emits
formats such as `int32`, `int64`, `float`, `double`, and `uuid` when available.

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
- `UrlAclMode::Delete`: remove configured URL ACLs. Delete mode is a
  machine-configuration operation; `Start()` and `Run()` process the delete and
  return without starting HTTP.sys listeners.
- Delete operations cannot be combined with `Ensure` or `Refresh` operations in
  the same HTTP.sys configuration. Run cleanup and apply/startup as separate
  deployment steps so the resulting machine state is explicit.
- `RemoveOnStop`: optional cleanup on graceful stop. Default is `false` because
  URL ACLs are usually deployment configuration, not process-local state.
- `IgnoreAccessDenied`: useful for apps that try to ensure the reservation when
  elevated, but can also run normally after deployment has already created it.

CapiWeb uses `HttpSetServiceConfiguration` and
`HttpDeleteServiceConfiguration` directly; it does not shell out to `netsh`.
If startup applies machine-level configuration and a later HTTP.sys startup
step fails, entries marked with `RemoveOnStop` are also cleaned up before the
failure is rethrown. HTTP.sys failures include the failing operation, Windows
error text, decimal error code, and an 8-digit hexadecimal code to make event log
and Win32 documentation lookups straightforward.

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
to leave the machine untouched. Delete mode needs only the binding IP:port; it
does not require a certificate hash or AppId and makes `Start()`/`Run()` return
without starting listeners. `RemoveOnStop` is available but defaults to
`false`, because certificate bindings are normally deployment state. Duplicate
SSL bindings for the same IP:port, and mixed delete/apply plans, are rejected
during application build so machine configuration is not touched with an
ambiguous deployment plan.

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
worker.StopTimeout = std::chrono::seconds(15);
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
Service names are optional, but named services must be unique
case-insensitively and cannot contain control characters because they appear in
lifecycle logs. Hosted services must provide `Start` or `Stop`; background
services must provide `Execute`, and these checks run when the service is
registered.

`AddBackgroundService` owns a worker thread and passes a cooperative
`std::stop_token` to the worker. Unhandled worker exceptions are logged and stop
the application by default; set `StopApplicationOnException = false` for
supervised workers that should fail independently. During shutdown the framework
requests cancellation and waits up to `BackgroundServiceOptions::StopTimeout`
(`30s` by default) before logging a warning; it still waits for the thread by
default so the worker cannot outlive objects it may reference. Set
`DetachOnStopTimeout = true` only for workers that are designed to stop without
touching the application object or other soon-to-be-destroyed state.

## In-Memory Testing

`WebApplication::Handle` runs the same middleware and endpoint pipeline without
starting HTTP.sys. It is intended for fast route, middleware, binding, and error
response tests that must not touch URL ACLs, ports, or certificate bindings:

```cpp
WebApplication app = WebApplication::Create();
app.UseProblemDetails();
app.MapGet("/api/hello/{name}", [](HttpContext& ctx) {
    return Results::Text("hello " + ctx.Request().RouteValue("name"));
});

HttpRequest request;
request.Method("GET").Path("/api/hello/ada");

HttpResponse response = app.Handle(std::move(request));
```

The repository includes `CapiWeb.Tests`, a small console test project covering
offline routing, method mismatch handling, parameter binding, problem-details
configuration, request decompression, response caching, and response
compression:

```powershell
msbuild .\capi.sln /p:Configuration=Debug /p:Platform=x64 /m
.\x64\Debug\CapiWeb.Tests.exe
```

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
should remove them. CapiWeb validates service names, dependencies, start mode,
stop timeouts, and NUL-sensitive text fields before opening SCM handles, so
invalid deployment commands fail without partially touching machine service
configuration.

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
- ETag/conditional response caching for eligible API responses
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
