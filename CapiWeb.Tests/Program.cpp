#include <Capi/Web.h>

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <zlib.h>

using namespace Capi::Web;
using namespace System::Text::Json;

namespace {

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TemporaryDirectory {
    std::filesystem::path Path;

    TemporaryDirectory() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        Path = std::filesystem::temp_directory_path() / ("capiweb-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(Path);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(Path, ignored);
    }
};

void Require(bool condition, std::string message) {
    if (!condition) {
        throw TestFailure(std::move(message));
    }
}

void RequireEqual(std::string_view expected, std::string_view actual, std::string_view message) {
    if (expected != actual) {
        throw TestFailure(std::string(message) + " expected '" + std::string(expected) + "' actual '" + std::string(actual) + "'");
    }
}

void RequireEqual(int expected, int actual, std::string_view message) {
    if (expected != actual) {
        throw TestFailure(std::string(message) + " expected " + std::to_string(expected) + " actual " + std::to_string(actual));
    }
}

template <class Fn>
void RequireThrowsWithMessage(Fn&& fn, std::string_view expectedMessagePart, std::string_view message) {
    try {
        fn();
    } catch (const std::exception& ex) {
        std::string actual = ex.what();
        if (actual.find(expectedMessagePart) == std::string::npos) {
            throw TestFailure(std::string(message) + " expected exception containing '" +
                std::string(expectedMessagePart) + "' actual '" + actual + "'");
        }
        return;
    }
    throw TestFailure(std::string(message) + " expected an exception");
}

JsonElement ParseJsonBody(const HttpResponse& response) {
    return JsonDocument::Parse(response.BodyText())->RootElement();
}

HttpResponse Send(WebApplication& app, std::string method, std::string path, std::string query = {}) {
    HttpRequest request;
    request.Method(std::move(method)).Path(std::move(path));
    if (!query.empty()) {
        request.QueryString(std::move(query));
    }
    return app.Handle(std::move(request));
}

std::string DeflateForTest(std::string_view input, int windowBits) {
    z_stream stream{};
    int init = deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
    if (init != Z_OK) {
        throw TestFailure("deflateInit2 failed");
    }

    std::vector<std::uint8_t> output;
    std::array<std::uint8_t, 4096> buffer{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    int result = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = deflate(&stream, Z_FINISH);
        if (result != Z_OK && result != Z_STREAM_END) {
            deflateEnd(&stream);
            throw TestFailure("deflate failed");
        }
        std::size_t produced = buffer.size() - stream.avail_out;
        output.insert(output.end(), buffer.data(), buffer.data() + produced);
    } while (result != Z_STREAM_END);

    deflateEnd(&stream);
    return std::string(reinterpret_cast<const char*>(output.data()), output.size());
}

std::string GzipForTest(std::string_view input) {
    return DeflateForTest(input, 15 | 16);
}

std::string ZlibDeflateForTest(std::string_view input) {
    return DeflateForTest(input, 15);
}

void RouteParametersAndTextResultsWork() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/hello/{name}", [](HttpContext& ctx) {
        return Results::Text("hello " + ctx.Request().RouteValue("name"));
    });

    HttpResponse response = Send(app, "GET", "/api/hello/ada");

    RequireEqual(200, response.StatusCode(), "route status");
    RequireEqual("hello ada", response.BodyText(), "route body");
    RequireEqual("text/plain; charset=utf-8", response.ContentType(), "route content type");
}

void MethodMismatchReturns405WithAllow() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/hello/{name}", [](HttpContext&) {
        return Results::Ok("ok");
    });

    HttpResponse response = Send(app, "POST", "/api/hello/ada");

    RequireEqual(405, response.StatusCode(), "405 status");
    RequireEqual("GET, HEAD", response.Headers().Get("Allow"), "405 allow header");
}

void EndpointRegistrationValidatesRoutesMethodsAndHandlers() {
    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/search?q=1", [] {
            return Results::Ok();
        });
    }, "query string", "route patterns should reject query strings");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("https://example.com/api", [] {
            return Results::Ok();
        });
    }, "application-relative", "route patterns should reject absolute URLs");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        auto api = app.MapGroup("/api?version=1");
        (void)api;
    }, "query string", "route group prefixes should reject query strings");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapMethods({ "BAD METHOD" }, "/api/bad-method", [](HttpContext&) {
            return Results::Ok();
        });
    }, "HTTP method", "MapMethods should reject invalid method tokens");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        EndpointHandler handler;
        app.MapMethods({ "GET" }, "/api/empty-handler", std::move(handler));
    }, "handler", "MapMethods should reject empty endpoint handlers");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        EndpointHandler handler;
        app.MapFallback(std::move(handler));
    }, "Fallback", "MapFallback should reject empty endpoint handlers");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/duplicate", [] {
            return Results::Ok();
        });
        app.MapGet("/api/duplicate/", [] {
            return Results::Ok();
        });
    }, "conflicts", "duplicate endpoint routes should be rejected");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapAny("/api/any", [] {
            return Results::Ok();
        });
        app.MapPost("/api/any", [] {
            return Results::Ok();
        });
    }, "conflicts", "MapAny should conflict with method-specific endpoints on the same route");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapFallback([] {
            return Results::NotFound();
        });
        app.MapFallback([] {
            return Results::NotFound();
        });
    }, "fallback endpoint", "duplicate fallback endpoints should be rejected");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/a", [] {
            return Results::Ok();
        }).WithName("GetItem");
        app.MapGet("/api/b", [] {
            return Results::Ok();
        }).WithName("getitem");
    }, "Duplicate endpoint name", "endpoint names should be unique case-insensitively");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/bad-name", [] {
            return Results::Ok();
        }).WithName("bad\nname");
    }, "control", "endpoint names should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/bad-tag", [] {
            return Results::Ok();
        }).WithTags({ "ok", "bad\r\ntag" });
    }, "control", "endpoint tags should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGroup("/api").WithTags({ "bad\n tag" });
    }, "control", "route group tags should reject control characters");
}

void HeadRequestsUseGetEndpointWithoutBody() {
    WebApplication app = WebApplication::Create();
    int calls = 0;
    app.MapGet("/api/hello/{name}", [&](HttpContext& ctx) {
        ++calls;
        return Results::Text("hello " + ctx.Request().RouteValue("name"));
    });

    HttpResponse response = Send(app, "HEAD", "/api/hello/ada");

    RequireEqual(200, response.StatusCode(), "HEAD status");
    Require(response.Body().empty(), "HEAD body should be suppressed");
    RequireEqual("9", response.Headers().Get("Content-Length"), "HEAD content length");
    RequireEqual("text/plain; charset=utf-8", response.ContentType(), "HEAD content type");
    RequireEqual(1, calls, "HEAD should invoke GET handler");
}

void ExplicitHeadEndpointWinsOverImplicitGetHead() {
    WebApplication app = WebApplication::Create();
    int getCalls = 0;
    int headCalls = 0;
    app.MapGet("/api/info", [&] {
        ++getCalls;
        return Results::Text("get-body");
    });
    app.MapHead("/api/info", [&] {
        ++headCalls;
        return Results::Text("head-body").Header("X-Head-Handler", "true");
    });

    HttpResponse head = Send(app, "HEAD", "/api/info");
    RequireEqual(200, head.StatusCode(), "explicit HEAD status");
    Require(head.Body().empty(), "explicit HEAD body should be suppressed");
    RequireEqual("9", head.Headers().Get("Content-Length"), "explicit HEAD content length");
    RequireEqual("true", head.Headers().Get("X-Head-Handler"), "explicit HEAD header");
    RequireEqual(0, getCalls, "explicit HEAD should not call GET handler");
    RequireEqual(1, headCalls, "explicit HEAD should call HEAD handler");

    HttpResponse get = Send(app, "GET", "/api/info");
    RequireEqual(200, get.StatusCode(), "GET after explicit HEAD status");
    RequireEqual("get-body", get.BodyText(), "GET after explicit HEAD body");
    RequireEqual(1, getCalls, "GET should call GET handler");
    RequireEqual(1, headCalls, "GET should not call HEAD handler");
}

void MapOptionsConvenienceWorks() {
    WebApplication app = WebApplication::Create();
    auto api = app.MapGroup("/api");
    int calls = 0;
    api.MapOptions("/items", [&] {
        ++calls;
        return Results::NoContent().Header("Allow", "GET, POST, OPTIONS");
    });

    HttpResponse response = Send(app, "OPTIONS", "/api/items");

    RequireEqual(204, response.StatusCode(), "MapOptions status");
    Require(response.Body().empty(), "MapOptions body should be empty");
    RequireEqual("GET, POST, OPTIONS", response.Headers().Get("Allow"), "MapOptions allow header");
    RequireEqual(1, calls, "MapOptions should call handler");
}

void ParameterBindingReadsRouteAndQueryValues() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/bind/{id}", Bind(
        [](int id, int multiplier) {
            JsonObject payload;
            payload.Add("id", JsonNode::Create(id));
            payload.Add("multiplier", JsonNode::Create(multiplier));
            payload.Add("product", JsonNode::Create(id * multiplier));
            return Results::Json(payload);
        },
        Route<int>("id"),
        Query<int>("multiplier")));
    app.MapGet("/api/query/multi", [](HttpContext& ctx) {
        JsonObject payload;
        JsonArray tags;
        for (const std::string& tag : ctx.Request().QueryValues("tag")) {
            tags.Add(JsonNode::Create(tag));
        }
        payload.Add("tags", tags);
        payload.Add("single", JsonNode::Create(ctx.Request().QueryValue("tag")));
        payload.Add("encoded", JsonNode::Create(ctx.Request().QueryValue("encoded")));
        payload.Add("missingCount", JsonNode::Create(static_cast<int>(ctx.Request().QueryValues("missing").size())));
        return Results::Json(payload);
    });
    app.MapGet("/api/query-vector", Bind(
        [](std::vector<std::string> tags, std::vector<int> ids, std::vector<int> missing) {
            JsonObject payload;
            JsonArray tagArray;
            for (const std::string& tag : tags) {
                tagArray.Add(JsonNode::Create(tag));
            }
            JsonArray idArray;
            for (int id : ids) {
                idArray.Add(JsonNode::Create(id));
            }
            payload.Add("tags", tagArray);
            payload.Add("ids", idArray);
            payload.Add("missingCount", JsonNode::Create(static_cast<int>(missing.size())));
            return Results::Json(payload);
        },
        Query<std::vector<std::string>>("tag"),
        Query<std::vector<int>>("id"),
        Query<std::vector<int>>("missing")));
    app.MapGet("/api/header-vector", Bind(
        [](std::vector<int> ids) {
            JsonObject payload;
            JsonArray idArray;
            for (int id : ids) {
                idArray.Add(JsonNode::Create(id));
            }
            payload.Add("ids", idArray);
            return Results::Json(payload);
        },
        Header<std::vector<int>>("X-Id")));

    HttpResponse response = Send(app, "GET", "/api/bind/7", "?multiplier=6");
    JsonElement body = ParseJsonBody(response);

    RequireEqual(200, response.StatusCode(), "binding status");
    RequireEqual(7, body.GetProperty("id").GetInt32(), "binding id");
    RequireEqual(6, body.GetProperty("multiplier").GetInt32(), "binding multiplier");
    RequireEqual(42, body.GetProperty("product").GetInt32(), "binding product");

    HttpResponse multi = Send(app, "GET", "/api/query/multi", "?tag=red&tag=blue&encoded=a+b");
    JsonElement multiBody = ParseJsonBody(multi);
    RequireEqual(200, multi.StatusCode(), "query multi-value status");
    RequireEqual(2, multiBody.GetProperty("tags").GetArrayLength(), "query multi-value count");
    RequireEqual("red", multiBody.GetProperty("tags")[0].GetString(), "query first value");
    RequireEqual("blue", multiBody.GetProperty("tags")[1].GetString(), "query second value");
    RequireEqual("blue", multiBody.GetProperty("single").GetString(), "query single accessor should keep last value");
    RequireEqual("a b", multiBody.GetProperty("encoded").GetString(), "query multi-value should decode plus as space");
    RequireEqual(0, multiBody.GetProperty("missingCount").GetInt32(), "query missing multi-value count");

    HttpResponse vectorResponse = Send(app, "GET", "/api/query-vector", "?tag=red&tag=blue&id=1&id=2");
    JsonElement vectorBody = ParseJsonBody(vectorResponse);
    RequireEqual(200, vectorResponse.StatusCode(), "query vector binding status");
    RequireEqual(2, vectorBody.GetProperty("tags").GetArrayLength(), "query vector tags count");
    RequireEqual("red", vectorBody.GetProperty("tags")[0].GetString(), "query vector first tag");
    RequireEqual("blue", vectorBody.GetProperty("tags")[1].GetString(), "query vector second tag");
    RequireEqual(2, vectorBody.GetProperty("ids").GetArrayLength(), "query vector ids count");
    RequireEqual(1, vectorBody.GetProperty("ids")[0].GetInt32(), "query vector first id");
    RequireEqual(2, vectorBody.GetProperty("ids")[1].GetInt32(), "query vector second id");
    RequireEqual(0, vectorBody.GetProperty("missingCount").GetInt32(), "query vector missing should bind empty vector");

    HttpResponse badVector = Send(app, "GET", "/api/query-vector", "?id=bad");
    JsonElement badVectorBody = ParseJsonBody(badVector);
    RequireEqual(400, badVector.StatusCode(), "query vector invalid value status");
    RequireEqual("Bad Request", badVectorBody.GetProperty("title").GetString(), "query vector invalid value title");

    HttpRequest headerVectorRequest;
    headerVectorRequest.Method("GET")
        .Path("/api/header-vector")
        .AddHeader("X-Id", "7")
        .AddHeader("X-Id", "9");
    HttpResponse headerVector = app.Handle(std::move(headerVectorRequest));
    JsonElement headerVectorBody = ParseJsonBody(headerVector);
    RequireEqual(200, headerVector.StatusCode(), "header vector binding status");
    RequireEqual(2, headerVectorBody.GetProperty("ids").GetArrayLength(), "header vector ids count");
    RequireEqual(7, headerVectorBody.GetProperty("ids")[0].GetInt32(), "header vector first id");
    RequireEqual(9, headerVectorBody.GetProperty("ids")[1].GetInt32(), "header vector second id");
}

void RouteConstraintsMatchBindAndDescribeParameters() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/orders/{id:int}", Bind(
        [](int id) {
            return Results::Text("order-" + std::to_string(id));
        },
        Route<int>("id")));
    app.MapGet("/api/orders/{slug:alpha}", [](HttpContext& ctx) {
        return Results::Text("slug-" + ctx.Request().RouteValue("slug"));
    }).ExcludeFromDescription();
    app.MapGet("/api/users/{id:guid}", [](HttpContext& ctx) {
        return Results::Text("user-" + ctx.Request().RouteValue("id"));
    });
    app.MapOpenApi();

    HttpResponse integer = Send(app, "GET", "/api/orders/42");
    RequireEqual(200, integer.StatusCode(), "route constraint int status");
    RequireEqual("order-42", integer.BodyText(), "route constraint int body");

    HttpResponse alpha = Send(app, "GET", "/api/orders/alpha");
    RequireEqual(200, alpha.StatusCode(), "route constraint alpha status");
    RequireEqual("slug-alpha", alpha.BodyText(), "route constraint alpha body");

    HttpResponse invalid = Send(app, "GET", "/api/orders/a1");
    RequireEqual(404, invalid.StatusCode(), "route constraint invalid status");

    const std::string guid = "00112233-4455-6677-8899-aabbccddeeff";
    HttpResponse guidResponse = Send(app, "GET", "/api/users/" + guid);
    RequireEqual(200, guidResponse.StatusCode(), "route constraint guid status");
    RequireEqual("user-" + guid, guidResponse.BodyText(), "route constraint guid body");

    HttpResponse invalidGuid = Send(app, "GET", "/api/users/not-a-guid");
    RequireEqual(404, invalidGuid.StatusCode(), "route constraint invalid guid status");

    HttpResponse openApi = Send(app, "GET", "/openapi.json");
    JsonElement root = ParseJsonBody(openApi);
    JsonElement paths = root.GetProperty("paths");
    Require(paths.HasProperty("/api/orders/{id}"), "OpenAPI should remove route constraint from path template");
    Require(!paths.HasProperty("/api/orders/{id:int}"), "OpenAPI should not expose route constraint syntax in path template");
    JsonElement parameter = paths.GetProperty("/api/orders/{id}").GetProperty("get").GetProperty("parameters")[0];
    RequireEqual("id", parameter.GetProperty("name").GetString(), "OpenAPI route constraint parameter name");
    RequireEqual("integer", parameter.GetProperty("schema").GetProperty("type").GetString(), "OpenAPI route constraint schema");
    RequireEqual("int32", parameter.GetProperty("schema").GetProperty("format").GetString(), "OpenAPI route constraint schema format");

    JsonElement guidParameter = paths.GetProperty("/api/users/{id}").GetProperty("get").GetProperty("parameters")[0];
    RequireEqual("string", guidParameter.GetProperty("schema").GetProperty("type").GetString(), "OpenAPI guid route constraint schema");
    RequireEqual("uuid", guidParameter.GetProperty("schema").GetProperty("format").GetString(), "OpenAPI guid route constraint format");

    RequireThrowsWithMessage([] {
        WebApplication invalidApp = WebApplication::Create();
        invalidApp.MapGet("/api/bad/{id:decimal}", [] {
            return Results::Ok();
        });
    }, "unsupported constraint", "unsupported route constraint should fail fast");

    RequireThrowsWithMessage([] {
        WebApplication invalidApp = WebApplication::Create();
        invalidApp.MapGet("/api/{id}/{id}", [] {
            return Results::Ok();
        });
    }, "duplicated", "duplicate route parameter names should fail fast");

    RequireThrowsWithMessage([] {
        WebApplication invalidApp = WebApplication::Create();
        invalidApp.MapGet("/files/{*path}/tail", [] {
            return Results::Ok();
        });
    }, "last segment", "catch-all route parameters should be final");

    RequireThrowsWithMessage([] {
        WebApplication invalidApp = WebApplication::Create();
        invalidApp.MapGet("/api/{bad name}", [] {
            return Results::Ok();
        });
    }, "invalid name", "route parameter names should reject spaces");

    RequireThrowsWithMessage([] {
        WebApplication invalidApp = WebApplication::Create();
        invalidApp.MapGet("/api/{id", [] {
            return Results::Ok();
        });
    }, "braces", "unmatched route parameter braces should fail fast");
}

void CookieBindingReadsRequestCookies() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/prefs", Bind(
        [](std::string theme, int visits, std::string missing) {
            JsonObject payload;
            payload.Add("theme", JsonNode::Create(theme));
            payload.Add("visits", JsonNode::Create(visits));
            payload.Add("missing", JsonNode::Create(missing));
            return Results::Json(payload);
        },
        Cookie<std::string>("theme"),
        Cookie<int>("visits"),
        Cookie<std::string>("missing").Default("fallback")));

    HttpRequest request;
    request.Method("GET")
        .Path("/api/prefs")
        .Header("Cookie", "theme=dark%20mode; visits=42");

    HttpResponse response = app.Handle(std::move(request));
    JsonElement body = ParseJsonBody(response);

    RequireEqual(200, response.StatusCode(), "cookie binding status");
    RequireEqual("dark mode", body.GetProperty("theme").GetString(), "cookie binding theme");
    RequireEqual(42, body.GetProperty("visits").GetInt32(), "cookie binding visits");
    RequireEqual("fallback", body.GetProperty("missing").GetString(), "cookie binding default");
}

