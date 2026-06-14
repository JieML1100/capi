#include <Capi/Web.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace Capi::Web;
using namespace System::Text::Json;

namespace {

WindowsServiceOptions CreateServiceOptions() {
    WindowsServiceOptions service;
    service.ServiceName = L"CapiSample";
    service.DisplayName = L"Capi Sample Web Service";
    service.Description = L"Sample CapiWeb HTTP.sys web service.";
    service.StartMode = WindowsServiceStartMode::Automatic;
    return service;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    WindowsServiceOptions service = CreateServiceOptions();
    if (argc > 1) {
        std::wstring command = argv[1];
        if (command == L"--install-service") {
            WindowsServiceHost::Install(service);
            std::wcout << L"Installed service: " << service.ServiceName << std::endl;
            return 0;
        }
        if (command == L"--uninstall-service") {
            WindowsServiceHost::Uninstall(service.ServiceName);
            std::wcout << L"Uninstalled service: " << service.ServiceName << std::endl;
            return 0;
        }
        if (command == L"--start-service") {
            WindowsServiceHost::Start(service.ServiceName);
            std::wcout << L"Started service: " << service.ServiceName << std::endl;
            return 0;
        }
        if (command == L"--stop-service") {
            WindowsServiceHost::Stop(service.ServiceName);
            std::wcout << L"Stopped service: " << service.ServiceName << std::endl;
            return 0;
        }
    }

    auto builder = WebApplication::CreateBuilder();
    builder
        .ConfigureFromJsonFile("appsettings.json", true)
        .ConfigureFromEnvironment()
        .ConfigureLogging([](const LogMessage& message) {
            std::cout << "[capi] " << message.Message << std::endl;
        });

    auto app = builder.Build();
    auto heartbeatTicks = std::make_shared<std::atomic<long long>>(0);

    app.UseForwardedHeaders();
    app.UseRequestId();
    ProblemDetailsOptions problemDetails;
    problemDetails.Customize = [](HttpContext&, ProblemDetails& problem) {
        problem.Extensions["service"] = JsonNode::Create("Capi.Sample");
    };
    app.UseProblemDetails(problemDetails);

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

            ClaimsPrincipal user = ClaimsPrincipal::Authenticated("sample-client", "ApiKey")
                .AddRole("admin")
                .AddClaim("scope", "sample.read");
            return AuthenticationResult::Success(std::move(user));
        },
        "ApiKey"
    });
    app.UseAuthentication(authentication);

    AuthorizationPolicy adminPolicy;
    adminPolicy.Name = "admin";
    adminPolicy.RequiredRoles = { "admin" };

    AuthorizationOptions authorization;
    authorization.Policies.push_back(std::move(adminPolicy));
    authorization.IncludeFailureDetails = true;
    app.UseAuthorization(authorization);

    app.UseMetrics();
    app.UseRequestLogging();
    RateLimitOptions rateLimit;
    rateLimit.PermitLimit = 120;
    rateLimit.Window = std::chrono::seconds(60);
    app.UseRateLimiter(rateLimit);
    app.UseResponseCompression();
    app.UseSecurityHeaders();
    app.UseCors();

    StaticFileOptions files;
    files.Root = "wwwroot";
    files.EnableSpaFallback = true;
    files.SpaFallbackExcludedPrefixes = { "/api" };
    files.CacheControl = "no-cache";
    files.EnableConditionalRequests = true;
    files.EnableRangeProcessing = true;
    app.UseStaticFiles(files);

    app.OnStarted([] {
        std::cout << "[capi] application started" << std::endl;
    });

    app.OnStopping([] {
        std::cout << "[capi] application stopping" << std::endl;
    });

    app.OnStopped([] {
        std::cout << "[capi] application stopped" << std::endl;
    });

    BackgroundServiceOptions heartbeat;
    heartbeat.Name = "heartbeat";
    heartbeat.StopApplicationOnException = true;
    heartbeat.Execute = [heartbeatTicks](WebApplication&, std::stop_token stop) {
        while (!stop.stop_requested()) {
            heartbeatTicks->fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    };
    app.AddBackgroundService(std::move(heartbeat));

    app.AddHealthCheck("self", [] {
        return HealthCheckResult::Healthy("Application is accepting requests.");
    });

    app.AddHealthCheck("lifetime", [&app] {
        if (app.IsStopping()) {
            return HealthCheckResult::Unhealthy("Application is draining.");
        }
        return HealthCheckResult::Healthy("Active requests: " + std::to_string(app.ActiveRequestCount()));
    });

    app.AddHealthCheck("static-files", [] {
        if (std::filesystem::exists("wwwroot/index.html")) {
            return HealthCheckResult::Healthy("wwwroot/index.html is available.");
        }
        return HealthCheckResult::Unhealthy("wwwroot/index.html is missing.");
    });

    app.AddHealthCheck("background-heartbeat", [heartbeatTicks] {
        return HealthCheckResult::Healthy("Heartbeat ticks: " + std::to_string(heartbeatTicks->load(std::memory_order_relaxed)));
    });

    HealthCheckOptions health;
    health.Path = "/api/health";
    health.IncludeDetails = true;
    app.MapHealthChecks(health)
        .WithName("Health")
        .WithTags({ "Operations" })
        .WithSummary("Returns aggregate service health.")
        .Produces(200)
        .Produces(503, "application/json", "Unhealthy");

    MetricsEndpointOptions metrics;
    metrics.Path = "/api/metrics";
    app.MapMetrics(metrics)
        .WithName("Metrics")
        .WithTags({ "Operations" })
        .WithSummary("Returns process-local request metrics.")
        .Produces(200);

    app.MapGet("/api/echo/{name}", [](HttpContext& ctx) {
        JsonObject result;
        result.Add("hello", JsonNode::Create(ctx.Request().RouteValue("name")));
        result.Add("from", JsonNode::Create(ctx.Request().RemoteAddress()));
        result.Add("scheme", JsonNode::Create(ctx.Request().Scheme()));
        result.Add("host", JsonNode::Create(ctx.Request().Host()));
        result.Add("traceId", JsonNode::Create(ctx.TraceIdentifier()));
        return Results::Ok(result);
    })
        .WithName("Echo")
        .WithTags({ "Sample" })
        .WithSummary("Echoes a route value and request metadata.")
        .Produces(200);

    app.MapGet("/api/secure", [](HttpContext& ctx) {
        JsonObject result;
        result.Add("message", JsonNode::Create("You reached an authorized endpoint."));
        result.Add("user", JsonNode::Create(ctx.User().Name()));
        result.Add("authType", JsonNode::Create(ctx.User().AuthenticationType()));
        result.Add("scope", JsonNode::Create(ctx.User().Claim("scope")));
        return Results::Ok(result);
    })
        .WithName("Secure")
        .WithTags({ "Sample" })
        .WithSummary("Demonstrates API key authentication and policy authorization.")
        .WithHeaderParameter("X-API-Key", "string", true, "Use demo-secret for the sample.")
        .Produces(200)
        .RequireAuthorization("admin");

    app.MapGet("/api/bind/{id}", Bind([](int id, int multiplier) {
        JsonObject result;
        result.Add("id", JsonNode::Create(id));
        result.Add("multiplier", JsonNode::Create(multiplier));
        result.Add("product", JsonNode::Create(id * multiplier));
        return Results::Ok(result);
    }, Route<int>("id"), Query<int>("multiplier").Default(1)))
        .WithName("Bind")
        .WithTags({ "Sample" })
        .WithSummary("Demonstrates route and query parameter binding.")
        .WithQueryParameter("multiplier", "integer", false, "Multiplier applied to the route id.")
        .Produces(200);

    app.MapPost("/api/sum", Bind([](int a, int b) {
        JsonObject result;
        result.Add("a", JsonNode::Create(a));
        result.Add("b", JsonNode::Create(b));
        result.Add("sum", JsonNode::Create(a + b));
        return Results::Ok(result);
    }, JsonProperty<int>("a"), JsonProperty<int>("b")))
        .WithName("Sum")
        .WithTags({ "Sample" })
        .WithSummary("Adds two JSON body properties.")
        .Accepts("application/json")
        .Produces(200);

    OpenApiOptions openApi;
    openApi.Path = "/openapi.json";
    openApi.Info.Title = "Capi Sample API";
    openApi.Info.Version = "v1";
    openApi.Info.Description = "HTTP.sys-hosted C++ WebAPI sample.";
    openApi.ServerUrls = { "http://localhost:8080" };
    OpenApiSecurityScheme apiKey;
    apiKey.Name = "ApiKey";
    apiKey.Type = "apiKey";
    apiKey.In = "header";
    apiKey.ParameterName = "X-API-Key";
    apiKey.Description = "Sample API key. Use demo-secret.";
    openApi.SecuritySchemes.push_back(std::move(apiKey));
    openApi.DefaultSecurityScheme = "ApiKey";
    app.MapOpenApi(openApi);

    SwaggerUiOptions docs;
    docs.Path = "/docs";
    docs.OpenApiPath = openApi.Path;
    docs.Title = "Capi Sample API";
    app.MapSwaggerUi(docs);

    std::cout << "Listening on http://localhost:8080/" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;
    return WindowsServiceHost::Run(app, std::move(service));
}
