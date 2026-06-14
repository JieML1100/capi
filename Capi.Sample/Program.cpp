#include <Capi/Web.h>

#include <iostream>

using namespace Capi::Web;
using namespace System::Text::Json;

int main() {
    auto builder = WebApplication::CreateBuilder();
    builder
        .UseUrls({ "http://localhost:8080/" })
        .UseDetailedErrors()
        .ConfigureLogging([](const LogMessage& message) {
            std::cout << "[capi] " << message.Message << std::endl;
        })
        .ConfigureHttpSys([](HttpSysOptions& http) {
            http.UrlReservation.Mode = UrlAclMode::Ensure;
            http.UrlReservation.IgnoreAccessDenied = true;
            http.MaxRequestBodyBytes = 4ull * 1024ull * 1024ull;
        });

    auto app = builder.Build();

    StaticFileOptions files;
    files.Root = "wwwroot";
    files.EnableSpaFallback = true;
    files.CacheControl = "no-cache";
    app.UseStaticFiles(files);

    app.MapGet("/api/health", [] {
        JsonObject result;
        result.Add("status", JsonNode::Create("ok"));
        result.Add("framework", JsonNode::Create("CapiWeb"));
        result.Add("version", JsonNode::Create(std::string(Version)));
        return Results::Ok(result);
    });

    app.MapGet("/api/echo/{name}", [](HttpContext& ctx) {
        JsonObject result;
        result.Add("hello", JsonNode::Create(ctx.Request().RouteValue("name")));
        result.Add("from", JsonNode::Create(ctx.Request().RemoteAddress()));
        return Results::Ok(result);
    });

    app.MapPost("/api/sum", [](HttpContext& ctx) {
        auto json = ctx.Request().Json();
        int a = json.GetProperty("a").GetInt32();
        int b = json.GetProperty("b").GetInt32();

        JsonObject result;
        result.Add("a", JsonNode::Create(a));
        result.Add("b", JsonNode::Create(b));
        result.Add("sum", JsonNode::Create(a + b));
        return Results::Ok(result);
    });

    std::cout << "Listening on http://localhost:8080/" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;
    app.Run();
    return 0;
}