void FormBindingReadsUrlEncodedBody() {
    WebApplication app = WebApplication::Create();
    app.MapPost("/api/form", Bind(
        [](std::string name, int count, bool remember, std::string missing) {
            JsonObject payload;
            payload.Add("name", JsonNode::Create(name));
            payload.Add("count", JsonNode::Create(count));
            payload.Add("remember", JsonNode::Create(remember));
            payload.Add("missing", JsonNode::Create(missing));
            return Results::Json(payload);
        },
        Form<std::string>("name"),
        Form<int>("count"),
        Form<bool>("remember"),
        Form<std::string>("missing").Default("fallback")));
    app.MapPost("/api/form/raw", [](HttpContext& ctx) {
        return Results::Text(ctx.Request().FormValue("name") + "|" + ctx.Request().FormValue("missing", "fallback"));
    });
    app.MapPost("/api/form/multi", [](HttpContext& ctx) {
        JsonObject payload;
        JsonArray tags;
        for (const std::string& tag : ctx.Request().FormValues("tag")) {
            tags.Add(JsonNode::Create(tag));
        }
        payload.Add("tags", tags);
        payload.Add("single", JsonNode::Create(ctx.Request().FormValue("tag")));
        payload.Add("missingCount", JsonNode::Create(static_cast<int>(ctx.Request().FormValues("missing").size())));
        return Results::Json(payload);
    });
    app.MapPost("/api/form/vector", Bind(
        [](std::vector<std::string> tags, std::vector<int> ids, std::vector<bool> flags) {
            JsonObject payload;
            JsonArray tagArray;
            for (const std::string& tag : tags) {
                tagArray.Add(JsonNode::Create(tag));
            }
            JsonArray idArray;
            for (int id : ids) {
                idArray.Add(JsonNode::Create(id));
            }
            JsonArray flagArray;
            for (bool flag : flags) {
                flagArray.Add(JsonNode::Create(flag));
            }
            payload.Add("tags", tagArray);
            payload.Add("ids", idArray);
            payload.Add("flags", flagArray);
            return Results::Json(payload);
        },
        Form<std::vector<std::string>>("tag"),
        Form<std::vector<int>>("id"),
        Form<std::vector<bool>>("flag")));

    HttpRequest request;
    request.Method("POST")
        .Path("/api/form")
        .Header("Content-Type", "application/x-www-form-urlencoded; charset=utf-8")
        .Body("name=Ada+Lovelace&count=3&remember=on");
    HttpResponse response = app.Handle(std::move(request));
    JsonElement body = ParseJsonBody(response);

    RequireEqual(200, response.StatusCode(), "form binding status");
    RequireEqual("Ada Lovelace", body.GetProperty("name").GetString(), "form binding name");
    RequireEqual(3, body.GetProperty("count").GetInt32(), "form binding count");
    Require(body.GetProperty("remember").GetBoolean(), "form binding remember");
    RequireEqual("fallback", body.GetProperty("missing").GetString(), "form binding default");

    HttpRequest rawRequest;
    rawRequest.Method("POST")
        .Path("/api/form/raw")
        .Header("Content-Type", "application/x-www-form-urlencoded")
        .Body("name=Grace%20Hopper");
    HttpResponse raw = app.Handle(std::move(rawRequest));
    RequireEqual(200, raw.StatusCode(), "form direct accessor status");
    RequireEqual("Grace Hopper|fallback", raw.BodyText(), "form direct accessor body");

    HttpRequest multiRequest;
    multiRequest.Method("POST")
        .Path("/api/form/multi")
        .Header("Content-Type", "application/x-www-form-urlencoded")
        .Body("tag=red&tag=blue&tag=");
    HttpResponse multi = app.Handle(std::move(multiRequest));
    JsonElement multiBody = ParseJsonBody(multi);
    RequireEqual(200, multi.StatusCode(), "form multi-value status");
    RequireEqual(3, multiBody.GetProperty("tags").GetArrayLength(), "form multi-value count");
    RequireEqual("red", multiBody.GetProperty("tags")[0].GetString(), "form first value");
    RequireEqual("blue", multiBody.GetProperty("tags")[1].GetString(), "form second value");
    RequireEqual("", multiBody.GetProperty("tags")[2].GetString(), "form empty repeated value");
    RequireEqual("", multiBody.GetProperty("single").GetString(), "form single accessor should keep last value");
    RequireEqual(0, multiBody.GetProperty("missingCount").GetInt32(), "form missing multi-value count");

    HttpRequest vectorRequest;
    vectorRequest.Method("POST")
        .Path("/api/form/vector")
        .Header("Content-Type", "application/x-www-form-urlencoded")
        .Body("tag=red&tag=blue&id=10&id=20&flag=true&flag=off");
    HttpResponse vectorResponse = app.Handle(std::move(vectorRequest));
    JsonElement vectorBody = ParseJsonBody(vectorResponse);
    RequireEqual(200, vectorResponse.StatusCode(), "form vector binding status");
    RequireEqual(2, vectorBody.GetProperty("tags").GetArrayLength(), "form vector tags count");
    RequireEqual("red", vectorBody.GetProperty("tags")[0].GetString(), "form vector first tag");
    RequireEqual("blue", vectorBody.GetProperty("tags")[1].GetString(), "form vector second tag");
    RequireEqual(2, vectorBody.GetProperty("ids").GetArrayLength(), "form vector ids count");
    RequireEqual(10, vectorBody.GetProperty("ids")[0].GetInt32(), "form vector first id");
    RequireEqual(20, vectorBody.GetProperty("ids")[1].GetInt32(), "form vector second id");
    Require(vectorBody.GetProperty("flags")[0].GetBoolean(), "form vector first bool");
    Require(!vectorBody.GetProperty("flags")[1].GetBoolean(), "form vector second bool");

    HttpRequest wrongTypeRequest;
    wrongTypeRequest.Method("POST")
        .Path("/api/form")
        .Header("Content-Type", "text/plain")
        .Body("name=Ada&count=3&remember=true");
    HttpResponse wrongType = app.Handle(std::move(wrongTypeRequest));
    JsonElement wrongTypeBody = ParseJsonBody(wrongType);
    RequireEqual(415, wrongType.StatusCode(), "form wrong content type status");
    RequireEqual("Unsupported Media Type", wrongTypeBody.GetProperty("title").GetString(), "form wrong content type title");
    Require(wrongTypeBody.GetProperty("detail").GetString().find("application/x-www-form-urlencoded") != std::string::npos,
        "form wrong content type detail");
}

std::string MultipartBodyForTest(std::string_view boundary) {
    std::string delimiter = "--" + std::string(boundary);
    return delimiter + "\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "Quarterly Report\r\n" +
        delimiter + "\r\n"
        "Content-Disposition: form-data; name=\"count\"\r\n"
        "\r\n"
        "3\r\n" +
        delimiter + "\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"report.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "X-Part: yes\r\n"
        "\r\n"
        "hello file\r\n" +
        delimiter + "--\r\n";
}

void MultipartFormDataParsesFieldsAndFiles() {
    WebApplication app = WebApplication::Create();
    app.MapPost("/api/bind", Bind(
        [](std::string title, int count) {
            JsonObject payload;
            payload.Add("title", JsonNode::Create(title));
            payload.Add("count", JsonNode::Create(count));
            return Results::Json(payload);
        },
        Form<std::string>("title"),
        Form<int>("count")));
    app.MapPost("/api/upload", [](HttpContext& ctx) {
        const HttpRequest& request = ctx.Request();
        const MultipartFile* file = request.File("upload");
        JsonObject payload;
        payload.Add("title", JsonNode::Create(request.FormValue("title")));
        payload.Add("count", JsonNode::Create(request.FormValue("count")));
        payload.Add("fileCount", JsonNode::Create(static_cast<int>(request.Files().size())));
        payload.Add("fileName", JsonNode::Create(file == nullptr ? "" : file->FileName));
        payload.Add("fileContentType", JsonNode::Create(file == nullptr ? "" : file->ContentType));
        payload.Add("fileHeader", JsonNode::Create(file == nullptr ? "" : file->Headers.Get("X-Part")));
        payload.Add("fileBody", JsonNode::Create(file == nullptr ? "" : file->Body));
        return Results::Json(payload);
    });

    const std::string boundary = "----CapiWebBoundary";

    HttpRequest bindRequest;
    bindRequest.Method("POST")
        .Path("/api/bind")
        .Header("Content-Type", "multipart/form-data; boundary=\"" + boundary + "\"")
        .Body(MultipartBodyForTest(boundary));
    HttpResponse bindResponse = app.Handle(std::move(bindRequest));
    JsonElement bindBody = ParseJsonBody(bindResponse);
    RequireEqual(200, bindResponse.StatusCode(), "multipart form binding status");
    RequireEqual("Quarterly Report", bindBody.GetProperty("title").GetString(), "multipart form binding title");
    RequireEqual(3, bindBody.GetProperty("count").GetInt32(), "multipart form binding count");

    HttpRequest uploadRequest;
    uploadRequest.Method("POST")
        .Path("/api/upload")
        .Header("Content-Type", "multipart/form-data; boundary=" + boundary)
        .Body(MultipartBodyForTest(boundary));
    HttpResponse upload = app.Handle(std::move(uploadRequest));
    JsonElement uploadBody = ParseJsonBody(upload);
    RequireEqual(200, upload.StatusCode(), "multipart upload status");
    RequireEqual("Quarterly Report", uploadBody.GetProperty("title").GetString(), "multipart upload title");
    RequireEqual("3", uploadBody.GetProperty("count").GetString(), "multipart upload count");
    RequireEqual(1, uploadBody.GetProperty("fileCount").GetInt32(), "multipart upload file count");
    RequireEqual("report.txt", uploadBody.GetProperty("fileName").GetString(), "multipart upload file name");
    RequireEqual("text/plain", uploadBody.GetProperty("fileContentType").GetString(), "multipart upload file content type");
    RequireEqual("yes", uploadBody.GetProperty("fileHeader").GetString(), "multipart upload custom part header");
    RequireEqual("hello file", uploadBody.GetProperty("fileBody").GetString(), "multipart upload file body");

    HttpRequest missingBoundaryRequest;
    missingBoundaryRequest.Method("POST")
        .Path("/api/upload")
        .Header("Content-Type", "multipart/form-data")
        .Body(MultipartBodyForTest(boundary));
    HttpResponse missingBoundary = app.Handle(std::move(missingBoundaryRequest));
    JsonElement missingBoundaryBody = ParseJsonBody(missingBoundary);
    RequireEqual(400, missingBoundary.StatusCode(), "multipart missing boundary status");
    RequireEqual("Bad Request", missingBoundaryBody.GetProperty("title").GetString(), "multipart missing boundary title");
    Require(missingBoundaryBody.GetProperty("detail").GetString().find("boundary") != std::string::npos,
        "multipart missing boundary detail");
}

std::string MultipartTwoFilesBodyForTest(std::string_view boundary) {
    std::string delimiter = "--" + std::string(boundary);
    return delimiter + "\r\n"
        "Content-Disposition: form-data; name=\"first\"; filename=\"one.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "one\r\n" +
        delimiter + "\r\n"
        "Content-Disposition: form-data; name=\"second\"; filename=\"two.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "two\r\n" +
        delimiter + "--\r\n";
}

void FormOptionsCanBeConfiguredAndLimitParsing() {
    WebApplicationBuilder configuredBuilder = WebApplication::CreateBuilder();
    configuredBuilder.ConfigureFromJson(R"json({
  "FormOptions": {
    "ValueCountLimit": 2,
    "KeyLengthLimit": 16,
    "ValueLengthLimit": 32,
    "MultipartBoundaryLengthLimit": 64,
    "MultipartFileCountLimit": 3,
    "MultipartHeadersCountLimit": 4,
    "MultipartHeadersLengthLimit": 256,
    "MultipartBodyLengthLimit": 1024
  }
})json");
    WebApplication configured = configuredBuilder.Build();
    const FormOptions& forms = configured.Options().Forms;
    RequireEqual(2, static_cast<int>(forms.ValueCountLimit), "json form value count limit");
    RequireEqual(16, static_cast<int>(forms.KeyLengthLimit), "json form key length limit");
    RequireEqual(32, static_cast<int>(forms.ValueLengthLimit), "json form value length limit");
    RequireEqual(64, static_cast<int>(forms.MultipartBoundaryLengthLimit), "json multipart boundary limit");
    RequireEqual(3, static_cast<int>(forms.MultipartFileCountLimit), "json multipart file count limit");
    RequireEqual(4, static_cast<int>(forms.MultipartHeadersCountLimit), "json multipart header count limit");
    RequireEqual(256, static_cast<int>(forms.MultipartHeadersLengthLimit), "json multipart header bytes limit");
    RequireEqual(1024, static_cast<int>(forms.MultipartBodyLengthLimit), "json multipart body limit");

    WebApplicationBuilder urlBuilder = WebApplication::CreateBuilder();
    urlBuilder.ConfigureFormOptions([](FormOptions& options) {
        options.ValueCountLimit = 1;
    });
    WebApplication urlApp = urlBuilder.Build();
    urlApp.MapPost("/api/form", [](HttpContext& ctx) {
        return Results::Text(ctx.Request().FormValue("a"));
    });

    HttpRequest tooManyValues;
    tooManyValues.Method("POST")
        .Path("/api/form")
        .Header("Content-Type", "application/x-www-form-urlencoded")
        .Body("a=1&b=2");
    HttpResponse tooManyValuesResponse = urlApp.Handle(std::move(tooManyValues));
    JsonElement tooManyValuesBody = ParseJsonBody(tooManyValuesResponse);
    RequireEqual(400, tooManyValuesResponse.StatusCode(), "form value count limit status");
    Require(tooManyValuesBody.GetProperty("detail").GetString().find("ValueCountLimit") != std::string::npos,
        "form value count limit detail");

    WebApplicationBuilder multipartBuilder = WebApplication::CreateBuilder();
    multipartBuilder.ConfigureFormOptions([](FormOptions& options) {
        options.MultipartHeadersCountLimit = 1;
    });
    WebApplication multipartApp = multipartBuilder.Build();
    multipartApp.MapPost("/api/upload", [](HttpContext& ctx) {
        return Results::Text(ctx.Request().FormValue("title"));
    });

    const std::string boundary = "----CapiWebBoundary";
    HttpRequest tooManyHeaders;
    tooManyHeaders.Method("POST")
        .Path("/api/upload")
        .Header("Content-Type", "multipart/form-data; boundary=" + boundary)
        .Body(MultipartBodyForTest(boundary));
    HttpResponse tooManyHeadersResponse = multipartApp.Handle(std::move(tooManyHeaders));
    JsonElement tooManyHeadersBody = ParseJsonBody(tooManyHeadersResponse);
    RequireEqual(400, tooManyHeadersResponse.StatusCode(), "multipart header count limit status");
    Require(tooManyHeadersBody.GetProperty("detail").GetString().find("MultipartHeadersCountLimit") != std::string::npos,
        "multipart header count limit detail");

    WebApplicationBuilder fileBuilder = WebApplication::CreateBuilder();
    fileBuilder.ConfigureFormOptions([](FormOptions& options) {
        options.MultipartFileCountLimit = 1;
    });
    WebApplication fileApp = fileBuilder.Build();
    fileApp.MapPost("/api/files", [](HttpContext& ctx) {
        return Results::Text(std::to_string(ctx.Request().Files().size()));
    });

    HttpRequest tooManyFiles;
    tooManyFiles.Method("POST")
        .Path("/api/files")
        .Header("Content-Type", "multipart/form-data; boundary=" + boundary)
        .Body(MultipartTwoFilesBodyForTest(boundary));
    HttpResponse tooManyFilesResponse = fileApp.Handle(std::move(tooManyFiles));
    JsonElement tooManyFilesBody = ParseJsonBody(tooManyFilesResponse);
    RequireEqual(400, tooManyFilesResponse.StatusCode(), "multipart file count limit status");
    Require(tooManyFilesBody.GetProperty("detail").GetString().find("MultipartFileCountLimit") != std::string::npos,
        "multipart file count limit detail");
}

void JsonBodyBindingReadsBodyAndProperties() {
    WebApplication app = WebApplication::Create();
    app.MapPost("/api/body", Bind(
        [](JsonElement body) {
            JsonObject payload;
            payload.Add("name", JsonNode::Create(body.GetProperty("name").GetString()));
            payload.Add("kind", JsonNode::Create(body.ValueKind() == JsonValueKind::Object ? "object" : "other"));
            return Results::Json(payload);
        },
        Body<JsonElement>()));
    app.MapPost("/api/sum", Bind(
        [](int a, int b, std::optional<std::string> label) {
            JsonObject payload;
            payload.Add("sum", JsonNode::Create(a + b));
            payload.Add("hasLabel", JsonNode::Create(label.has_value()));
            return Results::Json(payload);
        },
        JsonProperty<int>("a"),
        JsonProperty<int>("b"),
        JsonProperty<std::optional<std::string>>("label")));
    app.MapPost("/api/body/vector", Bind(
        [](std::vector<int> values) {
            JsonObject payload;
            JsonArray items;
            for (int value : values) {
                items.Add(JsonNode::Create(value));
            }
            payload.Add("values", items);
            return Results::Json(payload);
        },
        Body<std::vector<int>>()));
    app.MapPost("/api/property/vector", Bind(
        [](std::vector<std::string> tags) {
            JsonObject payload;
            JsonArray items;
            for (const std::string& tag : tags) {
                items.Add(JsonNode::Create(tag));
            }
            payload.Add("tags", items);
            return Results::Json(payload);
        },
        JsonProperty<std::vector<std::string>>("tags")));

    HttpRequest vendorJsonRequest;
    vendorJsonRequest.Method("POST")
        .Path("/api/body")
        .Header("Content-Type", "application/vnd.capi.test+json; charset=utf-8")
        .Body(R"json({"name":"Ada"})json");
    HttpResponse vendorJson = app.Handle(std::move(vendorJsonRequest));
    JsonElement vendorJsonBody = ParseJsonBody(vendorJson);
    RequireEqual(200, vendorJson.StatusCode(), "json body binding status");
    RequireEqual("Ada", vendorJsonBody.GetProperty("name").GetString(), "json body binding name");
    RequireEqual("object", vendorJsonBody.GetProperty("kind").GetString(), "json body binding kind");

    HttpRequest sumRequest;
    sumRequest.Method("POST")
        .Path("/api/sum")
        .Header("Content-Type", "application/json")
        .Body(R"json({"a":2,"b":5})json");
    HttpResponse sum = app.Handle(std::move(sumRequest));
    JsonElement sumBody = ParseJsonBody(sum);
    RequireEqual(200, sum.StatusCode(), "json property binding status");
    RequireEqual(7, sumBody.GetProperty("sum").GetInt32(), "json property binding sum");
    Require(!sumBody.GetProperty("hasLabel").GetBoolean(), "json property optional missing");

    HttpRequest bodyVectorRequest;
    bodyVectorRequest.Method("POST")
        .Path("/api/body/vector")
        .Header("Content-Type", "application/json")
        .Body(R"json([1,2,3])json");
    HttpResponse bodyVector = app.Handle(std::move(bodyVectorRequest));
    JsonElement bodyVectorBody = ParseJsonBody(bodyVector);
    RequireEqual(200, bodyVector.StatusCode(), "json body vector binding status");
    RequireEqual(3, bodyVectorBody.GetProperty("values").GetArrayLength(), "json body vector count");
    RequireEqual(1, bodyVectorBody.GetProperty("values")[0].GetInt32(), "json body vector first value");
    RequireEqual(3, bodyVectorBody.GetProperty("values")[2].GetInt32(), "json body vector third value");

    HttpRequest propertyVectorRequest;
    propertyVectorRequest.Method("POST")
        .Path("/api/property/vector")
        .Header("Content-Type", "application/json")
        .Body(R"json({"tags":["red","blue"]})json");
    HttpResponse propertyVector = app.Handle(std::move(propertyVectorRequest));
    JsonElement propertyVectorBody = ParseJsonBody(propertyVector);
    RequireEqual(200, propertyVector.StatusCode(), "json property vector binding status");
    RequireEqual(2, propertyVectorBody.GetProperty("tags").GetArrayLength(), "json property vector count");
    RequireEqual("red", propertyVectorBody.GetProperty("tags")[0].GetString(), "json property vector first tag");
    RequireEqual("blue", propertyVectorBody.GetProperty("tags")[1].GetString(), "json property vector second tag");

    HttpRequest invalidVectorRequest;
    invalidVectorRequest.Method("POST")
        .Path("/api/body/vector")
        .Header("Content-Type", "application/json")
        .Body(R"json({"not":"array"})json");
    HttpResponse invalidVector = app.Handle(std::move(invalidVectorRequest));
    JsonElement invalidVectorBody = ParseJsonBody(invalidVector);
    RequireEqual(400, invalidVector.StatusCode(), "json body vector object status");
    RequireEqual("Bad Request", invalidVectorBody.GetProperty("title").GetString(), "json body vector object title");

    HttpRequest missingTypeRequest;
    missingTypeRequest.Method("POST")
        .Path("/api/sum")
        .Body(R"json({"a":2,"b":5})json");
    HttpResponse missingType = app.Handle(std::move(missingTypeRequest));
    JsonElement missingTypeBody = ParseJsonBody(missingType);
    RequireEqual(415, missingType.StatusCode(), "json binding missing content type status");
    RequireEqual("Unsupported Media Type", missingTypeBody.GetProperty("title").GetString(), "json binding missing content type title");
    Require(missingTypeBody.GetProperty("detail").GetString().find("application/json") != std::string::npos,
        "json binding missing content type detail");

    HttpRequest unsupportedTypeRequest;
    unsupportedTypeRequest.Method("POST")
        .Path("/api/sum")
        .Header("Content-Type", "text/plain; note=+json")
        .Body(R"json({"a":2,"b":5})json");
    HttpResponse unsupportedType = app.Handle(std::move(unsupportedTypeRequest));
    JsonElement unsupportedTypeBody = ParseJsonBody(unsupportedType);
    RequireEqual(415, unsupportedType.StatusCode(), "json binding unsupported content type status");
    Require(unsupportedTypeBody.GetProperty("detail").GetString().find("text/plain") != std::string::npos,
        "json binding unsupported content type detail");

    HttpRequest invalidJsonRequest;
    invalidJsonRequest.Method("POST")
        .Path("/api/sum")
        .Header("Content-Type", "application/json")
        .Body("{");
    HttpResponse invalidJson = app.Handle(std::move(invalidJsonRequest));
    JsonElement invalidJsonBody = ParseJsonBody(invalidJson);
    RequireEqual(400, invalidJson.StatusCode(), "json binding invalid json status");
    RequireEqual("Bad Request", invalidJsonBody.GetProperty("title").GetString(), "json binding invalid json title");
}

void RequestDecompressionWorksInMemory() {
    WebApplication app = WebApplication::Create();

    RequestDecompressionOptions decompression;
    decompression.MaxDecompressedBodyBytes = 128;
    app.UseRequestDecompression(decompression);

    int sumCalls = 0;
    app.MapPost("/api/sum", Bind(
        [&](int a, int b) {
            ++sumCalls;
            return a + b;
        },
        JsonProperty<int>("a"),
        JsonProperty<int>("b")));
    app.MapPost("/api/echo", [](HttpContext& ctx) {
        const HttpRequest& request = ctx.Request();
        JsonObject payload;
        payload.Add("body", JsonNode::Create(request.Body()));
        payload.Add("encoding", JsonNode::Create(request.Header("Content-Encoding", "<none>")));
        payload.Add("length", JsonNode::Create(request.Header("Content-Length")));
        return Results::Json(payload);
    });

    const std::string json = R"json({"a":4,"b":8})json";
    std::string gzip = GzipForTest(json);
    HttpRequest gzipRequest;
    gzipRequest.Method("POST")
        .Path("/api/sum")
        .Header("Content-Type", "application/json")
        .Header("Content-Encoding", "gzip")
        .Header("Content-Length", std::to_string(gzip.size()))
        .Body(std::move(gzip));
    HttpResponse gzipResponse = app.Handle(std::move(gzipRequest));
    RequireEqual(200, gzipResponse.StatusCode(), "gzip request decompression status");
    RequireEqual("12", gzipResponse.BodyText(), "gzip request decompression body");
    RequireEqual(1, sumCalls, "gzip request decompression should call handler");

    const std::string text = "hello deflate";
    std::string deflated = ZlibDeflateForTest(text);
    HttpRequest deflateRequest;
    deflateRequest.Method("POST")
        .Path("/api/echo")
        .Header("Content-Encoding", "deflate")
        .Header("Content-Length", std::to_string(deflated.size()))
        .Body(std::move(deflated));
    HttpResponse deflateResponse = app.Handle(std::move(deflateRequest));
    JsonElement deflateBody = ParseJsonBody(deflateResponse);
    RequireEqual(200, deflateResponse.StatusCode(), "deflate request decompression status");
    RequireEqual(text, deflateBody.GetProperty("body").GetString(), "deflate request decompression body");
    RequireEqual("<none>", deflateBody.GetProperty("encoding").GetString(), "request decompression should remove content encoding");
    RequireEqual(std::to_string(text.size()), deflateBody.GetProperty("length").GetString(), "request decompression should update content length");

    HttpRequest unsupportedRequest;
    unsupportedRequest.Method("POST")
        .Path("/api/sum")
        .Header("Content-Type", "application/json")
        .Header("Content-Encoding", "br")
        .Body(json);
    HttpResponse unsupported = app.Handle(std::move(unsupportedRequest));
    JsonElement unsupportedBody = ParseJsonBody(unsupported);
    RequireEqual(415, unsupported.StatusCode(), "unsupported request content encoding status");
    RequireEqual("Unsupported Media Type", unsupportedBody.GetProperty("title").GetString(), "unsupported request content encoding title");
    RequireEqual(1, sumCalls, "unsupported request content encoding should not call handler");

    HttpRequest invalidRequest;
    invalidRequest.Method("POST")
        .Path("/api/sum")
        .Header("Content-Type", "application/json")
        .Header("Content-Encoding", "gzip")
        .Body("not gzip");
    HttpResponse invalid = app.Handle(std::move(invalidRequest));
    JsonElement invalidBody = ParseJsonBody(invalid);
    RequireEqual(400, invalid.StatusCode(), "invalid compressed request status");
    RequireEqual("Bad Request", invalidBody.GetProperty("title").GetString(), "invalid compressed request title");

    WebApplication limited = WebApplication::Create();
    RequestDecompressionOptions limitedOptions;
    limitedOptions.MaxDecompressedBodyBytes = 4;
    limited.UseRequestDecompression(limitedOptions);
    limited.MapPost("/api/echo", [](HttpContext& ctx) {
        return Results::Text(ctx.Request().Body());
    });

    std::string tooLargeBody = GzipForTest("12345");
    HttpRequest tooLargeRequest;
    tooLargeRequest.Method("POST")
        .Path("/api/echo")
        .Header("Content-Encoding", "gzip")
        .Body(std::move(tooLargeBody));
    HttpResponse tooLarge = limited.Handle(std::move(tooLargeRequest));
    JsonElement tooLargeProblem = ParseJsonBody(tooLarge);
    RequireEqual(413, tooLarge.StatusCode(), "too large decompressed request status");
    RequireEqual("Payload Too Large", tooLargeProblem.GetProperty("title").GetString(), "too large decompressed request title");
}

