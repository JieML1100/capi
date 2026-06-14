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

    app.MapGet("/api/health", [] {
        JsonObject result;
        result.Add("status", JsonNode::Create("ok"));
        return Results::Ok(result);
    });

    app.UseStaticFiles({ .Root = "wwwroot" });
    app.Run();
}
```

Default URL: `http://localhost:8080/`.

## Detailed Configuration

```cpp
auto builder = WebApplication::CreateBuilder();
builder
    .UseUrls({ "http://localhost:8080/", "http://127.0.0.1:8081/" })
    .UseDetailedErrors()
    .ConfigureLogging([](const LogMessage& message) {
        std::cout << message.Message << std::endl;
    })
    .ConfigureHttpSys([](HttpSysOptions& http) {
        http.WorkerCount = 8;
        http.RequestBufferSize = 128 * 1024;
        http.MaxRequestBodyBytes = 32ull * 1024ull * 1024ull;
        http.UrlReservation.Mode = UrlAclMode::Refresh;
        http.UrlReservation.SecurityDescriptor = L"D:(A;;GX;;;WD)";
    });

auto app = builder.Build();
```

## API + Frontend Hosting

```cpp
StaticFileOptions files;
files.Root = "wwwroot";
files.EnableSpaFallback = true;
files.CacheControl = "no-cache";

app.UseStaticFiles(files);
app.MapGet("/api/echo/{name}", [](HttpContext& ctx) {
    JsonObject result;
    result.Add("hello", JsonNode::Create(ctx.Request().RouteValue("name")));
    return Results::Ok(result);
});
```

Static file hosting rejects path traversal, serves default documents, maps common
content types, and can fall back to `index.html` for SPA routes.

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

## Sample

Build `Capi.Sample` and open `http://localhost:8080/`. The sample hosts:

- `GET /api/health`
- `GET /api/echo/{name}`
- `POST /api/sum`
- `wwwroot/index.html` from the same HTTP.sys listener