void ParameterBindingFailuresReturnBadRequest() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/bind/{id}", Bind(
        [](int id, int multiplier) {
            return Results::Text(std::to_string(id * multiplier));
        },
        Route<int>("id"),
        Query<int>("multiplier")));

    HttpResponse missing = Send(app, "GET", "/api/bind/7");
    JsonElement missingBody = ParseJsonBody(missing);
    RequireEqual(400, missing.StatusCode(), "missing binding status");
    RequireEqual("application/problem+json; charset=utf-8", missing.ContentType(), "missing binding content type");
    RequireEqual("Bad Request", missingBody.GetProperty("title").GetString(), "missing binding title");
    RequireEqual(400, missingBody.GetProperty("status").GetInt32(), "missing binding body status");
    RequireEqual("Missing required query parameter 'multiplier'.", missingBody.GetProperty("detail").GetString(), "missing binding detail");

    HttpResponse invalid = Send(app, "GET", "/api/bind/7", "?multiplier=not-an-int");
    JsonElement invalidBody = ParseJsonBody(invalid);
    RequireEqual(400, invalid.StatusCode(), "invalid binding status");
    RequireEqual("Bad Request", invalidBody.GetProperty("title").GetString(), "invalid binding title");
    RequireEqual(400, invalidBody.GetProperty("status").GetInt32(), "invalid binding body status");
    Require(invalidBody.GetProperty("detail").GetString().find("multiplier") != std::string::npos, "invalid binding detail should name parameter");
}

void ProblemDetailsUsesConfiguredDefaults() {
    WebApplicationBuilder builder = WebApplication::CreateBuilder();
    builder.ConfigureFromJson(R"json({
        "ProblemDetails": {
            "DefaultType": "https://example.test/problems",
            "IncludeTraceIdentifier": false
        }
    })json");

    WebApplication app = builder.Build();
    app.UseProblemDetails();
    app.MapGet("/missing", [](HttpContext&) {
        return Results::StatusCode(404);
    });

    HttpContext context;
    context.TraceIdentifier("trace-from-test");
    context.Request().Method("GET").Path("/missing");
    HttpResponse response = app.Handle(context);
    JsonElement body = ParseJsonBody(response);

    RequireEqual(404, response.StatusCode(), "problem status");
    RequireEqual("application/problem+json; charset=utf-8", response.ContentType(), "problem content type");
    RequireEqual("https://example.test/problems", body.GetProperty("type").GetString(), "problem type");
    RequireEqual("Not Found", body.GetProperty("title").GetString(), "problem title");
    RequireEqual(404, body.GetProperty("status").GetInt32(), "problem body status");
    RequireEqual("/missing", body.GetProperty("instance").GetString(), "problem instance");
    Require(!body.HasProperty("traceId"), "problem traceId should be disabled by configuration");
}

void ProblemDetailsOptionsValidateUnsafeConfiguration() {
    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureProblemDetails([](ProblemDetailsOptions& options) {
            options.IncludeTraceIdentifier = true;
            options.TraceIdentifierExtensionName.clear();
        });
        (void)builder.Build();
    }, "TraceIdentifierExtensionName", "problem details trace extension name should be required when enabled");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureProblemDetails([](ProblemDetailsOptions& options) {
            options.TraceIdentifierExtensionName = "status";
        });
        (void)builder.Build();
    }, "reserved", "problem details trace extension name should reject reserved properties");

    RequireThrowsWithMessage([] {
        ProblemDetailsOptions options;
        options.DefaultType = "https://example.test/problems\r\nX-Test: bad";
        WebApplication app = WebApplication::Create();
        app.UseProblemDetails(options);
    }, "control", "problem details default type should reject control characters");
}

void BuilderAndMiddlewareRejectEmptyCallbacks() {
    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        std::function<void(HttpSysOptions&)> configure;
        builder.ConfigureHttpSys(std::move(configure));
    }, "ConfigureHttpSys", "ConfigureHttpSys should reject empty callbacks");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        std::function<void(ProblemDetailsOptions&)> configure;
        builder.ConfigureProblemDetails(std::move(configure));
    }, "ConfigureProblemDetails", "ConfigureProblemDetails should reject empty callbacks");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        std::function<void(FormOptions&)> configure;
        builder.ConfigureFormOptions(std::move(configure));
    }, "ConfigureFormOptions", "ConfigureFormOptions should reject empty callbacks");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        Middleware middleware;
        app.Use(std::move(middleware));
    }, "Middleware", "Use should reject empty middleware");
}

void BuilderRejectsInvalidHttpSysOptions() {
    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureHttpSys([](HttpSysOptions& http) {
            http.UrlPrefixes.clear();
        });
        (void)builder.Build();
    }, "UrlPrefixes", "empty URL prefixes should fail");

    RequireThrowsWithMessage([] {
        WebApplicationOptions options;
        options.HttpSys.UrlPrefixes = { "ftp://localhost:8080/" };
        (void)WebApplication(std::move(options));
    }, "scheme", "invalid URL scheme should fail");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureHttpSys([](HttpSysOptions& http) {
            http.UrlPrefixes = { "http://localhost:8080", "HTTP://LOCALHOST:8080/" };
        });
        (void)builder.Build();
    }, "duplicate", "duplicate URL prefixes should fail");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureHttpSys([](HttpSysOptions& http) {
            http.ServerHeader = "bad\r\nheader";
        });
        (void)builder.Build();
    }, "ServerHeader", "invalid server header should fail");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureHttpSys([](HttpSysOptions& http) {
            http.Backpressure.OverloadedStatusCode = 200;
        });
        (void)builder.Build();
    }, "OverloadedStatusCode", "backpressure overload status should require error status codes");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureHttpSys([](HttpSysOptions& http) {
            http.Backpressure.StoppingRetryAfterSeconds = -1;
        });
        (void)builder.Build();
    }, "StoppingRetryAfterSeconds", "backpressure retry-after should reject negative values");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureHttpSys([](HttpSysOptions& http) {
            http.Backpressure.StoppingDetail = "stopping\r\nX-Test: bad";
        });
        (void)builder.Build();
    }, "StoppingDetail", "backpressure detail should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplicationOptions options;
        SslCertificateBinding binding;
        binding.CertificateHashHex = "not-hex";
        options.HttpSys.SslCertificateBindings.push_back(std::move(binding));
        (void)WebApplication(std::move(options));
    }, "CertificateHashHex", "invalid SSL hash should fail");

    RequireThrowsWithMessage([] {
        WebApplicationOptions options;
        SslCertificateBinding first;
        first.Port = 8443;
        first.CertificateHashHex = "00112233445566778899aabbccddeeff00112233";
        SslCertificateBinding second = first;
        second.CertificateHashHex = "112233445566778899aabbccddeeff0011223344";
        options.HttpSys.SslCertificateBindings = { first, second };
        (void)WebApplication(std::move(options));
    }, "duplicate IP:port", "duplicate SSL bindings should fail before touching machine configuration");

    RequireThrowsWithMessage([] {
        WebApplicationOptions options;
        options.HttpSys.UrlReservation.Mode = UrlAclMode::Delete;
        SslCertificateBinding binding;
        binding.Port = 8443;
        binding.CertificateHashHex = "00112233445566778899aabbccddeeff00112233";
        options.HttpSys.SslCertificateBindings.push_back(std::move(binding));
        (void)WebApplication(std::move(options));
    }, "Delete mode", "URL ACL delete mode should reject SSL ensure operations");

    RequireThrowsWithMessage([] {
        WebApplicationOptions options;
        options.HttpSys.UrlReservation.Mode = UrlAclMode::Ensure;
        SslCertificateBinding binding;
        binding.Mode = MachineConfigMode::Delete;
        binding.Port = 8443;
        options.HttpSys.SslCertificateBindings.push_back(std::move(binding));
        (void)WebApplication(std::move(options));
    }, "Delete mode", "SSL delete mode should reject URL ACL ensure operations");

    RequireThrowsWithMessage([] {
        WebApplicationOptions options;
        SslCertificateBinding remove;
        remove.Mode = MachineConfigMode::Delete;
        remove.Port = 8443;
        SslCertificateBinding ensure;
        ensure.Port = 9443;
        ensure.CertificateHashHex = "00112233445566778899aabbccddeeff00112233";
        options.HttpSys.SslCertificateBindings = { remove, ensure };
        (void)WebApplication(std::move(options));
    }, "Delete mode", "SSL delete mode should reject mixed ensure bindings");
}

void HttpSysBackpressureOptionsCanBeConfiguredFromJson() {
    TemporaryDirectory root;
    std::filesystem::path configPath = root.Path / "appsettings.json";
    {
        std::ofstream file(configPath, std::ios::binary);
        file << R"json({
  "HttpSys": {
    "MaxRequestUrlBytes": 4096,
    "MaxRequestHeaderBytes": 8192,
    "Backpressure": {
      "StoppingStatusCode": 503,
      "StoppingDetail": "Draining instance.",
      "StoppingRetryAfterSeconds": 15,
      "OverloadedStatusCode": 429,
      "OverloadedDetail": "Too many in-flight requests.",
      "OverloadedRetryAfterSeconds": 3
    }
  }
})json";
    }

    WebApplicationBuilder builder = WebApplication::CreateBuilder();
    builder.ConfigureFromJsonFile(configPath);
    WebApplication app = builder.Build();
    const HttpSysBackpressureOptions& backpressure = app.Options().HttpSys.Backpressure;

    RequireEqual(4096, static_cast<int>(app.Options().HttpSys.MaxRequestUrlBytes), "json max request URL bytes");
    RequireEqual(8192, static_cast<int>(app.Options().HttpSys.MaxRequestHeaderBytes), "json max request header bytes");
    RequireEqual(503, backpressure.StoppingStatusCode, "json backpressure stopping status");
    RequireEqual("Draining instance.", backpressure.StoppingDetail, "json backpressure stopping detail");
    RequireEqual(15, backpressure.StoppingRetryAfterSeconds, "json backpressure stopping retry-after");
    RequireEqual(429, backpressure.OverloadedStatusCode, "json backpressure overload status");
    RequireEqual("Too many in-flight requests.", backpressure.OverloadedDetail, "json backpressure overload detail");
    RequireEqual(3, backpressure.OverloadedRetryAfterSeconds, "json backpressure overload retry-after");
}

void JsonConfigurationRejectsInvalidKnownValueTypes() {
    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureFromJson(R"json({
  "HttpSys": {
    "MaxConcurrentRequests": "many"
  }
})json");
    }, "maxConcurrentRequests must be an integer value", "json integer options should reject invalid text");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureFromJson(R"json({
  "DetailedErrors": "sometimes"
})json");
    }, "detailedErrors must be a boolean value", "json boolean options should reject unknown text");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureFromJson(R"json({
  "HttpSys": {
    "UrlPrefixes": [ "http://localhost:8080/", { "url": "http://localhost:8081/" } ]
  }
})json");
    }, "urlPrefixes[1] must be a string value", "json string list options should reject object entries");

    RequireThrowsWithMessage([] {
        WebApplicationBuilder builder = WebApplication::CreateBuilder();
        builder.ConfigureFromJson(R"json({
  "HttpSys": {
    "StopTimeoutMs": "slow"
  }
})json");
    }, "stopTimeoutMs must be an integer value", "json duration options should reject invalid text");
}

void SystemExceptionsIncludeOperationAndErrorCodes() {
    HttpSysException http("HttpCreateRequestQueue", 5);
    std::string httpMessage = http.what();
    RequireEqual("HttpCreateRequestQueue", http.Operation(), "HTTP.sys exception operation");
    RequireEqual(5, static_cast<int>(http.ErrorCode()), "HTTP.sys exception error code");
    Require(httpMessage.find("HttpCreateRequestQueue failed:") != std::string::npos,
        "HTTP.sys exception should include operation");
    Require(httpMessage.find("(5, 0x00000005)") != std::string::npos,
        "HTTP.sys exception should include decimal and hex error codes");

    WindowsServiceException service("OpenSCManagerW", 5);
    std::string serviceMessage = service.what();
    RequireEqual("OpenSCManagerW", service.Operation(), "Windows service exception operation");
    RequireEqual(5, static_cast<int>(service.ErrorCode()), "Windows service exception error code");
    Require(serviceMessage.find("OpenSCManagerW failed:") != std::string::npos,
        "Windows service exception should include operation");
    Require(serviceMessage.find("(5, 0x00000005)") != std::string::npos,
        "Windows service exception should include decimal and hex error codes");
}

void WindowsServiceOptionsValidateBeforeScmAccess() {
    RequireThrowsWithMessage([] {
        WindowsServiceHost::Start(L"Bad\\Service");
    }, "slash", "Windows service names should reject backslashes before SCM access");

    RequireThrowsWithMessage([] {
        WindowsServiceHost::Stop(L"CapiSample", std::chrono::seconds(-1));
    }, "cannot be negative", "Windows service stop timeout should reject negative values before SCM access");

    RequireThrowsWithMessage([] {
        WindowsServiceHost::Uninstall(L"CapiSample", std::chrono::seconds(-1));
    }, "cannot be negative", "Windows service uninstall timeout should reject negative values before SCM access");

    RequireThrowsWithMessage([] {
        WindowsServiceOptions options;
        options.ServiceName = L"   ";
        WindowsServiceHost::Install(options, L"C:\\CapiSample.exe");
    }, "empty", "Windows service options should reject whitespace-only service names before SCM access");

    RequireThrowsWithMessage([] {
        WindowsServiceOptions options;
        options.ServiceName = L"CapiSample";
        options.DisplayName = std::wstring(L"Capi\0Sample", 11);
        WindowsServiceHost::Install(options, L"C:\\CapiSample.exe");
    }, "DisplayName", "Windows service options should reject embedded NUL display names before SCM access");

    RequireThrowsWithMessage([] {
        WindowsServiceOptions options;
        options.ServiceName = L"CapiSample";
        options.StartMode = static_cast<WindowsServiceStartMode>(99);
        WindowsServiceHost::Install(options, L"C:\\CapiSample.exe");
    }, "StartMode", "Windows service options should reject invalid start modes before SCM access");

    RequireThrowsWithMessage([] {
        WindowsServiceOptions options;
        options.ServiceName = L"CapiSample";
        options.Dependencies.push_back(L"");
        WindowsServiceHost::Install(options, L"C:\\CapiSample.exe");
    }, "Dependencies[0]", "Windows service options should reject empty dependencies before SCM access");

    RequireThrowsWithMessage([] {
        WindowsServiceOptions options;
        options.ServiceName = L"CapiSample";
        options.Arguments.push_back(std::wstring(L"--mode\0service", 14));
        WindowsServiceHost::Install(options, L"C:\\CapiSample.exe");
    }, "Arguments[0]", "Windows service options should reject embedded NUL arguments before SCM access");
}

void MachineConfigDeleteModesNeedOnlyTargetIdentity() {
    WebApplicationOptions options;
    options.HttpSys.UrlReservation.Mode = UrlAclMode::Delete;
    options.HttpSys.UrlReservation.SecurityDescriptor.clear();

    SslCertificateBinding binding;
    binding.Mode = MachineConfigMode::Delete;
    binding.Port = 8443;
    binding.CertificateHashHex.clear();
    binding.AppId.clear();
    options.HttpSys.SslCertificateBindings.push_back(std::move(binding));

    WebApplication app(std::move(options));
    Require(!app.IsRunning(), "delete-mode app should not be running before Start");
}

void ConfigurationChangesAreRejectedAfterStart() {
    WebApplicationOptions options;
    options.HttpSys.UrlReservation.Mode = UrlAclMode::Delete;
    options.HttpSys.UrlReservation.IgnoreAccessDenied = true;
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    options.HttpSys.UrlPrefixes = {
        "http://localhost:65000/capiweb-freeze-" + std::to_string(stamp) + "/"
    };

    WebApplication app(std::move(options));
    app.MapGet("/ready", [] {
        return Results::Ok("ready");
    }).WithName("Ready");
    auto api = app.MapGroup("/api");

    app.Start();
    Require(!app.IsRunning(), "delete-mode app should not start a listener");

    HttpResponse ready = Send(app, "GET", "/ready");
    RequireEqual(200, ready.StatusCode(), "configuration lock should not break in-memory handling");
    RequireEqual("ready", ready.BodyText(), "configuration lock should keep existing endpoints");

    RequireThrowsWithMessage([&] {
        app.UseRequestId();
    }, "after the application has started", "middleware should be rejected after start");

    RequireThrowsWithMessage([&] {
        app.MapGet("/late", [] {
            return Results::Ok();
        });
    }, "after the application has started", "endpoint mapping should be rejected after start");

    RequireThrowsWithMessage([&] {
        app.AddHealthCheck("late", [] {
            return HealthCheckResult::Healthy();
        });
    }, "after the application has started", "health checks should be rejected after start");

    RequireThrowsWithMessage([&] {
        app.WithName("TooLate");
    }, "after the application has started", "endpoint metadata should be rejected after start");

    RequireThrowsWithMessage([&] {
        api.MapGet("/late", [] {
            return Results::Ok();
        });
    }, "after the application has started", "route group mapping should be rejected after start");
}

void HostedServicesValidateRegistration() {
    HostedServiceOptions anonymousStart;
    anonymousStart.Start = [](WebApplication&) {};
    HostedServiceOptions anonymousStop;
    anonymousStop.Stop = [](WebApplication&) {};

    WebApplication unnamed = WebApplication::Create();
    unnamed.AddHostedService(std::move(anonymousStart));
    unnamed.AddHostedService(std::move(anonymousStop));

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        HostedServiceOptions service;
        service.Name = "worker\r\nspoofed";
        service.Start = [](WebApplication&) {};
        app.AddHostedService(std::move(service));
    }, "control", "hosted service name should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        BackgroundServiceOptions service;
        service.Name = "worker\nspoofed";
        service.Execute = [](WebApplication&, std::stop_token) {};
        app.AddBackgroundService(std::move(service));
    }, "control", "background service name should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        HostedServiceOptions service;
        service.Name = "empty";
        app.AddHostedService(std::move(service));
    }, "Start or Stop", "hosted service should require a lifecycle callback");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        BackgroundServiceOptions service;
        service.Name = "empty";
        app.AddBackgroundService(std::move(service));
    }, "Execute", "background service should require an execute callback");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        BackgroundServiceOptions service;
        service.Name = "worker";
        service.Execute = [](WebApplication&, std::stop_token) {};
        service.StopTimeout = std::chrono::milliseconds(-1);
        app.AddBackgroundService(std::move(service));
    }, "StopTimeout", "background service stop timeout should reject negative values");

    {
        WebApplication app = WebApplication::Create();
        BackgroundServiceOptions service;
        service.Name = "worker";
        service.Execute = [](WebApplication&, std::stop_token) {};
        service.StopTimeout = std::chrono::milliseconds(25);
        service.DetachOnStopTimeout = true;
        app.AddBackgroundService(std::move(service));
    }

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        HostedServiceOptions first;
        first.Name = "worker";
        first.Start = [](WebApplication&) {};
        app.AddHostedService(std::move(first));

        HostedServiceOptions second;
        second.Name = "WORKER";
        second.Stop = [](WebApplication&) {};
        app.AddHostedService(std::move(second));
    }, "Duplicate hosted service name", "hosted service names should be unique case-insensitively");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        HostedServiceOptions hosted;
        hosted.Name = "heartbeat";
        hosted.Start = [](WebApplication&) {};
        app.AddHostedService(std::move(hosted));

        BackgroundServiceOptions background;
        background.Name = "HEARTBEAT";
        background.Execute = [](WebApplication&, std::stop_token) {};
        app.AddBackgroundService(std::move(background));
    }, "Duplicate hosted service name", "background service names should share the hosted service namespace");
}

void InMemoryHandleUsesProductionErrorBoundary() {
    WebApplicationBuilder builder = WebApplication::CreateBuilder();
    builder.ConfigureHttpSys([](HttpSysOptions& http) {
        http.MaxRequestBodyBytes = 4;
    });
    WebApplication app = builder.Build();

    app.MapPost("/api/echo", [](HttpContext& ctx) {
        return Results::Text(ctx.Request().Body());
    });
    app.MapPost("/api/json", [](HttpContext& ctx) {
        ctx.Request().Json();
        return Results::Ok();
    });
    app.MapGet("/api/boom", [](HttpContext&) -> HttpResult {
        throw std::runtime_error("boom from test");
    });

    HttpRequest exactLimitRequest;
    exactLimitRequest.Method("POST").Path("/api/echo").Body("1234");
    HttpResponse exactLimit = app.Handle(std::move(exactLimitRequest));
    RequireEqual(200, exactLimit.StatusCode(), "body limit exact size status");
    RequireEqual("1234", exactLimit.BodyText(), "body limit exact size body");

    HttpRequest tooLargeRequest;
    tooLargeRequest.Method("POST").Path("/api/echo").Body("12345");
    HttpResponse tooLarge = app.Handle(std::move(tooLargeRequest));
    JsonElement tooLargeBody = ParseJsonBody(tooLarge);
    RequireEqual(413, tooLarge.StatusCode(), "body limit status");
    RequireEqual("application/problem+json; charset=utf-8", tooLarge.ContentType(), "body limit content type");
    RequireEqual("Payload Too Large", tooLargeBody.GetProperty("title").GetString(), "body limit problem title");
    RequireEqual(413, tooLargeBody.GetProperty("status").GetInt32(), "body limit problem status");

    HttpRequest invalidJsonRequest;
    invalidJsonRequest.Method("POST").Path("/api/json").Header("Content-Type", "application/json").Body("{");
    HttpResponse invalidJson = app.Handle(std::move(invalidJsonRequest));
    JsonElement invalidJsonBody = ParseJsonBody(invalidJson);
    RequireEqual(400, invalidJson.StatusCode(), "invalid json status");
    RequireEqual("Invalid JSON", invalidJsonBody.GetProperty("title").GetString(), "invalid json problem title");
    RequireEqual(400, invalidJsonBody.GetProperty("status").GetInt32(), "invalid json problem status");

    HttpResponse exception = Send(app, "GET", "/api/boom");
    JsonElement exceptionBody = ParseJsonBody(exception);
    RequireEqual(500, exception.StatusCode(), "unhandled exception status");
    RequireEqual("Internal Server Error", exceptionBody.GetProperty("title").GetString(), "unhandled exception title");
    RequireEqual(500, exceptionBody.GetProperty("status").GetInt32(), "unhandled exception body status");
    Require(!exceptionBody.HasProperty("detail"), "detailed errors should be disabled by default");
}

void RequestUrlAndHeaderSizeLimitsReturnProblemDetails() {
    WebApplicationBuilder builder = WebApplication::CreateBuilder();
    builder.ConfigureHttpSys([](HttpSysOptions& http) {
        http.MaxRequestUrlBytes = 8;
        http.MaxRequestHeaderBytes = 20;
    });
    WebApplication app = builder.Build();
    app.MapGet("/ok", [] {
        return Results::Ok("ok");
    });

    HttpResponse ok = Send(app, "GET", "/ok");
    RequireEqual(200, ok.StatusCode(), "request size limits should allow small requests");

    HttpResponse longUrl = Send(app, "GET", "/too-long");
    JsonElement longUrlBody = ParseJsonBody(longUrl);
    RequireEqual(414, longUrl.StatusCode(), "long request URL status");
    RequireEqual("URI Too Long", longUrlBody.GetProperty("title").GetString(), "long request URL title");
    RequireEqual(414, longUrlBody.GetProperty("status").GetInt32(), "long request URL body status");

    HttpRequest largeHeader;
    largeHeader.Method("GET")
        .Path("/ok")
        .Header("X-Test", "123456789012");
    HttpResponse largeHeaderResponse = app.Handle(std::move(largeHeader));
    JsonElement largeHeaderBody = ParseJsonBody(largeHeaderResponse);
    RequireEqual(431, largeHeaderResponse.StatusCode(), "large request header status");
    RequireEqual("Request Header Fields Too Large", largeHeaderBody.GetProperty("title").GetString(), "large request header title");
    RequireEqual(431, largeHeaderBody.GetProperty("status").GetInt32(), "large request header body status");
}

void CommonResultsHelpersSetExpectedStatusHeadersAndBodies() {
    TemporaryDirectory root;
    std::filesystem::path filePath = root.Path / "report.txt";
    {
        std::ofstream file(filePath, std::ios::binary);
        file << "download body";
    }

    WebApplication app = WebApplication::Create();
    app.MapPost("/api/items", [] {
        return Results::Created("/api/items/1");
    });
    app.MapPost("/api/jobs", [] {
        JsonObject payload;
        payload.Add("status", JsonNode::Create("queued"));
        return Results::Accepted("/api/jobs/42", payload);
    });
    app.MapPost("/api/conflict", [] {
        return Results::Conflict("duplicate item");
    });
    app.MapGet("/api/download", [filePath] {
        return Results::File(filePath, {}, "report.txt");
    });

    HttpResponse created = Send(app, "POST", "/api/items");
    RequireEqual(201, created.StatusCode(), "created helper status");
    RequireEqual("/api/items/1", created.Headers().Get("Location"), "created helper location");
    Require(created.Body().empty(), "created helper should not write a body");

    HttpResponse accepted = Send(app, "POST", "/api/jobs");
    JsonElement acceptedBody = ParseJsonBody(accepted);
    RequireEqual(202, accepted.StatusCode(), "accepted helper status");
    RequireEqual("/api/jobs/42", accepted.Headers().Get("Location"), "accepted helper location");
    RequireEqual("queued", acceptedBody.GetProperty("status").GetString(), "accepted helper body");

    HttpResponse conflict = Send(app, "POST", "/api/conflict");
    JsonElement conflictBody = ParseJsonBody(conflict);
    RequireEqual(409, conflict.StatusCode(), "conflict helper status");
    RequireEqual("Conflict", conflictBody.GetProperty("title").GetString(), "conflict helper title");
    RequireEqual("duplicate item", conflictBody.GetProperty("detail").GetString(), "conflict helper detail");

    HttpResponse file = Send(app, "GET", "/api/download");
    RequireEqual(200, file.StatusCode(), "file path helper status");
    RequireEqual("text/plain; charset=utf-8", file.ContentType(), "file path helper content type");
    Require(file.Headers().Get("Content-Disposition").find("report.txt") != std::string::npos, "file path helper disposition");
    RequireEqual("download body", file.BodyText(), "file path helper body");
}

void NamedEndpointPathGenerationWorks() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/products/{id:int}", [] {
        return Results::Ok();
    }).WithName("GetProduct");
    app.MapGet("/files/{*path}", [] {
        return Results::Ok();
    }).WithName("GetFile");
    app.MapPost("/api/products", [&app] {
        return Results::Created(app.PathByName("GetProduct", { { "id", "42" } }));
    });
    app.MapPost("/api/products/absolute", [&app](HttpContext& context) {
        return Results::Created(app.UrlByName(context, "GetProduct", { { "id", "42" } }, { { "source", "create" } }));
    });

    std::string productPath = app.PathByName("getproduct", { { "id", "42" } }, { { "page", "2" } });
    RequireEqual("/api/products/42?page=2", productPath, "named endpoint path should fill route and query values");

    std::string filePath = app.PathByName("GetFile", { { "path", "reports/2026 final.txt" } });
    RequireEqual("/files/reports/2026%20final.txt", filePath, "named endpoint catch-all path should encode segments");

    auto missingEndpoint = app.TryPathByName("Missing", { { "id", "42" } });
    Require(!missingEndpoint.has_value(), "TryPathByName should return empty for unknown endpoint names");

    auto missingValue = app.TryPathByName("GetProduct");
    Require(!missingValue.has_value(), "TryPathByName should return empty when required route values are missing");

    RequireThrowsWithMessage([&app] {
        (void)app.PathByName("GetProduct");
    }, "Missing route value", "PathByName should explain missing route values");

    RequireThrowsWithMessage([&app] {
        (void)app.PathByName("GetProduct", { { "id", "abc" } });
    }, "constraint", "PathByName should validate route constraints");

    RequireThrowsWithMessage([&app] {
        (void)app.PathByName("Missing", { { "id", "42" } });
    }, "No endpoint named", "PathByName should explain unknown endpoint names");

    HttpResponse created = Send(app, "POST", "/api/products");
    RequireEqual(201, created.StatusCode(), "created named endpoint status");
    RequireEqual("/api/products/42", created.Headers().Get("Location"), "created named endpoint location");

    HttpRequest absoluteRequest;
    absoluteRequest.Method("POST")
        .Path("/api/products/absolute")
        .Scheme("https")
        .Host("API.EXAMPLE.COM:8443");
    HttpResponse absolute = app.Handle(std::move(absoluteRequest));
    RequireEqual(201, absolute.StatusCode(), "created absolute named endpoint status");
    RequireEqual("https://api.example.com:8443/api/products/42?source=create",
        absolute.Headers().Get("Location"),
        "created absolute named endpoint location");

    HttpRequest headerHostRequest;
    headerHostRequest.Method("GET")
        .Path("/")
        .Scheme("http")
        .Header("Host", "api.example.com");
    std::string headerHostUrl = app.UrlByName(headerHostRequest, "GetProduct", { { "id", "42" } });
    RequireEqual("http://api.example.com/api/products/42", headerHostUrl, "UrlByName should use Host header when request host is empty");

    HttpRequest invalidHostRequest;
    invalidHostRequest.Method("GET").Path("/").Header("Host", "bad host");
    auto invalidHostUrl = app.TryUrlByName(invalidHostRequest, "GetProduct", { { "id", "42" } });
    Require(!invalidHostUrl.has_value(), "TryUrlByName should return empty for invalid request hosts");

    RequireThrowsWithMessage([&app] {
        HttpRequest request;
        request.Method("GET").Path("/").Header("Host", "bad host");
        (void)app.UrlByName(request, "GetProduct", { { "id", "42" } });
    }, "host", "UrlByName should explain invalid request hosts");

    RequireThrowsWithMessage([&app] {
        HttpRequest request;
        request.Method("GET").Path("/").Scheme("ftp").Host("api.example.com");
        (void)app.UrlByName(request, "GetProduct", { { "id", "42" } });
    }, "scheme", "UrlByName should reject non-http schemes");
}

void HeaderHelpersRejectUnsafeNamesAndValues() {
    RequireThrowsWithMessage([] {
        (void)Results::Ok().Header("X-Test", "bad\r\nvalue");
    }, "CR/LF", "result header value should reject CRLF");

    RequireThrowsWithMessage([] {
        std::string value = "bad";
        value.push_back('\0');
        value += "value";
        (void)Results::Ok().Header("X-Test", value);
    }, "control", "result header value should reject control characters");

    RequireThrowsWithMessage([] {
        (void)Results::Redirect("/next\r\nSet-Cookie: session=bad");
    }, "CR/LF", "redirect location should reject CRLF");

    RequireThrowsWithMessage([] {
        (void)Results::Created("/api/items/1\nX-Bad: yes");
    }, "CR/LF", "created location should reject CRLF");

    RequireThrowsWithMessage([] {
        HttpResponse response;
        response.Header("Bad:Name", "ok");
    }, "header name", "response header name should reject separators");

    WebApplication app = WebApplication::Create();
    app.MapGet("/api/bad-header", [] {
        return Results::Ok().Header("X-Test", "bad\r\nvalue");
    });

    HttpResponse response = Send(app, "GET", "/api/bad-header");
    JsonElement body = ParseJsonBody(response);
    RequireEqual(500, response.StatusCode(), "bad header handler status");
    RequireEqual("Internal Server Error", body.GetProperty("title").GetString(), "bad header handler problem title");
}

void RequestIdMiddlewareHandlesIncomingIdsSafely() {
    RequireThrowsWithMessage([] {
        RequestIdOptions invalid;
        invalid.ItemKey = "RequestId\nBad";
        WebApplication badApp = WebApplication::Create();
        badApp.UseRequestId(invalid);
    }, "ItemKey", "request id item key should reject control characters");

    WebApplication app = WebApplication::Create();

    RequestIdOptions options;
    options.MaxLength = 16;
    app.UseRequestId(options);
    app.MapGet("/api/trace", [](HttpContext& context) {
        JsonObject result;
        result.Add("traceId", JsonNode::Create(context.TraceIdentifier()));
        return Results::Ok(result);
    });

    HttpRequest trustedRequest;
    trustedRequest.Method("GET").Path("/api/trace").Header("X-Request-ID", "client-123");
    HttpResponse trusted = app.Handle(std::move(trustedRequest));
    JsonElement trustedBody = ParseJsonBody(trusted);
    RequireEqual(200, trusted.StatusCode(), "trusted request id status");
    RequireEqual("client-123", trusted.Headers().Get("X-Request-ID"), "trusted request id response header");
    RequireEqual("client-123", trustedBody.GetProperty("traceId").GetString(), "trusted request id trace");

    std::string oversizedId(64, 'x');
    HttpRequest oversizedRequest;
    oversizedRequest.Method("GET").Path("/api/trace").Header("X-Request-ID", oversizedId);
    HttpResponse oversized = app.Handle(std::move(oversizedRequest));
    JsonElement oversizedBody = ParseJsonBody(oversized);
    std::string fallbackId = oversized.Headers().Get("X-Request-ID");
    RequireEqual(200, oversized.StatusCode(), "oversized request id status");
    Require(!fallbackId.empty(), "oversized request id should keep generated response id");
    Require(fallbackId != oversizedId, "oversized request id should not be trusted");
    RequireEqual(fallbackId, oversizedBody.GetProperty("traceId").GetString(), "oversized request id trace should match response header");

    RequireThrowsWithMessage([] {
        HttpContext context;
        std::string value = "bad";
        value.push_back('\0');
        value += "trace";
        context.TraceIdentifier(value);
    }, "control", "trace identifier should reject control characters");
}

void ServerTimingAddsHeadersAndValidatesOptions() {
    std::vector<double> observedDurations;
    WebApplication app = WebApplication::Create();
    app.Use([&observedDurations](HttpContext& context, RequestDelegate next) {
        HttpResult result = next(context);
        auto item = context.Items().find("ServerTimingElapsedMilliseconds");
        if (item != context.Items().end()) {
            observedDurations.push_back(std::any_cast<double>(item->second));
        }
        return result;
    });

    ServerTimingOptions options;
    options.AddResponseTimeHeader = true;
    options.MetricDescription = "Capi \"API\"";
    app.UseServerTiming(options);
    app.MapGet("/api/timed", [] {
        return Results::Ok("ok").Header("Server-Timing", "db;dur=2");
    });

    HttpResponse response = Send(app, "GET", "/api/timed");
    std::string serverTiming = response.Headers().Get("Server-Timing");
    RequireEqual(200, response.StatusCode(), "server timing status");
    Require(serverTiming.find("db;dur=2") != std::string::npos, "server timing should preserve existing metrics");
    Require(serverTiming.find("app;dur=") != std::string::npos, "server timing should add app metric");
    Require(serverTiming.find("desc=\"Capi \\\"API\\\"\"") != std::string::npos, "server timing should quote descriptions");
    Require(response.Headers().Get("X-Response-Time").find("ms") != std::string::npos, "server timing response time header");
    RequireEqual(1, static_cast<int>(observedDurations.size()), "server timing should store elapsed item");
    Require(observedDurations.front() >= 0.0, "server timing elapsed item should be non-negative");

    WebApplication direct = WebApplication::Create();
    direct.UseServerTiming();
    direct.MapGet("/api/direct", [](HttpContext& context) {
        context.Response().Status(202).Write("direct");
        return Results::Empty();
    });

    HttpResponse directResponse = Send(direct, "GET", "/api/direct");
    RequireEqual(202, directResponse.StatusCode(), "server timing direct response status");
    RequireEqual("direct", directResponse.BodyText(), "server timing direct response body");
    Require(directResponse.Headers().Get("Server-Timing").find("app;dur=") != std::string::npos,
        "server timing should handle direct responses");

    RequireThrowsWithMessage([] {
        ServerTimingOptions invalid;
        invalid.ServerTimingHeaderName = "Bad:Name";
        WebApplication badApp = WebApplication::Create();
        badApp.UseServerTiming(invalid);
    }, "header name", "server timing should validate header names");

    RequireThrowsWithMessage([] {
        ServerTimingOptions invalid;
        invalid.MetricName = "bad metric";
        WebApplication badApp = WebApplication::Create();
        badApp.UseServerTiming(invalid);
    }, "metric token", "server timing should validate metric names");

    RequireThrowsWithMessage([] {
        ServerTimingOptions invalid;
        invalid.MetricDescription = "bad\nvalue";
        WebApplication badApp = WebApplication::Create();
        badApp.UseServerTiming(invalid);
    }, "control", "server timing should validate metric descriptions");

    RequireThrowsWithMessage([] {
        ServerTimingOptions invalid;
        invalid.DecimalPlaces = 7;
        WebApplication badApp = WebApplication::Create();
        badApp.UseServerTiming(invalid);
    }, "DecimalPlaces", "server timing should validate decimal places");
}

void MatchedEndpointMetadataFlowsToFiltersLogsAndMetrics() {
    std::vector<LogMessage> messages;
    auto builder = WebApplication::CreateBuilder();
    builder.ConfigureLogging([&](const LogMessage& message) {
        messages.push_back(message);
    });

    WebApplication app = builder.Build();
    MetricsOptions metrics;
    metrics.IncludePathCounters = true;
    app.UseMetrics(metrics);

    RequestLoggingOptions logging;
    logging.IncludeRemoteAddress = false;
    logging.IncludeTraceIdentifier = false;
    logging.IncludeEndpoint = true;
    app.UseRequestLogging(logging);

    std::string filterName;
    std::string filterPattern;
    app.MapGet("/api/items/{id:int}", [](HttpContext& context) {
        JsonObject payload;
        payload.Add("name", JsonNode::Create(context.EndpointName()));
        payload.Add("pattern", JsonNode::Create(context.EndpointPattern()));
        payload.Add("matched", JsonNode::Create(context.MatchedEndpoint().has_value()));
        return Results::Ok(payload);
    }).WithName("GetItem").AddEndpointFilter([&](HttpContext& context, RequestDelegate next) {
        filterName = context.EndpointName();
        filterPattern = context.EndpointPattern();
        return next(context);
    });
    app.MapMetrics();

    HttpResponse item = Send(app, "GET", "/api/items/42");
    JsonElement itemBody = ParseJsonBody(item);
    RequireEqual(200, item.StatusCode(), "matched endpoint status");
    RequireEqual("GetItem", itemBody.GetProperty("name").GetString(), "matched endpoint handler name");
    RequireEqual("/api/items/{id:int}", itemBody.GetProperty("pattern").GetString(), "matched endpoint handler pattern");
    Require(itemBody.GetProperty("matched").GetBoolean(), "matched endpoint handler should expose optional metadata");
    RequireEqual("GetItem", filterName, "matched endpoint filter name");
    RequireEqual("/api/items/{id:int}", filterPattern, "matched endpoint filter pattern");

    RequireEqual(1, static_cast<int>(messages.size()), "matched endpoint logging should write one message");
    Require(messages.front().Message.find("endpoint=GetItem") != std::string::npos,
        "request logging should include endpoint name when enabled");

    HttpResponse metricsResponse = Send(app, "GET", "/metrics");
    JsonElement metricsBody = ParseJsonBody(metricsResponse);
    JsonElement byPath = metricsBody.GetProperty("details").GetProperty("byPath");
    Require(byPath.HasProperty("/api/items/{id:int}"), "metrics should count route pattern");
    RequireEqual(1, byPath.GetProperty("/api/items/{id:int}").GetInt32(), "metrics route pattern count");
    Require(!byPath.HasProperty("/api/items/42"), "metrics should avoid raw URL path when route pattern is available");
}

void RequestLoggingRedactsQueryAndSanitizesDynamicFields() {
    std::vector<LogMessage> messages;
    auto builder = WebApplication::CreateBuilder();
    builder.ConfigureLogging([&](const LogMessage& message) {
        messages.push_back(message);
    });

    WebApplication app = builder.Build();
    RequestLoggingOptions options;
    options.IncludeQueryString = true;
    options.IncludeRemoteAddress = true;
    options.IncludeTraceIdentifier = false;
    app.UseRequestLogging(options);
    app.MapGet("/api/log", [] {
        return Results::Ok("ok");
    });

    HttpRequest request;
    request.Method("GET")
        .Path("/api/log")
        .QueryString("?token=secret&name=alice&api_key=hidden&code=oauth-code")
        .RemoteAddress("10.0.0.1\r\nspoofed");
    HttpResponse response = app.Handle(std::move(request));

    RequireEqual(200, response.StatusCode(), "request logging redaction response status");
    RequireEqual(1, static_cast<int>(messages.size()), "request logging should write one message");
    const std::string& message = messages.front().Message;
    Require(message.find("/api/log?token=[redacted]&name=alice&api_key=[redacted]&code=[redacted]") != std::string::npos,
        "request logging should redact configured query parameters");
    Require(message.find("secret") == std::string::npos, "request logging should not include token value");
    Require(message.find("hidden") == std::string::npos, "request logging should not include api key value");
    Require(message.find("oauth-code") == std::string::npos, "request logging should not include code value");
    Require(message.find("name=alice") != std::string::npos, "request logging should keep non-sensitive query parameters");
    Require(message.find('\r') == std::string::npos, "request logging should sanitize CR characters");
    Require(message.find('\n') == std::string::npos, "request logging should sanitize LF characters");
    Require(message.find("10.0.0.1??spoofed") != std::string::npos, "request logging should replace control characters");

    RequireThrowsWithMessage([] {
        RequestLoggingOptions invalid;
        invalid.RedactedQueryParameters = { " token" };
        WebApplication app = WebApplication::Create();
        app.UseRequestLogging(invalid);
    }, "whitespace", "request logging redaction parameter names should reject whitespace");

    RequireThrowsWithMessage([] {
        RequestLoggingOptions invalid;
        invalid.RedactedQueryValue = "bad\nvalue";
        WebApplication app = WebApplication::Create();
        app.UseRequestLogging(invalid);
    }, "control", "request logging redaction value should reject control characters");
}

void RateLimiterBoundsTrackedKeyCount() {
    WebApplication app = WebApplication::Create();

    RateLimitOptions options;
    options.PermitLimit = 2;
    options.Window = std::chrono::seconds(60);
    options.MaxTrackedKeys = 2;
    options.KeySelector = [](HttpContext& context) {
        return context.Request().Header("X-Client-Key");
    };
    app.UseRateLimiter(options);
    app.MapGet("/api/limited", [] {
        return Results::Ok("ok");
    });

    auto sendWithKey = [&app](std::string key) {
        HttpRequest request;
        request.Method("GET").Path("/api/limited").Header("X-Client-Key", std::move(key));
        return app.Handle(std::move(request));
    };

    HttpResponse first = sendWithKey("alpha");
    HttpResponse second = sendWithKey("bravo");
    HttpResponse overflow = sendWithKey("charlie");
    HttpResponse existing = sendWithKey("alpha");

    RequireEqual(200, first.StatusCode(), "rate limit first tracked key status");
    RequireEqual("2", first.Headers().Get("X-RateLimit-Limit"), "rate limit allowed result limit header");
    RequireEqual("1", first.Headers().Get("X-RateLimit-Remaining"), "rate limit allowed result remaining header");
    Require(!first.Headers().Get("X-RateLimit-Reset").empty(), "rate limit allowed result reset header");
    RequireEqual(200, second.StatusCode(), "rate limit second tracked key status");
    RequireEqual(429, overflow.StatusCode(), "rate limit max tracked keys overflow status");
    RequireEqual("2", overflow.Headers().Get("X-RateLimit-Limit"), "rate limit overflow limit header");
    RequireEqual("0", overflow.Headers().Get("X-RateLimit-Remaining"), "rate limit overflow remaining header");
    Require(!overflow.Headers().Get("Retry-After").empty(), "rate limit overflow retry-after header");
    RequireEqual(200, existing.StatusCode(), "rate limit existing tracked key should still be served");

    RequireThrowsWithMessage([] {
        RateLimitOptions invalid;
        invalid.RetryAfterHeaderName = "Bad:Name";
        WebApplication app = WebApplication::Create();
        app.UseRateLimiter(invalid);
    }, "header name", "rate limit header names should be validated");

    RequireThrowsWithMessage([] {
        RateLimitOptions invalid;
        invalid.MaxKeyLength = 8;
        WebApplication app = WebApplication::Create();
        app.UseRateLimiter(invalid);
    }, "MaxKeyLength", "rate limit max key length should reject unsafe small bounds");

    RequireThrowsWithMessage([] {
        RateLimitOptions invalid;
        invalid.ItemKey = "RateLimit\nKey";
        WebApplication app = WebApplication::Create();
        app.UseRateLimiter(invalid);
    }, "ItemKey", "rate limit item key should reject control characters");

    WebApplication boundedKeyApp = WebApplication::Create();
    RateLimitOptions bounded;
    bounded.PermitLimit = 1;
    bounded.Window = std::chrono::seconds(60);
    bounded.MaxKeyLength = 24;
    bounded.KeySelector = [](HttpContext& context) {
        return context.Request().Header("X-Client-Key");
    };
    boundedKeyApp.UseRateLimiter(bounded);
    boundedKeyApp.MapGet("/api/limited-key", [](HttpContext& context) {
        return Results::Text(std::any_cast<std::string>(context.Items().at("RateLimitKey")));
    });

    auto sendBoundedKey = [&boundedKeyApp](std::string key) {
        HttpRequest request;
        request.Method("GET").Path("/api/limited-key").Header("X-Client-Key", std::move(key));
        return boundedKeyApp.Handle(std::move(request));
    };

    std::string longAlpha = "tenant-alpha-" + std::string(256, 'a');
    std::string longBravo = "tenant-bravo-" + std::string(256, 'b');
    HttpResponse boundedFirst = sendBoundedKey(longAlpha);
    HttpResponse boundedRepeat = sendBoundedKey(longAlpha);
    HttpResponse boundedDifferent = sendBoundedKey(longBravo);

    RequireEqual(200, boundedFirst.StatusCode(), "rate limit bounded key first status");
    Require(boundedFirst.BodyText().size() <= bounded.MaxKeyLength, "rate limit stored key should honor maximum length");
    Require(boundedFirst.BodyText().find("~") != std::string::npos, "rate limit bounded key should include a hash separator");
    RequireEqual(429, boundedRepeat.StatusCode(), "rate limit bounded key should still enforce the original key quota");
    RequireEqual(200, boundedDifferent.StatusCode(), "rate limit bounded key should avoid simple-prefix collisions");
    Require(boundedDifferent.BodyText().size() <= bounded.MaxKeyLength, "rate limit second stored key should honor maximum length");
    Require(boundedDifferent.BodyText() != boundedFirst.BodyText(), "rate limit different long keys should have different bounded keys");
}

void StatusCodeValidationRejectsInvalidValues() {
    RequireThrowsWithMessage([] {
        (void)Results::StatusCode(99);
    }, "100 and 599", "status helper should reject status below valid range");

    RequireThrowsWithMessage([] {
        (void)Results::Text("bad", 700);
    }, "100 and 599", "text helper should reject status above valid range");

    RequireThrowsWithMessage([] {
        HttpResponse response;
        response.Status(42);
    }, "100 and 599", "response status should reject invalid status");

    RequireThrowsWithMessage([] {
        (void)Results::Problem("Bad", {}, 42);
    }, "100 and 599", "problem helper should reject invalid status");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/bad-metadata", [] {
            return Results::Ok();
        }).Produces(42);
    }, "100 and 599", "produces metadata should reject invalid status");

    HttpResult valid = Results::StatusCode(599);
    RequireEqual(599, valid.StatusCode(), "status helper should accept upper valid range");

    WebApplication app = WebApplication::Create();
    app.MapGet("/api/bad-status", [] {
        return Results::StatusCode(42);
    });

    HttpResponse response = Send(app, "GET", "/api/bad-status");
    JsonElement body = ParseJsonBody(response);
    RequireEqual(500, response.StatusCode(), "invalid status handler exception status");
    RequireEqual("Internal Server Error", body.GetProperty("title").GetString(), "invalid status handler problem title");
}

void HostFilteringRejectsUnexpectedHosts() {
    RequireThrowsWithMessage([] {
        HostFilteringOptions invalid;
        invalid.FailureDetail = "bad\r\ndetail";
        WebApplication badApp = WebApplication::Create();
        badApp.UseHostFiltering(invalid);
    }, "FailureDetail", "host filtering should reject unsafe failure details at registration time");

    RequireThrowsWithMessage([] {
        HostFilteringOptions invalid;
        invalid.ItemKey = "host\nrejected";
        WebApplication badApp = WebApplication::Create();
        badApp.UseHostFiltering(invalid);
    }, "ItemKey", "host filtering should reject unsafe item keys at registration time");

    WebApplication app = WebApplication::Create();

    ForwardedHeadersOptions forwarded;
    forwarded.RequireKnownProxy = false;
    app.UseForwardedHeaders(forwarded);

    HostFilteringOptions hosts;
    hosts.AllowedHosts = { "api.example.com", "*.example.net", "127.0.0.1:8080", "public.example.com" };
    app.UseHostFiltering(hosts);

    app.MapGet("/api/ok", [](HttpContext&) {
        return Results::Ok("ok");
    });

    auto sendWithHost = [&app](std::string host) {
        HttpRequest request;
        request.Method("GET").Path("/api/ok").Host(std::move(host));
        return app.Handle(std::move(request));
    };

    HttpResponse exact = sendWithHost("API.EXAMPLE.COM:444");
    RequireEqual(200, exact.StatusCode(), "host filtering exact host should allow any port when pattern has no port");

    HttpResponse wildcard = sendWithHost("svc.example.net");
    RequireEqual(200, wildcard.StatusCode(), "host filtering wildcard subdomain status");

    HttpResponse port = sendWithHost("127.0.0.1:8080");
    RequireEqual(200, port.StatusCode(), "host filtering exact port status");

    HttpResponse apex = sendWithHost("example.net");
    RequireEqual(400, apex.StatusCode(), "host filtering wildcard should not match apex domain");

    HttpResponse rejected = sendWithHost("evil.example.org");
    JsonElement rejectedBody = ParseJsonBody(rejected);
    RequireEqual(400, rejected.StatusCode(), "host filtering rejected status");
    RequireEqual("application/problem+json; charset=utf-8", rejected.ContentType(), "host filtering content type");
    RequireEqual("Bad Request", rejectedBody.GetProperty("title").GetString(), "host filtering problem title");
    RequireEqual(400, rejectedBody.GetProperty("status").GetInt32(), "host filtering problem status");

    HttpRequest forwardedRequest;
    forwardedRequest.Method("GET")
        .Path("/api/ok")
        .Host("internal.local")
        .Header("X-Forwarded-Host", "public.example.com");
    HttpResponse forwardedResponse = app.Handle(std::move(forwardedRequest));
    RequireEqual(200, forwardedResponse.StatusCode(), "host filtering should use forwarded host after forwarded middleware");

    HttpRequest invalidHeaderRequest;
    invalidHeaderRequest.Method("GET").Path("/api/ok").Header("Host", "bad host");
    HttpResponse invalidHeader = app.Handle(std::move(invalidHeaderRequest));
    RequireEqual(400, invalidHeader.StatusCode(), "host filtering should reject malformed host header");
}

void ForwardedHeadersValidateOptionsAndHostValues() {
    RequireThrowsWithMessage([] {
        ForwardedHeadersOptions invalid;
        invalid.ForwardLimit = 0;
        WebApplication badApp = WebApplication::Create();
        badApp.UseForwardedHeaders(invalid);
    }, "ForwardLimit", "forwarded headers should reject zero forward limit");

    RequireThrowsWithMessage([] {
        ForwardedHeadersOptions invalid;
        invalid.KnownProxies = { "bad proxy" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseForwardedHeaders(invalid);
    }, "KnownProxies", "forwarded headers should reject invalid known proxy entries");

    RequireThrowsWithMessage([] {
        ForwardedHeadersOptions invalid;
        invalid.AppliedItemKey = "Forwarded\nApplied";
        WebApplication badApp = WebApplication::Create();
        badApp.UseForwardedHeaders(invalid);
    }, "AppliedItemKey", "forwarded headers item key should reject control characters");

    WebApplication app = WebApplication::Create();
    ForwardedHeadersOptions forwarded;
    forwarded.RequireKnownProxy = false;
    app.UseForwardedHeaders(forwarded);
    app.MapGet("/api/host", [](HttpContext& context) {
        return Results::Text(context.Request().Host());
    });

    HttpRequest validHost;
    validHost.Method("GET")
        .Path("/api/host")
        .Host("internal.local")
        .Header("X-Forwarded-Host", "PUBLIC.EXAMPLE.COM:443");
    HttpResponse valid = app.Handle(std::move(validHost));
    RequireEqual(200, valid.StatusCode(), "valid forwarded host status");
    RequireEqual("public.example.com:443", valid.BodyText(), "valid forwarded host should be normalized");

    HttpRequest invalidHost;
    invalidHost.Method("GET")
        .Path("/api/host")
        .Host("internal.local")
        .Header("X-Forwarded-Host", "bad host");
    HttpResponse invalid = app.Handle(std::move(invalidHost));
    RequireEqual(200, invalid.StatusCode(), "invalid forwarded host status");
    RequireEqual("internal.local", invalid.BodyText(), "invalid forwarded host should not overwrite request host");
}

void HttpsRedirectionRedirectsHttpRequests() {
    RequireThrowsWithMessage([] {
        HttpsRedirectionOptions invalid;
        invalid.FailureDetail = "bad\r\ndetail";
        WebApplication badApp = WebApplication::Create();
        badApp.UseHttpsRedirection(invalid);
    }, "FailureDetail", "https redirection should reject unsafe failure details at registration time");

    WebApplication app = WebApplication::Create();
    bool handlerInvoked = false;

    HttpsRedirectionOptions redirection;
    redirection.HttpsPort = 8443;
    app.UseHttpsRedirection(redirection);

    app.MapGet("/api/ok", [&handlerInvoked](HttpContext&) {
        handlerInvoked = true;
        return Results::Ok("ok");
    });

    HttpRequest httpRequest;
    httpRequest.Method("GET").Path("/api/ok").QueryString("?x=1").Scheme("http").Host("example.com:8080");
    HttpResponse redirect = app.Handle(std::move(httpRequest));

    RequireEqual(307, redirect.StatusCode(), "https redirection status");
    RequireEqual("https://example.com:8443/api/ok?x=1", redirect.Headers().Get("Location"), "https redirection location");
    Require(!handlerInvoked, "https redirection should short-circuit downstream handler");

    HttpRequest httpsRequest;
    httpsRequest.Method("GET").Path("/api/ok").Scheme("https").Host("example.com");
    HttpResponse passThrough = app.Handle(std::move(httpsRequest));
    RequireEqual(200, passThrough.StatusCode(), "https redirection should allow https requests");
    RequireEqual("ok", passThrough.BodyText(), "https redirection pass-through body");

    HttpRequest invalidHostRequest;
    invalidHostRequest.Method("GET").Path("/api/ok").Scheme("http").Header("Host", "bad host");
    HttpResponse invalidHost = app.Handle(std::move(invalidHostRequest));
    JsonElement invalidHostBody = ParseJsonBody(invalidHost);
    RequireEqual(400, invalidHost.StatusCode(), "https redirection invalid host status");
    RequireEqual("Bad Request", invalidHostBody.GetProperty("title").GetString(), "https redirection invalid host title");
}

void HstsAddsHeaderOnlyForHttpsRequests() {
    WebApplication app = WebApplication::Create();

    HstsOptions hsts;
    hsts.MaxAge = std::chrono::seconds(123);
    hsts.IncludeSubDomains = true;
    hsts.Preload = true;
    app.UseHsts(hsts);

    app.MapGet("/api/ok", [](HttpContext&) {
        return Results::Ok("ok");
    });

    HttpRequest httpsRequest;
    httpsRequest.Method("GET").Path("/api/ok").Scheme("https").Host("api.example.com");
    HttpResponse https = app.Handle(std::move(httpsRequest));
    RequireEqual(200, https.StatusCode(), "hsts https status");
    RequireEqual("max-age=123; includeSubDomains; preload", https.Headers().Get("Strict-Transport-Security"), "hsts header");

    HttpRequest httpRequest;
    httpRequest.Method("GET").Path("/api/ok").Scheme("http").Host("api.example.com");
    HttpResponse http = app.Handle(std::move(httpRequest));
    RequireEqual(200, http.StatusCode(), "hsts http status");
    Require(http.Headers().Get("Strict-Transport-Security").empty(), "hsts should not be added to http responses");

    HttpRequest localhostRequest;
    localhostRequest.Method("GET").Path("/api/ok").Scheme("https").Host("localhost");
    HttpResponse localhost = app.Handle(std::move(localhostRequest));
    RequireEqual(200, localhost.StatusCode(), "hsts localhost status");
    Require(localhost.Headers().Get("Strict-Transport-Security").empty(), "hsts should skip localhost by default");
}

void HstsOptionsValidateUnsafeConfiguration() {
    RequireThrowsWithMessage([] {
        HstsOptions invalid;
        invalid.MaxAge = std::chrono::seconds(-1);
        WebApplication badApp = WebApplication::Create();
        badApp.UseHsts(invalid);
    }, "MaxAge", "hsts should reject negative max age at registration time");

    RequireThrowsWithMessage([] {
        HstsOptions invalid;
        invalid.ExcludedHosts = { "bad host" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseHsts(invalid);
    }, "HstsOptions::ExcludedHosts", "hsts should reject invalid excluded host pattern at registration time");
}

void SecurityHeadersAddDefaultsAndValidateOptions() {
    WebApplication app = WebApplication::Create();

    SecurityHeadersOptions options;
    options.ContentSecurityPolicy = "default-src 'self'";
    app.UseSecurityHeaders(options);
    app.MapGet("/api/defaults", [] {
        return Results::Ok("ok");
    });
    app.MapGet("/api/existing", [] {
        return Results::Ok("ok")
            .Header("X-Frame-Options", "SAMEORIGIN")
            .Header("Content-Security-Policy", "default-src 'none'");
    });

    HttpResponse defaults = Send(app, "GET", "/api/defaults");
    RequireEqual(200, defaults.StatusCode(), "security headers defaults status");
    RequireEqual("nosniff", defaults.Headers().Get("X-Content-Type-Options"), "security headers content type options");
    RequireEqual("DENY", defaults.Headers().Get("X-Frame-Options"), "security headers frame options");
    RequireEqual("no-referrer", defaults.Headers().Get("Referrer-Policy"), "security headers referrer policy");
    RequireEqual("same-origin", defaults.Headers().Get("Cross-Origin-Opener-Policy"), "security headers coop");
    RequireEqual("default-src 'self'", defaults.Headers().Get("Content-Security-Policy"), "security headers csp");

    HttpResponse existing = Send(app, "GET", "/api/existing");
    RequireEqual("SAMEORIGIN", existing.Headers().Get("X-Frame-Options"), "security headers should not overwrite existing frame options");
    RequireEqual("default-src 'none'", existing.Headers().Get("Content-Security-Policy"), "security headers should not overwrite existing csp");

    RequireThrowsWithMessage([] {
        SecurityHeadersOptions invalid;
        invalid.ContentSecurityPolicy = "default-src 'self'\r\nX-Test: bad";
        WebApplication badApp = WebApplication::Create();
        badApp.UseSecurityHeaders(invalid);
    }, "control", "security headers should validate csp at registration time");

    RequireThrowsWithMessage([] {
        SecurityHeadersOptions invalid;
        invalid.FrameOptions = "DENY\nX-Test: bad";
        WebApplication badApp = WebApplication::Create();
        badApp.UseSecurityHeaders(invalid);
    }, "control", "security headers should validate frame options at registration time");
}

void CorsCanRejectInvalidPreflightRequests() {
    WebApplication app = WebApplication::Create();

    CorsOptions cors;
    cors.AllowedOrigins = { "https://app.example.com" };
    cors.AllowedMethods = { "GET", "POST" };
    cors.AllowedHeaders = { "X-Api-Key", "Content-Type" };
    cors.AllowCredentials = true;
    cors.MaxAgeSeconds = 120;
    cors.RejectInvalidPreflightRequests = true;
    app.UseCors(cors);

    app.MapGet("/api/data", [](HttpContext&) {
        return Results::Ok("data");
    });

    HttpRequest validPreflight;
    validPreflight.Method("OPTIONS")
        .Path("/api/data")
        .Header("Origin", "https://app.example.com")
        .Header("Access-Control-Request-Method", "POST")
        .Header("Access-Control-Request-Headers", "x-api-key, content-type");
    HttpResponse valid = app.Handle(std::move(validPreflight));
    RequireEqual(204, valid.StatusCode(), "valid cors preflight status");
    RequireEqual("https://app.example.com", valid.Headers().Get("Access-Control-Allow-Origin"), "valid cors preflight origin");
    RequireEqual("true", valid.Headers().Get("Access-Control-Allow-Credentials"), "valid cors preflight credentials");
    RequireEqual("GET, POST", valid.Headers().Get("Access-Control-Allow-Methods"), "valid cors preflight methods");
    RequireEqual("X-Api-Key, Content-Type", valid.Headers().Get("Access-Control-Allow-Headers"), "valid cors preflight headers");
    RequireEqual("120", valid.Headers().Get("Access-Control-Max-Age"), "valid cors preflight max age");
    Require(valid.Headers().Get("Vary").find("Origin") != std::string::npos, "valid cors preflight vary");

    HttpRequest invalidMethod;
    invalidMethod.Method("OPTIONS")
        .Path("/api/data")
        .Header("Origin", "https://app.example.com")
        .Header("Access-Control-Request-Method", "DELETE");
    HttpResponse invalidMethodResponse = app.Handle(std::move(invalidMethod));
    JsonElement invalidMethodBody = ParseJsonBody(invalidMethodResponse);
    RequireEqual(403, invalidMethodResponse.StatusCode(), "invalid cors method status");
    RequireEqual("Forbidden", invalidMethodBody.GetProperty("title").GetString(), "invalid cors method title");
    RequireEqual(403, invalidMethodBody.GetProperty("status").GetInt32(), "invalid cors method body status");
    Require(invalidMethodResponse.Headers().Get("Access-Control-Allow-Origin").empty(), "invalid cors method should not emit allow origin");

    HttpRequest invalidHeader;
    invalidHeader.Method("OPTIONS")
        .Path("/api/data")
        .Header("Origin", "https://app.example.com")
        .Header("Access-Control-Request-Method", "POST")
        .Header("Access-Control-Request-Headers", "X-Secret");
    HttpResponse invalidHeaderResponse = app.Handle(std::move(invalidHeader));
    JsonElement invalidHeaderBody = ParseJsonBody(invalidHeaderResponse);
    RequireEqual(403, invalidHeaderResponse.StatusCode(), "invalid cors header status");
    RequireEqual("One or more CORS request headers are not allowed.", invalidHeaderBody.GetProperty("detail").GetString(), "invalid cors header detail");
}

void CorsOptionsValidateUnsafeConfiguration() {
    WebApplication app = WebApplication::Create();

    CorsOptions valid;
    valid.AllowedOrigins = { "https://app.example.com" };
    valid.ExposedHeaders = { "X-Trace-ID" };
    app.UseCors(valid);
    app.MapGet("/api/data", [] {
        return Results::Ok("data");
    });

    HttpRequest request;
    request.Method("GET").Path("/api/data").Header("Origin", "https://app.example.com");
    HttpResponse response = app.Handle(std::move(request));
    RequireEqual(200, response.StatusCode(), "valid cors options status");
    RequireEqual("X-Trace-ID", response.Headers().Get("Access-Control-Expose-Headers"), "valid cors exposed headers");

    RequireThrowsWithMessage([] {
        CorsOptions invalid;
        invalid.AllowedOrigins = { "https://app.example.com\r\nX-Test: bad" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseCors(invalid);
    }, "control", "cors origins should reject control characters");

    RequireThrowsWithMessage([] {
        CorsOptions invalid;
        invalid.AllowedMethods = { "GET", "BAD METHOD" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseCors(invalid);
    }, "header name", "cors methods should reject invalid tokens");

    RequireThrowsWithMessage([] {
        CorsOptions invalid;
        invalid.AllowedHeaders = { "X-Api-Key", "Bad Header" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseCors(invalid);
    }, "header name", "cors allowed headers should reject invalid names");

    RequireThrowsWithMessage([] {
        CorsOptions invalid;
        invalid.ExposedHeaders = { "X-Trace\r\nX-Bad: yes" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseCors(invalid);
    }, "control", "cors exposed headers should reject control characters");

    RequireThrowsWithMessage([] {
        CorsOptions invalid;
        invalid.MaxAgeSeconds = -1;
        WebApplication badApp = WebApplication::Create();
        badApp.UseCors(invalid);
    }, "MaxAgeSeconds", "cors max age should reject negative values");
}

void CorsWildcardWithCredentialsEchoesOrigin() {
    WebApplication app = WebApplication::Create();

    CorsOptions cors;
    cors.AllowedOrigins = { "*" };
    cors.AllowCredentials = true;
    app.UseCors(cors);

    app.MapGet("/api/data", [](HttpContext&) {
        return Results::Ok("data");
    });

    HttpRequest request;
    request.Method("GET").Path("/api/data").Header("Origin", "https://tenant.example.com");
    HttpResponse response = app.Handle(std::move(request));

    RequireEqual(200, response.StatusCode(), "cors wildcard credentials status");
    RequireEqual("https://tenant.example.com", response.Headers().Get("Access-Control-Allow-Origin"), "cors wildcard credentials origin");
    RequireEqual("true", response.Headers().Get("Access-Control-Allow-Credentials"), "cors wildcard credentials header");
    Require(response.Headers().Get("Vary").find("Origin") != std::string::npos, "cors wildcard credentials vary");
}

void CookiesCanBeReadAppendedAndDeleted() {
    WebApplication app = WebApplication::Create();

    app.MapGet("/api/cookies", [](HttpContext& ctx) {
        CookieOptions session;
        session.Path = "/";
        session.MaxAge = std::chrono::seconds(60);
        session.HttpOnly = true;
        session.Secure = true;
        session.SameSite = CookieSameSiteMode::Lax;

        CookieOptions remove;
        remove.Path = "/";

        HttpResult result = Results::Text(
            ctx.Request().Cookie("theme") + "|" +
            ctx.Request().Cookie("empty", "not-empty") + "|" +
            ctx.Request().Cookie("missing", "fallback"));
        result.AppendCookie("session", "abc 123", session);
        result.DeleteCookie("old", remove);
        return result;
    });

    HttpRequest request;
    request.Method("GET")
        .Path("/api/cookies")
        .Header("Cookie", "theme=dark%20mode; empty=; ignored; bad name=value");
    HttpResponse response = app.Handle(std::move(request));
    std::vector<std::string> cookies = response.Headers().GetValues("Set-Cookie");

    RequireEqual(200, response.StatusCode(), "cookie response status");
    RequireEqual("dark mode||fallback", response.BodyText(), "cookie request parsing");
    RequireEqual(2, static_cast<int>(cookies.size()), "set-cookie count");
    Require(cookies[0].find("session=abc%20123") != std::string::npos, "session cookie value should be encoded");
    Require(cookies[0].find("Path=/") != std::string::npos, "session cookie path");
    Require(cookies[0].find("Max-Age=60") != std::string::npos, "session cookie max age");
    Require(cookies[0].find("Secure") != std::string::npos, "session cookie secure");
    Require(cookies[0].find("HttpOnly") != std::string::npos, "session cookie httponly");
    Require(cookies[0].find("SameSite=Lax") != std::string::npos, "session cookie samesite");
    Require(cookies[1].find("old=") != std::string::npos, "delete cookie name");
    Require(cookies[1].find("Expires=Thu, 01 Jan 1970 00:00:00 GMT") != std::string::npos, "delete cookie expires");
    Require(cookies[1].find("Max-Age=0") != std::string::npos, "delete cookie max age");

    RequireThrowsWithMessage([] {
        CookieOptions invalid;
        invalid.Path = "/\r\nSet-Cookie: bad=1";
        (void)Results::Ok().AppendCookie("session", "value", invalid);
    }, "CookieOptions::Path", "cookie path should reject control characters");

    RequireThrowsWithMessage([] {
        CookieOptions invalid;
        invalid.Domain = "example.com; Secure";
        (void)Results::Ok().AppendCookie("session", "value", invalid);
    }, "CookieOptions::Domain", "cookie domain should reject semicolons");

    RequireThrowsWithMessage([] {
        CookieOptions invalid;
        invalid.SameSite = static_cast<CookieSameSiteMode>(99);
        (void)Results::Ok().AppendCookie("session", "value", invalid);
    }, "SameSite", "cookie same-site mode should reject invalid enum values");
}

void RouteGroupAppliesPrefixAuthorizationAndTags() {
    WebApplication app = WebApplication::Create();

    AuthenticationOptions authentication;
    authentication.Schemes.push_back(AuthenticationScheme{
        "ApiKey",
        [](HttpContext& ctx) {
            if (ctx.Request().Header("X-API-Key") != "secret") {
                return AuthenticationResult::NoResult();
            }
            return AuthenticationResult::Success(
                ClaimsPrincipal::Authenticated("group-test", "ApiKey").AddRole("admin"));
        },
        "ApiKey"
    });
    app.UseAuthentication(authentication);

    AuthorizationPolicy adminPolicy;
    adminPolicy.Name = "admin";
    adminPolicy.RequiredRoles = { "admin" };

    AuthorizationOptions authorization;
    authorization.Policies.push_back(std::move(adminPolicy));
    app.UseAuthorization(authorization);

    auto api = app.MapGroup("/api").WithTags({ "Grouped" }).RequireAuthorization("admin");
    api.MapGet("/secure", [](HttpContext&) {
        return Results::Ok("secure");
    });
    api.MapGet("/public", [](HttpContext&) {
        return Results::Ok("public");
    }).AllowAnonymous();

    HttpResponse unauthorized = Send(app, "GET", "/api/secure");
    RequireEqual(401, unauthorized.StatusCode(), "group auth should protect secure endpoint");

    HttpRequest authorizedRequest;
    authorizedRequest.Method("GET").Path("/api/secure").Header("X-API-Key", "secret");
    HttpResponse authorized = app.Handle(std::move(authorizedRequest));
    RequireEqual(200, authorized.StatusCode(), "group auth should allow valid principal");
    RequireEqual("secure", authorized.BodyText(), "group auth body");

    HttpResponse publicResponse = Send(app, "GET", "/api/public");
    RequireEqual(200, publicResponse.StatusCode(), "allow anonymous should override group auth");
    RequireEqual("public", publicResponse.BodyText(), "allow anonymous body");

    auto nested = api.MapGroup("/v1");
    nested.MapGet("/nested", [](HttpContext&) {
        return Results::Ok("nested");
    });

    HttpRequest nestedRequest;
    nestedRequest.Method("GET").Path("/api/v1/nested").Header("X-API-Key", "secret");
    HttpResponse nestedResponse = app.Handle(std::move(nestedRequest));
    RequireEqual(200, nestedResponse.StatusCode(), "nested group should compose prefixes and inherit auth");
    RequireEqual("nested", nestedResponse.BodyText(), "nested group body");
}

void AuthorizationPolicyFluentBuildersRequireRolesAndClaims() {
    WebApplication app = WebApplication::Create();

    AuthenticationOptions authentication;
    authentication.Schemes.push_back(AuthenticationScheme{
        "ApiKey",
        [](HttpContext& ctx) {
            std::string key = ctx.Request().Header("X-API-Key");
            if (key.empty()) {
                return AuthenticationResult::NoResult();
            }
            if (key == "ops") {
                return AuthenticationResult::Success(
                    ClaimsPrincipal::Authenticated("ops-client", "ApiKey")
                        .AddRole("ops")
                        .AddClaim("scope", "read")
                        .AddClaim("tenant", "blue"));
            }
            if (key == "role-only") {
                return AuthenticationResult::Success(
                    ClaimsPrincipal::Authenticated("role-client", "ApiKey")
                        .AddRole("ops")
                        .AddClaim("tenant", "blue"));
            }
            if (key == "claim-only") {
                return AuthenticationResult::Success(
                    ClaimsPrincipal::Authenticated("claim-client", "ApiKey")
                        .AddRole("user")
                        .AddClaim("scope", "read")
                        .AddClaim("tenant", "blue"));
            }
            return AuthenticationResult::Fail("Invalid API key.", "ApiKey");
        },
        "ApiKey"
    });
    app.UseAuthentication(authentication);

    AuthorizationPolicy opsPolicy;
    opsPolicy.Name = "ops-read";
    opsPolicy
        .RequireRoles({ "ops", "admin", "ops" })
        .RequireClaim("scope", "read")
        .RequireClaim("tenant");

    RequireEqual(2, static_cast<int>(opsPolicy.RequiredRoles.size()), "fluent policy should de-duplicate roles");
    RequireEqual(2, static_cast<int>(opsPolicy.RequiredClaims.size()), "fluent policy should add required claims");

    AuthorizationOptions authorization;
    authorization.Policies.push_back(opsPolicy);
    app.UseAuthorization(authorization);

    app.MapGet("/api/ops", [](HttpContext& ctx) {
        return Results::Ok(ctx.User().Name());
    }).RequireAuthorization("ops-read");

    HttpResponse anonymous = Send(app, "GET", "/api/ops");
    RequireEqual(401, anonymous.StatusCode(), "fluent policy should require authentication");

    HttpRequest roleOnlyRequest;
    roleOnlyRequest.Method("GET").Path("/api/ops").Header("X-API-Key", "role-only");
    HttpResponse roleOnly = app.Handle(std::move(roleOnlyRequest));
    RequireEqual(403, roleOnly.StatusCode(), "fluent policy should reject missing claim value");

    HttpRequest claimOnlyRequest;
    claimOnlyRequest.Method("GET").Path("/api/ops").Header("X-API-Key", "claim-only");
    HttpResponse claimOnly = app.Handle(std::move(claimOnlyRequest));
    RequireEqual(403, claimOnly.StatusCode(), "fluent policy should reject missing role");

    HttpRequest authorizedRequest;
    authorizedRequest.Method("GET").Path("/api/ops").Header("X-API-Key", "ops");
    HttpResponse authorized = app.Handle(std::move(authorizedRequest));
    RequireEqual(200, authorized.StatusCode(), "fluent policy should allow matching principal");
    RequireEqual("ops-client", authorized.BodyText(), "fluent policy should expose authorized user");

    RequireThrowsWithMessage([] {
        AuthorizationPolicy invalid;
        invalid.RequireRole(" ");
    }, "role", "fluent policy roles should reject empty values");

    RequireThrowsWithMessage([] {
        AuthorizationPolicy invalid;
        invalid.RequireClaim(" ");
    }, "type", "fluent policy claim types should reject empty values");

    RequireThrowsWithMessage([] {
        AuthorizationPolicy invalid;
        invalid.RequireClaim("scope", "read\r\nX-Test: bad");
    }, "control", "fluent policy claim values should reject control characters");
}

void EndpointAndGroupAuthorizationConvenienceRequiresRolesAndClaims() {
    WebApplication app = WebApplication::Create();

    AuthenticationOptions authentication;
    authentication.Schemes.push_back(AuthenticationScheme{
        "ApiKey",
        [](HttpContext& ctx) {
            std::string key = ctx.Request().Header("X-API-Key");
            if (key.empty()) {
                return AuthenticationResult::NoResult();
            }
            if (key == "admin-read") {
                return AuthenticationResult::Success(
                    ClaimsPrincipal::Authenticated("admin-read", "ApiKey")
                        .AddRole("admin")
                        .AddClaim("tenant", "blue")
                        .AddClaim("scope", "read"));
            }
            if (key == "admin") {
                return AuthenticationResult::Success(
                    ClaimsPrincipal::Authenticated("admin", "ApiKey")
                        .AddRole("admin")
                        .AddClaim("tenant", "blue"));
            }
            if (key == "reader") {
                return AuthenticationResult::Success(
                    ClaimsPrincipal::Authenticated("reader", "ApiKey")
                        .AddRole("reader")
                        .AddClaim("tenant", "blue")
                        .AddClaim("scope", "read"));
            }
            if (key == "ops-audit") {
                return AuthenticationResult::Success(
                    ClaimsPrincipal::Authenticated("ops-audit", "ApiKey")
                        .AddRole("ops")
                        .AddClaim("tenant", "blue")
                        .AddClaim("scope", "audit"));
            }
            return AuthenticationResult::Fail("Invalid API key.", "ApiKey");
        },
        "ApiKey"
    });
    app.UseAuthentication(authentication);
    app.UseAuthorization();

    auto admin = app.MapGroup("/api/admin")
        .RequireRoles({ "admin", "ops" })
        .RequireClaim("tenant", "blue");

    admin.MapGet("/reports", [](HttpContext&) {
        return Results::Ok("reports");
    }).RequireClaim("scope", "read");

    admin.MapGet("/status", [] {
        return Results::Ok("public");
    }).AllowAnonymous();

    auto audit = admin.MapGroup("/audit").RequireClaim("scope", "audit");
    audit.MapGet("/events", [](HttpContext&) {
        return Results::Ok("events");
    });

    HttpResponse anonymous = Send(app, "GET", "/api/admin/reports");
    RequireEqual(401, anonymous.StatusCode(), "group convenience auth should challenge anonymous callers");

    HttpRequest missingClaimRequest;
    missingClaimRequest.Method("GET").Path("/api/admin/reports").Header("X-API-Key", "admin");
    HttpResponse missingClaim = app.Handle(std::move(missingClaimRequest));
    RequireEqual(403, missingClaim.StatusCode(), "endpoint convenience claim should be required");

    HttpRequest missingRoleRequest;
    missingRoleRequest.Method("GET").Path("/api/admin/reports").Header("X-API-Key", "reader");
    HttpResponse missingRole = app.Handle(std::move(missingRoleRequest));
    RequireEqual(403, missingRole.StatusCode(), "group convenience roles should be required");

    HttpRequest authorizedRequest;
    authorizedRequest.Method("GET").Path("/api/admin/reports").Header("X-API-Key", "admin-read");
    HttpResponse authorized = app.Handle(std::move(authorizedRequest));
    RequireEqual(200, authorized.StatusCode(), "group and endpoint convenience auth should allow matching callers");
    RequireEqual("reports", authorized.BodyText(), "group and endpoint convenience auth body");

    HttpResponse publicResponse = Send(app, "GET", "/api/admin/status");
    RequireEqual(200, publicResponse.StatusCode(), "allow anonymous should override convenience auth");
    RequireEqual("public", publicResponse.BodyText(), "allow anonymous convenience body");

    HttpRequest auditRequest;
    auditRequest.Method("GET").Path("/api/admin/audit/events").Header("X-API-Key", "ops-audit");
    HttpResponse auditResponse = app.Handle(std::move(auditRequest));
    RequireEqual(200, auditResponse.StatusCode(), "nested group convenience auth should inherit and add requirements");
    RequireEqual("events", auditResponse.BodyText(), "nested group convenience auth body");

    HttpRequest auditMissingScopeRequest;
    auditMissingScopeRequest.Method("GET").Path("/api/admin/audit/events").Header("X-API-Key", "admin-read");
    HttpResponse auditMissingScope = app.Handle(std::move(auditMissingScopeRequest));
    RequireEqual(403, auditMissingScope.StatusCode(), "nested group convenience auth should require child claims");

    RequireThrowsWithMessage([] {
        WebApplication badApp = WebApplication::Create();
        badApp.MapGet("/bad", [] {
            return Results::Ok();
        }).RequireRole(" ");
    }, "role", "endpoint convenience role should reject empty values");
}

void AuthenticationOptionsValidateUnsafeConfiguration() {
    auto handler = [](HttpContext&) {
        return AuthenticationResult::NoResult();
    };

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ " ApiKey", handler, "ApiKey" });
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "whitespace", "authentication scheme names should reject leading whitespace");

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ "ApiKey\r\nBad", handler, "ApiKey" });
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "control", "authentication scheme names should reject control characters");

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ "ApiKey", handler, "ApiKey\r\nBad" });
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "Challenge", "authentication challenges should reject control characters");

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ "ApiKey", handler, "ApiKey" });
        authentication.UserItemKey = "User\nBad";
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "UserItemKey", "authentication user item key should reject control characters");

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ "ApiKey", handler, "ApiKey" });
        authentication.FailureItemKey = "Failure\r\nBad";
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "FailureItemKey", "authentication failure item key should reject control characters");

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ "ApiKey", handler, "ApiKey" });
        authentication.ChallengeItemKey = "Challenge\nBad";
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "ChallengeItemKey", "authentication challenge item key should reject control characters");

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ "ApiKey", handler, "ApiKey" });
        authentication.DefaultScheme = "ApiKey ";
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "DefaultScheme", "default authentication scheme should reject trailing whitespace");

    RequireThrowsWithMessage([&] {
        AuthenticationOptions authentication;
        authentication.Schemes.push_back(AuthenticationScheme{ "ApiKey", handler, "ApiKey" });
        authentication.Schemes.push_back(AuthenticationScheme{ "apikey", handler, "ApiKey" });
        WebApplication app = WebApplication::Create();
        app.UseAuthentication(authentication);
    }, "Duplicate authentication scheme", "authentication schemes should reject case-insensitive duplicates");
}

void AuthorizationOptionsValidateUnsafeConfiguration() {
    RequireThrowsWithMessage([] {
        AuthorizationOptions authorization;
        authorization.UserItemKey = "User\nBad";
        WebApplication app = WebApplication::Create();
        app.UseAuthorization(authorization);
    }, "UserItemKey", "authorization user item key should reject control characters");

    RequireThrowsWithMessage([] {
        AuthorizationOptions authorization;
        authorization.FailureItemKey = "Failure\r\nBad";
        WebApplication app = WebApplication::Create();
        app.UseAuthorization(authorization);
    }, "FailureItemKey", "authorization failure item key should reject control characters");

    RequireThrowsWithMessage([] {
        AuthorizationOptions authorization;
        authorization.AuthenticationChallengeItemKey = "Challenge\nBad";
        WebApplication app = WebApplication::Create();
        app.UseAuthorization(authorization);
    }, "AuthenticationChallengeItemKey", "authorization challenge item key should reject control characters");

    RequireThrowsWithMessage([] {
        AuthorizationOptions authorization;
        AuthorizationPolicy policy;
        policy.Name = "admin";
        policy.RequiredRoles = { " " };
        authorization.Policies.push_back(std::move(policy));
        WebApplication app = WebApplication::Create();
        app.UseAuthorization(authorization);
    }, "RequiredRoles", "authorization roles should reject empty values");

    RequireThrowsWithMessage([] {
        AuthorizationOptions authorization;
        AuthorizationPolicy policy;
        policy.Name = "admin\r\nX-Test: bad";
        authorization.Policies.push_back(std::move(policy));
        WebApplication app = WebApplication::Create();
        app.UseAuthorization(authorization);
    }, "control", "authorization policy names should reject control characters");

    RequireThrowsWithMessage([] {
        AuthorizationOptions authorization;
        AuthorizationPolicy policy;
        policy.Name = "claims";
        policy.RequiredClaims.push_back({ "scope\r\nX-Test: bad", "read" });
        authorization.Policies.push_back(std::move(policy));
        WebApplication app = WebApplication::Create();
        app.UseAuthorization(authorization);
    }, "control", "authorization claim types should reject control characters");

    RequireThrowsWithMessage([] {
        AuthorizationOptions authorization;
        authorization.DefaultPolicy.RequiredRoles = { "admin\nops" };
        WebApplication app = WebApplication::Create();
        app.UseAuthorization(authorization);
    }, "control", "default authorization roles should reject control characters");
}

void EndpointFiltersCanWrapAndShortCircuitHandlers() {
    WebApplication app = WebApplication::Create();

    int groupBefore = 0;
    int groupAfter = 0;
    int endpointBefore = 0;
    int endpointAfter = 0;
    int itemHandlerCalls = 0;
    int blockedHandlerCalls = 0;

    auto api = app.MapGroup("/api").AddEndpointFilter([&](HttpContext& ctx, RequestDelegate next) {
        ++groupBefore;
        HttpResult result = next(ctx);
        ++groupAfter;
        result.Header("X-Group-Filter", "yes");
        return result;
    });

    api.MapGet("/items/{id}", [&](HttpContext& ctx) {
        ++itemHandlerCalls;
        return Results::Text("item-" + ctx.Request().RouteValue("id"));
    }).AddEndpointFilter([&](HttpContext& ctx, RequestDelegate next) {
        ++endpointBefore;
        HttpResult result = next(ctx);
        ++endpointAfter;
        result.Header("X-Endpoint-Filter", ctx.Request().RouteValue("id"));
        return result;
    });

    api.MapGet("/blocked", [&](HttpContext&) {
        ++blockedHandlerCalls;
        return Results::Ok("blocked");
    }).AddEndpointFilter([](HttpContext&, RequestDelegate) {
        return Results::StatusCode(418).Header("X-Short-Circuit", "true");
    });

    auto nested = api.MapGroup("/v1").AddEndpointFilter([](HttpContext& ctx, RequestDelegate next) {
        HttpResult result = next(ctx);
        result.Header("X-Nested-Filter", ctx.Request().RouteValue("id", "none"));
        return result;
    });
    nested.MapGet("/items/{id}", [](HttpContext& ctx) {
        return Results::Text("nested-" + ctx.Request().RouteValue("id"));
    });

    HttpResponse item = Send(app, "GET", "/api/items/42");
    RequireEqual(200, item.StatusCode(), "endpoint filter item status");
    RequireEqual("item-42", item.BodyText(), "endpoint filter item body");
    RequireEqual("yes", item.Headers().Get("X-Group-Filter"), "endpoint filter group header");
    RequireEqual("42", item.Headers().Get("X-Endpoint-Filter"), "endpoint filter endpoint header");
    RequireEqual(1, groupBefore, "group filter before count after first request");
    RequireEqual(1, groupAfter, "group filter after count after first request");
    RequireEqual(1, endpointBefore, "endpoint filter before count after first request");
    RequireEqual(1, endpointAfter, "endpoint filter after count after first request");
    RequireEqual(1, itemHandlerCalls, "endpoint filter handler count after first request");

    HttpResponse blocked = Send(app, "GET", "/api/blocked");
    RequireEqual(418, blocked.StatusCode(), "endpoint filter short circuit status");
    RequireEqual("true", blocked.Headers().Get("X-Short-Circuit"), "endpoint filter short circuit header");
    RequireEqual("yes", blocked.Headers().Get("X-Group-Filter"), "group filter should wrap short circuit");
    RequireEqual(0, blockedHandlerCalls, "short circuit should skip handler");

    HttpResponse nestedResponse = Send(app, "GET", "/api/v1/items/7");
    RequireEqual(200, nestedResponse.StatusCode(), "nested endpoint filter status");
    RequireEqual("nested-7", nestedResponse.BodyText(), "nested endpoint filter body");
    RequireEqual("yes", nestedResponse.Headers().Get("X-Group-Filter"), "nested endpoint should inherit parent filter");
    RequireEqual("7", nestedResponse.Headers().Get("X-Nested-Filter"), "nested endpoint should apply child filter");
}

void AcceptsMetadataValidatesRequestContentType() {
    WebApplication app = WebApplication::Create();
    int jsonCalls = 0;
    int optionalCalls = 0;
    int wildcardCalls = 0;

    app.MapPost("/api/json", [&](HttpContext&) {
        ++jsonCalls;
        return Results::Ok("json");
    }).Accepts("application/json");

    app.MapPost("/api/optional", [&](HttpContext&) {
        ++optionalCalls;
        return Results::Ok("optional");
    }).Accepts("application/json", false);

    app.MapPost("/api/wildcard", [&](HttpContext&) {
        ++wildcardCalls;
        return Results::Ok("wildcard");
    }).Accepts("application/*");

    HttpRequest validJsonRequest;
    validJsonRequest.Method("POST")
        .Path("/api/json")
        .Header("Content-Type", "application/json; charset=utf-8")
        .Body("{}");
    HttpResponse validJson = app.Handle(std::move(validJsonRequest));
    RequireEqual(200, validJson.StatusCode(), "accepts valid json status");
    RequireEqual(1, jsonCalls, "accepts valid json should call handler");

    HttpRequest missingRequest;
    missingRequest.Method("POST").Path("/api/json").Body("{}");
    HttpResponse missing = app.Handle(std::move(missingRequest));
    JsonElement missingBody = ParseJsonBody(missing);
    RequireEqual(415, missing.StatusCode(), "accepts missing content type status");
    RequireEqual("Unsupported Media Type", missingBody.GetProperty("title").GetString(), "accepts missing content type title");
    RequireEqual(415, missingBody.GetProperty("status").GetInt32(), "accepts missing content type problem status");
    Require(missingBody.GetProperty("detail").GetString().find("application/json") != std::string::npos, "accepts missing detail should name supported type");
    RequireEqual(1, jsonCalls, "accepts missing content type should not call handler");

    HttpRequest unsupportedRequest;
    unsupportedRequest.Method("POST")
        .Path("/api/json")
        .Header("Content-Type", "text/plain")
        .Body("{}");
    HttpResponse unsupported = app.Handle(std::move(unsupportedRequest));
    JsonElement unsupportedBody = ParseJsonBody(unsupported);
    RequireEqual(415, unsupported.StatusCode(), "accepts unsupported content type status");
    Require(unsupportedBody.GetProperty("detail").GetString().find("text/plain") != std::string::npos, "accepts unsupported detail should name actual type");
    RequireEqual(1, jsonCalls, "accepts unsupported content type should not call handler");

    HttpRequest optionalMissingRequest;
    optionalMissingRequest.Method("POST").Path("/api/optional").Body("{}");
    HttpResponse optionalMissing = app.Handle(std::move(optionalMissingRequest));
    RequireEqual(200, optionalMissing.StatusCode(), "optional accepts missing content type status");
    RequireEqual(1, optionalCalls, "optional accepts missing content type should call handler");

    HttpRequest optionalUnsupportedRequest;
    optionalUnsupportedRequest.Method("POST")
        .Path("/api/optional")
        .Header("Content-Type", "text/plain")
        .Body("{}");
    HttpResponse optionalUnsupported = app.Handle(std::move(optionalUnsupportedRequest));
    RequireEqual(415, optionalUnsupported.StatusCode(), "optional accepts unsupported content type status");
    RequireEqual(1, optionalCalls, "optional accepts unsupported content type should not call handler");

    HttpRequest wildcardRequest;
    wildcardRequest.Method("POST")
        .Path("/api/wildcard")
        .Header("Content-Type", "application/problem+json")
        .Body("{}");
    HttpResponse wildcard = app.Handle(std::move(wildcardRequest));
    RequireEqual(200, wildcard.StatusCode(), "wildcard accepts status");
    RequireEqual(1, wildcardCalls, "wildcard accepts should call handler");
}

void ProducesMetadataValidatesAcceptHeader() {
    WebApplication app = WebApplication::Create();
    int jsonCalls = 0;
    int textCalls = 0;
    int noContentCalls = 0;

    app.MapGet("/api/json", [&] {
        ++jsonCalls;
        return Results::Json(42);
    }).Produces(200, "application/json");

    app.MapGet("/api/text", [&] {
        ++textCalls;
        return Results::Text("ok");
    }).Produces(200, "text/plain");

    app.MapDelete("/api/no-content", [&] {
        ++noContentCalls;
        return Results::NoContent();
    }).Produces(204);

    HttpResponse noAccept = Send(app, "GET", "/api/json");
    RequireEqual(200, noAccept.StatusCode(), "produces missing accept status");
    RequireEqual(1, jsonCalls, "produces missing accept should call handler");

    HttpRequest wildcardRequest;
    wildcardRequest.Method("GET")
        .Path("/api/json")
        .Header("Accept", "application/*");
    HttpResponse wildcard = app.Handle(std::move(wildcardRequest));
    RequireEqual(200, wildcard.StatusCode(), "produces wildcard accept status");
    RequireEqual(2, jsonCalls, "produces wildcard accept should call handler");

    HttpRequest unsupportedRequest;
    unsupportedRequest.Method("GET")
        .Path("/api/json")
        .Header("Accept", "text/plain");
    HttpResponse unsupported = app.Handle(std::move(unsupportedRequest));
    JsonElement unsupportedBody = ParseJsonBody(unsupported);
    RequireEqual(406, unsupported.StatusCode(), "produces unsupported accept status");
    RequireEqual("Not Acceptable", unsupportedBody.GetProperty("title").GetString(), "produces unsupported accept title");
    RequireEqual(406, unsupportedBody.GetProperty("status").GetInt32(), "produces unsupported accept problem status");
    Require(unsupportedBody.GetProperty("detail").GetString().find("application/json") != std::string::npos, "produces unsupported detail should name supported type");
    RequireEqual(2, jsonCalls, "produces unsupported accept should not call handler");

    HttpRequest rejectedSpecificRequest;
    rejectedSpecificRequest.Method("GET")
        .Path("/api/json")
        .Header("Accept", "application/json;q=0, */*;q=1");
    HttpResponse rejectedSpecific = app.Handle(std::move(rejectedSpecificRequest));
    RequireEqual(406, rejectedSpecific.StatusCode(), "produces q0 specific accept status");
    RequireEqual(2, jsonCalls, "produces q0 specific accept should not call handler");

    HttpRequest textRequest;
    textRequest.Method("GET")
        .Path("/api/text")
        .Header("Accept", "text/*;q=0.5");
    HttpResponse text = app.Handle(std::move(textRequest));
    RequireEqual(200, text.StatusCode(), "produces text wildcard accept status");
    RequireEqual(1, textCalls, "produces text wildcard accept should call handler");

    HttpRequest noContentRequest;
    noContentRequest.Method("DELETE")
        .Path("/api/no-content")
        .Header("Accept", "application/xml");
    HttpResponse noContent = app.Handle(std::move(noContentRequest));
    RequireEqual(204, noContent.StatusCode(), "produces no-content accept status");
    RequireEqual(1, noContentCalls, "produces no-content should ignore accept body negotiation");
}

void BodylessStatusesSuppressBodiesAndContentLength() {
    WebApplication app = WebApplication::Create();
    app.MapGet("/api/no-content", [] {
        return Results::Text("should not be sent", 204)
            .Header("Content-Length", "999");
    });
    app.MapGet("/api/not-modified", [] {
        return Results::Text("cached body", 304)
            .Header("Content-Length", "999")
            .Header("ETag", "\"v1\"");
    });

    HttpResponse noContent = Send(app, "GET", "/api/no-content");
    RequireEqual(204, noContent.StatusCode(), "bodyless 204 status");
    Require(noContent.Body().empty(), "204 body should be suppressed");
    Require(noContent.Headers().Get("Content-Length").empty(), "204 content length should be omitted");

    HttpResponse notModified = Send(app, "GET", "/api/not-modified");
    RequireEqual(304, notModified.StatusCode(), "bodyless 304 status");
    Require(notModified.Body().empty(), "304 body should be suppressed");
    Require(notModified.Headers().Get("Content-Length").empty(), "304 content length should be omitted");
    RequireEqual("\"v1\"", notModified.Headers().Get("ETag"), "304 etag should remain");
}

void OpenApiEndpointWorksInMemory() {
    WebApplication app = WebApplication::Create();
    OpenApiParameter correlation;
    correlation.Name = "X-Correlation-ID";
    correlation.In = "header";
    correlation.SchemaType = "string";
    correlation.SchemaFormat = "uuid";
    correlation.Description = "Correlation id";
    app.MapGet("/api/ping", [] {
        return Results::Ok("pong");
    }).WithTags({ "Ops" })
        .WithParameter(correlation)
        .WithQueryArrayParameter("tag", "string", false, "Repeated tags")
        .WithHeaderArrayParameter("X-Shard-ID", "integer", false, {}, "int32");
    app.MapPost("/api/items", [] {
        return Results::Ok();
    }).Accepts("application/json").Produces(201, "application/json");
    app.MapHead("/api/items", [] {
        return Results::Ok().Header("X-Head", "items");
    }).Produces(200);
    app.MapOptions("/api/items", [] {
        return Results::NoContent();
    }).Produces(204);
    app.MapDelete("/api/items/{id}", [] {
        return Results::NoContent();
    }).Produces(204);
    app.MapAny("/api/any", [] {
        return Results::Ok();
    });
    app.MapOpenApi();

    HttpResponse response = Send(app, "GET", "/openapi.json");
    if (response.StatusCode() != 200) {
        throw TestFailure("OpenAPI status expected 200 actual " + std::to_string(response.StatusCode()) + " body " + response.BodyText());
    }

    JsonElement root = ParseJsonBody(response);
    JsonElement paths = root.GetProperty("paths");
    Require(paths.HasProperty("/api/ping"), "OpenAPI should include /api/ping");
    JsonElement operation = paths.GetProperty("/api/ping").GetProperty("get");
    RequireEqual("Ops", operation.GetProperty("tags")[0].GetString(), "OpenAPI should include endpoint tags");
    JsonElement correlationSchema = operation.GetProperty("parameters")[0].GetProperty("schema");
    RequireEqual("uuid", correlationSchema.GetProperty("format").GetString(), "OpenAPI should include manual parameter format");
    JsonElement tagSchema = operation.GetProperty("parameters")[1].GetProperty("schema");
    RequireEqual("array", tagSchema.GetProperty("type").GetString(), "OpenAPI query array parameter schema type");
    RequireEqual("string", tagSchema.GetProperty("items").GetProperty("type").GetString(), "OpenAPI query array item type");
    JsonElement shardSchema = operation.GetProperty("parameters")[2].GetProperty("schema");
    RequireEqual("array", shardSchema.GetProperty("type").GetString(), "OpenAPI header array parameter schema type");
    RequireEqual("integer", shardSchema.GetProperty("items").GetProperty("type").GetString(), "OpenAPI header array item type");
    RequireEqual("int32", shardSchema.GetProperty("items").GetProperty("format").GetString(), "OpenAPI header array item format");

    JsonElement createOperation = paths.GetProperty("/api/items").GetProperty("post");
    JsonElement createResponses = createOperation.GetProperty("responses");
    Require(createOperation.HasProperty("requestBody"), "OpenAPI should include accepted request body");
    Require(createResponses.HasProperty("201"), "OpenAPI should include declared success response");
    Require(createResponses.HasProperty("406"), "OpenAPI should include not acceptable response for produced content");
    Require(createResponses.HasProperty("415"), "OpenAPI should include unsupported media type for accepted content");
    Require(paths.GetProperty("/api/items").HasProperty("head"), "OpenAPI should include explicit HEAD operation");
    Require(paths.GetProperty("/api/items").HasProperty("options"), "OpenAPI should include explicit OPTIONS operation");

    JsonElement deleteResponses = paths.GetProperty("/api/items/{id}").GetProperty("delete").GetProperty("responses");
    Require(deleteResponses.HasProperty("204"), "OpenAPI should include no-content response");
    Require(!deleteResponses.HasProperty("406"), "OpenAPI should not add 406 for no-body responses");

    JsonElement anyPath = paths.GetProperty("/api/any");
    Require(anyPath.HasProperty("get"), "OpenAPI MapAny should include GET");
    Require(anyPath.HasProperty("head"), "OpenAPI MapAny should include HEAD");
    Require(anyPath.HasProperty("options"), "OpenAPI MapAny should include OPTIONS");
}

void BuiltInEndpointAndOpenApiOptionsValidateConfiguration() {
    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        MetricsEndpointOptions options;
        options.Path = "https://example.com/metrics";
        app.MapMetrics(options);
    }, "MetricsEndpointOptions::Path", "metrics endpoint path should reject absolute URLs");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        HealthCheckOptions options;
        options.Path = "/health?ready=1";
        app.MapHealthChecks(options);
    }, "query string", "health endpoint path should reject query strings");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        HealthCheckOptions options;
        options.FailureStatusCode = 200;
        app.MapHealthChecks(options);
    }, "FailureStatusCode", "health endpoint failure status code should reject success values");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.AddHealthCheck("db\nprimary", [] {
            return HealthCheckResult::Healthy();
        });
    }, "control", "health check names should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.AddHealthCheck("db", [] {
            return HealthCheckResult::Healthy();
        }, { "ready\r\nspoofed" });
    }, "control", "health check tags should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        HealthCheckOptions options;
        options.Tags = { "ready\nbad" };
        app.MapHealthChecks(options);
    }, "control", "health endpoint tags should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/bad-header", [] {
            return Results::Ok();
        }).WithHeaderParameter("Bad Header", "string");
    }, "header name", "OpenAPI header parameters should validate header names");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/bad-array-header", [] {
            return Results::Ok();
        }).WithHeaderArrayParameter("Bad Header", "string");
    }, "header name", "OpenAPI header array parameters should validate header names");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        OpenApiParameter parameter;
        parameter.Name = "tag";
        parameter.In = "query";
        parameter.IsArray = true;
        parameter.ItemsSchemaType = "string\r\nbad";
        app.MapGet("/api/bad-array-parameter", [] {
            return Results::Ok();
        }).WithParameter(parameter);
    }, "ItemsSchemaType", "OpenAPI array item schema type should reject control characters");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        app.MapGet("/api/bad-produces", [] {
            return Results::Ok();
        }).Produces(200, "bad media type");
    }, "media type", "Produces should validate response content type");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        OpenApiOptions options;
        options.Info.Title = " ";
        app.MapOpenApi(options);
    }, "Info.Title", "OpenAPI title should be required");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        OpenApiOptions options;
        OpenApiSecurityScheme scheme;
        scheme.Name = "ApiKey";
        scheme.Type = "apiKey";
        scheme.In = "header";
        scheme.ParameterName = "Bad Header";
        options.SecuritySchemes.push_back(std::move(scheme));
        app.MapOpenApi(options);
    }, "ParameterName", "OpenAPI apiKey header parameter should validate header names");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        OpenApiOptions options;
        OpenApiSecurityScheme scheme;
        scheme.Name = "ApiKey";
        options.SecuritySchemes.push_back(std::move(scheme));
        options.DefaultSecurityScheme = "Missing";
        app.MapOpenApi(options);
    }, "DefaultSecurityScheme", "OpenAPI default security scheme should reference a registered scheme");

    RequireThrowsWithMessage([] {
        WebApplication app = WebApplication::Create();
        SwaggerUiOptions options;
        options.OpenApiPath = "//cdn.example.com/openapi.json";
        app.MapSwaggerUi(options);
    }, "SwaggerUiOptions::OpenApiPath", "Swagger UI OpenAPI path should be application-relative");
}

void HealthChecksCanBeFilteredByTags() {
    WebApplication app = WebApplication::Create();
    int liveCalls = 0;
    int readyCalls = 0;

    app.AddHealthCheck("self", [&liveCalls] {
        ++liveCalls;
        return HealthCheckResult::Healthy("process is responsive");
    }, { "live", "LIVE", " " });
    app.AddHealthCheck("database", [&readyCalls] {
        ++readyCalls;
        return HealthCheckResult::Unhealthy("database is unavailable");
    }, { "ready" });

    HealthCheckOptions live;
    live.Path = "/health/live";
    live.IncludeDetails = true;
    live.Tags = { "live" };
    app.MapHealthChecks(live);

    HealthCheckOptions ready;
    ready.Path = "/health/ready";
    ready.IncludeDetails = true;
    ready.Tags = { "ready" };
    app.MapHealthChecks(ready);

    HealthCheckOptions customFailure;
    customFailure.Path = "/health/custom-failure";
    customFailure.Tags = { "ready" };
    customFailure.FailureStatusCode = 429;
    app.MapHealthChecks(customFailure);

    HealthCheckOptions minimal;
    minimal.Path = "/health/minimal";
    minimal.IncludeApplicationState = false;
    minimal.Tags = { "live" };
    app.MapHealthChecks(minimal);

    HttpResponse liveResponse = Send(app, "GET", "/health/live");
    JsonElement liveBody = ParseJsonBody(liveResponse);
    JsonElement liveChecks = liveBody.GetProperty("checks");

    RequireEqual(200, liveResponse.StatusCode(), "live health status");
    RequireEqual("Healthy", liveBody.GetProperty("status").GetString(), "live health body status");
    Require(!liveBody.GetProperty("isRunning").GetBoolean(), "live health should include running state");
    Require(!liveBody.GetProperty("isStopping").GetBoolean(), "live health should include stopping state");
    RequireEqual(0, liveBody.GetProperty("activeRequests").GetInt32(), "live health should include active request count");
    Require(liveChecks.HasProperty("self"), "live health should include self check");
    Require(!liveChecks.HasProperty("database"), "live health should not include readiness check");
    RequireEqual(1, liveChecks.GetProperty("self").GetProperty("tags").GetArrayLength(), "live tags should be normalized");
    RequireEqual("live", liveChecks.GetProperty("self").GetProperty("tags")[0].GetString(), "live tag value");

    HttpResponse readyResponse = Send(app, "GET", "/health/ready");
    JsonElement readyBody = ParseJsonBody(readyResponse);
    JsonElement readyChecks = readyBody.GetProperty("checks");

    RequireEqual(503, readyResponse.StatusCode(), "ready health status");
    RequireEqual("Unhealthy", readyBody.GetProperty("status").GetString(), "ready health body status");
    Require(!readyChecks.HasProperty("self"), "ready health should not include live check");
    Require(readyChecks.HasProperty("database"), "ready health should include database check");

    HttpResponse customFailureResponse = Send(app, "GET", "/health/custom-failure");
    JsonElement customFailureBody = ParseJsonBody(customFailureResponse);
    RequireEqual(429, customFailureResponse.StatusCode(), "custom failure health status");
    RequireEqual("Unhealthy", customFailureBody.GetProperty("status").GetString(), "custom failure health body status");

    HttpResponse minimalResponse = Send(app, "GET", "/health/minimal");
    JsonElement minimalBody = ParseJsonBody(minimalResponse);
    RequireEqual(200, minimalResponse.StatusCode(), "minimal health status");
    Require(!minimalBody.HasProperty("isRunning"), "minimal health should omit running state");
    Require(!minimalBody.HasProperty("isStopping"), "minimal health should omit stopping state");
    Require(!minimalBody.HasProperty("activeRequests"), "minimal health should omit active request count");

    RequireEqual(2, liveCalls, "live check execution count");
    RequireEqual(2, readyCalls, "ready check execution count");
}

void MetricsEndpointWorksInMemory() {
    WebApplication app = WebApplication::Create();
    app.UseMetrics();
    app.MapMetrics();
    app.MapGet("/api/ping", [] {
        return Results::Ok("pong");
    });
    app.MapGet("/api/head", [] {
        return Results::Text("head-body");
    });
    app.MapGet("/api/boom", []() -> HttpResult {
        throw std::runtime_error("metrics failure");
    });

    HttpResponse ping = Send(app, "GET", "/api/ping");
    RequireEqual(200, ping.StatusCode(), "metrics setup request status");
    HttpResponse head = Send(app, "HEAD", "/api/head");
    RequireEqual(200, head.StatusCode(), "metrics setup head status");
    Require(head.Body().empty(), "metrics setup head body");
    HttpResponse missing = Send(app, "GET", "/api/missing");
    RequireEqual(404, missing.StatusCode(), "metrics setup client error status");
    HttpResponse boom = Send(app, "GET", "/api/boom");
    RequireEqual(500, boom.StatusCode(), "metrics setup server error status");

    HttpResponse response = Send(app, "GET", "/metrics");
    JsonElement body = ParseJsonBody(response);

    RequireEqual(200, response.StatusCode(), "metrics endpoint status");
    Require(body.HasProperty("started"), "metrics should include started counter");
    Require(body.HasProperty("completed"), "metrics should include completed counter");
    Require(body.HasProperty("active"), "metrics should include active counter");
    RequireEqual(1, body.GetProperty("clientErrors").GetInt32(), "metrics should count 4xx responses");
    RequireEqual(1, body.GetProperty("serverErrors").GetInt32(), "metrics should count 5xx responses");
    RequireEqual(1, body.GetProperty("failed").GetInt32(), "metrics failed should count server errors");
    Require(body.HasProperty("isRunning"), "metrics should include application running state");
    Require(body.HasProperty("totalResponseBytes"), "metrics should include total response bytes");
    Require(body.HasProperty("maxResponseBytes"), "metrics should include max response bytes");
    Require(body.HasProperty("averageResponseBytes"), "metrics should include average response bytes");
    Require(body.GetProperty("totalResponseBytes").GetInt32() >= 4, "metrics should count non-empty response bytes");
    Require(body.GetProperty("maxResponseBytes").GetInt32() >= 4, "metrics should track max response bytes");
}

void StaticFilesHonorHeadConditionalAndRangeRequests() {
    TemporaryDirectory root;
    const std::string body = "hello static";
    {
        std::ofstream file(root.Path / "app.txt", std::ios::binary);
        file << body;
    }

    WebApplication app = WebApplication::Create();
    StaticFileOptions files;
    files.Root = root.Path;
    files.EnableSpaFallback = false;
    files.CacheControl = "public, max-age=60";
    app.UseStaticFiles(files);

    HttpResponse get = Send(app, "GET", "/app.txt");
    std::string etag = get.Headers().Get("ETag");

    RequireEqual(200, get.StatusCode(), "static GET status");
    RequireEqual(body, get.BodyText(), "static GET body");
    Require(!etag.empty(), "static GET should include ETag");
    Require(!get.Headers().Get("Last-Modified").empty(), "static GET should include Last-Modified");
    RequireEqual("bytes", get.Headers().Get("Accept-Ranges"), "static GET accept ranges");
    RequireEqual(std::to_string(body.size()), get.Headers().Get("Content-Length"), "static GET content length");
    RequireEqual("public, max-age=60", get.Headers().Get("Cache-Control"), "static GET cache control");

    HttpResponse head = Send(app, "HEAD", "/app.txt");
    RequireEqual(200, head.StatusCode(), "static HEAD status");
    Require(head.Body().empty(), "static HEAD body should be suppressed");
    RequireEqual(std::to_string(body.size()), head.Headers().Get("Content-Length"), "static HEAD content length");
    RequireEqual(etag, head.Headers().Get("ETag"), "static HEAD etag");

    HttpRequest conditionalRequest;
    conditionalRequest.Method("GET").Path("/app.txt").Header("If-None-Match", etag);
    HttpResponse conditional = app.Handle(std::move(conditionalRequest));
    RequireEqual(304, conditional.StatusCode(), "static conditional status");
    Require(conditional.Body().empty(), "static conditional body should be suppressed");
    RequireEqual(etag, conditional.Headers().Get("ETag"), "static conditional etag");

    HttpRequest rangeRequest;
    rangeRequest.Method("GET").Path("/app.txt").Header("Range", "bytes=6-11");
    HttpResponse range = app.Handle(std::move(rangeRequest));
    RequireEqual(206, range.StatusCode(), "static range status");
    RequireEqual("static", range.BodyText(), "static range body");
    RequireEqual("bytes 6-11/12", range.Headers().Get("Content-Range"), "static range header");
}

void StaticFilesBoundFullBodyBuffering() {
    TemporaryDirectory root;
    const std::string body = "0123456789abcdef";
    {
        std::ofstream file(root.Path / "large.txt", std::ios::binary);
        file << body;
    }

    StaticFileOptions files;
    files.Root = root.Path;
    files.EnableSpaFallback = false;
    files.MaximumFileSizeBytes = 8;

    WebApplication app = WebApplication::Create();
    app.UseStaticFiles(files);

    HttpResponse full = Send(app, "GET", "/large.txt");
    RequireEqual(413, full.StatusCode(), "static file over maximum full GET status");
    Require(full.Body().empty(), "static file over maximum body should be empty");

    HttpResponse head = Send(app, "HEAD", "/large.txt");
    RequireEqual(200, head.StatusCode(), "static file HEAD should not require body buffering");
    Require(head.Body().empty(), "static file HEAD body should be empty");
    RequireEqual(std::to_string(body.size()), head.Headers().Get("Content-Length"), "static file HEAD should report full content length");

    HttpRequest smallRangeRequest;
    smallRangeRequest.Method("GET").Path("/large.txt").Header("Range", "bytes=0-3");
    HttpResponse smallRange = app.Handle(std::move(smallRangeRequest));
    RequireEqual(206, smallRange.StatusCode(), "static file small range status");
    RequireEqual("0123", smallRange.BodyText(), "static file small range body");
    RequireEqual("bytes 0-3/16", smallRange.Headers().Get("Content-Range"), "static file small range content range");

    HttpRequest largeRangeRequest;
    largeRangeRequest.Method("GET").Path("/large.txt").Header("Range", "bytes=0-12");
    HttpResponse largeRange = app.Handle(std::move(largeRangeRequest));
    RequireEqual(413, largeRange.StatusCode(), "static file large range should honor maximum body size");

    StaticFileOptions unlimited = files;
    unlimited.MaximumFileSizeBytes = 0;
    WebApplication unlimitedApp = WebApplication::Create();
    unlimitedApp.UseStaticFiles(unlimited);

    HttpResponse unlimitedFull = Send(unlimitedApp, "GET", "/large.txt");
    RequireEqual(200, unlimitedFull.StatusCode(), "static file unlimited full GET status");
    RequireEqual(body, unlimitedFull.BodyText(), "static file unlimited full GET body");
}

void StaticFileOptionsValidateUnsafeConfiguration() {
    TemporaryDirectory root;
    {
        std::ofstream file(root.Path / "data.custom", std::ios::binary);
        file << "custom";
    }

    StaticFileOptions valid;
    valid.Root = root.Path;
    valid.EnableSpaFallback = false;
    valid.ServeUnknownFileTypes = true;
    valid.UnknownFileContentType = "application/x-custom";
    valid.CacheControl = "public, max-age=5";

    WebApplication app = WebApplication::Create();
    app.UseStaticFiles(valid);
    HttpResponse response = Send(app, "GET", "/data.custom");
    RequireEqual(200, response.StatusCode(), "valid static file options status");
    RequireEqual("application/x-custom", response.ContentType(), "valid static file unknown content type");
    RequireEqual("public, max-age=5", response.Headers().Get("Cache-Control"), "valid static file cache control");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.CacheControl = "public\r\nX-Test: bad";
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "control", "static file cache-control should reject control characters");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.ServeUnknownFileTypes = true;
        options.UnknownFileContentType = "application/octet-stream\r\nX-Test: bad";
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "control", "static file unknown content type should reject control characters");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.DefaultFiles = { "../index.html" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "path segments", "static file default files should reject traversal segments");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.SpaFallbackFile = "../index.html";
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "path segments", "static file spa fallback should reject traversal segments");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.RequestPath = "/assets?version=1";
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "query string", "static file request path should reject query strings");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.SpaFallbackExcludedPrefixes = { "/api?ready=1" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "query string", "static file SPA excluded prefixes should reject query strings");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.SpaFallbackExcludedPrefixes = { "" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "cannot be empty", "static file SPA excluded prefixes should reject empty values");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.SpaFallbackExcludedPrefixes = { "https://example.com/api" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "application-relative", "static file SPA excluded prefixes should reject absolute URLs");

    RequireThrowsWithMessage([] {
        StaticFileOptions options;
        options.BlockedPathSegments = { ".git/config" };
        WebApplication badApp = WebApplication::Create();
        badApp.UseStaticFiles(options);
    }, "not a path", "static file blocked path segments should reject path patterns");
}

void StaticFilesRejectSensitiveAndHiddenPathsByDefault() {
    TemporaryDirectory root;
    std::filesystem::create_directories(root.Path / ".git");
    {
        std::ofstream file(root.Path / "index.html", std::ios::binary);
        file << "spa index";
    }
    {
        std::ofstream file(root.Path / "public.json", std::ios::binary);
        file << R"({"public":true})";
    }
    {
        std::ofstream file(root.Path / "appsettings.json", std::ios::binary);
        file << R"({"secret":true})";
    }
    {
        std::ofstream file(root.Path / ".env.local", std::ios::binary);
        file << "SECRET=1";
    }
    {
        std::ofstream file(root.Path / ".secret.txt", std::ios::binary);
        file << "hidden text";
    }
    {
        std::ofstream file(root.Path / "web.config", std::ios::binary);
        file << "<configuration />";
    }
    {
        std::ofstream file(root.Path / ".git" / "config", std::ios::binary);
        file << "repository metadata";
    }

    StaticFileOptions files;
    files.Root = root.Path;
    files.ServeUnknownFileTypes = true;
    files.EnableSpaFallback = true;

    WebApplication app = WebApplication::Create();
    app.UseStaticFiles(files);

    HttpResponse publicJson = Send(app, "GET", "/public.json");
    RequireEqual(200, publicJson.StatusCode(), "public static json status");
    RequireEqual(R"({"public":true})", publicJson.BodyText(), "public static json body");

    HttpResponse appSettings = Send(app, "GET", "/appsettings.json");
    RequireEqual(404, appSettings.StatusCode(), "appsettings static file should be blocked");

    HttpResponse env = Send(app, "GET", "/.env.local");
    RequireEqual(404, env.StatusCode(), "env static file should be blocked");

    HttpResponse dotFile = Send(app, "GET", "/.secret.txt");
    RequireEqual(404, dotFile.StatusCode(), "dot static file should be blocked");

    HttpResponse webConfig = Send(app, "GET", "/web.config");
    RequireEqual(404, webConfig.StatusCode(), "web.config static file should be blocked");

    HttpResponse gitConfig = Send(app, "GET", "/.git/config");
    RequireEqual(404, gitConfig.StatusCode(), "git metadata path should be blocked before SPA fallback");
    Require(gitConfig.BodyText() != "spa index", "blocked static path should not fall through to SPA fallback");

    HttpResponse apiMiss = Send(app, "GET", "/api/health");
    RequireEqual(404, apiMiss.StatusCode(), "default SPA fallback should exclude API prefix");
    Require(apiMiss.BodyText() != "spa index", "API misses should not return SPA HTML");

    HttpResponse apiPrefixBoundary = Send(app, "GET", "/apix/health");
    RequireEqual(200, apiPrefixBoundary.StatusCode(), "SPA excluded prefixes should be path-segment aware");
    RequireEqual("spa index", apiPrefixBoundary.BodyText(), "SPA excluded prefix should not match adjacent path text");

    StaticFileOptions relaxed = files;
    relaxed.ServeHiddenFiles = true;
    relaxed.BlockedFileNames.clear();
    relaxed.BlockedPathSegments.clear();
    relaxed.EnableSpaFallback = false;

    WebApplication relaxedApp = WebApplication::Create();
    relaxedApp.UseStaticFiles(relaxed);

    HttpResponse relaxedDot = Send(relaxedApp, "GET", "/.secret.txt");
    RequireEqual(200, relaxedDot.StatusCode(), "relaxed static dot file status");
    RequireEqual("hidden text", relaxedDot.BodyText(), "relaxed static dot file body");

    HttpResponse relaxedConfig = Send(relaxedApp, "GET", "/web.config");
    RequireEqual(200, relaxedConfig.StatusCode(), "relaxed static web.config status");
    RequireEqual("<configuration />", relaxedConfig.BodyText(), "relaxed static web.config body");
}

void ResponseCachingWorksInMemory() {
    WebApplication app = WebApplication::Create();

    ResponseCachingOptions caching;
    caching.CacheControl = "public, max-age=60";
    app.UseResponseCaching(caching);

    const std::string body = "cacheable api body";
    app.MapGet("/api/cacheable", [body] {
        return Results::Text(body);
    });
    app.MapGet("/api/auth", [] {
        return Results::Text("secret");
    });
    app.MapGet("/api/cookie", [] {
        HttpResult result = Results::Text("cookie");
        result.AppendCookie("session", "abc");
        return result;
    });

    HttpRequest firstRequest;
    firstRequest.Method("GET").Path("/api/cacheable");
    HttpResponse first = app.Handle(std::move(firstRequest));
    std::string etag = first.Headers().Get("ETag");

    RequireEqual(200, first.StatusCode(), "response caching first status");
    RequireEqual(body, first.BodyText(), "response caching first body");
    Require(!etag.empty(), "response caching should add etag");
    RequireEqual("public, max-age=60", first.Headers().Get("Cache-Control"), "response caching cache-control");

    HttpRequest conditionalRequest;
    conditionalRequest.Method("GET").Path("/api/cacheable").Header("If-None-Match", etag);
    HttpResponse conditional = app.Handle(std::move(conditionalRequest));
    RequireEqual(304, conditional.StatusCode(), "response caching conditional status");
    Require(conditional.Body().empty(), "response caching conditional body should be empty");
    RequireEqual(etag, conditional.Headers().Get("ETag"), "response caching conditional etag");
    RequireEqual("public, max-age=60", conditional.Headers().Get("Cache-Control"), "response caching conditional cache-control");

    HttpRequest headRequest;
    headRequest.Method("HEAD").Path("/api/cacheable");
    HttpResponse head = app.Handle(std::move(headRequest));
    RequireEqual(200, head.StatusCode(), "response caching head status");
    Require(head.Body().empty(), "response caching head body should be suppressed");
    RequireEqual(etag, head.Headers().Get("ETag"), "response caching head etag");
    RequireEqual(std::to_string(body.size()), head.Headers().Get("Content-Length"), "response caching head content length");

    HttpRequest authRequest;
    authRequest.Method("GET").Path("/api/auth").Header("Authorization", "Bearer token");
    HttpResponse auth = app.Handle(std::move(authRequest));
    RequireEqual(200, auth.StatusCode(), "response caching auth status");
    Require(auth.Headers().Get("ETag").empty(), "response caching should skip authenticated responses by default");

    HttpRequest cookieRequest;
    cookieRequest.Method("GET").Path("/api/cookie");
    HttpResponse cookie = app.Handle(std::move(cookieRequest));
    RequireEqual(200, cookie.StatusCode(), "response caching cookie status");
    Require(cookie.Headers().Get("ETag").empty(), "response caching should skip set-cookie responses by default");
}

void ResponseCompressionAndCachingOptionsValidateUnsafeConfiguration() {
    RequireThrowsWithMessage([] {
        ResponseCompressionOptions options;
        options.GzipLevel = 10;
        WebApplication app = WebApplication::Create();
        app.UseResponseCompression(options);
    }, "GzipLevel", "compression gzip level should reject values above zlib range");

    RequireThrowsWithMessage([] {
        ResponseCompressionOptions options;
        options.MimeTypes = { "text/plain", "" };
        WebApplication app = WebApplication::Create();
        app.UseResponseCompression(options);
    }, "cannot be empty", "compression mime types should reject empty entries");

    RequireThrowsWithMessage([] {
        ResponseCompressionOptions options;
        options.ExcludedMimeTypes = { "image/png\r\nX-Test: bad" };
        WebApplication app = WebApplication::Create();
        app.UseResponseCompression(options);
    }, "control", "compression excluded mime types should reject control characters");

    RequireThrowsWithMessage([] {
        ResponseCompressionOptions options;
        options.MimeTypes = { "text/plain, application/json" };
        WebApplication app = WebApplication::Create();
        app.UseResponseCompression(options);
    }, "single media type", "compression mime types should reject comma-separated entries");

    RequireThrowsWithMessage([] {
        ResponseCachingOptions options;
        options.CacheControl = "public\r\nX-Test: bad";
        WebApplication app = WebApplication::Create();
        app.UseResponseCaching(options);
    }, "control", "response caching cache-control should reject control characters");
}

void ResponseCompressionWorksInMemory() {
    WebApplication app = WebApplication::Create();

    ResponseCompressionOptions compression;
    compression.MinimumBodySize = 1;
    app.UseResponseCompression(compression);

    const std::string body(4096, 'a');
    app.MapGet("/api/data", [body](HttpContext&) {
        return Results::Text(body);
    });

    HttpRequest request;
    request.Method("GET").Path("/api/data").Header("Accept-Encoding", "gzip");
    HttpResponse response = app.Handle(std::move(request));

    RequireEqual(200, response.StatusCode(), "compression status");
    RequireEqual("gzip", response.Headers().Get("Content-Encoding"), "compression encoding");
    Require(response.Headers().Get("Vary").find("Accept-Encoding") != std::string::npos, "compression vary header");
    Require(response.Body().size() < body.size(), "compressed body should be smaller");
}

using TestCase = void (*)();

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, TestCase>> tests = {
        { "RouteParametersAndTextResultsWork", RouteParametersAndTextResultsWork },
        { "MethodMismatchReturns405WithAllow", MethodMismatchReturns405WithAllow },
        { "EndpointRegistrationValidatesRoutesMethodsAndHandlers", EndpointRegistrationValidatesRoutesMethodsAndHandlers },
        { "HeadRequestsUseGetEndpointWithoutBody", HeadRequestsUseGetEndpointWithoutBody },
        { "ExplicitHeadEndpointWinsOverImplicitGetHead", ExplicitHeadEndpointWinsOverImplicitGetHead },
        { "MapOptionsConvenienceWorks", MapOptionsConvenienceWorks },
        { "ParameterBindingReadsRouteAndQueryValues", ParameterBindingReadsRouteAndQueryValues },
        { "RouteConstraintsMatchBindAndDescribeParameters", RouteConstraintsMatchBindAndDescribeParameters },
        { "CookieBindingReadsRequestCookies", CookieBindingReadsRequestCookies },
        { "FormBindingReadsUrlEncodedBody", FormBindingReadsUrlEncodedBody },
        { "MultipartFormDataParsesFieldsAndFiles", MultipartFormDataParsesFieldsAndFiles },
        { "FormOptionsCanBeConfiguredAndLimitParsing", FormOptionsCanBeConfiguredAndLimitParsing },
        { "JsonBodyBindingReadsBodyAndProperties", JsonBodyBindingReadsBodyAndProperties },
        { "RequestDecompressionWorksInMemory", RequestDecompressionWorksInMemory },
        { "ParameterBindingFailuresReturnBadRequest", ParameterBindingFailuresReturnBadRequest },
        { "ProblemDetailsUsesConfiguredDefaults", ProblemDetailsUsesConfiguredDefaults },
        { "ProblemDetailsOptionsValidateUnsafeConfiguration", ProblemDetailsOptionsValidateUnsafeConfiguration },
        { "BuilderAndMiddlewareRejectEmptyCallbacks", BuilderAndMiddlewareRejectEmptyCallbacks },
        { "BuilderRejectsInvalidHttpSysOptions", BuilderRejectsInvalidHttpSysOptions },
        { "HttpSysBackpressureOptionsCanBeConfiguredFromJson", HttpSysBackpressureOptionsCanBeConfiguredFromJson },
        { "JsonConfigurationRejectsInvalidKnownValueTypes", JsonConfigurationRejectsInvalidKnownValueTypes },
        { "SystemExceptionsIncludeOperationAndErrorCodes", SystemExceptionsIncludeOperationAndErrorCodes },
        { "WindowsServiceOptionsValidateBeforeScmAccess", WindowsServiceOptionsValidateBeforeScmAccess },
        { "MachineConfigDeleteModesNeedOnlyTargetIdentity", MachineConfigDeleteModesNeedOnlyTargetIdentity },
        { "ConfigurationChangesAreRejectedAfterStart", ConfigurationChangesAreRejectedAfterStart },
        { "HostedServicesValidateRegistration", HostedServicesValidateRegistration },
        { "InMemoryHandleUsesProductionErrorBoundary", InMemoryHandleUsesProductionErrorBoundary },
        { "RequestUrlAndHeaderSizeLimitsReturnProblemDetails", RequestUrlAndHeaderSizeLimitsReturnProblemDetails },
        { "CommonResultsHelpersSetExpectedStatusHeadersAndBodies", CommonResultsHelpersSetExpectedStatusHeadersAndBodies },
        { "NamedEndpointPathGenerationWorks", NamedEndpointPathGenerationWorks },
        { "HeaderHelpersRejectUnsafeNamesAndValues", HeaderHelpersRejectUnsafeNamesAndValues },
        { "RequestIdMiddlewareHandlesIncomingIdsSafely", RequestIdMiddlewareHandlesIncomingIdsSafely },
        { "ServerTimingAddsHeadersAndValidatesOptions", ServerTimingAddsHeadersAndValidatesOptions },
        { "MatchedEndpointMetadataFlowsToFiltersLogsAndMetrics", MatchedEndpointMetadataFlowsToFiltersLogsAndMetrics },
        { "RequestLoggingRedactsQueryAndSanitizesDynamicFields", RequestLoggingRedactsQueryAndSanitizesDynamicFields },
        { "RateLimiterBoundsTrackedKeyCount", RateLimiterBoundsTrackedKeyCount },
        { "StatusCodeValidationRejectsInvalidValues", StatusCodeValidationRejectsInvalidValues },
        { "HostFilteringRejectsUnexpectedHosts", HostFilteringRejectsUnexpectedHosts },
        { "ForwardedHeadersValidateOptionsAndHostValues", ForwardedHeadersValidateOptionsAndHostValues },
        { "HttpsRedirectionRedirectsHttpRequests", HttpsRedirectionRedirectsHttpRequests },
        { "HstsAddsHeaderOnlyForHttpsRequests", HstsAddsHeaderOnlyForHttpsRequests },
        { "HstsOptionsValidateUnsafeConfiguration", HstsOptionsValidateUnsafeConfiguration },
        { "SecurityHeadersAddDefaultsAndValidateOptions", SecurityHeadersAddDefaultsAndValidateOptions },
        { "CorsCanRejectInvalidPreflightRequests", CorsCanRejectInvalidPreflightRequests },
        { "CorsOptionsValidateUnsafeConfiguration", CorsOptionsValidateUnsafeConfiguration },
        { "CorsWildcardWithCredentialsEchoesOrigin", CorsWildcardWithCredentialsEchoesOrigin },
        { "CookiesCanBeReadAppendedAndDeleted", CookiesCanBeReadAppendedAndDeleted },
        { "RouteGroupAppliesPrefixAuthorizationAndTags", RouteGroupAppliesPrefixAuthorizationAndTags },
        { "AuthorizationPolicyFluentBuildersRequireRolesAndClaims", AuthorizationPolicyFluentBuildersRequireRolesAndClaims },
        { "EndpointAndGroupAuthorizationConvenienceRequiresRolesAndClaims", EndpointAndGroupAuthorizationConvenienceRequiresRolesAndClaims },
        { "AuthenticationOptionsValidateUnsafeConfiguration", AuthenticationOptionsValidateUnsafeConfiguration },
        { "AuthorizationOptionsValidateUnsafeConfiguration", AuthorizationOptionsValidateUnsafeConfiguration },
        { "EndpointFiltersCanWrapAndShortCircuitHandlers", EndpointFiltersCanWrapAndShortCircuitHandlers },
        { "AcceptsMetadataValidatesRequestContentType", AcceptsMetadataValidatesRequestContentType },
        { "ProducesMetadataValidatesAcceptHeader", ProducesMetadataValidatesAcceptHeader },
        { "BodylessStatusesSuppressBodiesAndContentLength", BodylessStatusesSuppressBodiesAndContentLength },
        { "OpenApiEndpointWorksInMemory", OpenApiEndpointWorksInMemory },
        { "BuiltInEndpointAndOpenApiOptionsValidateConfiguration", BuiltInEndpointAndOpenApiOptionsValidateConfiguration },
        { "HealthChecksCanBeFilteredByTags", HealthChecksCanBeFilteredByTags },
        { "MetricsEndpointWorksInMemory", MetricsEndpointWorksInMemory },
        { "StaticFilesHonorHeadConditionalAndRangeRequests", StaticFilesHonorHeadConditionalAndRangeRequests },
        { "StaticFilesBoundFullBodyBuffering", StaticFilesBoundFullBodyBuffering },
        { "StaticFileOptionsValidateUnsafeConfiguration", StaticFileOptionsValidateUnsafeConfiguration },
        { "StaticFilesRejectSensitiveAndHiddenPathsByDefault", StaticFilesRejectSensitiveAndHiddenPathsByDefault },
        { "ResponseCachingWorksInMemory", ResponseCachingWorksInMemory },
        { "ResponseCompressionAndCachingOptionsValidateUnsafeConfiguration", ResponseCompressionAndCachingOptionsValidateUnsafeConfiguration },
        { "ResponseCompressionWorksInMemory", ResponseCompressionWorksInMemory },
    };

    int failed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": unknown exception" << '\n';
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed." << std::endl;
        return 1;
    }

    std::cout << tests.size() << " test(s) passed." << std::endl;
    return 0;
}
