#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <Capi/Web.h>

#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <ws2tcpip.h>
#include <http.h>
#include <winsvc.h>

#include <process.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <exception>
#include <thread>

#include <zlib.h>

#pragma comment(lib, "httpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace Capi::Web {

namespace {

constexpr HTTPAPI_VERSION HttpApiVersion{ 2, 0 };

using namespace System::Text::Json;

std::string FormatWin32Message(unsigned long errorCode) {
    LPSTR buffer = nullptr;
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);

    std::string message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
        LocalFree(buffer);
    } else {
        message = "Windows error " + std::to_string(errorCode);
    }
    return message;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        codePage = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (size <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed: " + FormatWin32Message(GetLastError()));
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed: " + FormatWin32Message(GetLastError()));
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string ToLowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string ToUpperAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return result;
}

std::string GenerateTraceIdentifier() {
    GUID guid{};
    if (CoCreateGuid(&guid) == S_OK) {
        wchar_t buffer[39]{};
        if (StringFromGUID2(guid, buffer, 39) > 0) {
            std::string value = ToLowerAscii(WideToUtf8(buffer));
            if (value.size() >= 2 && value.front() == '{' && value.back() == '}') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
    }

    static std::atomic<std::uint64_t> fallbackCounter{ 1 };
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "req-" + std::to_string(ticks) + "-" + std::to_string(fallbackCounter.fetch_add(1));
}

bool IsValidHeaderName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (unsigned char ch : name) {
        if (ch <= 32 || ch >= 127 || ch == ':' || ch == '\r' || ch == '\n') {
            return false;
        }
    }
    return true;
}

void ValidateHeaderName(std::string_view name, std::string_view optionName) {
    if (!IsValidHeaderName(name)) {
        throw std::invalid_argument(std::string(optionName) + " must be a non-empty HTTP header name.");
    }
}

std::string Trim(std::string value);
bool EqualsIgnoreCase(std::string_view left, std::string_view right);

std::string StripQuotes(std::string value) {
    value = Trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::string NormalizeForwardedAddress(std::string value) {
    value = StripQuotes(std::move(value));
    if (value.empty()) {
        return {};
    }
    if (value.front() == '[') {
        std::size_t close = value.find(']');
        if (close != std::string::npos) {
            return value.substr(1, close - 1);
        }
    }

    std::size_t firstColon = value.find(':');
    std::size_t lastColon = value.rfind(':');
    if (firstColon != std::string::npos && firstColon == lastColon) {
        std::string port = value.substr(firstColon + 1);
        if (!port.empty() && std::all_of(port.begin(), port.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            value = value.substr(0, firstColon);
        }
    }
    return value;
}

std::vector<std::string> SplitCommaSeparatedHeader(std::string_view value) {
    std::vector<std::string> result;
    std::size_t position = 0;
    while (position <= value.size()) {
        std::size_t comma = value.find(',', position);
        std::string token(value.substr(position, comma == std::string_view::npos ? std::string_view::npos : comma - position));
        token = StripQuotes(std::move(token));
        if (!token.empty()) {
            result.push_back(std::move(token));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        position = comma + 1;
    }
    return result;
}

std::optional<std::string> SelectForwardedValue(std::string_view headerValue, std::uint32_t forwardLimit) {
    std::vector<std::string> values = SplitCommaSeparatedHeader(headerValue);
    if (values.empty()) {
        return std::nullopt;
    }
    if (forwardLimit == 0 || forwardLimit >= values.size()) {
        return values.front();
    }
    return values[values.size() - forwardLimit];
}

bool ContainsUnsafeHeaderValueCharacters(std::string_view value) {
    return value.find('\r') != std::string_view::npos || value.find('\n') != std::string_view::npos;
}

std::string NormalizeForwardedScheme(std::string value) {
    value = ToLowerAscii(StripQuotes(std::move(value)));
    if (value.empty()) {
        return {};
    }
    for (unsigned char ch : value) {
        bool valid = std::isalnum(ch) != 0 || ch == '+' || ch == '-' || ch == '.';
        if (!valid) {
            return {};
        }
    }
    return value;
}

bool IsKnownProxy(std::string remoteAddress, const ForwardedHeadersOptions& options) {
    if (!options.RequireKnownProxy) {
        return true;
    }
    remoteAddress = NormalizeForwardedAddress(std::move(remoteAddress));
    for (const std::string& proxy : options.KnownProxies) {
        if (EqualsIgnoreCase(remoteAddress, NormalizeForwardedAddress(proxy))) {
            return true;
        }
    }
    return false;
}

std::string JoinStrings(const std::vector<std::string>& values, std::string_view separator) {
    std::string result;
    for (const std::string& value : values) {
        if (!result.empty()) {
            result.append(separator);
        }
        result.append(value);
    }
    return result;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool ContainsTokenIgnoreCase(const std::vector<std::string>& values, std::string_view token) {
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
        return EqualsIgnoreCase(value, token);
    });
}

bool StartsWithIgnoreCase(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && EqualsIgnoreCase(value.substr(0, prefix.size()), prefix);
}

std::string Trim(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

void Log(const Logger& logger, LogLevel level, std::string message, unsigned long errorCode = 0) {
    if (logger) {
        logger(LogMessage{ level, std::move(message), errorCode });
    }
}

std::string EnsureLeadingSlash(std::string path) {
    if (path.empty()) {
        return "/";
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

std::string NormalizeRoutePath(std::string path) {
    path = EnsureLeadingSlash(std::move(path));
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

bool IsPathUnderPrefix(std::string_view path, std::string_view prefix) {
    std::string normalizedPath = NormalizeRoutePath(std::string(path));
    std::string normalizedPrefix = NormalizeRoutePath(std::string(prefix));
    if (normalizedPrefix == "/") {
        return true;
    }
    return EqualsIgnoreCase(normalizedPath, normalizedPrefix) ||
        (StartsWithIgnoreCase(normalizedPath, normalizedPrefix) &&
            normalizedPath.size() > normalizedPrefix.size() &&
            normalizedPath[normalizedPrefix.size()] == '/');
}

std::vector<std::string> SplitPath(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') {
            ++pos;
        }
        if (pos >= path.size()) {
            break;
        }
        std::size_t next = path.find('/', pos);
        if (next == std::string_view::npos) {
            next = path.size();
        }
        segments.emplace_back(path.substr(pos, next - pos));
        pos = next;
    }
    return segments;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(std::string_view value, bool plusAsSpace) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        char ch = value[index];
        if (ch == '%' && index + 2 < value.size()) {
            int high = HexValue(value[index + 1]);
            int low = HexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        if (plusAsSpace && ch == '+') {
            result.push_back(' ');
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

ValueMap ParseQueryString(std::string_view queryString) {
    ValueMap values;
    if (!queryString.empty() && queryString.front() == '?') {
        queryString.remove_prefix(1);
    }
    std::size_t pos = 0;
    while (pos <= queryString.size()) {
        std::size_t amp = queryString.find('&', pos);
        if (amp == std::string_view::npos) {
            amp = queryString.size();
        }
        std::string_view pair = queryString.substr(pos, amp - pos);
        if (!pair.empty()) {
            std::size_t equals = pair.find('=');
            std::string key = UrlDecode(pair.substr(0, equals), true);
            std::string value = equals == std::string_view::npos
                ? std::string()
                : UrlDecode(pair.substr(equals + 1), true);
            values[std::move(key)] = std::move(value);
        }
        if (amp == queryString.size()) {
            break;
        }
        pos = amp + 1;
    }
    return values;
}

std::string NormalizeUrlPrefix(std::string prefix) {
    prefix = Trim(std::move(prefix));
    if (prefix.empty()) {
        throw std::invalid_argument("URL prefix cannot be empty.");
    }
    if (prefix.back() != '/') {
        prefix.push_back('/');
    }
    return prefix;
}

std::wstring NormalizeUrlPrefixWide(std::string_view prefix) {
    return Utf8ToWide(NormalizeUrlPrefix(std::string(prefix)));
}

std::string SocketAddressToString(const SOCKADDR* address) {
    if (address == nullptr) {
        return {};
    }
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), buffer.data(), static_cast<DWORD>(buffer.size())) != nullptr) {
            return buffer.data();
        }
    } else if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (InetNtopA(AF_INET6, const_cast<IN6_ADDR*>(&ipv6->sin6_addr), buffer.data(), static_cast<DWORD>(buffer.size())) != nullptr) {
            return buffer.data();
        }
    }
    return {};
}

std::string MethodFromHttpRequest(const HTTP_REQUEST& request) {
    switch (request.Verb) {
    case HttpVerbOPTIONS: return "OPTIONS";
    case HttpVerbGET: return "GET";
    case HttpVerbHEAD: return "HEAD";
    case HttpVerbPOST: return "POST";
    case HttpVerbPUT: return "PUT";
    case HttpVerbDELETE: return "DELETE";
    case HttpVerbTRACE: return "TRACE";
    case HttpVerbCONNECT: return "CONNECT";
    case HttpVerbTRACK: return "TRACK";
    case HttpVerbMOVE: return "MOVE";
    case HttpVerbCOPY: return "COPY";
    case HttpVerbPROPFIND: return "PROPFIND";
    case HttpVerbPROPPATCH: return "PROPPATCH";
    case HttpVerbMKCOL: return "MKCOL";
    case HttpVerbLOCK: return "LOCK";
    case HttpVerbUNLOCK: return "UNLOCK";
    case HttpVerbSEARCH: return "SEARCH";
    case HttpVerbUnknown:
        return request.pUnknownVerb == nullptr ? "UNKNOWN" : std::string(request.pUnknownVerb, request.UnknownVerbLength);
    default:
        return "UNKNOWN";
    }
}

const char* RequestKnownHeaderName(HTTP_HEADER_ID id) {
    switch (id) {
    case HttpHeaderCacheControl: return "Cache-Control";
    case HttpHeaderConnection: return "Connection";
    case HttpHeaderDate: return "Date";
    case HttpHeaderKeepAlive: return "Keep-Alive";
    case HttpHeaderPragma: return "Pragma";
    case HttpHeaderTrailer: return "Trailer";
    case HttpHeaderTransferEncoding: return "Transfer-Encoding";
    case HttpHeaderUpgrade: return "Upgrade";
    case HttpHeaderVia: return "Via";
    case HttpHeaderWarning: return "Warning";
    case HttpHeaderAllow: return "Allow";
    case HttpHeaderContentLength: return "Content-Length";
    case HttpHeaderContentType: return "Content-Type";
    case HttpHeaderContentEncoding: return "Content-Encoding";
    case HttpHeaderContentLanguage: return "Content-Language";
    case HttpHeaderContentLocation: return "Content-Location";
    case HttpHeaderContentMd5: return "Content-MD5";
    case HttpHeaderContentRange: return "Content-Range";
    case HttpHeaderExpires: return "Expires";
    case HttpHeaderLastModified: return "Last-Modified";
    case HttpHeaderAccept: return "Accept";
    case HttpHeaderAcceptCharset: return "Accept-Charset";
    case HttpHeaderAcceptEncoding: return "Accept-Encoding";
    case HttpHeaderAcceptLanguage: return "Accept-Language";
    case HttpHeaderAuthorization: return "Authorization";
    case HttpHeaderCookie: return "Cookie";
    case HttpHeaderExpect: return "Expect";
    case HttpHeaderFrom: return "From";
    case HttpHeaderHost: return "Host";
    case HttpHeaderIfMatch: return "If-Match";
    case HttpHeaderIfModifiedSince: return "If-Modified-Since";
    case HttpHeaderIfNoneMatch: return "If-None-Match";
    case HttpHeaderIfRange: return "If-Range";
    case HttpHeaderIfUnmodifiedSince: return "If-Unmodified-Since";
    case HttpHeaderMaxForwards: return "Max-Forwards";
    case HttpHeaderProxyAuthorization: return "Proxy-Authorization";
    case HttpHeaderReferer: return "Referer";
    case HttpHeaderRange: return "Range";
    case HttpHeaderTe: return "TE";
    case HttpHeaderTranslate: return "Translate";
    case HttpHeaderUserAgent: return "User-Agent";
    default: return nullptr;
    }
}

void CopyRequestHeaders(const HTTP_REQUEST_HEADERS& source, HeaderCollection& target) {
    for (USHORT index = 0; index < HttpHeaderRequestMaximum; ++index) {
        const HTTP_KNOWN_HEADER& header = source.KnownHeaders[index];
        if (header.RawValueLength == 0 || header.pRawValue == nullptr) {
            continue;
        }
        const char* name = RequestKnownHeaderName(static_cast<HTTP_HEADER_ID>(index));
        if (name != nullptr) {
            target.Add(name, std::string(header.pRawValue, header.RawValueLength));
        }
    }
    for (USHORT index = 0; index < source.UnknownHeaderCount; ++index) {
        const HTTP_UNKNOWN_HEADER& header = source.pUnknownHeaders[index];
        if (header.NameLength == 0 || header.pName == nullptr) {
            continue;
        }
        target.Add(std::string(header.pName, header.NameLength), std::string(header.pRawValue, header.RawValueLength));
    }
}

std::string StatusReason(int statusCode) {
    switch (statusCode) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 206: return "Partial Content";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

std::vector<std::uint8_t> StringToBytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(text.data());
    return std::vector<std::uint8_t>(first, first + text.size());
}

constexpr std::string_view ProblemDetailsJsonContentType = "application/problem+json; charset=utf-8";

std::string ContentTypeMediaType(std::string contentType) {
    std::size_t semicolon = contentType.find(';');
    if (semicolon != std::string::npos) {
        contentType = contentType.substr(0, semicolon);
    }
    return ToLowerAscii(Trim(std::move(contentType)));
}

bool IsProblemDetailsContentType(std::string contentType) {
    return ContentTypeMediaType(std::move(contentType)) == "application/problem+json";
}

bool IsProblemStatus(int statusCode) noexcept {
    return statusCode >= 400 && statusCode <= 599;
}

bool IsReservedProblemProperty(std::string_view name) {
    return EqualsIgnoreCase(name, "type") ||
        EqualsIgnoreCase(name, "title") ||
        EqualsIgnoreCase(name, "status") ||
        EqualsIgnoreCase(name, "detail") ||
        EqualsIgnoreCase(name, "instance");
}

std::string DefaultProblemType(const ProblemDetailsOptions& options) {
    if (!options.IncludeType) {
        return {};
    }
    return options.DefaultType.empty() ? "about:blank" : options.DefaultType;
}

std::string ProblemInstanceFromContext(const HttpContext& context) {
    if (!context.Request().RawUrl().empty()) {
        return context.Request().RawUrl();
    }
    if (!context.Request().QueryString().empty()) {
        std::string query = context.Request().QueryString();
        return context.Request().Path() + (query.front() == '?' ? query : "?" + query);
    }
    return context.Request().Path();
}

void EnrichProblemDetails(HttpContext& context, ProblemDetails& problem, const ProblemDetailsOptions& options) {
    if (problem.Type.empty()) {
        problem.Type = DefaultProblemType(options);
    }
    if (problem.Title.empty()) {
        problem.Title = StatusReason(problem.Status);
    }
    if (options.IncludeInstance && problem.Instance.empty()) {
        problem.Instance = ProblemInstanceFromContext(context);
    }
    if (options.IncludeTraceIdentifier &&
        !options.TraceIdentifierExtensionName.empty() &&
        !context.TraceIdentifier().empty() &&
        problem.Extensions.find(options.TraceIdentifierExtensionName) == problem.Extensions.end()) {
        problem.Extensions.emplace(
            options.TraceIdentifierExtensionName,
            System::Text::Json::JsonNode::Create(context.TraceIdentifier()));
    }
}

void CustomizeProblemDetails(
    HttpContext& context,
    ProblemDetails& problem,
    const ProblemDetailsOptions& options,
    const Logger* logger = nullptr) {
    if (!options.Customize) {
        return;
    }
    try {
        options.Customize(context, problem);
    } catch (const std::exception& ex) {
        if (logger != nullptr) {
            Log(*logger, LogLevel::Warning, std::string("ProblemDetails customization failed: ") + ex.what());
        }
    } catch (...) {
        if (logger != nullptr) {
            Log(*logger, LogLevel::Warning, "ProblemDetails customization failed with an unknown exception.");
        }
    }
}

System::Text::Json::JsonObject CreateProblemObject(const ProblemDetails& details) {
    using namespace System::Text::Json;

    JsonObject problem;
    if (!details.Type.empty()) {
        problem.Add("type", JsonNode::Create(details.Type));
    }
    if (!details.Title.empty()) {
        problem.Add("title", JsonNode::Create(details.Title));
    }
    problem.Add("status", JsonNode::Create(details.Status));
    if (!details.Detail.empty()) {
        problem.Add("detail", JsonNode::Create(details.Detail));
    }
    if (!details.Instance.empty()) {
        problem.Add("instance", JsonNode::Create(details.Instance));
    }
    for (const auto& [name, value] : details.Extensions) {
        if (!name.empty() && !IsReservedProblemProperty(name)) {
            problem.Set(name, value);
        }
    }
    return problem;
}

HttpResult ProblemDetailsResult(const ProblemDetails& details) {
    return Results::Content(
        System::Text::Json::JsonSerializer::Serialize(CreateProblemObject(details)),
        std::string(ProblemDetailsJsonContentType),
        details.Status);
}

ProblemDetails CreateProblemDetails(
    std::string title,
    std::string detail,
    int statusCode,
    const ProblemDetailsOptions& options) {
    ProblemDetails problem;
    problem.Type = DefaultProblemType(options);
    problem.Title = std::move(title);
    problem.Status = statusCode;
    problem.Detail = std::move(detail);
    return problem;
}

HttpResult CreateProblemResult(
    HttpContext& context,
    std::string title,
    std::string detail,
    int statusCode,
    const ProblemDetailsOptions& options,
    const Logger* logger = nullptr) {
    ProblemDetails problem = CreateProblemDetails(std::move(title), std::move(detail), statusCode, options);
    EnrichProblemDetails(context, problem, options);
    CustomizeProblemDetails(context, problem, options, logger);
    return ProblemDetailsResult(problem);
}

HttpResult CreateErrorResult(
    HttpContext& context,
    std::string title,
    const std::exception& ex,
    int statusCode,
    bool detailedErrors,
    const ProblemDetailsOptions& options,
    const Logger* logger = nullptr) {
    return CreateProblemResult(
        context,
        std::move(title),
        detailedErrors ? ex.what() : std::string(),
        statusCode,
        options,
        logger);
}

HttpResult CreateErrorResult(
    HttpContext& context,
    std::string title,
    std::string detail,
    int statusCode,
    bool detailedErrors,
    const ProblemDetailsOptions& options,
    const Logger* logger = nullptr) {
    return CreateProblemResult(
        context,
        std::move(title),
        detailedErrors ? std::move(detail) : std::string(),
        statusCode,
        options,
        logger);
}

std::optional<ProblemDetails> TryReadProblemDetails(const HttpResult& result, const ProblemDetailsOptions& options) {
    if (result.Body().empty()) {
        return std::nullopt;
    }

    try {
        std::string json(reinterpret_cast<const char*>(result.Body().data()), result.Body().size());
        auto document = System::Text::Json::JsonDocument::Parse(json);
        System::Text::Json::JsonElement root = document->RootElement();
        if (root.ValueKind() != System::Text::Json::JsonValueKind::Object) {
            return std::nullopt;
        }

        ProblemDetails problem;
        problem.Type = DefaultProblemType(options);
        problem.Title = StatusReason(result.StatusCode());
        problem.Status = result.StatusCode();

        for (const auto& [name, value] : root.EnumerateObject()) {
            if (EqualsIgnoreCase(name, "type") && value.ValueKind() == System::Text::Json::JsonValueKind::String) {
                problem.Type = value.GetString();
            } else if (EqualsIgnoreCase(name, "title") && value.ValueKind() == System::Text::Json::JsonValueKind::String) {
                problem.Title = value.GetString();
            } else if (EqualsIgnoreCase(name, "status")) {
                int status = 0;
                if (value.TryGetInt32(status)) {
                    problem.Status = status;
                }
            } else if (EqualsIgnoreCase(name, "detail") && value.ValueKind() == System::Text::Json::JsonValueKind::String) {
                problem.Detail = value.GetString();
            } else if (EqualsIgnoreCase(name, "instance") && value.ValueKind() == System::Text::Json::JsonValueKind::String) {
                problem.Instance = value.GetString();
            } else if (!IsReservedProblemProperty(name)) {
                problem.Extensions[name] = System::Text::Json::JsonNode::Parse(value.GetRawText());
            }
        }
        return problem;
    } catch (...) {
        return std::nullopt;
    }
}

HttpResult PreserveNonBodyHeaders(HttpResult replacement, const HttpResult& source) {
    for (const auto& [name, value] : source.Headers().Items()) {
        if (!EqualsIgnoreCase(name, "Content-Type") && !EqualsIgnoreCase(name, "Content-Length")) {
            replacement.Header(name, value);
        }
    }
    return replacement;
}

HttpResult ApplyProblemDetails(HttpContext& context, HttpResult result, const ProblemDetailsOptions& options) {
    if (!result.HasResponse() || !IsProblemStatus(result.StatusCode())) {
        return result;
    }

    ProblemDetails problem;
    if (result.Body().empty()) {
        problem = CreateProblemDetails(StatusReason(result.StatusCode()), {}, result.StatusCode(), options);
    } else {
        if (!IsProblemDetailsContentType(result.Headers().Get("Content-Type"))) {
            return result;
        }
        auto parsed = TryReadProblemDetails(result, options);
        if (!parsed) {
            return result;
        }
        problem = std::move(*parsed);
    }

    problem.Status = result.StatusCode();
    EnrichProblemDetails(context, problem, options);
    CustomizeProblemDetails(context, problem, options);
    return PreserveNonBodyHeaders(ProblemDetailsResult(problem), result);
}

const char* HealthStatusName(HealthStatus status) noexcept {
    switch (status) {
    case HealthStatus::Healthy: return "Healthy";
    case HealthStatus::Degraded: return "Degraded";
    case HealthStatus::Unhealthy: return "Unhealthy";
    default: return "Unknown";
    }
}

HealthStatus CombineHealthStatus(HealthStatus current, HealthStatus next) noexcept {
    if (current == HealthStatus::Unhealthy || next == HealthStatus::Unhealthy) {
        return HealthStatus::Unhealthy;
    }
    if (current == HealthStatus::Degraded || next == HealthStatus::Degraded) {
        return HealthStatus::Degraded;
    }
    return HealthStatus::Healthy;
}

HttpResult CreateHealthCheckResult(const std::vector<std::pair<std::string, HealthCheck>>& checks, const HealthCheckOptions& options) {
    using namespace System::Text::Json;

    HealthStatus overall = HealthStatus::Healthy;
    JsonObject details;
    auto started = std::chrono::steady_clock::now();

    for (const auto& [name, check] : checks) {
        HealthCheckResult result;
        auto checkStarted = std::chrono::steady_clock::now();
        try {
            result = check();
        } catch (const std::exception& ex) {
            result = HealthCheckResult::Unhealthy(ex.what());
        } catch (...) {
            result = HealthCheckResult::Unhealthy("Unknown exception.");
        }

        overall = CombineHealthStatus(overall, result.Status);

        if (options.IncludeDetails) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - checkStarted);
            JsonObject item;
            item.Add("status", JsonNode::Create(HealthStatusName(result.Status)));
            item.Add("durationMs", JsonNode::Create(static_cast<long long>(elapsed.count())));
            if (!result.Description.empty()) {
                item.Add("description", JsonNode::Create(result.Description));
            }
            details.Add(name, item);
        }
    }

    auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    int statusCode = (overall == HealthStatus::Unhealthy ||
        (overall == HealthStatus::Degraded && options.TreatDegradedAsFailure))
        ? 503
        : 200;

    JsonObject payload;
    payload.Add("status", JsonNode::Create(HealthStatusName(overall)));
    payload.Add("durationMs", JsonNode::Create(static_cast<long long>(totalElapsed.count())));
    if (options.IncludeDetails) {
        payload.Add("checks", details);
    }
    return Results::Json(payload, statusCode);
}

void UpdateAtomicMax(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
    std::uint64_t current = target.load();
    while (current < value && !target.compare_exchange_weak(current, value)) {
    }
}

struct RequestMetricsStore {
    std::chrono::steady_clock::time_point StartedAt = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> Started{ 0 };
    std::atomic<std::uint64_t> Completed{ 0 };
    std::atomic<std::uint64_t> Failed{ 0 };
    std::atomic<std::uint64_t> Active{ 0 };
    std::atomic<std::uint64_t> TotalDurationMs{ 0 };
    std::atomic<std::uint64_t> MaxDurationMs{ 0 };
    mutable std::mutex Mutex;
    std::map<std::string, std::uint64_t> ByMethod;
    std::map<int, std::uint64_t> ByStatusCode;
    std::map<std::string, std::uint64_t> ByPath;

    void RecordStarted() noexcept {
        Started.fetch_add(1);
        Active.fetch_add(1);
    }

    void RecordCompleted(const HttpContext& context, int statusCode, std::chrono::milliseconds duration, const MetricsOptions& options) {
        Completed.fetch_add(1);
        Active.fetch_sub(1);
        if (statusCode >= 500) {
            Failed.fetch_add(1);
        }

        std::uint64_t elapsed = static_cast<std::uint64_t>((std::max)(duration.count(), 0ll));
        TotalDurationMs.fetch_add(elapsed);
        UpdateAtomicMax(MaxDurationMs, elapsed);

        if (options.IncludeMethodCounters || options.IncludeStatusCodeCounters || options.IncludePathCounters) {
            std::lock_guard lock(Mutex);
            if (options.IncludeMethodCounters) {
                ++ByMethod[context.Request().Method()];
            }
            if (options.IncludeStatusCodeCounters) {
                ++ByStatusCode[statusCode];
            }
            if (options.IncludePathCounters) {
                ++ByPath[context.Request().Path()];
            }
        }
    }

    System::Text::Json::JsonObject Snapshot(const MetricsEndpointOptions& options) const {
        using namespace System::Text::Json;

        std::uint64_t completed = Completed.load();
        std::uint64_t totalDuration = TotalDurationMs.load();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - StartedAt);

        JsonObject payload;
        payload.Add("started", JsonNode::Create(static_cast<long long>(Started.load())));
        payload.Add("completed", JsonNode::Create(static_cast<long long>(completed)));
        payload.Add("failed", JsonNode::Create(static_cast<long long>(Failed.load())));
        payload.Add("active", JsonNode::Create(static_cast<long long>(Active.load())));
        payload.Add("uptimeSeconds", JsonNode::Create(static_cast<long long>(uptime.count())));
        payload.Add("totalDurationMs", JsonNode::Create(static_cast<long long>(totalDuration)));
        payload.Add("maxDurationMs", JsonNode::Create(static_cast<long long>(MaxDurationMs.load())));
        payload.Add("averageDurationMs", JsonNode::Create(completed == 0 ? 0.0 : static_cast<double>(totalDuration) / static_cast<double>(completed)));

        if (options.IncludeDetails) {
            JsonObject details;
            {
                std::lock_guard lock(Mutex);
                JsonObject byMethod;
                for (const auto& [method, count] : ByMethod) {
                    byMethod.Add(method, JsonNode::Create(static_cast<long long>(count)));
                }
                JsonObject byStatusCode;
                for (const auto& [status, count] : ByStatusCode) {
                    byStatusCode.Add(std::to_string(status), JsonNode::Create(static_cast<long long>(count)));
                }
                JsonObject byPath;
                for (const auto& [path, count] : ByPath) {
                    byPath.Add(path, JsonNode::Create(static_cast<long long>(count)));
                }
                details.Add("byMethod", byMethod);
                details.Add("byStatusCode", byStatusCode);
                details.Add("byPath", byPath);
            }
            payload.Add("details", details);
        }

        return payload;
    }
};

struct RateLimitDecision {
    bool Allowed = true;
    std::string Key;
    std::uint32_t Limit = 0;
    std::uint32_t Remaining = 0;
    std::uint32_t RetryAfterSeconds = 0;
    std::uint32_t ResetAfterSeconds = 0;
};

class FixedWindowRateLimiter {
public:
    RateLimitDecision Check(HttpContext& context, const RateLimitOptions& options) {
        RateLimitDecision decision;
        decision.Limit = options.PermitLimit;
        if (options.PermitLimit == 0 || options.Window.count() <= 0) {
            decision.Remaining = options.PermitLimit;
            return decision;
        }

        decision.Key = BuildKey(context, options);
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        CleanupExpired(now, options.Window);

        Entry& entry = entries_[decision.Key];
        if (entry.WindowStarted.time_since_epoch().count() == 0 ||
            now - entry.WindowStarted >= options.Window) {
            entry.WindowStarted = now;
            entry.Count = 0;
        }
        entry.LastSeen = now;

        auto resetAfter = std::chrono::duration_cast<std::chrono::seconds>(options.Window - (now - entry.WindowStarted));
        if (resetAfter.count() <= 0) {
            resetAfter = std::chrono::seconds(1);
        }
        decision.ResetAfterSeconds = static_cast<std::uint32_t>(resetAfter.count());

        if (entry.Count >= options.PermitLimit) {
            decision.Allowed = false;
            decision.Remaining = 0;
            decision.RetryAfterSeconds = decision.ResetAfterSeconds;
            return decision;
        }

        ++entry.Count;
        decision.Remaining = options.PermitLimit - entry.Count;
        return decision;
    }

private:
    struct Entry {
        std::chrono::steady_clock::time_point WindowStarted{};
        std::chrono::steady_clock::time_point LastSeen{};
        std::uint32_t Count = 0;
    };

    static std::string BuildKey(HttpContext& context, const RateLimitOptions& options) {
        std::string key;
        if (options.KeySelector) {
            key = Trim(options.KeySelector(context));
        }
        if (key.empty()) {
            key = context.Request().RemoteAddress();
        }
        if (key.empty()) {
            key = "global";
        }
        return key;
    }

    void CleanupExpired(std::chrono::steady_clock::time_point now, std::chrono::seconds window) {
        if (now < nextCleanup_) {
            return;
        }
        nextCleanup_ = now + (std::max)(window, std::chrono::seconds(30));
        auto ttl = (std::max)(window * 2, std::chrono::seconds(60));
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.LastSeen.time_since_epoch().count() != 0 &&
                now - it->second.LastSeen > ttl) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::chrono::steady_clock::time_point nextCleanup_{};
};

void AddRateLimitHeaders(HttpResult& result, const RateLimitOptions& options, const RateLimitDecision& decision) {
    if (!options.AddRateLimitHeaders) {
        return;
    }
    if (!decision.Allowed) {
        result.Header(options.RetryAfterHeaderName, std::to_string(decision.RetryAfterSeconds));
    }
    result.Header(options.LimitHeaderName, std::to_string(decision.Limit));
    result.Header(options.RemainingHeaderName, std::to_string(decision.Remaining));
    result.Header(options.ResetHeaderName, std::to_string(decision.ResetAfterSeconds));
}

void AddRateLimitHeaders(HeaderCollection& headers, const RateLimitOptions& options, const RateLimitDecision& decision) {
    if (!options.AddRateLimitHeaders) {
        return;
    }
    if (!decision.Allowed) {
        headers.Set(options.RetryAfterHeaderName, std::to_string(decision.RetryAfterSeconds));
    }
    headers.Set(options.LimitHeaderName, std::to_string(decision.Limit));
    headers.Set(options.RemainingHeaderName, std::to_string(decision.Remaining));
    headers.Set(options.ResetHeaderName, std::to_string(decision.ResetAfterSeconds));
}

void ValidateRateLimitOptions(const RateLimitOptions& options) {
    if (options.RejectedStatusCode < 400 || options.RejectedStatusCode > 599) {
        throw std::out_of_range("RateLimitOptions::RejectedStatusCode must be a 4xx or 5xx status code.");
    }
    if (options.Window.count() < 0) {
        throw std::out_of_range("RateLimitOptions::Window cannot be negative.");
    }
    if (options.AddRateLimitHeaders) {
        ValidateHeaderName(options.RetryAfterHeaderName, "RateLimitOptions::RetryAfterHeaderName");
        ValidateHeaderName(options.LimitHeaderName, "RateLimitOptions::LimitHeaderName");
        ValidateHeaderName(options.RemainingHeaderName, "RateLimitOptions::RemainingHeaderName");
        ValidateHeaderName(options.ResetHeaderName, "RateLimitOptions::ResetHeaderName");
    }
}

std::string EscapeQuotedHeaderValue(std::string value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string ContentTypeForExtension(const std::filesystem::path& path) {
    std::string ext = ToLowerAscii(path.extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".xml") return "application/xml; charset=utf-8";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".zip") return "application/zip";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    return {};
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    stream.seekg(0, std::ios::end);
    std::streamoff size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max<std::streamoff>(0, size)));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return bytes;
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length) {
    if (length > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::length_error("Static file range is too large to buffer.");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>((std::max<std::streamsize>)(0, stream.gcount())));
    }
    return bytes;
}

std::chrono::system_clock::time_point ToSystemClock(std::filesystem::file_time_type fileTime) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
}

std::string FormatHttpDate(std::chrono::system_clock::time_point value) {
    std::time_t time = std::chrono::system_clock::to_time_t(value);
    std::tm utc{};
    gmtime_s(&utc, &time);

    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return buffer;
}

std::optional<std::chrono::system_clock::time_point> ParseHttpDate(std::string value) {
    value = Trim(std::move(value));
    if (value.empty()) {
        return std::nullopt;
    }

    const char* formats[] = {
        "%a, %d %b %Y %H:%M:%S GMT",
        "%A, %d-%b-%y %H:%M:%S GMT",
        "%a %b %d %H:%M:%S %Y",
    };

    for (const char* format : formats) {
        std::tm parsed{};
        std::istringstream stream(value);
        stream.imbue(std::locale::classic());
        stream >> std::get_time(&parsed, format);
        if (!stream.fail()) {
            std::time_t time = _mkgmtime(&parsed);
            if (time != static_cast<std::time_t>(-1)) {
                return std::chrono::system_clock::from_time_t(time);
            }
        }
    }
    return std::nullopt;
}

std::string ToHex(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << value;
    return stream.str();
}

struct StaticFileMetadata {
    std::uint64_t Size = 0;
    std::chrono::system_clock::time_point LastModified;
    std::string LastModifiedHeader;
    std::string ETag;
};

StaticFileMetadata GetStaticFileMetadata(const std::filesystem::path& path) {
    StaticFileMetadata metadata;
    metadata.Size = static_cast<std::uint64_t>(std::filesystem::file_size(path));
    metadata.LastModified = std::chrono::time_point_cast<std::chrono::seconds>(ToSystemClock(std::filesystem::last_write_time(path)));
    metadata.LastModifiedHeader = FormatHttpDate(metadata.LastModified);
    auto ticks = static_cast<std::uint64_t>(metadata.LastModified.time_since_epoch().count());
    metadata.ETag = "W/\"" + ToHex(metadata.Size) + "-" + ToHex(ticks) + "\"";
    return metadata;
}

std::string StripWeakEntityTagPrefix(std::string value) {
    value = Trim(std::move(value));
    if (value.size() > 2 && (value[0] == 'W' || value[0] == 'w') && value[1] == '/') {
        value.erase(0, 2);
    }
    return value;
}

bool EntityTagMatches(std::string_view candidate, std::string_view current) {
    std::string left = Trim(std::string(candidate));
    if (left == "*") {
        return true;
    }
    return left == current || StripWeakEntityTagPrefix(left) == StripWeakEntityTagPrefix(std::string(current));
}

bool EntityTagListMatches(std::string_view candidates, std::string_view current) {
    std::size_t position = 0;
    while (position <= candidates.size()) {
        std::size_t comma = candidates.find(',', position);
        std::string token(candidates.substr(position, comma == std::string_view::npos ? std::string_view::npos : comma - position));
        if (EntityTagMatches(token, current)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        position = comma + 1;
    }
    return false;
}

void AddStaticFileHeaders(HttpResult& result, const StaticFileOptions& options, const StaticFileMetadata& metadata) {
    if (!options.CacheControl.empty()) {
        result.Header("Cache-Control", options.CacheControl);
    }
    if (options.AddETagHeader) {
        result.Header("ETag", metadata.ETag);
    }
    if (options.AddLastModifiedHeader) {
        result.Header("Last-Modified", metadata.LastModifiedHeader);
    }
    if (options.EnableRangeProcessing) {
        result.Header("Accept-Ranges", "bytes");
    }
}

bool IsNotModified(const StaticFileOptions& options, const HttpRequest& request, const StaticFileMetadata& metadata) {
    if (!options.EnableConditionalRequests) {
        return false;
    }

    std::string ifNoneMatch = request.Header("If-None-Match");
    if (!ifNoneMatch.empty()) {
        return options.AddETagHeader && EntityTagListMatches(ifNoneMatch, metadata.ETag);
    }

    std::string ifModifiedSince = request.Header("If-Modified-Since");
    if (!ifModifiedSince.empty() && options.AddLastModifiedHeader) {
        auto since = ParseHttpDate(ifModifiedSince);
        return since && metadata.LastModified <= *since;
    }
    return false;
}

bool ShouldUseRange(const StaticFileOptions& options, const HttpRequest& request, const StaticFileMetadata& metadata) {
    if (!options.EnableRangeProcessing || request.Header("Range").empty()) {
        return false;
    }

    std::string ifRange = request.Header("If-Range");
    if (ifRange.empty()) {
        return true;
    }
    if (options.AddETagHeader && EntityTagMatches(ifRange, metadata.ETag)) {
        return true;
    }
    if (options.AddLastModifiedHeader) {
        auto date = ParseHttpDate(ifRange);
        return date && metadata.LastModified <= *date;
    }
    return false;
}

struct ByteRange {
    std::uint64_t Start = 0;
    std::uint64_t End = 0;
};

enum class RangeParseStatus {
    NoRange,
    Unsupported,
    Invalid,
    Unsatisfiable,
    Satisfiable,
};

RangeParseStatus TryParseSingleByteRange(std::string header, std::uint64_t length, ByteRange& range) {
    header = Trim(std::move(header));
    if (header.empty()) {
        return RangeParseStatus::NoRange;
    }
    if (!StartsWithIgnoreCase(header, "bytes=")) {
        return RangeParseStatus::Invalid;
    }

    std::string spec = Trim(header.substr(6));
    if (spec.find(',') != std::string::npos) {
        return RangeParseStatus::Unsupported;
    }

    std::size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return RangeParseStatus::Invalid;
    }

    std::string startText = Trim(spec.substr(0, dash));
    std::string endText = Trim(spec.substr(dash + 1));
    if (startText.empty() && endText.empty()) {
        return RangeParseStatus::Invalid;
    }
    if (length == 0) {
        return RangeParseStatus::Unsatisfiable;
    }

    auto parseUnsigned = [](const std::string& text, std::uint64_t& value) {
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return false;
        }
        try {
            std::size_t consumed = 0;
            unsigned long long parsed = std::stoull(text, &consumed, 10);
            if (consumed != text.size()) {
                return false;
            }
            value = static_cast<std::uint64_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    };

    if (startText.empty()) {
        std::uint64_t suffixLength = 0;
        if (!parseUnsigned(endText, suffixLength)) {
            return RangeParseStatus::Invalid;
        }
        if (suffixLength == 0) {
            return RangeParseStatus::Unsatisfiable;
        }
        if (suffixLength >= length) {
            range.Start = 0;
        } else {
            range.Start = length - suffixLength;
        }
        range.End = length - 1;
        return RangeParseStatus::Satisfiable;
    }

    std::uint64_t start = 0;
    if (!parseUnsigned(startText, start)) {
        return RangeParseStatus::Invalid;
    }
    if (start >= length) {
        return RangeParseStatus::Unsatisfiable;
    }

    std::uint64_t end = length - 1;
    if (!endText.empty() && !parseUnsigned(endText, end)) {
        return RangeParseStatus::Invalid;
    }
    if (end < start) {
        return RangeParseStatus::Unsatisfiable;
    }
    range.Start = start;
    range.End = (std::min)(end, length - 1);
    return RangeParseStatus::Satisfiable;
}

std::wstring LowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool IsSameOrChildPath(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::wstring rootText = LowerWide(std::filesystem::weakly_canonical(root).wstring());
    std::wstring candidateText = LowerWide(std::filesystem::weakly_canonical(candidate).wstring());
    if (!rootText.empty() && rootText.back() != L'\\' && rootText.back() != L'/') {
        rootText.push_back(L'\\');
    }
    if (candidateText == rootText.substr(0, rootText.size() - 1)) {
        return true;
    }
    return candidateText.size() >= rootText.size() && candidateText.compare(0, rootText.size(), rootText) == 0;
}

std::optional<std::filesystem::path> ResolveStaticFilePath(const StaticFileOptions& options, const HttpRequest& request) {
    std::string method = ToLowerAscii(request.Method());
    if (method != "get" && method != "head") {
        return std::nullopt;
    }

    std::string requestPath = NormalizeRoutePath(request.Path());
    std::string mount = NormalizeRoutePath(options.RequestPath);
    std::string remainder;
    if (mount == "/") {
        remainder = requestPath == "/" ? "" : requestPath.substr(1);
    } else {
        if (!StartsWithIgnoreCase(requestPath, mount)) {
            return std::nullopt;
        }
        if (requestPath.size() != mount.size() && requestPath[mount.size()] != '/') {
            return std::nullopt;
        }
        remainder = requestPath.size() == mount.size() ? "" : requestPath.substr(mount.size() + 1);
    }

    std::filesystem::path root = std::filesystem::absolute(options.Root);
    std::filesystem::path relative;
    for (const std::string& rawSegment : SplitPath(remainder)) {
        std::string segment = UrlDecode(rawSegment, false);
        if (segment.empty() || segment == "." || segment == ".." || segment.find('\\') != std::string::npos) {
            return std::nullopt;
        }
        relative /= Utf8ToWide(segment);
    }

    std::filesystem::path candidate = root / relative;
    if (std::filesystem::is_directory(candidate)) {
        for (const std::string& defaultFile : options.DefaultFiles) {
            std::filesystem::path defaultCandidate = candidate / Utf8ToWide(defaultFile);
            if (std::filesystem::is_regular_file(defaultCandidate) && IsSameOrChildPath(root, defaultCandidate)) {
                return defaultCandidate;
            }
        }
        return std::nullopt;
    }

    if (std::filesystem::is_regular_file(candidate) && IsSameOrChildPath(root, candidate)) {
        return candidate;
    }

    if (options.EnableSpaFallback) {
        for (const std::string& excludedPrefix : options.SpaFallbackExcludedPrefixes) {
            if (IsPathUnderPrefix(requestPath, excludedPrefix)) {
                return std::nullopt;
            }
        }
        std::string accept = request.Header("Accept");
        bool acceptsHtml = accept.empty() || accept.find("text/html") != std::string::npos;
        bool looksLikePageRoute = relative.extension().empty();
        std::filesystem::path fallback = root / Utf8ToWide(options.SpaFallbackFile);
        if (acceptsHtml && looksLikePageRoute && std::filesystem::is_regular_file(fallback) && IsSameOrChildPath(root, fallback)) {
            return fallback;
        }
    }

    return std::nullopt;
}

std::optional<HttpResult> TryServeStaticFile(const StaticFileOptions& options, HttpContext& context) {
    auto path = ResolveStaticFilePath(options, context.Request());
    if (!path) {
        return std::nullopt;
    }

    std::string contentType = ContentTypeForExtension(*path);
    if (contentType.empty()) {
        if (!options.ServeUnknownFileTypes) {
            return std::nullopt;
        }
        contentType = options.UnknownFileContentType;
    }

    StaticFileMetadata metadata = GetStaticFileMetadata(*path);
    if (IsNotModified(options, context.Request(), metadata)) {
        HttpResult result = Results::StatusCode(304);
        AddStaticFileHeaders(result, options, metadata);
        return result;
    }

    if (ShouldUseRange(options, context.Request(), metadata)) {
        ByteRange range;
        RangeParseStatus rangeStatus = TryParseSingleByteRange(context.Request().Header("Range"), metadata.Size, range);
        if (rangeStatus == RangeParseStatus::Invalid || rangeStatus == RangeParseStatus::Unsatisfiable) {
            HttpResult result = Results::StatusCode(416);
            result.Header("Content-Range", "bytes */" + std::to_string(metadata.Size));
            AddStaticFileHeaders(result, options, metadata);
            return result;
        }
        if (rangeStatus == RangeParseStatus::Satisfiable) {
            std::uint64_t length = range.End - range.Start + 1;
            auto bytes = ReadFileBytes(*path, range.Start, length);
            HttpResult result = Results::Bytes(bytes, contentType, 206);
            result.Header("Content-Range",
                "bytes " + std::to_string(range.Start) + "-" + std::to_string(range.End) + "/" + std::to_string(metadata.Size));
            AddStaticFileHeaders(result, options, metadata);
            return result;
        }
    }

    auto bytes = ReadFileBytes(*path);
    HttpResult result = Results::File(std::move(bytes), contentType);
    AddStaticFileHeaders(result, options, metadata);
    return result;
}

void SetHeaderIfMissing(HeaderCollection& headers, std::string name, std::string value) {
    if (!headers.Contains(name)) {
        headers.Set(std::move(name), std::move(value));
    }
}

void SetHeaderIfMissing(HttpResult& result, std::string name, std::string value) {
    if (!result.Headers().Contains(name)) {
        result.Header(std::move(name), std::move(value));
    }
}

bool HeaderValueContainsToken(std::string_view value, std::string_view token) {
    std::size_t position = 0;
    while (position <= value.size()) {
        std::size_t comma = value.find(',', position);
        std::string item(value.substr(position, comma == std::string_view::npos ? std::string_view::npos : comma - position));
        if (EqualsIgnoreCase(Trim(std::move(item)), token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        position = comma + 1;
    }
    return false;
}

bool HeadersContainToken(const HeaderCollection& headers, std::string_view name, std::string_view token) {
    for (const std::string& value : headers.GetValues(name)) {
        if (HeaderValueContainsToken(value, token)) {
            return true;
        }
    }
    return false;
}

void AppendHeaderToken(HeaderCollection& headers, std::string name, std::string token) {
    if (HeadersContainToken(headers, name, token)) {
        return;
    }

    std::vector<std::string> values = headers.GetValues(name);
    std::string combined = JoinStrings(values, ", ");
    if (!combined.empty()) {
        combined.append(", ");
    }
    combined.append(token);
    headers.Set(std::move(name), std::move(combined));
}

std::string MediaTypeFromContentType(std::string contentType) {
    std::size_t semicolon = contentType.find(';');
    if (semicolon != std::string::npos) {
        contentType.erase(semicolon);
    }
    return ToLowerAscii(Trim(std::move(contentType)));
}

bool MediaTypeMatchesPattern(std::string_view mediaType, std::string_view pattern) {
    std::string normalizedPattern = ToLowerAscii(Trim(std::string(pattern)));
    if (normalizedPattern.empty()) {
        return false;
    }
    if (normalizedPattern.back() == '/') {
        return StartsWithIgnoreCase(mediaType, normalizedPattern);
    }
    return EqualsIgnoreCase(mediaType, normalizedPattern);
}

bool IsCompressibleContentType(const ResponseCompressionOptions& options, std::string contentType) {
    std::string mediaType = MediaTypeFromContentType(std::move(contentType));
    if (mediaType.empty()) {
        return false;
    }

    for (const std::string& excluded : options.ExcludedMimeTypes) {
        if (MediaTypeMatchesPattern(mediaType, excluded)) {
            return false;
        }
    }

    if (options.MimeTypes.empty()) {
        return true;
    }

    for (const std::string& included : options.MimeTypes) {
        if (MediaTypeMatchesPattern(mediaType, included)) {
            return true;
        }
    }
    return false;
}

double ParseEncodingQuality(std::string_view parameters) {
    double quality = 1.0;
    std::size_t position = 0;
    while (position <= parameters.size()) {
        std::size_t semicolon = parameters.find(';', position);
        std::string parameter(parameters.substr(position, semicolon == std::string_view::npos ? std::string_view::npos : semicolon - position));
        parameter = Trim(std::move(parameter));
        std::size_t equals = parameter.find('=');
        if (equals != std::string::npos) {
            std::string name = Trim(parameter.substr(0, equals));
            std::string value = Trim(parameter.substr(equals + 1));
            if (EqualsIgnoreCase(name, "q")) {
                try {
                    quality = std::stod(value);
                } catch (...) {
                    quality = 0.0;
                }
            }
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        position = semicolon + 1;
    }
    if (quality < 0.0) {
        return 0.0;
    }
    if (quality > 1.0) {
        return 1.0;
    }
    return quality;
}

bool RequestAcceptsGzip(const HttpRequest& request) {
    std::string header = request.Header("Accept-Encoding");
    if (header.empty()) {
        return false;
    }

    std::optional<bool> gzipAllowed;
    bool wildcardAllowed = false;
    std::size_t position = 0;
    while (position <= header.size()) {
        std::size_t comma = header.find(',', position);
        std::string item(header.substr(position, comma == std::string::npos ? std::string::npos : comma - position));
        item = Trim(std::move(item));
        if (!item.empty()) {
            std::size_t semicolon = item.find(';');
            std::string coding = Trim(semicolon == std::string::npos ? item : item.substr(0, semicolon));
            double quality = semicolon == std::string::npos ? 1.0 : ParseEncodingQuality(item.substr(semicolon + 1));
            if (EqualsIgnoreCase(coding, "gzip")) {
                gzipAllowed = quality > 0.0;
            } else if (coding == "*") {
                wildcardAllowed = quality > 0.0;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        position = comma + 1;
    }

    return gzipAllowed.value_or(wildcardAllowed);
}

std::optional<std::vector<std::uint8_t>> GzipCompress(const std::vector<std::uint8_t>& input, int level) {
    if (input.empty()) {
        return std::vector<std::uint8_t>();
    }

    level = (std::max)(-1, (std::min)(9, level));

    z_stream stream{};
    int init = deflateInit2(&stream, level, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY);
    if (init != Z_OK) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> output;
    output.reserve(input.size());
    std::array<std::uint8_t, 64 * 1024> buffer{};

    const std::uint8_t* next = input.data();
    std::size_t remaining = input.size();
    int result = Z_OK;
    do {
        uInt chunk = static_cast<uInt>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<uInt>::max)())));
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(next));
        stream.avail_in = chunk;
        next += chunk;
        remaining -= chunk;

        int flush = remaining == 0 ? Z_FINISH : Z_NO_FLUSH;
        do {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            result = deflate(&stream, flush);
            if (result != Z_OK && result != Z_STREAM_END) {
                deflateEnd(&stream);
                return std::nullopt;
            }

            std::size_t produced = buffer.size() - stream.avail_out;
            output.insert(output.end(), buffer.data(), buffer.data() + produced);
        } while (stream.avail_in != 0 || stream.avail_out == 0 || (flush == Z_FINISH && result != Z_STREAM_END));
    } while (result != Z_STREAM_END);

    deflateEnd(&stream);
    return output;
}

void WeakenEntityTagForCompressedResponse(HeaderCollection& headers) {
    std::string etag = Trim(headers.Get("ETag"));
    if (etag.empty() || StartsWithIgnoreCase(etag, "W/")) {
        return;
    }
    headers.Set("ETag", "W/" + etag);
}

bool ShouldCompressResponse(
    const HttpRequest& request,
    int statusCode,
    const HeaderCollection& headers,
    const std::vector<std::uint8_t>& body,
    std::string contentType,
    const ResponseCompressionOptions& options) {
    if (!options.Enable || EqualsIgnoreCase(request.Method(), "HEAD")) {
        return false;
    }
    if (!options.EnableForHttps && EqualsIgnoreCase(request.Scheme(), "https")) {
        return false;
    }
    if (!RequestAcceptsGzip(request)) {
        return false;
    }

    if (statusCode < 200 || statusCode >= 300 || statusCode == 204 || statusCode == 206 || statusCode == 304) {
        return false;
    }
    if (body.size() < options.MinimumBodySize) {
        return false;
    }
    if (headers.Contains("Content-Encoding") ||
        headers.Contains("Content-Range") ||
        headers.Contains("Content-MD5") ||
        headers.Contains("Digest")) {
        return false;
    }
    if (HeadersContainToken(headers, "Cache-Control", "no-transform")) {
        return false;
    }
    return IsCompressibleContentType(options, std::move(contentType));
}

template <class WriteBody>
void ApplyCompressedBody(
    const HttpRequest& request,
    int statusCode,
    HeaderCollection& headers,
    const std::vector<std::uint8_t>& body,
    std::string contentType,
    const ResponseCompressionOptions& options,
    WriteBody writeBody) {
    if (!ShouldCompressResponse(request, statusCode, headers, body, std::move(contentType), options)) {
        return;
    }

    auto compressed = GzipCompress(body, options.GzipLevel);
    if (!compressed) {
        return;
    }
    if (options.SkipWhenNotSmaller && compressed->size() >= body.size()) {
        return;
    }

    std::size_t compressedSize = compressed->size();
    writeBody(std::move(*compressed));
    headers.Set("Content-Encoding", "gzip");
    headers.Set("Content-Length", std::to_string(compressedSize));
    AppendHeaderToken(headers, "Vary", "Accept-Encoding");
    WeakenEntityTagForCompressedResponse(headers);
}

void ApplyResponseCompression(HttpContext& context, const ResponseCompressionOptions& options) {
    HttpResponse& response = context.Response();
    ApplyCompressedBody(
        context.Request(),
        response.StatusCode(),
        response.Headers(),
        response.Body(),
        response.ContentType(),
        options,
        [&](std::vector<std::uint8_t> bytes) {
            response.WriteBytes(std::move(bytes));
        });
}

void ApplyResponseCompression(const HttpRequest& request, HttpResult& result, const ResponseCompressionOptions& options) {
    if (!result.HasResponse()) {
        return;
    }

    ApplyCompressedBody(
        request,
        result.StatusCode(),
        result.Headers(),
        result.Body(),
        result.Headers().Get("Content-Type"),
        options,
        [&](std::vector<std::uint8_t> bytes) {
            result.WriteBytes(std::move(bytes));
        });
}

bool AuthenticationSchemeExists(const AuthenticationOptions& options, std::string_view name) {
    return std::any_of(options.Schemes.begin(), options.Schemes.end(), [&](const AuthenticationScheme& scheme) {
        return EqualsIgnoreCase(scheme.Name, name);
    });
}

void ValidateAuthenticationOptions(const AuthenticationOptions& options) {
    if (options.Schemes.empty()) {
        throw std::invalid_argument("AuthenticationOptions must contain at least one scheme.");
    }

    for (std::size_t index = 0; index < options.Schemes.size(); ++index) {
        const AuthenticationScheme& scheme = options.Schemes[index];
        if (Trim(scheme.Name).empty()) {
            throw std::invalid_argument("Authentication scheme name cannot be empty.");
        }
        if (!scheme.Handler) {
            throw std::invalid_argument("Authentication scheme '" + scheme.Name + "' must provide a handler.");
        }
        if (!scheme.Challenge.empty() && ContainsUnsafeHeaderValueCharacters(scheme.Challenge)) {
            throw std::invalid_argument("Authentication scheme '" + scheme.Name + "' challenge cannot contain CR/LF.");
        }
        for (std::size_t other = index + 1; other < options.Schemes.size(); ++other) {
            if (EqualsIgnoreCase(scheme.Name, options.Schemes[other].Name)) {
                throw std::invalid_argument("Duplicate authentication scheme: " + scheme.Name);
            }
        }
    }

    if (!options.DefaultScheme.empty() && !AuthenticationSchemeExists(options, options.DefaultScheme)) {
        throw std::invalid_argument("Default authentication scheme '" + options.DefaultScheme + "' is not registered.");
    }
}

void ValidateAuthorizationOptions(const AuthorizationOptions& options) {
    if (!options.Challenge.empty() && ContainsUnsafeHeaderValueCharacters(options.Challenge)) {
        throw std::invalid_argument("AuthorizationOptions::Challenge cannot contain CR/LF.");
    }

    for (std::size_t index = 0; index < options.Policies.size(); ++index) {
        const AuthorizationPolicy& policy = options.Policies[index];
        if (Trim(policy.Name).empty()) {
            throw std::invalid_argument("Authorization policy name cannot be empty.");
        }
        for (const AuthorizationClaimRequirement& claim : policy.RequiredClaims) {
            if (Trim(claim.Type).empty()) {
                throw std::invalid_argument("Authorization policy '" + policy.Name + "' has an empty claim type.");
            }
        }
        for (std::size_t other = index + 1; other < options.Policies.size(); ++other) {
            if (EqualsIgnoreCase(policy.Name, options.Policies[other].Name)) {
                throw std::invalid_argument("Duplicate authorization policy: " + policy.Name);
            }
        }
    }

    for (const AuthorizationClaimRequirement& claim : options.DefaultPolicy.RequiredClaims) {
        if (Trim(claim.Type).empty()) {
            throw std::invalid_argument("Default authorization policy has an empty claim type.");
        }
    }
}

void StoreStringItem(HttpContext& context, const std::string& key, std::string value) {
    if (!key.empty()) {
        context.Items()[key] = std::move(value);
    }
}

void StoreUserItem(HttpContext& context, const std::string& key, const ClaimsPrincipal& principal) {
    if (!key.empty()) {
        context.Items()[key] = principal;
    }
}

void AuthenticateRequest(HttpContext& context, const AuthenticationOptions& options) {
    for (const AuthenticationScheme& scheme : options.Schemes) {
        bool isSelected = options.DefaultScheme.empty() || EqualsIgnoreCase(scheme.Name, options.DefaultScheme);
        if (!isSelected) {
            continue;
        }

        if (!scheme.Challenge.empty()) {
            StoreStringItem(context, options.ChallengeItemKey, scheme.Challenge);
        }

        AuthenticationResult result = scheme.Handler(context);
        if (result.Succeeded()) {
            ClaimsPrincipal principal = result.Principal();
            if (principal.AuthenticationType().empty()) {
                principal.AuthenticationType(scheme.Name);
            }
            context.User(std::move(principal));
            StoreUserItem(context, options.UserItemKey, context.User());
            return;
        }

        if (result.Failed()) {
            StoreStringItem(context, options.FailureItemKey, result.Failure());
            std::string challenge = result.Challenge().empty() ? scheme.Challenge : result.Challenge();
            StoreStringItem(context, options.ChallengeItemKey, std::move(challenge));
            return;
        }

        if (!options.DefaultScheme.empty()) {
            return;
        }
    }
}

bool IsCorsOriginAllowed(const CorsOptions& options, const std::string& origin) {
    if (origin.empty()) {
        return false;
    }
    return ContainsTokenIgnoreCase(options.AllowedOrigins, "*") ||
        ContainsTokenIgnoreCase(options.AllowedOrigins, origin);
}

std::string ResolveCorsOrigin(const CorsOptions& options, const std::string& origin) {
    if (!IsCorsOriginAllowed(options, origin)) {
        return {};
    }
    if (options.AllowCredentials) {
        return origin;
    }
    return ContainsTokenIgnoreCase(options.AllowedOrigins, "*") ? "*" : origin;
}

void ApplyCorsHeaders(HeaderCollection& headers, const CorsOptions& options, const HttpRequest& request) {
    std::string origin = request.Header("Origin");
    std::string allowedOrigin = ResolveCorsOrigin(options, origin);
    if (allowedOrigin.empty()) {
        return;
    }

    headers.Set("Access-Control-Allow-Origin", allowedOrigin);
    if (allowedOrigin != "*") {
        headers.Set("Vary", "Origin");
    }
    if (options.AllowCredentials) {
        headers.Set("Access-Control-Allow-Credentials", "true");
    }
    if (!options.ExposedHeaders.empty()) {
        headers.Set("Access-Control-Expose-Headers", JoinStrings(options.ExposedHeaders, ", "));
    }
}

void ApplyCorsHeaders(HttpResult& result, const CorsOptions& options, const HttpRequest& request) {
    HeaderCollection headers;
    ApplyCorsHeaders(headers, options, request);
    for (const auto& [name, value] : headers.Items()) {
        result.Header(name, value);
    }
}

HttpResult CreateCorsPreflightResult(const CorsOptions& options, const HttpRequest& request) {
    HttpResult result = Results::NoContent();
    ApplyCorsHeaders(result, options, request);

    std::string allowMethods = JoinStrings(options.AllowedMethods, ", ");
    std::string allowHeaders = ContainsTokenIgnoreCase(options.AllowedHeaders, "*")
        ? request.Header("Access-Control-Request-Headers", "*")
        : JoinStrings(options.AllowedHeaders, ", ");

    result.Header("Access-Control-Allow-Methods", allowMethods);
    result.Header("Access-Control-Allow-Headers", allowHeaders);
    if (options.MaxAgeSeconds > 0) {
        result.Header("Access-Control-Max-Age", std::to_string(options.MaxAgeSeconds));
    }
    return result;
}

void ApplySecurityHeaders(HeaderCollection& headers, const SecurityHeadersOptions& options) {
    if (options.AddContentTypeOptions) {
        SetHeaderIfMissing(headers, "X-Content-Type-Options", "nosniff");
    }
    if (options.AddFrameOptions) {
        SetHeaderIfMissing(headers, "X-Frame-Options", options.FrameOptions);
    }
    if (options.AddReferrerPolicy) {
        SetHeaderIfMissing(headers, "Referrer-Policy", options.ReferrerPolicy);
    }
    if (options.AddCrossOriginOpenerPolicy) {
        SetHeaderIfMissing(headers, "Cross-Origin-Opener-Policy", options.CrossOriginOpenerPolicy);
    }
    if (!options.ContentSecurityPolicy.empty()) {
        SetHeaderIfMissing(headers, "Content-Security-Policy", options.ContentSecurityPolicy);
    }
}

void ApplySecurityHeaders(HttpResult& result, const SecurityHeadersOptions& options) {
    HeaderCollection headers;
    ApplySecurityHeaders(headers, options);
    for (const auto& [name, value] : headers.Items()) {
        SetHeaderIfMissing(result, name, value);
    }
}

struct RouteSegment {
    std::string Literal;
    std::string Name;
    bool IsParameter = false;
    bool IsCatchAll = false;
};

struct RouteEndpoint {
    std::vector<std::string> Methods;
    std::string Pattern;
    std::vector<RouteSegment> Segments;
    EndpointHandler Handler;
    bool IsFallback = false;
    bool RequireAuthorization = false;
    bool AllowAnonymous = false;
    std::vector<std::string> AuthorizationPolicies;
    bool ExcludeFromDescription = false;
    std::string Name;
    std::vector<std::string> Tags;
    std::string Summary;
    std::string Description;
    std::vector<OpenApiParameter> OpenApiParameters;
    std::vector<OpenApiResponse> OpenApiResponses;
    std::vector<OpenApiRequestBody> OpenApiRequestBodies;
};

struct HostedServiceRegistration {
    std::string Name;
    HostedServiceCallback Start;
    HostedServiceCallback Stop;
    bool Started = false;
};

std::vector<RouteSegment> CompileRoutePattern(std::string_view pattern) {
    std::vector<RouteSegment> compiled;
    for (std::string segment : SplitPath(NormalizeRoutePath(std::string(pattern)))) {
        if (segment.size() >= 2 && segment.front() == '{' && segment.back() == '}') {
            segment = segment.substr(1, segment.size() - 2);
            RouteSegment routeSegment;
            routeSegment.IsParameter = true;
            if (!segment.empty() && segment.front() == '*') {
                routeSegment.IsCatchAll = true;
                segment.erase(segment.begin());
            }
            if (segment.empty()) {
                throw std::invalid_argument("Route parameter name cannot be empty.");
            }
            routeSegment.Name = std::move(segment);
            compiled.push_back(std::move(routeSegment));
        } else {
            compiled.push_back(RouteSegment{ ToLowerAscii(segment), {}, false, false });
        }
    }
    return compiled;
}

bool EndpointAllowsMethod(const RouteEndpoint& endpoint, std::string_view method) {
    if (endpoint.Methods.empty()) {
        return true;
    }
    for (const std::string& allowed : endpoint.Methods) {
        if (EqualsIgnoreCase(allowed, method)) {
            return true;
        }
        if (EqualsIgnoreCase(method, "HEAD") && EqualsIgnoreCase(allowed, "GET")) {
            return true;
        }
    }
    return false;
}

bool MatchEndpointPath(const RouteEndpoint& endpoint, const HttpRequest& request, ValueMap& routeValues) {
    std::vector<std::string> pathSegments = SplitPath(NormalizeRoutePath(request.Path()));
    std::size_t pathIndex = 0;
    for (std::size_t patternIndex = 0; patternIndex < endpoint.Segments.size(); ++patternIndex) {
        const RouteSegment& segment = endpoint.Segments[patternIndex];
        if (segment.IsCatchAll) {
            std::string value;
            for (; pathIndex < pathSegments.size(); ++pathIndex) {
                if (!value.empty()) {
                    value.push_back('/');
                }
                value += UrlDecode(pathSegments[pathIndex], false);
            }
            routeValues[segment.Name] = std::move(value);
            return true;
        }

        if (pathIndex >= pathSegments.size()) {
            return false;
        }

        if (segment.IsParameter) {
            routeValues[segment.Name] = UrlDecode(pathSegments[pathIndex], false);
        } else if (!EqualsIgnoreCase(segment.Literal, pathSegments[pathIndex])) {
            return false;
        }
        ++pathIndex;
    }
    return pathIndex == pathSegments.size();
}

bool MatchEndpoint(const RouteEndpoint& endpoint, const HttpRequest& request, ValueMap& routeValues) {
    if (!EndpointAllowsMethod(endpoint, request.Method())) {
        return false;
    }
    return MatchEndpointPath(endpoint, request, routeValues);
}

const AuthorizationPolicy* FindAuthorizationPolicy(const AuthorizationOptions& options, std::string_view name) {
    for (const AuthorizationPolicy& policy : options.Policies) {
        if (EqualsIgnoreCase(policy.Name, name)) {
            return &policy;
        }
    }
    return nullptr;
}

std::optional<std::string> TryGetStringItem(const HttpContext& context, const std::string& key) {
    if (key.empty()) {
        return std::nullopt;
    }
    auto it = context.Items().find(key);
    if (it == context.Items().end()) {
        return std::nullopt;
    }
    if (const auto* value = std::any_cast<std::string>(&it->second)) {
        return *value;
    }
    return std::nullopt;
}

std::string ResolveAuthenticationChallenge(const HttpContext& context, const AuthorizationOptions& options) {
    if (!options.Challenge.empty()) {
        return options.Challenge;
    }
    if (auto challenge = TryGetStringItem(context, options.AuthenticationChallengeItemKey)) {
        return *challenge;
    }
    return {};
}

bool EndpointNeedsAuthorization(const RouteEndpoint& endpoint, const AuthorizationOptions& options) {
    if (endpoint.AllowAnonymous) {
        return false;
    }
    return endpoint.RequireAuthorization || options.RequireAuthenticatedUserByDefault;
}

std::optional<std::string> EvaluateAuthorizationPolicy(
    HttpContext& context,
    const ClaimsPrincipal& user,
    const AuthorizationPolicy& policy) {
    if (policy.RequireAuthenticatedUser && !user.IsAuthenticated()) {
        return "The endpoint requires an authenticated user.";
    }

    if (!policy.RequiredRoles.empty()) {
        bool inRole = false;
        for (const std::string& role : policy.RequiredRoles) {
            if (user.IsInRole(role)) {
                inRole = true;
                break;
            }
        }
        if (!inRole) {
            return "The authenticated user is missing a required role.";
        }
    }

    for (const AuthorizationClaimRequirement& requirement : policy.RequiredClaims) {
        if (requirement.Value.empty()) {
            if (!user.HasClaim(requirement.Type)) {
                return "The authenticated user is missing a required claim.";
            }
        } else if (!user.HasClaim(requirement.Type, requirement.Value)) {
            return "The authenticated user is missing a required claim value.";
        }
    }

    if (policy.Assertion && !policy.Assertion(context, user)) {
        return "The authorization assertion rejected the request.";
    }
    return std::nullopt;
}

std::optional<HttpResult> TryAuthorizeEndpoint(
    const RouteEndpoint& endpoint,
    HttpContext& context,
    const AuthorizationOptions& options) {
    if (!EndpointNeedsAuthorization(endpoint, options)) {
        return std::nullopt;
    }

    const ClaimsPrincipal& user = context.User();
    std::vector<AuthorizationPolicy> policies;
    if (endpoint.AuthorizationPolicies.empty()) {
        policies.push_back(options.DefaultPolicy);
    } else {
        for (const std::string& policyName : endpoint.AuthorizationPolicies) {
            const AuthorizationPolicy* policy = FindAuthorizationPolicy(options, policyName);
            if (policy == nullptr) {
                return Results::Problem("Authorization Policy Not Found", "Policy '" + policyName + "' is not registered.", 500);
            }
            policies.push_back(*policy);
        }
    }

    for (const AuthorizationPolicy& policy : policies) {
        auto failure = EvaluateAuthorizationPolicy(context, user, policy);
        if (!failure) {
            continue;
        }

        if (!options.FailureItemKey.empty()) {
            context.Items()[options.FailureItemKey] = *failure;
        }

        if (!user.IsAuthenticated()) {
            HttpResult result = Results::Unauthorized(options.IncludeFailureDetails ? *failure : std::string{});
            std::string challenge = ResolveAuthenticationChallenge(context, options);
            if (!challenge.empty() && !ContainsUnsafeHeaderValueCharacters(challenge)) {
                result.Header("WWW-Authenticate", std::move(challenge));
            }
            return result;
        }
        return Results::Forbidden(options.IncludeFailureDetails ? *failure : std::string{});
    }

    return std::nullopt;
}

std::string OpenApiPathFromPattern(std::string pattern) {
    std::string result;
    result.reserve(pattern.size());
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (pattern[index] == '{' && index + 1 < pattern.size() && pattern[index + 1] == '*') {
            result.push_back('{');
            ++index;
        } else {
            result.push_back(pattern[index]);
        }
    }
    return NormalizeRoutePath(result);
}

std::vector<std::string> OpenApiMethodsForEndpoint(const RouteEndpoint& endpoint) {
    if (!endpoint.Methods.empty()) {
        return endpoint.Methods;
    }
    return { "GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
}

std::string OpenApiMethodName(std::string method) {
    return ToLowerAscii(std::move(method));
}

std::string GenerateOperationId(const RouteEndpoint& endpoint, const std::string& method) {
    if (!endpoint.Name.empty()) {
        return endpoint.Name;
    }

    std::string operation = ToLowerAscii(method);
    for (char ch : endpoint.Pattern) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            operation.push_back(ch);
        } else if (ch == '/' || ch == '{' || ch == '}' || ch == '*' || ch == '-') {
            if (!operation.empty() && operation.back() != '_') {
                operation.push_back('_');
            }
        }
    }
    while (!operation.empty() && operation.back() == '_') {
        operation.pop_back();
    }
    return operation.empty() ? "operation" : operation;
}

JsonObject CreateOpenApiSchema(std::string type) {
    JsonObject schema;
    if (type.empty()) {
        type = "string";
    }
    schema.Add("type", JsonNode::Create(type));
    return schema;
}

void AddOpenApiParameter(JsonArray& parameters, const OpenApiParameter& parameter) {
    if (parameter.Name.empty() || parameter.In.empty()) {
        return;
    }

    JsonObject item;
    item.Add("name", JsonNode::Create(parameter.Name));
    item.Add("in", JsonNode::Create(parameter.In));
    item.Add("required", JsonNode::Create(parameter.Required || EqualsIgnoreCase(parameter.In, "path")));
    if (!parameter.Description.empty()) {
        item.Add("description", JsonNode::Create(parameter.Description));
    }
    item.Add("schema", CreateOpenApiSchema(parameter.SchemaType));
    parameters.Add(item);
}

bool HasOpenApiParameter(const JsonArray& parameters, std::string_view name, std::string_view in) {
    for (const JsonNode& node : parameters.Items()) {
        JsonObject item = node.AsObject();
        JsonNode nameNode;
        JsonNode inNode;
        if (item.TryGetValue("name", nameNode) && item.TryGetValue("in", inNode) &&
            EqualsIgnoreCase(nameNode.AsElement().GetString(), name) &&
            EqualsIgnoreCase(inNode.AsElement().GetString(), in)) {
            return true;
        }
    }
    return false;
}

void AddOpenApiResponse(JsonObject& responses, const OpenApiResponse& response) {
    int statusCode = response.StatusCode == 0 ? 200 : response.StatusCode;
    std::string description = response.Description.empty() ? StatusReason(statusCode) : response.Description;
    if (description.empty()) {
        description = "Response";
    }

    JsonObject item;
    item.Add("description", JsonNode::Create(description));
    if (!response.ContentType.empty() && statusCode != 204 && statusCode != 304) {
        JsonObject content;
        JsonObject mediaType;
        mediaType.Add("schema", CreateOpenApiSchema("object"));
        content.Add(response.ContentType, mediaType);
        item.Add("content", content);
    }
    responses.Set(std::to_string(statusCode), item);
}

bool HasOpenApiResponse(const JsonObject& responses, int statusCode) {
    return responses.ContainsKey(std::to_string(statusCode));
}

void AddOpenApiRequestBody(JsonObject& operation, const std::vector<OpenApiRequestBody>& requestBodies) {
    if (requestBodies.empty()) {
        return;
    }

    JsonObject requestBody;
    JsonObject content;
    bool required = false;
    std::string description;
    for (const OpenApiRequestBody& body : requestBodies) {
        if (body.ContentType.empty()) {
            continue;
        }
        JsonObject mediaType;
        mediaType.Add("schema", CreateOpenApiSchema("object"));
        content.Add(body.ContentType, mediaType);
        required = required || body.Required;
        if (description.empty()) {
            description = body.Description;
        }
    }
    if (content.Count() == 0) {
        return;
    }
    if (!description.empty()) {
        requestBody.Add("description", JsonNode::Create(description));
    }
    requestBody.Add("required", JsonNode::Create(required));
    requestBody.Add("content", content);
    operation.Add("requestBody", requestBody);
}

std::string ResolveOpenApiSecuritySchemeName(const OpenApiOptions& options) {
    if (!options.DefaultSecurityScheme.empty()) {
        return options.DefaultSecurityScheme;
    }
    for (const OpenApiSecurityScheme& scheme : options.SecuritySchemes) {
        if (!scheme.Name.empty()) {
            return scheme.Name;
        }
    }
    return {};
}

bool EndpointHasOpenApiAuthorization(const RouteEndpoint& endpoint, const AuthorizationOptions& authorizationOptions) {
    return EndpointNeedsAuthorization(endpoint, authorizationOptions);
}

JsonObject CreateOpenApiOperation(
    const RouteEndpoint& endpoint,
    const std::string& method,
    const OpenApiOptions& options,
    const AuthorizationOptions& authorizationOptions) {
    JsonObject operation;
    operation.Add("operationId", JsonNode::Create(GenerateOperationId(endpoint, method)));
    if (!endpoint.Summary.empty()) {
        operation.Add("summary", JsonNode::Create(endpoint.Summary));
    }
    if (!endpoint.Description.empty()) {
        operation.Add("description", JsonNode::Create(endpoint.Description));
    }
    if (!endpoint.Tags.empty()) {
        JsonArray tags;
        for (const std::string& tag : endpoint.Tags) {
            tags.Add(tag);
        }
        operation.Add("tags", tags);
    }

    JsonArray parameters;
    for (const RouteSegment& segment : endpoint.Segments) {
        if (!segment.IsParameter) {
            continue;
        }
        OpenApiParameter parameter;
        parameter.Name = segment.Name;
        parameter.In = "path";
        parameter.SchemaType = "string";
        parameter.Required = true;
        AddOpenApiParameter(parameters, parameter);
    }
    for (const OpenApiParameter& parameter : endpoint.OpenApiParameters) {
        if (!HasOpenApiParameter(parameters, parameter.Name, parameter.In)) {
            AddOpenApiParameter(parameters, parameter);
        }
    }
    if (parameters.Count() != 0) {
        operation.Add("parameters", parameters);
    }

    AddOpenApiRequestBody(operation, endpoint.OpenApiRequestBodies);

    JsonObject responses;
    if (endpoint.OpenApiResponses.empty()) {
        AddOpenApiResponse(responses, OpenApiResponse{ 200, "application/json", "OK" });
    } else {
        for (const OpenApiResponse& response : endpoint.OpenApiResponses) {
            AddOpenApiResponse(responses, response);
        }
    }

    if (EndpointHasOpenApiAuthorization(endpoint, authorizationOptions)) {
        if (!HasOpenApiResponse(responses, 401)) {
            AddOpenApiResponse(responses, OpenApiResponse{ 401, "application/problem+json", "Unauthorized" });
        }
        if (!HasOpenApiResponse(responses, 403)) {
            AddOpenApiResponse(responses, OpenApiResponse{ 403, "application/problem+json", "Forbidden" });
        }

        std::string securityScheme = ResolveOpenApiSecuritySchemeName(options);
        if (!securityScheme.empty()) {
            JsonArray security;
            JsonObject requirement;
            JsonArray scopes;
            requirement.Add(securityScheme, scopes);
            security.Add(requirement);
            operation.Add("security", security);
        }
    }

    operation.Add("responses", responses);
    return operation;
}

void AddOpenApiSecuritySchemes(JsonObject& root, const OpenApiOptions& options) {
    if (options.SecuritySchemes.empty()) {
        return;
    }

    JsonObject securitySchemes;
    for (const OpenApiSecurityScheme& scheme : options.SecuritySchemes) {
        if (scheme.Name.empty() || scheme.Type.empty()) {
            continue;
        }
        JsonObject item;
        item.Add("type", JsonNode::Create(scheme.Type));
        if (!scheme.Description.empty()) {
            item.Add("description", JsonNode::Create(scheme.Description));
        }
        if (EqualsIgnoreCase(scheme.Type, "apiKey")) {
            item.Add("name", JsonNode::Create(scheme.ParameterName.empty() ? "Authorization" : scheme.ParameterName));
            item.Add("in", JsonNode::Create(scheme.In.empty() ? "header" : scheme.In));
        } else if (EqualsIgnoreCase(scheme.Type, "http")) {
            item.Add("scheme", JsonNode::Create(scheme.Scheme.empty() ? "bearer" : scheme.Scheme));
            if (!scheme.BearerFormat.empty()) {
                item.Add("bearerFormat", JsonNode::Create(scheme.BearerFormat));
            }
        }
        securitySchemes.Add(scheme.Name, item);
    }

    if (securitySchemes.Count() == 0) {
        return;
    }

    JsonObject components;
    components.Add("securitySchemes", securitySchemes);
    root.Add("components", components);
}

JsonObject CreateOpenApiDocument(
    const std::vector<RouteEndpoint>& endpoints,
    const OpenApiOptions& options,
    const AuthorizationOptions& authorizationOptions) {
    JsonObject root;
    root.Add("openapi", JsonNode::Create("3.0.3"));

    JsonObject info;
    info.Add("title", JsonNode::Create(options.Info.Title));
    info.Add("version", JsonNode::Create(options.Info.Version));
    if (!options.Info.Description.empty()) {
        info.Add("description", JsonNode::Create(options.Info.Description));
    }
    root.Add("info", info);

    if (!options.ServerUrls.empty()) {
        JsonArray servers;
        for (const std::string& url : options.ServerUrls) {
            if (url.empty()) {
                continue;
            }
            JsonObject server;
            server.Add("url", JsonNode::Create(url));
            servers.Add(server);
        }
        if (servers.Count() != 0) {
            root.Add("servers", servers);
        }
    }

    std::map<std::string, std::vector<const RouteEndpoint*>> grouped;
    for (const RouteEndpoint& endpoint : endpoints) {
        if (endpoint.ExcludeFromDescription || endpoint.IsFallback) {
            continue;
        }
        grouped[OpenApiPathFromPattern(endpoint.Pattern)].push_back(&endpoint);
    }

    JsonObject paths;
    for (const auto& [path, pathEndpoints] : grouped) {
        JsonObject pathItem;
        for (const RouteEndpoint* endpoint : pathEndpoints) {
            for (const std::string& method : OpenApiMethodsForEndpoint(*endpoint)) {
                pathItem.Set(OpenApiMethodName(method), CreateOpenApiOperation(*endpoint, method, options, authorizationOptions));
            }
        }
        paths.Set(path, pathItem);
    }
    root.Add("paths", paths);
    AddOpenApiSecuritySchemes(root, options);
    return root;
}

std::string HtmlEncode(std::string_view value) {
    std::string encoded;
    for (char ch : value) {
        switch (ch) {
        case '&': encoded += "&amp;"; break;
        case '<': encoded += "&lt;"; break;
        case '>': encoded += "&gt;"; break;
        case '"': encoded += "&quot;"; break;
        default: encoded.push_back(ch); break;
        }
    }
    return encoded;
}

std::string JavaScriptStringLiteral(std::string_view value) {
    std::string encoded = "\"";
    for (char ch : value) {
        switch (ch) {
        case '\\': encoded += "\\\\"; break;
        case '"': encoded += "\\\""; break;
        case '\n': encoded += "\\n"; break;
        case '\r': encoded += "\\r"; break;
        case '\t': encoded += "\\t"; break;
        default: encoded.push_back(ch); break;
        }
    }
    encoded.push_back('"');
    return encoded;
}

std::string CreateSwaggerUiHtml(const SwaggerUiOptions& options) {
    std::string title = options.Title.empty() ? "CapiWeb API" : options.Title;
    std::string openApiPath = options.OpenApiPath.empty() ? "/openapi.json" : EnsureLeadingSlash(options.OpenApiPath);
    std::ostringstream html;
    html << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        << "<title>" << HtmlEncode(title) << "</title>"
        << "<style>body{margin:0;font:14px Segoe UI,Arial,sans-serif;background:#f6f8fa;color:#1f2933;}"
        << "header{padding:20px 28px;background:#102a43;color:white;}main{padding:24px 28px;max-width:1120px;margin:auto;}"
        << ".op{background:white;border:1px solid #d9e2ec;border-radius:8px;margin:12px 0;padding:14px 16px;}"
        << ".method{display:inline-block;min-width:64px;padding:5px 8px;border-radius:6px;background:#1f6f5b;color:white;font-weight:700;text-align:center;text-transform:uppercase;}"
        << ".path{font-family:Consolas,monospace;font-size:15px;margin-left:10px;}p{color:#52606d;}pre{white-space:pre-wrap;background:#102a43;color:#d9eefe;padding:12px;border-radius:6px;}</style></head><body>"
        << "<header><h1>" << HtmlEncode(title) << "</h1><div>OpenAPI: <code>" << HtmlEncode(openApiPath) << "</code></div></header>"
        << "<main id=\"app\"><p>Loading API description...</p></main>"
        << "<script>const specUrl=" << JavaScriptStringLiteral(openApiPath) << ";"
        << "const app=document.getElementById('app');"
        << "function esc(s){return String(s??'').replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));}"
        << "fetch(specUrl,{headers:{Accept:'application/json'}}).then(r=>r.json()).then(spec=>{"
        << "document.title=spec.info?.title||document.title;"
        << "let html='<h2>'+esc(spec.info?.title||'API')+'</h2><p>'+esc(spec.info?.description||'')+'</p>';"
        << "for(const [path,item] of Object.entries(spec.paths||{})){for(const [method,op] of Object.entries(item)){"
        << "html+='<section class=\"op\"><div><span class=\"method\">'+esc(method)+'</span><span class=\"path\">'+esc(path)+'</span></div>';"
        << "html+='<h3>'+esc(op.summary||op.operationId||'')+'</h3><p>'+esc(op.description||'')+'</p>';"
        << "if(op.parameters?.length){html+='<strong>Parameters</strong><ul>'+op.parameters.map(p=>'<li>'+esc(p.name)+' in '+esc(p.in)+(p.required?' required':'')+'</li>').join('')+'</ul>';}"
        << "html+='<strong>Responses</strong><ul>'+Object.entries(op.responses||{}).map(([code,r])=>'<li>'+esc(code)+' '+esc(r.description||'')+'</li>').join('')+'</ul>';"
        << "html+='</section>';}}app.innerHTML=html;}).catch(err=>{app.innerHTML='<pre>'+esc(err)+'</pre>';});</script></body></html>";
    return html.str();
}

class HttpApiScope {
public:
    explicit HttpApiScope(ULONG flags) : flags_(flags) {
        ULONG result = HttpInitialize(HttpApiVersion, flags_, nullptr);
        if (result != NO_ERROR) {
            throw HttpSysException("HttpInitialize", result);
        }
    }

    ~HttpApiScope() {
        HttpTerminate(flags_, nullptr);
    }

    HttpApiScope(const HttpApiScope&) = delete;
    HttpApiScope& operator=(const HttpApiScope&) = delete;

private:
    ULONG flags_;
};

void AddUrlReservation(std::string_view prefix, const std::wstring& securityDescriptor) {
    std::wstring widePrefix = NormalizeUrlPrefixWide(prefix);
    HTTP_SERVICE_CONFIG_URLACL_SET set{};
    set.KeyDesc.pUrlPrefix = const_cast<PWSTR>(widePrefix.c_str());
    set.ParamDesc.pStringSecurityDescriptor = const_cast<PWSTR>(securityDescriptor.c_str());

    ULONG result = HttpSetServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &set, sizeof(set), nullptr);
    if (result != NO_ERROR && result != ERROR_ALREADY_EXISTS) {
        throw HttpSysException("HttpSetServiceConfiguration(URLACL)", result);
    }
}

void DeleteUrlReservation(std::string_view prefix, bool ignoreMissing) {
    std::wstring widePrefix = NormalizeUrlPrefixWide(prefix);
    HTTP_SERVICE_CONFIG_URLACL_SET set{};
    set.KeyDesc.pUrlPrefix = const_cast<PWSTR>(widePrefix.c_str());

    ULONG result = HttpDeleteServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &set, sizeof(set), nullptr);
    if (result == NO_ERROR || (ignoreMissing && result == ERROR_FILE_NOT_FOUND)) {
        return;
    }
    throw HttpSysException("HttpDeleteServiceConfiguration(URLACL)", result);
}

std::vector<std::uint8_t> ParseHexBytes(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0 || ch == ':' || ch == '-';
    }), value.end());
    if (value.empty() || value.size() % 2 != 0) {
        throw std::invalid_argument("CertificateHashHex must contain an even number of hex digits.");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        int high = HexValue(value[index]);
        int low = HexValue(value[index + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("CertificateHashHex contains a non-hex character.");
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

GUID ParseGuid(std::string_view value) {
    std::wstring wide = Utf8ToWide(value);
    GUID guid{};
    HRESULT result = CLSIDFromString(wide.c_str(), &guid);
    if (FAILED(result)) {
        throw std::invalid_argument("AppId must be a GUID string, for example {00000000-0000-0000-0000-000000000000}.");
    }
    return guid;
}

SOCKADDR_STORAGE BuildSslSocketAddress(const SslCertificateBinding& binding) {
    SOCKADDR_STORAGE storage{};
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(binding.Port);
    if (InetPtonA(AF_INET, binding.Ip.c_str(), &ipv4.sin_addr) == 1) {
        std::memcpy(&storage, &ipv4, sizeof(ipv4));
        return storage;
    }

    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_port = htons(binding.Port);
    if (InetPtonA(AF_INET6, binding.Ip.c_str(), &ipv6.sin6_addr) == 1) {
        std::memcpy(&storage, &ipv6, sizeof(ipv6));
        return storage;
    }

    throw std::invalid_argument("SSL binding Ip must be an IPv4 or IPv6 address.");
}

DWORD BuildSslFlags(const SslCertificateBinding& binding) {
    DWORD flags = 0;
    if (binding.DisableHttp2) {
        flags |= HTTP_SERVICE_CONFIG_SSL_FLAG_DISABLE_HTTP2;
    }
    if (binding.DisableQuic) {
        flags |= HTTP_SERVICE_CONFIG_SSL_FLAG_DISABLE_QUIC;
    }
    if (binding.DisableTls13) {
        flags |= HTTP_SERVICE_CONFIG_SSL_FLAG_DISABLE_TLS13;
    }
    if (binding.DisableLegacyTls) {
        flags |= HTTP_SERVICE_CONFIG_SSL_FLAG_DISABLE_LEGACY_TLS;
    }
    if (binding.NegotiateClientCertificate) {
        flags |= HTTP_SERVICE_CONFIG_SSL_FLAG_NEGOTIATE_CLIENT_CERT;
    }
    return flags;
}

HTTP_SERVICE_CONFIG_SSL_SET BuildSslConfigSet(
    const SslCertificateBinding& binding,
    SOCKADDR_STORAGE& address,
    std::vector<std::uint8_t>& hash,
    GUID& appId,
    std::wstring& storeName) {
    address = BuildSslSocketAddress(binding);
    hash = ParseHexBytes(binding.CertificateHashHex);
    appId = ParseGuid(binding.AppId);
    storeName = binding.CertificateStoreName.empty() ? L"MY" : binding.CertificateStoreName;

    HTTP_SERVICE_CONFIG_SSL_SET set{};
    set.KeyDesc.pIpPort = reinterpret_cast<PSOCKADDR>(&address);
    set.ParamDesc.SslHashLength = static_cast<ULONG>(hash.size());
    set.ParamDesc.pSslHash = hash.data();
    set.ParamDesc.AppId = appId;
    set.ParamDesc.pSslCertStoreName = const_cast<PWSTR>(storeName.c_str());
    set.ParamDesc.DefaultFlags = BuildSslFlags(binding);
    return set;
}

void AddSslBinding(const SslCertificateBinding& binding) {
    SOCKADDR_STORAGE address{};
    std::vector<std::uint8_t> hash;
    GUID appId{};
    std::wstring storeName;
    HTTP_SERVICE_CONFIG_SSL_SET set = BuildSslConfigSet(binding, address, hash, appId, storeName);

    ULONG result = HttpSetServiceConfiguration(nullptr, HttpServiceConfigSSLCertInfo, &set, sizeof(set), nullptr);
    if (result != NO_ERROR && result != ERROR_ALREADY_EXISTS) {
        throw HttpSysException("HttpSetServiceConfiguration(SSL)", result);
    }
}

void DeleteSslBinding(const SslCertificateBinding& binding, bool ignoreMissing) {
    SOCKADDR_STORAGE address = BuildSslSocketAddress(binding);
    HTTP_SERVICE_CONFIG_SSL_SET set{};
    set.KeyDesc.pIpPort = reinterpret_cast<PSOCKADDR>(&address);

    ULONG result = HttpDeleteServiceConfiguration(nullptr, HttpServiceConfigSSLCertInfo, &set, sizeof(set), nullptr);
    if (result == NO_ERROR || (ignoreMissing && result == ERROR_FILE_NOT_FOUND)) {
        return;
    }
    throw HttpSysException("HttpDeleteServiceConfiguration(SSL)", result);
}

USHORT ToHttpTimeoutSeconds(std::chrono::seconds value) {
    if (value.count() <= 0) {
        return 0;
    }
    if (value.count() > std::numeric_limits<USHORT>::max()) {
        return std::numeric_limits<USHORT>::max();
    }
    return static_cast<USHORT>(value.count());
}

HTTP_TIMEOUT_LIMIT_INFO BuildTimeoutInfo(const HttpSysTimeoutOptions& options) {
    HTTP_TIMEOUT_LIMIT_INFO info{};
    info.Flags.Present = 1;
    info.EntityBody = ToHttpTimeoutSeconds(options.EntityBody);
    info.DrainEntityBody = ToHttpTimeoutSeconds(options.DrainEntityBody);
    info.RequestQueue = ToHttpTimeoutSeconds(options.RequestQueue);
    info.IdleConnection = ToHttpTimeoutSeconds(options.IdleConnection);
    info.HeaderWait = ToHttpTimeoutSeconds(options.HeaderWait);
    info.MinSendRate = options.MinSendRateBytesPerSecond;
    return info;
}

bool TryGetPropertyIgnoreCase(const System::Text::Json::JsonElement& object, std::string_view name, System::Text::Json::JsonElement& value) {
    if (object.ValueKind() != System::Text::Json::JsonValueKind::Object) {
        return false;
    }
    if (object.TryGetProperty(name, value)) {
        return true;
    }
    for (const auto& property : object.EnumerateObject()) {
        if (EqualsIgnoreCase(property.first, name)) {
            value = property.second;
            return true;
        }
    }
    return false;
}

bool IsJsonNullish(const System::Text::Json::JsonElement& value) {
    return value.ValueKind() == System::Text::Json::JsonValueKind::Undefined ||
        value.ValueKind() == System::Text::Json::JsonValueKind::Null;
}

std::optional<std::string> JsonAsString(const System::Text::Json::JsonElement& value) {
    if (value.ValueKind() == System::Text::Json::JsonValueKind::String) {
        return value.GetString();
    }
    if (value.ValueKind() == System::Text::Json::JsonValueKind::Number ||
        value.ValueKind() == System::Text::Json::JsonValueKind::True ||
        value.ValueKind() == System::Text::Json::JsonValueKind::False) {
        return value.ToString();
    }
    return std::nullopt;
}

bool JsonAsBool(const System::Text::Json::JsonElement& value, bool& out) {
    if (value.ValueKind() == System::Text::Json::JsonValueKind::True ||
        value.ValueKind() == System::Text::Json::JsonValueKind::False) {
        out = value.GetBoolean();
        return true;
    }
    if (value.ValueKind() == System::Text::Json::JsonValueKind::Number) {
        int numeric = 0;
        if (value.TryGetInt32(numeric)) {
            out = numeric != 0;
            return true;
        }
    }
    if (value.ValueKind() == System::Text::Json::JsonValueKind::String) {
        std::string text = ToLowerAscii(Trim(value.GetString()));
        if (text == "true" || text == "1" || text == "yes" || text == "on") {
            out = true;
            return true;
        }
        if (text == "false" || text == "0" || text == "no" || text == "off") {
            out = false;
            return true;
        }
    }
    return false;
}

bool JsonAsInt64(const System::Text::Json::JsonElement& value, long long& out) {
    if (value.ValueKind() == System::Text::Json::JsonValueKind::Number) {
        return value.TryGetInt64(out);
    }
    if (value.ValueKind() == System::Text::Json::JsonValueKind::String) {
        try {
            std::size_t consumed = 0;
            std::string text = Trim(value.GetString());
            long long parsed = std::stoll(text, &consumed, 10);
            if (consumed == text.size()) {
                out = parsed;
                return true;
            }
        } catch (...) {
        }
    }
    return false;
}

std::vector<std::string> JsonAsStringList(const System::Text::Json::JsonElement& value) {
    std::vector<std::string> result;
    if (value.ValueKind() == System::Text::Json::JsonValueKind::Array) {
        for (const auto& item : value.EnumerateArray()) {
            auto text = JsonAsString(item);
            if (text) {
                result.push_back(*text);
            }
        }
    } else {
        auto text = JsonAsString(value);
        if (text) {
            result.push_back(*text);
        }
    }
    return result;
}

void ApplyString(const System::Text::Json::JsonElement& object, std::string_view name, std::string& target) {
    System::Text::Json::JsonElement value;
    if (TryGetPropertyIgnoreCase(object, name, value) && !IsJsonNullish(value)) {
        auto text = JsonAsString(value);
        if (text) {
            target = *text;
        }
    }
}

void ApplyWString(const System::Text::Json::JsonElement& object, std::string_view name, std::wstring& target) {
    System::Text::Json::JsonElement value;
    if (TryGetPropertyIgnoreCase(object, name, value) && !IsJsonNullish(value)) {
        auto text = JsonAsString(value);
        if (text) {
            target = Utf8ToWide(*text);
        }
    }
}

void ApplyBool(const System::Text::Json::JsonElement& object, std::string_view name, bool& target) {
    System::Text::Json::JsonElement value;
    bool parsed = false;
    if (TryGetPropertyIgnoreCase(object, name, value) && JsonAsBool(value, parsed)) {
        target = parsed;
    }
}

template <class Integer>
void ApplyInteger(const System::Text::Json::JsonElement& object, std::string_view name, Integer& target) {
    System::Text::Json::JsonElement value;
    long long parsed = 0;
    if (!TryGetPropertyIgnoreCase(object, name, value) || !JsonAsInt64(value, parsed)) {
        return;
    }
    if constexpr (std::is_unsigned_v<Integer>) {
        if (parsed < 0 || static_cast<unsigned long long>(parsed) > static_cast<unsigned long long>((std::numeric_limits<Integer>::max)())) {
            throw std::out_of_range(std::string(name) + " is outside the supported integer range.");
        }
    } else {
        if (parsed < static_cast<long long>((std::numeric_limits<Integer>::min)()) ||
            parsed > static_cast<long long>((std::numeric_limits<Integer>::max)())) {
            throw std::out_of_range(std::string(name) + " is outside the supported integer range.");
        }
    }
    target = static_cast<Integer>(parsed);
}

void ApplySeconds(const System::Text::Json::JsonElement& object, std::string_view name, std::chrono::seconds& target) {
    System::Text::Json::JsonElement value;
    long long parsed = 0;
    if (TryGetPropertyIgnoreCase(object, name, value) && JsonAsInt64(value, parsed)) {
        if (parsed < 0) {
            throw std::out_of_range(std::string(name) + " cannot be negative.");
        }
        target = std::chrono::seconds(parsed);
    }
}

void ApplyMilliseconds(const System::Text::Json::JsonElement& object, std::string_view name, std::chrono::milliseconds& target) {
    System::Text::Json::JsonElement value;
    long long parsed = 0;
    if (TryGetPropertyIgnoreCase(object, name, value) && JsonAsInt64(value, parsed)) {
        if (parsed < 0) {
            throw std::out_of_range(std::string(name) + " cannot be negative.");
        }
        target = std::chrono::milliseconds(parsed);
    }
}

MachineConfigMode ParseMachineConfigMode(std::string value) {
    value = ToLowerAscii(Trim(std::move(value)));
    if (value == "disabled" || value == "disable" || value == "none" || value == "off") {
        return MachineConfigMode::Disabled;
    }
    if (value == "ensure" || value == "add" || value == "create") {
        return MachineConfigMode::Ensure;
    }
    if (value == "refresh" || value == "replace" || value == "update") {
        return MachineConfigMode::Refresh;
    }
    if (value == "delete" || value == "remove") {
        return MachineConfigMode::Delete;
    }
    throw std::invalid_argument("Unknown machine config mode: " + value);
}

void ApplyMachineConfigMode(const System::Text::Json::JsonElement& object, std::string_view name, MachineConfigMode& target) {
    System::Text::Json::JsonElement value;
    if (TryGetPropertyIgnoreCase(object, name, value) && !IsJsonNullish(value)) {
        auto text = JsonAsString(value);
        if (text) {
            target = ParseMachineConfigMode(*text);
        }
    }
}

void ApplyStringList(const System::Text::Json::JsonElement& object, std::string_view name, std::vector<std::string>& target) {
    System::Text::Json::JsonElement value;
    if (TryGetPropertyIgnoreCase(object, name, value) && !IsJsonNullish(value)) {
        target = JsonAsStringList(value);
    }
}

void ApplyUrlReservationOptions(const System::Text::Json::JsonElement& object, UrlReservationOptions& target) {
    ApplyMachineConfigMode(object, "mode", target.Mode);
    ApplyWString(object, "securityDescriptor", target.SecurityDescriptor);
    ApplyBool(object, "removeOnStop", target.RemoveOnStop);
    ApplyBool(object, "ignoreAccessDenied", target.IgnoreAccessDenied);
}

void ApplyTimeoutOptions(const System::Text::Json::JsonElement& object, HttpSysTimeoutOptions& target) {
    ApplySeconds(object, "entityBodySeconds", target.EntityBody);
    ApplySeconds(object, "drainEntityBodySeconds", target.DrainEntityBody);
    ApplySeconds(object, "requestQueueSeconds", target.RequestQueue);
    ApplySeconds(object, "idleConnectionSeconds", target.IdleConnection);
    ApplySeconds(object, "headerWaitSeconds", target.HeaderWait);
    ApplyInteger(object, "minSendRateBytesPerSecond", target.MinSendRateBytesPerSecond);
}

void ApplyProblemDetailsOptions(const System::Text::Json::JsonElement& object, ProblemDetailsOptions& target) {
    ApplyString(object, "defaultType", target.DefaultType);
    ApplyBool(object, "includeType", target.IncludeType);
    ApplyBool(object, "includeInstance", target.IncludeInstance);
    ApplyBool(object, "includeTraceIdentifier", target.IncludeTraceIdentifier);
    ApplyString(object, "traceIdentifierExtensionName", target.TraceIdentifierExtensionName);
}

void ApplySslBindingOptions(const System::Text::Json::JsonElement& object, SslCertificateBinding& target) {
    ApplyString(object, "ip", target.Ip);
    ApplyInteger(object, "port", target.Port);
    ApplyString(object, "certificateHashHex", target.CertificateHashHex);
    ApplyWString(object, "certificateStoreName", target.CertificateStoreName);
    ApplyString(object, "appId", target.AppId);
    ApplyMachineConfigMode(object, "mode", target.Mode);
    ApplyBool(object, "removeOnStop", target.RemoveOnStop);
    ApplyBool(object, "ignoreAccessDenied", target.IgnoreAccessDenied);
    ApplyBool(object, "disableHttp2", target.DisableHttp2);
    ApplyBool(object, "disableQuic", target.DisableQuic);
    ApplyBool(object, "disableTls13", target.DisableTls13);
    ApplyBool(object, "disableLegacyTls", target.DisableLegacyTls);
    ApplyBool(object, "negotiateClientCertificate", target.NegotiateClientCertificate);
}

void ApplySslBindings(const System::Text::Json::JsonElement& value, std::vector<SslCertificateBinding>& target) {
    target.clear();
    if (value.ValueKind() == System::Text::Json::JsonValueKind::Array) {
        for (const auto& item : value.EnumerateArray()) {
            if (item.ValueKind() != System::Text::Json::JsonValueKind::Object) {
                continue;
            }
            SslCertificateBinding binding;
            ApplySslBindingOptions(item, binding);
            target.push_back(std::move(binding));
        }
    } else if (value.ValueKind() == System::Text::Json::JsonValueKind::Object) {
        SslCertificateBinding binding;
        ApplySslBindingOptions(value, binding);
        target.push_back(std::move(binding));
    }
}

void ApplyHttpSysOptions(const System::Text::Json::JsonElement& object, HttpSysOptions& target) {
    ApplyStringList(object, "urlPrefixes", target.UrlPrefixes);
    ApplyStringList(object, "urls", target.UrlPrefixes);
    ApplyInteger(object, "workerCount", target.WorkerCount);
    ApplyInteger(object, "requestBufferSize", target.RequestBufferSize);
    ApplyInteger(object, "maxRequestBodyBytes", target.MaxRequestBodyBytes);
    ApplyInteger(object, "maxConcurrentRequests", target.MaxConcurrentRequests);
    ApplyMilliseconds(object, "stopTimeoutMs", target.StopTimeout);
    ApplyBool(object, "addServerHeader", target.AddServerHeader);
    ApplyString(object, "serverHeader", target.ServerHeader);

    System::Text::Json::JsonElement section;
    if (TryGetPropertyIgnoreCase(object, "urlReservation", section) && section.ValueKind() == System::Text::Json::JsonValueKind::Object) {
        ApplyUrlReservationOptions(section, target.UrlReservation);
    }
    if (TryGetPropertyIgnoreCase(object, "timeouts", section) && section.ValueKind() == System::Text::Json::JsonValueKind::Object) {
        ApplyTimeoutOptions(section, target.Timeouts);
    }
    if (TryGetPropertyIgnoreCase(object, "sslCertificateBindings", section) ||
        TryGetPropertyIgnoreCase(object, "sslBindings", section)) {
        ApplySslBindings(section, target.SslCertificateBindings);
    }
}

void ApplyWebApplicationOptions(const System::Text::Json::JsonElement& root, WebApplicationOptions& target) {
    if (root.ValueKind() != System::Text::Json::JsonValueKind::Object) {
        throw std::invalid_argument("CapiWeb configuration root must be a JSON object.");
    }

    ApplyBool(root, "detailedErrors", target.DetailedErrors);
    ApplyStringList(root, "urls", target.HttpSys.UrlPrefixes);

    System::Text::Json::JsonElement section;
    if (TryGetPropertyIgnoreCase(root, "httpSys", section) && section.ValueKind() == System::Text::Json::JsonValueKind::Object) {
        ApplyHttpSysOptions(section, target.HttpSys);
    }
    if (TryGetPropertyIgnoreCase(root, "problemDetails", section) && section.ValueKind() == System::Text::Json::JsonValueKind::Object) {
        ApplyProblemDetailsOptions(section, target.ProblemDetails);
    }
}

std::optional<std::string> GetEnvironmentValue(const std::string& prefix, std::string_view name) {
    std::string environmentName = prefix + std::string(name);
    DWORD required = GetEnvironmentVariableA(environmentName.c_str(), nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }

    std::string value(required, '\0');
    DWORD copied = GetEnvironmentVariableA(environmentName.c_str(), value.data(), required);
    if (copied == 0 || copied >= required) {
        return std::nullopt;
    }
    value.resize(copied);
    return value;
}

bool HasAnyEnvironmentValue(const std::string& prefix, std::initializer_list<std::string_view> names) {
    for (std::string_view name : names) {
        if (GetEnvironmentValue(prefix, name)) {
            return true;
        }
    }
    return false;
}

bool ParseBoolText(std::string value, std::string_view name) {
    value = ToLowerAscii(Trim(std::move(value)));
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    throw std::invalid_argument(std::string(name) + " must be a boolean value.");
}

long long ParseInt64Text(std::string value, std::string_view name) {
    value = Trim(std::move(value));
    try {
        std::size_t consumed = 0;
        long long parsed = std::stoll(value, &consumed, 10);
        if (consumed == value.size()) {
            return parsed;
        }
    } catch (...) {
    }
    throw std::invalid_argument(std::string(name) + " must be an integer value.");
}

template <class Integer>
void ApplyEnvironmentInteger(const std::string& prefix, std::string_view name, Integer& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (!value) {
        return;
    }
    long long parsed = ParseInt64Text(*value, name);
    if constexpr (std::is_unsigned_v<Integer>) {
        if (parsed < 0 || static_cast<unsigned long long>(parsed) > static_cast<unsigned long long>((std::numeric_limits<Integer>::max)())) {
            throw std::out_of_range(std::string(name) + " is outside the supported integer range.");
        }
    } else {
        if (parsed < static_cast<long long>((std::numeric_limits<Integer>::min)()) ||
            parsed > static_cast<long long>((std::numeric_limits<Integer>::max)())) {
            throw std::out_of_range(std::string(name) + " is outside the supported integer range.");
        }
    }
    target = static_cast<Integer>(parsed);
}

void ApplyEnvironmentBool(const std::string& prefix, std::string_view name, bool& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (value) {
        target = ParseBoolText(*value, name);
    }
}

void ApplyEnvironmentString(const std::string& prefix, std::string_view name, std::string& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (value) {
        target = *value;
    }
}

void ApplyEnvironmentWString(const std::string& prefix, std::string_view name, std::wstring& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (value) {
        target = Utf8ToWide(*value);
    }
}

void ApplyEnvironmentSeconds(const std::string& prefix, std::string_view name, std::chrono::seconds& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (!value) {
        return;
    }
    long long parsed = ParseInt64Text(*value, name);
    if (parsed < 0) {
        throw std::out_of_range(std::string(name) + " cannot be negative.");
    }
    target = std::chrono::seconds(parsed);
}

void ApplyEnvironmentMilliseconds(const std::string& prefix, std::string_view name, std::chrono::milliseconds& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (!value) {
        return;
    }
    long long parsed = ParseInt64Text(*value, name);
    if (parsed < 0) {
        throw std::out_of_range(std::string(name) + " cannot be negative.");
    }
    target = std::chrono::milliseconds(parsed);
}

std::vector<std::string> ParseEnvironmentList(std::string value) {
    std::vector<std::string> result;
    char separator = value.find(';') == std::string::npos ? ',' : ';';
    std::size_t position = 0;
    while (position <= value.size()) {
        std::size_t next = value.find(separator, position);
        std::string token = Trim(value.substr(position, next == std::string::npos ? std::string::npos : next - position));
        if (!token.empty()) {
            result.push_back(std::move(token));
        }
        if (next == std::string::npos) {
            break;
        }
        position = next + 1;
    }
    return result;
}

void ApplyEnvironmentStringList(const std::string& prefix, std::string_view name, std::vector<std::string>& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (value) {
        target = ParseEnvironmentList(*value);
    }
}

void ApplyEnvironmentMachineConfigMode(const std::string& prefix, std::string_view name, MachineConfigMode& target) {
    auto value = GetEnvironmentValue(prefix, name);
    if (value) {
        target = ParseMachineConfigMode(*value);
    }
}

void ApplyEnvironmentTimeoutOptions(const std::string& prefix, HttpSysTimeoutOptions& target) {
    ApplyEnvironmentSeconds(prefix, "HTTP_SYS__TIMEOUTS__ENTITY_BODY_SECONDS", target.EntityBody);
    ApplyEnvironmentSeconds(prefix, "HTTP_SYS__TIMEOUTS__DRAIN_ENTITY_BODY_SECONDS", target.DrainEntityBody);
    ApplyEnvironmentSeconds(prefix, "HTTP_SYS__TIMEOUTS__REQUEST_QUEUE_SECONDS", target.RequestQueue);
    ApplyEnvironmentSeconds(prefix, "HTTP_SYS__TIMEOUTS__IDLE_CONNECTION_SECONDS", target.IdleConnection);
    ApplyEnvironmentSeconds(prefix, "HTTP_SYS__TIMEOUTS__HEADER_WAIT_SECONDS", target.HeaderWait);
    ApplyEnvironmentInteger(prefix, "HTTP_SYS__TIMEOUTS__MIN_SEND_RATE_BYTES_PER_SECOND", target.MinSendRateBytesPerSecond);
}

void ApplyEnvironmentUrlReservationOptions(const std::string& prefix, UrlReservationOptions& target) {
    ApplyEnvironmentMachineConfigMode(prefix, "HTTP_SYS__URL_RESERVATION__MODE", target.Mode);
    ApplyEnvironmentWString(prefix, "HTTP_SYS__URL_RESERVATION__SECURITY_DESCRIPTOR", target.SecurityDescriptor);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__URL_RESERVATION__REMOVE_ON_STOP", target.RemoveOnStop);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__URL_RESERVATION__IGNORE_ACCESS_DENIED", target.IgnoreAccessDenied);
}

void ApplyEnvironmentProblemDetailsOptions(const std::string& prefix, ProblemDetailsOptions& target) {
    ApplyEnvironmentString(prefix, "PROBLEM_DETAILS__DEFAULT_TYPE", target.DefaultType);
    ApplyEnvironmentBool(prefix, "PROBLEM_DETAILS__INCLUDE_TYPE", target.IncludeType);
    ApplyEnvironmentBool(prefix, "PROBLEM_DETAILS__INCLUDE_INSTANCE", target.IncludeInstance);
    ApplyEnvironmentBool(prefix, "PROBLEM_DETAILS__INCLUDE_TRACE_IDENTIFIER", target.IncludeTraceIdentifier);
    ApplyEnvironmentString(prefix, "PROBLEM_DETAILS__TRACE_IDENTIFIER_EXTENSION_NAME", target.TraceIdentifierExtensionName);
}

void ApplyEnvironmentSslBinding(const std::string& prefix, std::vector<SslCertificateBinding>& target) {
    if (!HasAnyEnvironmentValue(prefix, {
            "HTTP_SYS__SSL_CERTIFICATE_BINDING__IP",
            "HTTP_SYS__SSL_CERTIFICATE_BINDING__PORT",
            "HTTP_SYS__SSL_CERTIFICATE_BINDING__CERTIFICATE_HASH_HEX",
            "HTTP_SYS__SSL_CERTIFICATE_BINDING__CERTIFICATE_STORE_NAME",
            "HTTP_SYS__SSL_CERTIFICATE_BINDING__APP_ID",
            "HTTP_SYS__SSL_CERTIFICATE_BINDING__MODE" })) {
        return;
    }

    SslCertificateBinding binding = target.empty() ? SslCertificateBinding{} : target.front();
    ApplyEnvironmentString(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__IP", binding.Ip);
    ApplyEnvironmentInteger(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__PORT", binding.Port);
    ApplyEnvironmentString(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__CERTIFICATE_HASH_HEX", binding.CertificateHashHex);
    ApplyEnvironmentWString(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__CERTIFICATE_STORE_NAME", binding.CertificateStoreName);
    ApplyEnvironmentString(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__APP_ID", binding.AppId);
    ApplyEnvironmentMachineConfigMode(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__MODE", binding.Mode);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__REMOVE_ON_STOP", binding.RemoveOnStop);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__IGNORE_ACCESS_DENIED", binding.IgnoreAccessDenied);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__DISABLE_HTTP2", binding.DisableHttp2);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__DISABLE_QUIC", binding.DisableQuic);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__DISABLE_TLS13", binding.DisableTls13);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__DISABLE_LEGACY_TLS", binding.DisableLegacyTls);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__SSL_CERTIFICATE_BINDING__NEGOTIATE_CLIENT_CERTIFICATE", binding.NegotiateClientCertificate);
    target = { std::move(binding) };
}

void ApplyEnvironmentHttpSysOptions(const std::string& prefix, HttpSysOptions& target) {
    ApplyEnvironmentStringList(prefix, "HTTP_SYS__URL_PREFIXES", target.UrlPrefixes);
    ApplyEnvironmentStringList(prefix, "HTTP_SYS__URLS", target.UrlPrefixes);
    ApplyEnvironmentInteger(prefix, "HTTP_SYS__WORKER_COUNT", target.WorkerCount);
    ApplyEnvironmentInteger(prefix, "HTTP_SYS__REQUEST_BUFFER_SIZE", target.RequestBufferSize);
    ApplyEnvironmentInteger(prefix, "HTTP_SYS__MAX_REQUEST_BODY_BYTES", target.MaxRequestBodyBytes);
    ApplyEnvironmentInteger(prefix, "HTTP_SYS__MAX_CONCURRENT_REQUESTS", target.MaxConcurrentRequests);
    ApplyEnvironmentMilliseconds(prefix, "HTTP_SYS__STOP_TIMEOUT_MS", target.StopTimeout);
    ApplyEnvironmentBool(prefix, "HTTP_SYS__ADD_SERVER_HEADER", target.AddServerHeader);
    ApplyEnvironmentString(prefix, "HTTP_SYS__SERVER_HEADER", target.ServerHeader);
    ApplyEnvironmentUrlReservationOptions(prefix, target.UrlReservation);
    ApplyEnvironmentTimeoutOptions(prefix, target.Timeouts);
    ApplyEnvironmentSslBinding(prefix, target.SslCertificateBindings);
}

void ApplyEnvironmentOptions(const std::string& prefix, WebApplicationOptions& target) {
    ApplyEnvironmentBool(prefix, "DETAILED_ERRORS", target.DetailedErrors);
    ApplyEnvironmentStringList(prefix, "URLS", target.HttpSys.UrlPrefixes);
    ApplyEnvironmentHttpSysOptions(prefix, target.HttpSys);
    ApplyEnvironmentProblemDetailsOptions(prefix, target.ProblemDetails);
}

HTTP_HEADER_ID ResponseKnownHeaderId(std::string_view name, bool& known) {
    known = true;
    if (EqualsIgnoreCase(name, "Cache-Control")) return HttpHeaderCacheControl;
    if (EqualsIgnoreCase(name, "Connection")) return HttpHeaderConnection;
    if (EqualsIgnoreCase(name, "Date")) return HttpHeaderDate;
    if (EqualsIgnoreCase(name, "Keep-Alive")) return HttpHeaderKeepAlive;
    if (EqualsIgnoreCase(name, "Pragma")) return HttpHeaderPragma;
    if (EqualsIgnoreCase(name, "Trailer")) return HttpHeaderTrailer;
    if (EqualsIgnoreCase(name, "Transfer-Encoding")) return HttpHeaderTransferEncoding;
    if (EqualsIgnoreCase(name, "Upgrade")) return HttpHeaderUpgrade;
    if (EqualsIgnoreCase(name, "Via")) return HttpHeaderVia;
    if (EqualsIgnoreCase(name, "Warning")) return HttpHeaderWarning;
    if (EqualsIgnoreCase(name, "Allow")) return HttpHeaderAllow;
    if (EqualsIgnoreCase(name, "Content-Length")) return HttpHeaderContentLength;
    if (EqualsIgnoreCase(name, "Content-Type")) return HttpHeaderContentType;
    if (EqualsIgnoreCase(name, "Content-Encoding")) return HttpHeaderContentEncoding;
    if (EqualsIgnoreCase(name, "Content-Language")) return HttpHeaderContentLanguage;
    if (EqualsIgnoreCase(name, "Content-Location")) return HttpHeaderContentLocation;
    if (EqualsIgnoreCase(name, "Content-MD5")) return HttpHeaderContentMd5;
    if (EqualsIgnoreCase(name, "Content-Range")) return HttpHeaderContentRange;
    if (EqualsIgnoreCase(name, "Expires")) return HttpHeaderExpires;
    if (EqualsIgnoreCase(name, "Last-Modified")) return HttpHeaderLastModified;
    if (EqualsIgnoreCase(name, "Accept-Ranges")) return HttpHeaderAcceptRanges;
    if (EqualsIgnoreCase(name, "Age")) return HttpHeaderAge;
    if (EqualsIgnoreCase(name, "ETag")) return HttpHeaderEtag;
    if (EqualsIgnoreCase(name, "Location")) return HttpHeaderLocation;
    if (EqualsIgnoreCase(name, "Proxy-Authenticate")) return HttpHeaderProxyAuthenticate;
    if (EqualsIgnoreCase(name, "Retry-After")) return HttpHeaderRetryAfter;
    if (EqualsIgnoreCase(name, "Server")) return HttpHeaderServer;
    if (EqualsIgnoreCase(name, "Vary")) return HttpHeaderVary;
    if (EqualsIgnoreCase(name, "WWW-Authenticate")) return HttpHeaderWwwAuthenticate;
    known = false;
    return HttpHeaderResponseMaximum;
}

}  // namespace

class HttpSysServer {
public:
    HttpSysServer(HttpSysOptions options, Logger logger, bool detailedErrors, ProblemDetailsOptions problemDetailsOptions)
        : options_(std::move(options)),
          logger_(std::move(logger)),
          detailedErrors_(detailedErrors),
          problemDetailsOptions_(std::move(problemDetailsOptions)) {
    }

    ~HttpSysServer() {
        Stop();
    }

    void Start(std::function<HttpResult(HttpContext&)> handler) {
        if (running_.load()) {
            return;
        }
        handler_ = std::move(handler);
        accepting_.store(false);
        activeRequests_.store(0);
        for (std::string& prefix : options_.UrlPrefixes) {
            prefix = NormalizeUrlPrefix(std::move(prefix));
        }
        UrlReservationManager::Apply(options_.UrlPrefixes, options_.UrlReservation);
        SslBindingManager::Apply(options_.SslCertificateBindings);

        apiScope_ = std::make_unique<HttpApiScope>(HTTP_INITIALIZE_SERVER);

        ULONG result = HttpCreateServerSession(HttpApiVersion, &serverSessionId_, 0);
        if (result != NO_ERROR) {
            throw HttpSysException("HttpCreateServerSession", result);
        }

        result = HttpCreateUrlGroup(serverSessionId_, &urlGroupId_, 0);
        if (result != NO_ERROR) {
            throw HttpSysException("HttpCreateUrlGroup", result);
        }

        result = HttpCreateRequestQueue(HttpApiVersion, nullptr, nullptr, 0, &requestQueue_);
        if (result != NO_ERROR) {
            throw HttpSysException("HttpCreateRequestQueue", result);
        }

        HTTP_TIMEOUT_LIMIT_INFO timeoutInfo = BuildTimeoutInfo(options_.Timeouts);
        result = HttpSetUrlGroupProperty(urlGroupId_, HttpServerTimeoutsProperty, &timeoutInfo, sizeof(timeoutInfo));
        if (result != NO_ERROR) {
            throw HttpSysException("HttpSetUrlGroupProperty(HttpServerTimeoutsProperty)", result);
        }

        HTTP_BINDING_INFO bindingInfo{};
        bindingInfo.Flags.Present = 1;
        bindingInfo.RequestQueueHandle = requestQueue_;
        result = HttpSetUrlGroupProperty(urlGroupId_, HttpServerBindingProperty, &bindingInfo, sizeof(bindingInfo));
        if (result != NO_ERROR) {
            throw HttpSysException("HttpSetUrlGroupProperty(HttpServerBindingProperty)", result);
        }

        for (const std::string& prefix : options_.UrlPrefixes) {
            std::wstring widePrefix = Utf8ToWide(prefix);
            result = HttpAddUrlToUrlGroup(urlGroupId_, widePrefix.c_str(), 0, 0);
            if (result != NO_ERROR) {
                throw HttpSysException("HttpAddUrlToUrlGroup(" + prefix + ")", result);
            }
        }

        SetUrlGroupState(HttpEnabledStateActive);
        accepting_.store(true);
        running_.store(true);
        std::uint32_t workerCount = options_.WorkerCount;
        if (workerCount == 0) {
            workerCount = std::max<std::uint32_t>(2, std::thread::hardware_concurrency());
        }
        workers_.reserve(workerCount);
        for (std::uint32_t index = 0; index < workerCount; ++index) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
        Log(logger_, LogLevel::Information, "HTTP.sys server started.");
    }

    void Stop() {
        if (!running_.exchange(false)) {
            accepting_.store(false);
            CleanupHandles();
            return;
        }

        accepting_.store(false);
        TrySetUrlGroupState(HttpEnabledStateInactive);
        if (!WaitForActiveRequestsToDrain(options_.StopTimeout)) {
            Log(logger_, LogLevel::Warning,
                "Graceful shutdown timed out with " + std::to_string(activeRequests_.load()) +
                    " active request(s).");
        }

        if (requestQueue_ != nullptr) {
            HttpShutdownRequestQueue(requestQueue_);
        }

        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        CleanupHandles();

        if (options_.UrlReservation.RemoveOnStop) {
            try {
                UrlReservationManager::Remove(options_.UrlPrefixes);
            } catch (const std::exception& ex) {
                Log(logger_, LogLevel::Warning, ex.what());
            }
        }
        for (const SslCertificateBinding& binding : options_.SslCertificateBindings) {
            if (!binding.RemoveOnStop) {
                continue;
            }
            try {
                SslBindingManager::Remove(binding);
            } catch (const std::exception& ex) {
                Log(logger_, LogLevel::Warning, ex.what());
            }
        }
        Log(logger_, LogLevel::Information, "HTTP.sys server stopped.");
    }

    bool IsRunning() const noexcept {
        return running_.load();
    }

    std::uint64_t ActiveRequestCount() const noexcept {
        return activeRequests_.load();
    }

private:
    class RequestSlot {
    public:
        explicit RequestSlot(HttpSysServer& server) noexcept
            : server_(&server) {
        }

        ~RequestSlot() {
            if (server_ != nullptr) {
                server_->ReleaseRequestSlot();
            }
        }

        RequestSlot(const RequestSlot&) = delete;
        RequestSlot& operator=(const RequestSlot&) = delete;

        RequestSlot(RequestSlot&& other) noexcept
            : server_(std::exchange(other.server_, nullptr)) {
        }

        RequestSlot& operator=(RequestSlot&& other) noexcept {
            if (this != &other) {
                if (server_ != nullptr) {
                    server_->ReleaseRequestSlot();
                }
                server_ = std::exchange(other.server_, nullptr);
            }
            return *this;
        }

    private:
        HttpSysServer* server_ = nullptr;
    };

    void SetUrlGroupState(HTTP_ENABLED_STATE state) {
        if (urlGroupId_ == 0) {
            return;
        }
        HTTP_STATE_INFO stateInfo{};
        stateInfo.Flags.Present = 1;
        stateInfo.State = state;
        ULONG result = HttpSetUrlGroupProperty(urlGroupId_, HttpServerStateProperty, &stateInfo, sizeof(stateInfo));
        if (result != NO_ERROR) {
            throw HttpSysException("HttpSetUrlGroupProperty(HttpServerStateProperty)", result);
        }
    }

    void TrySetUrlGroupState(HTTP_ENABLED_STATE state) noexcept {
        try {
            SetUrlGroupState(state);
        } catch (const std::exception& ex) {
            Log(logger_, LogLevel::Warning, ex.what());
        }
    }

    bool TryAcquireRequestSlot() {
        if (!accepting_.load()) {
            return false;
        }

        for (;;) {
            std::uint64_t current = activeRequests_.load();
            if (options_.MaxConcurrentRequests != 0 && current >= options_.MaxConcurrentRequests) {
                return false;
            }
            if (activeRequests_.compare_exchange_weak(current, current + 1)) {
                return true;
            }
        }
    }

    void ReleaseRequestSlot() noexcept {
        std::uint64_t previous = activeRequests_.fetch_sub(1);
        if (previous <= 1) {
            activeRequestsDrained_.notify_all();
        }
    }

    bool WaitForActiveRequestsToDrain(std::chrono::milliseconds timeout) {
        if (activeRequests_.load() == 0) {
            return true;
        }
        std::unique_lock lock(activeRequestsMutex_);
        if (timeout.count() == 0) {
            return activeRequests_.load() == 0;
        }
        return activeRequestsDrained_.wait_for(lock, timeout, [this] {
            return activeRequests_.load() == 0;
        });
    }

    void CleanupHandles() noexcept {
        if (urlGroupId_ != 0) {
            HttpCloseUrlGroup(urlGroupId_);
            urlGroupId_ = 0;
        }
        if (requestQueue_ != nullptr) {
            CloseHandle(requestQueue_);
            requestQueue_ = nullptr;
        }
        if (serverSessionId_ != 0) {
            HttpCloseServerSession(serverSessionId_);
            serverSessionId_ = 0;
        }
        apiScope_.reset();
    }

    void WorkerLoop() {
        while (running_.load()) {
            std::vector<std::uint8_t> buffer(std::max<std::uint32_t>(options_.RequestBufferSize, sizeof(HTTP_REQUEST) + 2048));
            auto* request = reinterpret_cast<PHTTP_REQUEST>(buffer.data());
            HTTP_REQUEST_ID requestId = 0;
            ULONG bytesReturned = 0;
            ULONG result = HttpReceiveHttpRequest(
                requestQueue_,
                requestId,
                HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY,
                request,
                static_cast<ULONG>(buffer.size()),
                &bytesReturned,
                nullptr);

            if (result == ERROR_MORE_DATA) {
                requestId = request->RequestId;
                buffer.resize(bytesReturned);
                request = reinterpret_cast<PHTTP_REQUEST>(buffer.data());
                result = HttpReceiveHttpRequest(
                    requestQueue_,
                    requestId,
                    HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY,
                    request,
                    static_cast<ULONG>(buffer.size()),
                    &bytesReturned,
                    nullptr);
            }

            if (result == NO_ERROR) {
                HandleRequest(*request);
                continue;
            }

            if (!running_.load() || result == ERROR_OPERATION_ABORTED || result == ERROR_INVALID_HANDLE) {
                break;
            }
            Log(logger_, LogLevel::Warning, "HttpReceiveHttpRequest failed: " + FormatWin32Message(result), result);
        }
    }

    std::string ReadBody(const HTTP_REQUEST& request) {
        std::string body;
        auto append = [&](const void* data, std::size_t size) {
            if (options_.MaxRequestBodyBytes != 0 && body.size() + size > options_.MaxRequestBodyBytes) {
                throw std::length_error("Request body exceeds MaxRequestBodyBytes.");
            }
            body.append(static_cast<const char*>(data), size);
        };

        for (USHORT index = 0; index < request.EntityChunkCount; ++index) {
            const HTTP_DATA_CHUNK& chunk = request.pEntityChunks[index];
            if (chunk.DataChunkType == HttpDataChunkFromMemory && chunk.FromMemory.pBuffer != nullptr) {
                append(chunk.FromMemory.pBuffer, chunk.FromMemory.BufferLength);
            }
        }

        if ((request.Flags & HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS) != 0) {
            std::array<char, 64 * 1024> chunkBuffer{};
            for (;;) {
                ULONG bytesRead = 0;
                ULONG result = HttpReceiveRequestEntityBody(
                    requestQueue_,
                    request.RequestId,
                    0,
                    chunkBuffer.data(),
                    static_cast<ULONG>(chunkBuffer.size()),
                    &bytesRead,
                    nullptr);
                if (result == NO_ERROR && bytesRead > 0) {
                    append(chunkBuffer.data(), bytesRead);
                    continue;
                }
                if (result == ERROR_HANDLE_EOF || (result == NO_ERROR && bytesRead == 0)) {
                    break;
                }
                throw HttpSysException("HttpReceiveRequestEntityBody", result);
            }
        }
        return body;
    }

    HttpContext BuildContext(const HTTP_REQUEST& source) {
        HttpContext context;
        context.traceIdentifier_ = GenerateTraceIdentifier();
        HttpRequest& request = context.request_;
        request.method_ = MethodFromHttpRequest(source);
        request.rawUrl_ = source.pRawUrl == nullptr ? std::string() : std::string(source.pRawUrl, source.RawUrlLength);
        if (source.CookedUrl.pAbsPath != nullptr && source.CookedUrl.AbsPathLength > 0) {
            request.path_ = WideToUtf8(std::wstring_view(source.CookedUrl.pAbsPath, source.CookedUrl.AbsPathLength / sizeof(wchar_t)));
        } else {
            request.path_ = "/";
        }
        request.path_ = NormalizeRoutePath(request.path_);
        if (source.CookedUrl.pQueryString != nullptr && source.CookedUrl.QueryStringLength > 0) {
            request.queryString_ = WideToUtf8(std::wstring_view(source.CookedUrl.pQueryString, source.CookedUrl.QueryStringLength / sizeof(wchar_t)));
        }
        request.query_ = ParseQueryString(request.queryString_);
        request.remoteAddress_ = SocketAddressToString(source.Address.pRemoteAddress);
        CopyRequestHeaders(source.Headers, request.headers_);
        if (source.CookedUrl.pHost != nullptr && source.CookedUrl.HostLength > 0) {
            request.host_ = WideToUtf8(std::wstring_view(source.CookedUrl.pHost, source.CookedUrl.HostLength / sizeof(wchar_t)));
        }
        if (request.host_.empty()) {
            request.host_ = request.headers_.Get("Host");
        }
        if (source.CookedUrl.pFullUrl != nullptr && source.CookedUrl.FullUrlLength > 0) {
            std::string fullUrl = WideToUtf8(std::wstring_view(source.CookedUrl.pFullUrl, source.CookedUrl.FullUrlLength / sizeof(wchar_t)));
            std::size_t schemeEnd = fullUrl.find("://");
            if (schemeEnd != std::string::npos) {
                request.scheme_ = ToLowerAscii(fullUrl.substr(0, schemeEnd));
            }
        } else if (source.pSslInfo != nullptr) {
            request.scheme_ = "https";
        } else {
            request.scheme_ = "http";
        }
        request.body_ = ReadBody(source);
        return context;
    }

    HttpResult ExecuteHandler(HttpContext& context) {
        try {
            return handler_(context);
        } catch (const std::length_error& ex) {
            Log(logger_, LogLevel::Warning, ex.what());
            return CreateErrorResult(context, "Payload Too Large", ex, 413, detailedErrors_, problemDetailsOptions_, &logger_);
        } catch (const System::Text::Json::JsonException& ex) {
            Log(logger_, LogLevel::Warning, ex.what());
            return CreateErrorResult(context, "Invalid JSON", ex, 400, detailedErrors_, problemDetailsOptions_, &logger_);
        } catch (const std::exception& ex) {
            Log(logger_, LogLevel::Error, ex.what());
            return CreateErrorResult(context, "Internal Server Error", ex, 500, detailedErrors_, problemDetailsOptions_, &logger_);
        } catch (...) {
            Log(logger_, LogLevel::Error, "Unknown exception.");
            return CreateErrorResult(context, "Internal Server Error", "Unknown exception.", 500, detailedErrors_, problemDetailsOptions_, &logger_);
        }
    }

    void SendServiceUnavailable(const HTTP_REQUEST& source, std::string detail, int retryAfterSeconds) {
        HttpContext context;
        context.traceIdentifier_ = GenerateTraceIdentifier();
        context.request_.method_ = MethodFromHttpRequest(source);
        context.request_.scheme_ = source.pSslInfo == nullptr ? "http" : "https";
        if (source.CookedUrl.pAbsPath != nullptr && source.CookedUrl.AbsPathLength > 0) {
            try {
                context.request_.path_ = NormalizeRoutePath(WideToUtf8(std::wstring_view(
                    source.CookedUrl.pAbsPath,
                    source.CookedUrl.AbsPathLength / sizeof(wchar_t))));
            } catch (...) {
                context.request_.path_ = "/";
            }
        } else {
            context.request_.path_ = "/";
        }

        HttpResult result = CreateProblemResult(context, "Service Unavailable", std::move(detail), 503, problemDetailsOptions_, &logger_);
        if (retryAfterSeconds > 0) {
            result.Header("Retry-After", std::to_string(retryAfterSeconds));
        }
        result.Apply(context.Response());
        SendResponse(source.RequestId, context);
    }

    void HandleRequest(const HTTP_REQUEST& source) {
        if (!accepting_.load()) {
            SendServiceUnavailable(source, "Server is stopping.", 5);
            return;
        }
        if (!TryAcquireRequestSlot()) {
            SendServiceUnavailable(source, "Too many concurrent requests.", 1);
            return;
        }

        RequestSlot slot(*this);
        HttpContext context;
        try {
            context = BuildContext(source);
            HttpResult result = ExecuteHandler(context);
            result.Apply(context.Response());
        } catch (const std::length_error& ex) {
            context.response_ = HttpResponse();
            if (context.traceIdentifier_.empty()) {
                context.traceIdentifier_ = GenerateTraceIdentifier();
            }
            Log(logger_, LogLevel::Warning, ex.what());
            CreateErrorResult(context, "Payload Too Large", ex, 413, detailedErrors_, problemDetailsOptions_, &logger_).Apply(context.Response());
        } catch (const std::exception& ex) {
            context.response_ = HttpResponse();
            if (context.traceIdentifier_.empty()) {
                context.traceIdentifier_ = GenerateTraceIdentifier();
            }
            Log(logger_, LogLevel::Error, ex.what());
            CreateErrorResult(context, "Internal Server Error", ex, 500, detailedErrors_, problemDetailsOptions_, &logger_).Apply(context.Response());
        }
        SendResponse(source.RequestId, context);
    }

    void SendResponse(HTTP_REQUEST_ID requestId, const HttpContext& context) {
        const HttpResponse& source = context.Response();
        HTTP_RESPONSE response{};
        response.StatusCode = static_cast<USHORT>(source.StatusCode());
        std::string reason = StatusReason(source.StatusCode());
        response.pReason = reason.c_str();
        response.ReasonLength = static_cast<USHORT>(reason.size());

        std::vector<HTTP_UNKNOWN_HEADER> unknownHeaders;
        std::vector<std::pair<std::string, std::string>> unknownStorage;
        unknownStorage.reserve(source.Headers().Items().size() + 3);

        bool hasContentLength = source.Headers().Contains("Content-Length");
        bool hasServer = source.Headers().Contains("Server");

        auto addHeader = [&](const std::string& name, const std::string& value) {
            bool known = false;
            HTTP_HEADER_ID id = ResponseKnownHeaderId(name, known);
            if (known && !EqualsIgnoreCase(name, "Set-Cookie")) {
                HTTP_KNOWN_HEADER& header = response.Headers.KnownHeaders[id];
                header.pRawValue = value.c_str();
                header.RawValueLength = static_cast<USHORT>(value.size());
            } else {
                unknownStorage.emplace_back(name, value);
            }
        };

        for (const auto& [name, value] : source.Headers().Items()) {
            addHeader(name, value);
        }
        if (options_.AddServerHeader && !hasServer) {
            addHeader("Server", options_.ServerHeader);
        }

        bool suppressBody = EqualsIgnoreCase(context.Request().Method(), "HEAD") ||
            source.StatusCode() == 204 ||
            source.StatusCode() == 304;

        std::string contentLength;
        if (!hasContentLength) {
            contentLength = std::to_string(source.Body().size());
            addHeader("Content-Length", contentLength);
        }

        unknownHeaders.reserve(unknownStorage.size());
        for (auto& [name, value] : unknownStorage) {
            HTTP_UNKNOWN_HEADER header{};
            header.pName = name.c_str();
            header.NameLength = static_cast<USHORT>(name.size());
            header.pRawValue = value.c_str();
            header.RawValueLength = static_cast<USHORT>(value.size());
            unknownHeaders.push_back(header);
        }
        response.Headers.UnknownHeaderCount = static_cast<USHORT>(unknownHeaders.size());
        response.Headers.pUnknownHeaders = unknownHeaders.empty() ? nullptr : unknownHeaders.data();

        HTTP_DATA_CHUNK chunk{};
        if (!suppressBody && !source.Body().empty()) {
            chunk.DataChunkType = HttpDataChunkFromMemory;
            chunk.FromMemory.pBuffer = const_cast<std::uint8_t*>(source.Body().data());
            chunk.FromMemory.BufferLength = static_cast<ULONG>(source.Body().size());
            response.EntityChunkCount = 1;
            response.pEntityChunks = &chunk;
        }

        ULONG bytesSent = 0;
        ULONG result = HttpSendHttpResponse(
            requestQueue_,
            requestId,
            0,
            &response,
            nullptr,
            &bytesSent,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (result != NO_ERROR) {
            Log(logger_, LogLevel::Warning, "HttpSendHttpResponse failed: " + FormatWin32Message(result), result);
        }
    }

    HttpSysOptions options_;
    Logger logger_;
    bool detailedErrors_ = false;
    ProblemDetailsOptions problemDetailsOptions_;
    std::function<HttpResult(HttpContext&)> handler_;
    std::unique_ptr<HttpApiScope> apiScope_;
    HTTP_SERVER_SESSION_ID serverSessionId_ = 0;
    HTTP_URL_GROUP_ID urlGroupId_ = 0;
    HANDLE requestQueue_ = nullptr;
    std::atomic<bool> running_{ false };
    std::atomic<bool> accepting_{ false };
    std::atomic<std::uint64_t> activeRequests_{ 0 };
    std::mutex activeRequestsMutex_;
    std::condition_variable activeRequestsDrained_;
    std::vector<std::thread> workers_;
};

HttpSysException::HttpSysException(std::string operation, unsigned long errorCode)
    : std::runtime_error(operation + " failed: " + FormatWin32Message(errorCode) + " (" + std::to_string(errorCode) + ")"),
      operation_(std::move(operation)),
      errorCode_(errorCode) {
}

const std::string& HttpSysException::Operation() const noexcept {
    return operation_;
}

unsigned long HttpSysException::ErrorCode() const noexcept {
    return errorCode_;
}

WindowsServiceException::WindowsServiceException(std::string operation, unsigned long errorCode)
    : std::runtime_error(operation + " failed: " + FormatWin32Message(errorCode) + " (" + std::to_string(errorCode) + ")"),
      operation_(std::move(operation)),
      errorCode_(errorCode) {
}

const std::string& WindowsServiceException::Operation() const noexcept {
    return operation_;
}

unsigned long WindowsServiceException::ErrorCode() const noexcept {
    return errorCode_;
}

ClaimsPrincipal ClaimsPrincipal::Anonymous() {
    return {};
}

ClaimsPrincipal ClaimsPrincipal::Authenticated(std::string name, std::string authenticationType) {
    ClaimsPrincipal principal;
    principal.isAuthenticated_ = true;
    principal.name_ = std::move(name);
    principal.authenticationType_ = std::move(authenticationType);
    if (!principal.name_.empty()) {
        principal.AddClaim("name", principal.name_);
    }
    return principal;
}

bool ClaimsPrincipal::IsAuthenticated() const noexcept { return isAuthenticated_; }
const std::string& ClaimsPrincipal::Name() const noexcept { return name_; }
const std::string& ClaimsPrincipal::AuthenticationType() const noexcept { return authenticationType_; }
const std::vector<std::string>& ClaimsPrincipal::Roles() const noexcept { return roles_; }
const std::unordered_map<std::string, std::vector<std::string>>& ClaimsPrincipal::Claims() const noexcept { return claims_; }

ClaimsPrincipal& ClaimsPrincipal::IsAuthenticated(bool authenticated) {
    isAuthenticated_ = authenticated;
    return *this;
}

ClaimsPrincipal& ClaimsPrincipal::Name(std::string name) {
    name_ = std::move(name);
    return *this;
}

ClaimsPrincipal& ClaimsPrincipal::AuthenticationType(std::string authenticationType) {
    authenticationType_ = std::move(authenticationType);
    return *this;
}

ClaimsPrincipal& ClaimsPrincipal::AddRole(std::string role) {
    role = Trim(std::move(role));
    if (!role.empty() && !IsInRole(role)) {
        roles_.push_back(role);
        AddClaim("role", std::move(role));
    }
    return *this;
}

ClaimsPrincipal& ClaimsPrincipal::AddClaim(std::string type, std::string value) {
    type = Trim(std::move(type));
    if (!type.empty()) {
        claims_[std::move(type)].push_back(std::move(value));
    }
    return *this;
}

bool ClaimsPrincipal::IsInRole(std::string_view role) const {
    return std::any_of(roles_.begin(), roles_.end(), [&](const std::string& current) {
        return EqualsIgnoreCase(current, role);
    });
}

bool ClaimsPrincipal::HasClaim(std::string_view type) const {
    return std::any_of(claims_.begin(), claims_.end(), [&](const auto& entry) {
        return EqualsIgnoreCase(entry.first, type) && !entry.second.empty();
    });
}

bool ClaimsPrincipal::HasClaim(std::string_view type, std::string_view value) const {
    for (const auto& [currentType, values] : claims_) {
        if (!EqualsIgnoreCase(currentType, type)) {
            continue;
        }
        for (const std::string& currentValue : values) {
            if (EqualsIgnoreCase(currentValue, value)) {
                return true;
            }
        }
    }
    return false;
}

std::string ClaimsPrincipal::Claim(std::string_view type, std::string defaultValue) const {
    for (const auto& [currentType, values] : claims_) {
        if (EqualsIgnoreCase(currentType, type) && !values.empty()) {
            return values.front();
        }
    }
    return defaultValue;
}

AuthenticationResult AuthenticationResult::Success(ClaimsPrincipal principal) {
    AuthenticationResult result;
    result.succeeded_ = true;
    result.none_ = false;
    result.principal_ = std::move(principal);
    result.principal_.IsAuthenticated(true);
    return result;
}

AuthenticationResult AuthenticationResult::NoResult() {
    return {};
}

AuthenticationResult AuthenticationResult::Fail(std::string failure, std::string challenge) {
    AuthenticationResult result;
    result.none_ = false;
    result.failure_ = std::move(failure);
    result.challenge_ = std::move(challenge);
    return result;
}

bool AuthenticationResult::Succeeded() const noexcept { return succeeded_; }
bool AuthenticationResult::None() const noexcept { return none_; }
bool AuthenticationResult::Failed() const noexcept { return !succeeded_ && !none_; }
const ClaimsPrincipal& AuthenticationResult::Principal() const noexcept { return principal_; }
const std::string& AuthenticationResult::Failure() const noexcept { return failure_; }
const std::string& AuthenticationResult::Challenge() const noexcept { return challenge_; }

void HeaderCollection::Add(std::string name, std::string value) {
    entries_.emplace_back(std::move(name), std::move(value));
}

void HeaderCollection::Set(std::string name, std::string value) {
    Remove(name);
    Add(std::move(name), std::move(value));
}

bool HeaderCollection::Remove(std::string_view name) {
    std::size_t oldSize = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return EqualsIgnoreCase(entry.first, name);
    }), entries_.end());
    return entries_.size() != oldSize;
}

void HeaderCollection::Clear() {
    entries_.clear();
}

bool HeaderCollection::Contains(std::string_view name) const {
    return TryGet(name).has_value();
}

std::optional<std::string> HeaderCollection::TryGet(std::string_view name) const {
    for (const Entry& entry : entries_) {
        if (EqualsIgnoreCase(entry.first, name)) {
            return entry.second;
        }
    }
    return std::nullopt;
}

std::string HeaderCollection::Get(std::string_view name, std::string defaultValue) const {
    auto value = TryGet(name);
    return value ? *value : std::move(defaultValue);
}

std::vector<std::string> HeaderCollection::GetValues(std::string_view name) const {
    std::vector<std::string> values;
    for (const Entry& entry : entries_) {
        if (EqualsIgnoreCase(entry.first, name)) {
            values.push_back(entry.second);
        }
    }
    return values;
}

const std::vector<HeaderCollection::Entry>& HeaderCollection::Items() const noexcept {
    return entries_;
}

const std::string& HttpRequest::Method() const noexcept { return method_; }
const std::string& HttpRequest::Path() const noexcept { return path_; }
const std::string& HttpRequest::QueryString() const noexcept { return queryString_; }
const std::string& HttpRequest::RawUrl() const noexcept { return rawUrl_; }
const std::string& HttpRequest::Body() const noexcept { return body_; }
const std::string& HttpRequest::RemoteAddress() const noexcept { return remoteAddress_; }
const std::string& HttpRequest::Scheme() const noexcept { return scheme_; }
const std::string& HttpRequest::Host() const noexcept { return host_; }
const HeaderCollection& HttpRequest::Headers() const noexcept { return headers_; }
const ValueMap& HttpRequest::Query() const noexcept { return query_; }
const ValueMap& HttpRequest::RouteValues() const noexcept { return routeValues_; }

HttpRequest& HttpRequest::RemoteAddress(std::string remoteAddress) {
    remoteAddress_ = Trim(std::move(remoteAddress));
    return *this;
}

HttpRequest& HttpRequest::Scheme(std::string scheme) {
    scheme = NormalizeForwardedScheme(std::move(scheme));
    if (scheme.empty()) {
        throw std::invalid_argument("Request scheme cannot be empty or invalid.");
    }
    scheme_ = std::move(scheme);
    return *this;
}

HttpRequest& HttpRequest::Host(std::string host) {
    host = Trim(std::move(host));
    if (host.empty() || ContainsUnsafeHeaderValueCharacters(host)) {
        throw std::invalid_argument("Request host cannot be empty or contain CR/LF.");
    }
    host_ = std::move(host);
    headers_.Set("Host", host_);
    return *this;
}

HttpRequest& HttpRequest::Header(std::string name, std::string value) {
    ValidateHeaderName(name, "HttpRequest::Header name");
    headers_.Set(std::move(name), std::move(value));
    return *this;
}

std::string HttpRequest::Header(std::string_view name, std::string defaultValue) const {
    return headers_.Get(name, std::move(defaultValue));
}

std::string HttpRequest::QueryValue(std::string_view name, std::string defaultValue) const {
    auto it = query_.find(std::string(name));
    return it == query_.end() ? std::move(defaultValue) : it->second;
}

std::string HttpRequest::RouteValue(std::string_view name, std::string defaultValue) const {
    auto it = routeValues_.find(std::string(name));
    return it == routeValues_.end() ? std::move(defaultValue) : it->second;
}

std::string HttpRequest::Value(std::string_view name, std::string defaultValue) const {
    auto route = routeValues_.find(std::string(name));
    if (route != routeValues_.end()) {
        return route->second;
    }
    auto query = query_.find(std::string(name));
    return query == query_.end() ? std::move(defaultValue) : query->second;
}

bool HttpRequest::HasJsonContentType() const {
    std::string contentType = ToLowerAscii(Header("Content-Type"));
    return contentType.find("application/json") != std::string::npos ||
        contentType.find("+json") != std::string::npos;
}

System::Text::Json::JsonElement HttpRequest::Json() const {
    if (!jsonDocument_) {
        jsonDocument_ = System::Text::Json::JsonDocument::Parse(body_);
    }
    return jsonDocument_->RootElement();
}

int HttpResponse::StatusCode() const noexcept { return statusCode_; }

HttpResponse& HttpResponse::Status(int statusCode) {
    statusCode_ = statusCode;
    return *this;
}

HeaderCollection& HttpResponse::Headers() noexcept { return headers_; }
const HeaderCollection& HttpResponse::Headers() const noexcept { return headers_; }
const std::vector<std::uint8_t>& HttpResponse::Body() const noexcept { return body_; }

std::string HttpResponse::BodyText() const {
    return std::string(reinterpret_cast<const char*>(body_.data()), body_.size());
}

std::string HttpResponse::ContentType() const {
    return headers_.Get("Content-Type");
}

HttpResponse& HttpResponse::Header(std::string name, std::string value) {
    headers_.Set(std::move(name), std::move(value));
    return *this;
}

HttpResponse& HttpResponse::ContentType(std::string contentType) {
    return Header("Content-Type", std::move(contentType));
}

HttpResponse& HttpResponse::Write(std::string_view text) {
    body_ = StringToBytes(text);
    return *this;
}

HttpResponse& HttpResponse::WriteBytes(const void* data, std::size_t size) {
    const auto* first = static_cast<const std::uint8_t*>(data);
    body_.assign(first, first + size);
    return *this;
}

HttpResponse& HttpResponse::WriteBytes(std::vector<std::uint8_t> bytes) {
    body_ = std::move(bytes);
    return *this;
}

HttpResponse& HttpResponse::ClearBody() {
    body_.clear();
    return *this;
}

HttpRequest& HttpContext::Request() noexcept { return request_; }
const HttpRequest& HttpContext::Request() const noexcept { return request_; }
HttpResponse& HttpContext::Response() noexcept { return response_; }
const HttpResponse& HttpContext::Response() const noexcept { return response_; }
const std::string& HttpContext::TraceIdentifier() const noexcept { return traceIdentifier_; }
HttpContext& HttpContext::TraceIdentifier(std::string traceIdentifier) {
    traceIdentifier = Trim(std::move(traceIdentifier));
    if (traceIdentifier.empty()) {
        throw std::invalid_argument("TraceIdentifier cannot be empty.");
    }
    traceIdentifier_ = std::move(traceIdentifier);
    return *this;
}
std::unordered_map<std::string, std::any>& HttpContext::Items() noexcept { return items_; }
const std::unordered_map<std::string, std::any>& HttpContext::Items() const noexcept { return items_; }
ClaimsPrincipal& HttpContext::User() noexcept { return user_; }
const ClaimsPrincipal& HttpContext::User() const noexcept { return user_; }
HttpContext& HttpContext::User(ClaimsPrincipal user) {
    user_ = std::move(user);
    return *this;
}

bool HttpResult::HasResponse() const noexcept { return hasResponse_; }
int HttpResult::StatusCode() const noexcept { return statusCode_; }
HeaderCollection& HttpResult::Headers() noexcept { return headers_; }
const HeaderCollection& HttpResult::Headers() const noexcept { return headers_; }
const std::vector<std::uint8_t>& HttpResult::Body() const noexcept { return body_; }

HttpResult& HttpResult::Header(std::string name, std::string value) {
    hasResponse_ = true;
    headers_.Set(std::move(name), std::move(value));
    return *this;
}

HttpResult& HttpResult::WriteBytes(std::vector<std::uint8_t> bytes) {
    hasResponse_ = true;
    body_ = std::move(bytes);
    return *this;
}

void HttpResult::Apply(HttpResponse& response) const {
    if (!hasResponse_) {
        return;
    }
    response.Status(statusCode_);
    for (const auto& [name, value] : headers_.Items()) {
        response.Headers().Set(name, value);
    }
    response.WriteBytes(body_);
}

HttpResult HttpResult::Create(int statusCode, std::string contentType, std::vector<std::uint8_t> body) {
    HttpResult result;
    result.hasResponse_ = true;
    result.statusCode_ = statusCode;
    result.body_ = std::move(body);
    if (!contentType.empty()) {
        result.headers_.Set("Content-Type", std::move(contentType));
    }
    return result;
}

HttpResult Results::Empty() {
    return {};
}

HttpResult Results::StatusCode(int statusCode) {
    return HttpResult::Create(statusCode, {}, {});
}

HttpResult Results::Text(std::string_view text, int statusCode, std::string contentType) {
    return HttpResult::Create(statusCode, std::move(contentType), StringToBytes(text));
}

HttpResult Results::Content(std::string_view content, std::string contentType, int statusCode) {
    return HttpResult::Create(statusCode, std::move(contentType), StringToBytes(content));
}

HttpResult Results::Bytes(const void* data, std::size_t size, std::string contentType, int statusCode) {
    const auto* first = static_cast<const std::uint8_t*>(data);
    return HttpResult::Create(statusCode, std::move(contentType), std::vector<std::uint8_t>(first, first + size));
}

HttpResult Results::Bytes(const std::vector<std::uint8_t>& bytes, std::string contentType, int statusCode) {
    return HttpResult::Create(statusCode, std::move(contentType), bytes);
}

HttpResult Results::File(std::vector<std::uint8_t> bytes, std::string contentType, std::string downloadName) {
    HttpResult result = HttpResult::Create(200, std::move(contentType), std::move(bytes));
    if (!downloadName.empty()) {
        result.headers_.Set("Content-Disposition", "attachment; filename=\"" + EscapeQuotedHeaderValue(downloadName) + "\"");
    }
    return result;
}

HttpResult Results::Json(const System::Text::Json::JsonElement& value, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(value), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Json(const System::Text::Json::JsonNode& value, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(value), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Json(std::nullptr_t, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(nullptr), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Json(bool value, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(value), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Json(int value, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(value), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Json(long long value, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(value), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Json(double value, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(value), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Json(std::string_view value, int statusCode) {
    return Content(System::Text::Json::JsonSerializer::Serialize(value), "application/json; charset=utf-8", statusCode);
}

HttpResult Results::Ok() {
    return StatusCode(200);
}

HttpResult Results::Ok(std::string_view text) {
    return Text(text);
}

HttpResult Results::Ok(const System::Text::Json::JsonNode& value) {
    return Json(value);
}

HttpResult Results::Created(std::string location, const System::Text::Json::JsonNode& value) {
    HttpResult result = Json(value, 201);
    result.headers_.Set("Location", std::move(location));
    return result;
}

HttpResult Results::NoContent() {
    return StatusCode(204);
}

HttpResult Results::BadRequest(std::string detail) {
    return Problem("Bad Request", std::move(detail), 400);
}

HttpResult Results::Unauthorized(std::string detail) {
    return Problem("Unauthorized", std::move(detail), 401);
}

HttpResult Results::Forbidden(std::string detail) {
    return Problem("Forbidden", std::move(detail), 403);
}

HttpResult Results::NotFound(std::string detail) {
    return Problem("Not Found", std::move(detail), 404);
}

HttpResult Results::TooManyRequests(std::string detail) {
    return Problem("Too Many Requests", std::move(detail), 429);
}

HttpResult Results::Problem(std::string title, std::string detail, int statusCode) {
    return ProblemDetailsResult(CreateProblemDetails(std::move(title), std::move(detail), statusCode, ProblemDetailsOptions{}));
}

HttpResult Results::Redirect(std::string location, bool permanent) {
    HttpResult result = StatusCode(permanent ? 301 : 302);
    result.headers_.Set("Location", std::move(location));
    return result;
}

HealthCheckResult HealthCheckResult::Healthy(std::string description) {
    return HealthCheckResult{ HealthStatus::Healthy, std::move(description) };
}

HealthCheckResult HealthCheckResult::Degraded(std::string description) {
    return HealthCheckResult{ HealthStatus::Degraded, std::move(description) };
}

HealthCheckResult HealthCheckResult::Unhealthy(std::string description) {
    return HealthCheckResult{ HealthStatus::Unhealthy, std::move(description) };
}

void UrlReservationManager::Apply(const std::vector<std::string>& prefixes, const UrlReservationOptions& options) {
    if (options.Mode == UrlAclMode::Disabled) {
        return;
    }
    try {
        switch (options.Mode) {
        case UrlAclMode::Ensure:
            Ensure(prefixes, options.SecurityDescriptor);
            break;
        case UrlAclMode::Refresh:
            Refresh(prefixes, options.SecurityDescriptor);
            break;
        case UrlAclMode::Delete:
            Remove(prefixes);
            break;
        case UrlAclMode::Disabled:
            break;
        }
    } catch (const HttpSysException& ex) {
        if (options.IgnoreAccessDenied && ex.ErrorCode() == ERROR_ACCESS_DENIED) {
            return;
        }
        throw;
    }
}

void UrlReservationManager::Ensure(const std::vector<std::string>& prefixes, const std::wstring& securityDescriptor) {
    HttpApiScope scope(HTTP_INITIALIZE_CONFIG);
    for (const std::string& prefix : prefixes) {
        AddUrlReservation(prefix, securityDescriptor);
    }
}

void UrlReservationManager::Refresh(const std::vector<std::string>& prefixes, const std::wstring& securityDescriptor) {
    HttpApiScope scope(HTTP_INITIALIZE_CONFIG);
    for (const std::string& prefix : prefixes) {
        DeleteUrlReservation(prefix, true);
        AddUrlReservation(prefix, securityDescriptor);
    }
}

void UrlReservationManager::Remove(const std::vector<std::string>& prefixes, bool ignoreMissing) {
    HttpApiScope scope(HTTP_INITIALIZE_CONFIG);
    for (const std::string& prefix : prefixes) {
        DeleteUrlReservation(prefix, ignoreMissing);
    }
}

void SslBindingManager::Apply(const std::vector<SslCertificateBinding>& bindings) {
    for (const SslCertificateBinding& binding : bindings) {
        if (binding.Mode == MachineConfigMode::Disabled) {
            continue;
        }
        try {
            switch (binding.Mode) {
            case MachineConfigMode::Ensure:
                Ensure(binding);
                break;
            case MachineConfigMode::Refresh:
                Refresh(binding);
                break;
            case MachineConfigMode::Delete:
                Remove(binding);
                break;
            case MachineConfigMode::Disabled:
                break;
            }
        } catch (const HttpSysException& ex) {
            if (binding.IgnoreAccessDenied && ex.ErrorCode() == ERROR_ACCESS_DENIED) {
                continue;
            }
            throw;
        }
    }
}

void SslBindingManager::Ensure(const SslCertificateBinding& binding) {
    HttpApiScope scope(HTTP_INITIALIZE_CONFIG);
    AddSslBinding(binding);
}

void SslBindingManager::Refresh(const SslCertificateBinding& binding) {
    HttpApiScope scope(HTTP_INITIALIZE_CONFIG);
    DeleteSslBinding(binding, true);
    AddSslBinding(binding);
}

void SslBindingManager::Remove(const SslCertificateBinding& binding, bool ignoreMissing) {
    HttpApiScope scope(HTTP_INITIALIZE_CONFIG);
    DeleteSslBinding(binding, ignoreMissing);
}

WebApplicationBuilder::WebApplicationBuilder() = default;

WebApplicationBuilder::WebApplicationBuilder(WebApplicationOptions options)
    : options_(std::move(options)) {
}

WebApplicationBuilder& WebApplicationBuilder::UseUrls(std::initializer_list<std::string> prefixes) {
    options_.HttpSys.UrlPrefixes.assign(prefixes.begin(), prefixes.end());
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::UseUrls(std::vector<std::string> prefixes) {
    options_.HttpSys.UrlPrefixes = std::move(prefixes);
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::ConfigureFromJson(std::string_view json) {
    System::Text::Json::JsonDocumentOptions documentOptions;
    documentOptions.CommentHandling = System::Text::Json::JsonCommentHandling::Skip;
    documentOptions.AllowTrailingCommas = true;
    documentOptions.MaxDepth = 128;

    auto document = System::Text::Json::JsonDocument::Parse(json, documentOptions);
    ApplyWebApplicationOptions(document->RootElement(), options_);
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::ConfigureFromJsonFile(const std::filesystem::path& path, bool optional) {
    if (!std::filesystem::exists(path)) {
        if (optional) {
            return *this;
        }
        throw std::runtime_error("Configuration file not found: " + path.string());
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open configuration file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return ConfigureFromJson(buffer.str());
}

WebApplicationBuilder& WebApplicationBuilder::ConfigureFromEnvironment(std::string prefix) {
    ApplyEnvironmentOptions(prefix, options_);
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::ConfigureHttpSys(std::function<void(HttpSysOptions&)> configure) {
    configure(options_.HttpSys);
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::ConfigureProblemDetails(std::function<void(ProblemDetailsOptions&)> configure) {
    configure(options_.ProblemDetails);
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::ConfigureLogging(Logger logger) {
    options_.Log = std::move(logger);
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::UseDetailedErrors(bool enabled) {
    options_.DetailedErrors = enabled;
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::UseUrlAcl(UrlAclMode mode, std::wstring securityDescriptor) {
    options_.HttpSys.UrlReservation.Mode = mode;
    options_.HttpSys.UrlReservation.SecurityDescriptor = std::move(securityDescriptor);
    return *this;
}

WebApplicationBuilder& WebApplicationBuilder::UseHttps(SslCertificateBinding binding) {
    if (binding.Port == 0) {
        throw std::invalid_argument("HTTPS binding port cannot be 0.");
    }
    options_.HttpSys.SslCertificateBindings.push_back(binding);

    std::string prefix = "https://+:" + std::to_string(binding.Port) + "/";
    bool exists = std::any_of(options_.HttpSys.UrlPrefixes.begin(), options_.HttpSys.UrlPrefixes.end(), [&](const std::string& existing) {
        return EqualsIgnoreCase(NormalizeUrlPrefix(existing), prefix);
    });
    if (!exists) {
        options_.HttpSys.UrlPrefixes.push_back(std::move(prefix));
    }
    return *this;
}

WebApplication WebApplicationBuilder::Build() const {
    return WebApplication(options_);
}

const WebApplicationOptions& WebApplicationBuilder::Options() const noexcept {
    return options_;
}

struct WebApplication::Impl {
    explicit Impl(WebApplicationOptions appOptions)
        : options(std::move(appOptions)) {
    }

    WebApplicationOptions options;
    std::vector<RouteEndpoint> endpoints;
    std::vector<Middleware> middlewares;
    std::vector<std::pair<std::string, HealthCheck>> healthChecks;
    std::shared_ptr<RequestMetricsStore> metrics = std::make_shared<RequestMetricsStore>();
    std::vector<ApplicationCallback> startedCallbacks;
    std::vector<ApplicationCallback> stoppingCallbacks;
    std::vector<ApplicationCallback> stoppedCallbacks;
    std::vector<HostedServiceRegistration> hostedServices;
    std::optional<RouteEndpoint> fallback;
    AuthorizationOptions authorizationOptions;
    bool authorizationConfigured = false;
    std::optional<std::size_t> lastEndpointIndex;
    bool lastEndpointIsFallback = false;
    std::unique_ptr<HttpSysServer> server;
    std::mutex lifecycleMutex;
    mutable std::mutex mutex;
    std::condition_variable stopped;
    std::atomic<bool> running{ false };
    std::atomic<bool> stopping{ false };

    std::string HostedServiceName(std::size_t index) const {
        const std::string& name = hostedServices[index].Name;
        return name.empty() ? "HostedService[" + std::to_string(index) + "]" : name;
    }

    void StopHostedServices(WebApplication& app, std::size_t count) noexcept {
        for (std::size_t offset = 0; offset < count; ++offset) {
            std::size_t index = count - 1 - offset;
            HostedServiceRegistration& service = hostedServices[index];
            if (!service.Started) {
                continue;
            }
            try {
                if (service.Stop) {
                    service.Stop(app);
                }
            } catch (const std::exception& ex) {
                Log(options.Log, LogLevel::Error, "Hosted service '" + HostedServiceName(index) + "' Stop failed: " + ex.what());
            } catch (...) {
                Log(options.Log, LogLevel::Error, "Hosted service '" + HostedServiceName(index) + "' Stop failed with an unknown exception.");
            }
            service.Started = false;
        }
    }

    void StopHostedServices(WebApplication& app) noexcept {
        StopHostedServices(app, hostedServices.size());
    }

    void StartHostedServices(WebApplication& app) {
        std::size_t started = 0;
        try {
            for (std::size_t index = 0; index < hostedServices.size(); ++index) {
                HostedServiceRegistration& service = hostedServices[index];
                if (service.Start) {
                    service.Start(app);
                }
                service.Started = true;
                ++started;
            }
        } catch (const std::exception& ex) {
            Log(options.Log, LogLevel::Error, "Hosted service '" + HostedServiceName(started) + "' Start failed: " + ex.what());
            StopHostedServices(app, started);
            throw;
        } catch (...) {
            Log(options.Log, LogLevel::Error, "Hosted service '" + HostedServiceName(started) + "' Start failed with an unknown exception.");
            StopHostedServices(app, started);
            throw;
        }
    }

    RouteEndpoint& LastEndpoint(std::string_view operation) {
        RouteEndpoint* endpoint = nullptr;
        if (lastEndpointIsFallback) {
            if (fallback) {
                endpoint = &*fallback;
            }
        } else if (lastEndpointIndex && *lastEndpointIndex < endpoints.size()) {
            endpoint = &endpoints[*lastEndpointIndex];
        }

        if (endpoint == nullptr) {
            throw std::logic_error(std::string(operation) + " must be called after mapping an endpoint.");
        }
        return *endpoint;
    }

    HttpResult InvokeEndpoint(const RouteEndpoint& endpoint, HttpContext& current) {
        AuthorizationOptions effectiveAuthorization = authorizationOptions;
        auto authorizationResult = TryAuthorizeEndpoint(endpoint, current, effectiveAuthorization);
        if (authorizationResult) {
            return *authorizationResult;
        }
        return endpoint.Handler(current);
    }

    HttpResult Dispatch(HttpContext& context) {
        RequestDelegate terminal = [this](HttpContext& current) {
            std::vector<std::string> allowedMethods;
            for (const RouteEndpoint& endpoint : endpoints) {
                ValueMap routeValues;
                if (!MatchEndpointPath(endpoint, current.Request(), routeValues)) {
                    continue;
                }

                if (EndpointAllowsMethod(endpoint, current.Request().Method())) {
                    current.request_.routeValues_ = std::move(routeValues);
                    return InvokeEndpoint(endpoint, current);
                }

                for (const std::string& method : endpoint.Methods) {
                    if (!ContainsTokenIgnoreCase(allowedMethods, method)) {
                        allowedMethods.push_back(method);
                    }
                    if (EqualsIgnoreCase(method, "GET") && !ContainsTokenIgnoreCase(allowedMethods, "HEAD")) {
                        allowedMethods.push_back("HEAD");
                    }
                }
            }
            if (fallback && EndpointAllowsMethod(*fallback, current.Request().Method())) {
                return InvokeEndpoint(*fallback, current);
            }
            if (!allowedMethods.empty()) {
                return Results::StatusCode(405).Header("Allow", JoinStrings(allowedMethods, ", "));
            }
            return Results::NotFound("No endpoint matched " + current.Request().Method() + " " + current.Request().Path());
        };

        RequestDelegate pipeline = terminal;
        for (auto it = middlewares.rbegin(); it != middlewares.rend(); ++it) {
            Middleware middleware = *it;
            RequestDelegate next = pipeline;
            pipeline = [middleware, next](HttpContext& current) {
                return middleware(current, next);
            };
        }
        return pipeline(context);
    }

    void NotifyStopped() {
        {
            std::lock_guard lock(mutex);
            running.store(false);
            stopping.store(false);
        }
        stopped.notify_all();
    }
};

namespace {

std::mutex g_registryMutex;
std::vector<WebApplication*> g_runningApplications;
std::once_flag g_consoleHandlerFlag;

BOOL WINAPI ConsoleControlHandler(DWORD controlType) {
    if (controlType != CTRL_C_EVENT &&
        controlType != CTRL_BREAK_EVENT &&
        controlType != CTRL_CLOSE_EVENT &&
        controlType != CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }

    std::vector<WebApplication*> apps;
    {
        std::lock_guard lock(g_registryMutex);
        apps = g_runningApplications;
    }

    std::thread([apps] {
        for (WebApplication* app : apps) {
            if (app != nullptr) {
                app->Stop();
            }
        }
    }).detach();

    return TRUE;
}

void RegisterConsoleHandlerOnce() {
    std::call_once(g_consoleHandlerFlag, [] {
        SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    });
}

void RegisterRunningApplication(WebApplication* app) {
    RegisterConsoleHandlerOnce();
    std::lock_guard lock(g_registryMutex);
    if (std::find(g_runningApplications.begin(), g_runningApplications.end(), app) == g_runningApplications.end()) {
        g_runningApplications.push_back(app);
    }
}

void UnregisterRunningApplication(WebApplication* app) {
    std::lock_guard lock(g_registryMutex);
    g_runningApplications.erase(std::remove(g_runningApplications.begin(), g_runningApplications.end(), app), g_runningApplications.end());
}

class ServiceHandle {
public:
    ServiceHandle() = default;
    explicit ServiceHandle(SC_HANDLE handle) noexcept
        : handle_(handle) {
    }

    ~ServiceHandle() {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
    }

    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;

    ServiceHandle(ServiceHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {
    }

    ServiceHandle& operator=(ServiceHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                CloseServiceHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    SC_HANDLE Get() const noexcept {
        return handle_;
    }

    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    SC_HANDLE handle_ = nullptr;
};

[[noreturn]] void ThrowServiceError(std::string operation, DWORD errorCode = GetLastError()) {
    throw WindowsServiceException(std::move(operation), errorCode);
}

void ValidateServiceName(const std::wstring& serviceName) {
    if (serviceName.empty()) {
        throw std::invalid_argument("Windows service name cannot be empty.");
    }
}

std::wstring EffectiveDisplayName(const WindowsServiceOptions& options) {
    return options.DisplayName.empty() ? options.ServiceName : options.DisplayName;
}

DWORD ToServiceStartType(WindowsServiceStartMode mode) noexcept {
    switch (mode) {
    case WindowsServiceStartMode::Manual:
        return SERVICE_DEMAND_START;
    case WindowsServiceStartMode::Automatic:
    case WindowsServiceStartMode::AutomaticDelayed:
        return SERVICE_AUTO_START;
    default:
        return SERVICE_DEMAND_START;
    }
}

std::wstring QuoteCommandLineArgument(std::wstring_view value) {
    if (value.empty()) {
        return L"\"\"";
    }

    bool needsQuotes = false;
    for (wchar_t ch : value) {
        if (std::iswspace(ch) || ch == L'"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return std::wstring(value);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::filesystem::path CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            ThrowServiceError("GetModuleFileNameW");
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring BuildServiceCommandLine(const std::filesystem::path& executablePath, const std::vector<std::wstring>& arguments) {
    std::wstring commandLine = QuoteCommandLineArgument(executablePath.wstring());
    for (const std::wstring& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += QuoteCommandLineArgument(argument);
    }
    return commandLine;
}

std::vector<wchar_t> BuildServiceDependencies(const std::vector<std::wstring>& dependencies) {
    std::vector<wchar_t> result;
    if (dependencies.empty()) {
        return result;
    }
    for (const std::wstring& dependency : dependencies) {
        if (dependency.empty()) {
            continue;
        }
        result.insert(result.end(), dependency.begin(), dependency.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

ServiceHandle OpenServiceControlManager(DWORD access) {
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, access));
    if (!scm) {
        ThrowServiceError("OpenSCManagerW");
    }
    return scm;
}

SERVICE_STATUS_PROCESS QueryServiceStatusProcess(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
        ThrowServiceError("QueryServiceStatusEx");
    }
    return status;
}

void WaitForServiceState(SC_HANDLE service, DWORD desiredState, std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    SERVICE_STATUS_PROCESS status = QueryServiceStatusProcess(service);
    while (status.dwCurrentState != desiredState) {
        if (status.dwCurrentState == SERVICE_STOPPED && desiredState != SERVICE_STOPPED) {
            ThrowServiceError("WaitForServiceState", status.dwWin32ExitCode == NO_ERROR ? ERROR_SERVICE_NOT_ACTIVE : status.dwWin32ExitCode);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ThrowServiceError("WaitForServiceState", ERROR_TIMEOUT);
        }

        DWORD waitMs = std::clamp(status.dwWaitHint / 10, 250ul, 2000ul);
        Sleep(waitMs);
        status = QueryServiceStatusProcess(service);
    }
}

void StopServiceHandle(SC_HANDLE service, std::chrono::seconds timeout) {
    SERVICE_STATUS_PROCESS status = QueryServiceStatusProcess(service);
    if (status.dwCurrentState == SERVICE_STOPPED) {
        return;
    }

    if (status.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS serviceStatus{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &serviceStatus)) {
            DWORD error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) {
                ThrowServiceError("ControlService(STOP)", error);
            }
        }
    }
    WaitForServiceState(service, SERVICE_STOPPED, timeout);
}

void SetServiceDescription(SC_HANDLE service, const std::wstring& description) {
    if (description.empty()) {
        return;
    }
    SERVICE_DESCRIPTIONW serviceDescription{};
    serviceDescription.lpDescription = const_cast<LPWSTR>(description.c_str());
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &serviceDescription)) {
        ThrowServiceError("ChangeServiceConfig2W(DESCRIPTION)");
    }
}

void SetDelayedAutoStart(SC_HANDLE service, WindowsServiceStartMode startMode) {
    if (startMode == WindowsServiceStartMode::Manual) {
        return;
    }
    SERVICE_DELAYED_AUTO_START_INFO info{};
    info.fDelayedAutostart = startMode == WindowsServiceStartMode::AutomaticDelayed ? TRUE : FALSE;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info)) {
        ThrowServiceError("ChangeServiceConfig2W(DELAYED_AUTO_START)");
    }
}

struct ServiceRuntimeState {
    std::mutex Mutex;
    WebApplication* App = nullptr;
    WindowsServiceOptions Options;
    SERVICE_STATUS_HANDLE StatusHandle = nullptr;
    SERVICE_STATUS Status{};
    DWORD CheckPoint = 1;
    std::atomic<bool> RunningAsService{ false };
};

ServiceRuntimeState g_serviceRuntime;

void SetRuntimeServiceStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0) {
    std::lock_guard lock(g_serviceRuntime.Mutex);
    if (g_serviceRuntime.StatusHandle == nullptr) {
        return;
    }

    g_serviceRuntime.Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceRuntime.Status.dwCurrentState = state;
    g_serviceRuntime.Status.dwWin32ExitCode = win32ExitCode;
    g_serviceRuntime.Status.dwServiceSpecificExitCode = 0;
    g_serviceRuntime.Status.dwWaitHint = waitHint;
    g_serviceRuntime.Status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        g_serviceRuntime.Status.dwCheckPoint = g_serviceRuntime.CheckPoint++;
    } else {
        g_serviceRuntime.Status.dwCheckPoint = 0;
        g_serviceRuntime.CheckPoint = 1;
    }

    SetServiceStatus(g_serviceRuntime.StatusHandle, &g_serviceRuntime.Status);
}

DWORD WINAPI ServiceControlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_INTERROGATE) {
        DWORD currentState = SERVICE_START_PENDING;
        {
            std::lock_guard lock(g_serviceRuntime.Mutex);
            currentState = g_serviceRuntime.Status.dwCurrentState;
        }
        SetRuntimeServiceStatus(currentState);
        return NO_ERROR;
    }

    if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN) {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    WebApplication* app = nullptr;
    {
        std::lock_guard lock(g_serviceRuntime.Mutex);
        app = g_serviceRuntime.App;
    }

    SetRuntimeServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
    if (app != nullptr) {
        std::thread([app] {
            app->Stop();
        }).detach();
    }
    return NO_ERROR;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    WebApplication* app = nullptr;
    std::wstring serviceName;
    {
        std::lock_guard lock(g_serviceRuntime.Mutex);
        app = g_serviceRuntime.App;
        serviceName = g_serviceRuntime.Options.ServiceName;
    }

    SERVICE_STATUS_HANDLE statusHandle = RegisterServiceCtrlHandlerExW(serviceName.c_str(), ServiceControlHandler, nullptr);
    if (statusHandle == nullptr) {
        return;
    }

    {
        std::lock_guard lock(g_serviceRuntime.Mutex);
        g_serviceRuntime.StatusHandle = statusHandle;
        g_serviceRuntime.RunningAsService.store(true);
    }

    SetRuntimeServiceStatus(SERVICE_START_PENDING, NO_ERROR, 30000);
    try {
        if (app == nullptr) {
            throw std::runtime_error("Windows service application is not configured.");
        }
        app->Start();
        SetRuntimeServiceStatus(SERVICE_RUNNING);
        app->WaitForShutdown();
        SetRuntimeServiceStatus(SERVICE_STOPPED);
    } catch (const std::exception& ex) {
        if (app != nullptr) {
            Log(app->Options().Log, LogLevel::Critical, std::string("Windows service failed: ") + ex.what());
        }
        SetRuntimeServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    } catch (...) {
        if (app != nullptr) {
            Log(app->Options().Log, LogLevel::Critical, "Windows service failed with an unknown exception.");
        }
        SetRuntimeServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    }

    {
        std::lock_guard lock(g_serviceRuntime.Mutex);
        g_serviceRuntime.RunningAsService.store(false);
        g_serviceRuntime.StatusHandle = nullptr;
    }
}

void ClearServiceRuntime(WebApplication* app) {
    std::lock_guard lock(g_serviceRuntime.Mutex);
    if (g_serviceRuntime.App == app) {
        g_serviceRuntime.App = nullptr;
        g_serviceRuntime.Options = WindowsServiceOptions{};
        g_serviceRuntime.StatusHandle = nullptr;
        g_serviceRuntime.Status = SERVICE_STATUS{};
        g_serviceRuntime.CheckPoint = 1;
        g_serviceRuntime.RunningAsService.store(false);
    }
}

void RunApplicationCallbacks(const std::vector<ApplicationCallback>& callbacks, const Logger& logger, std::string_view phase) {
    for (const ApplicationCallback& callback : callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (const std::exception& ex) {
            Log(logger, LogLevel::Error, std::string(phase) + " callback failed: " + ex.what());
        } catch (...) {
            Log(logger, LogLevel::Error, std::string(phase) + " callback failed with an unknown exception.");
        }
    }
}

}  // namespace

bool WindowsServiceHost::IsRunningAsService() {
    return g_serviceRuntime.RunningAsService.load();
}

int WindowsServiceHost::Run(WebApplication& app, WindowsServiceOptions options) {
    ValidateServiceName(options.ServiceName);
    bool fallbackToConsole = options.FallbackToConsole;
    std::wstring serviceName = options.ServiceName;

    {
        std::lock_guard lock(g_serviceRuntime.Mutex);
        if (g_serviceRuntime.App != nullptr) {
            throw std::logic_error("Only one Windows service host can run in the current process.");
        }
        g_serviceRuntime.App = &app;
        g_serviceRuntime.Options = std::move(options);
        g_serviceRuntime.Status = SERVICE_STATUS{};
        g_serviceRuntime.CheckPoint = 1;
        g_serviceRuntime.RunningAsService.store(false);
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { serviceName.data(), ServiceMain },
        { nullptr, nullptr },
    };

    if (StartServiceCtrlDispatcherW(serviceTable)) {
        ClearServiceRuntime(&app);
        return 0;
    }

    DWORD error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT && fallbackToConsole) {
        ClearServiceRuntime(&app);
        app.Run();
        return 0;
    }

    ClearServiceRuntime(&app);
    ThrowServiceError("StartServiceCtrlDispatcherW", error);
}

void WindowsServiceHost::Install(const WindowsServiceOptions& options, const std::filesystem::path& executablePath) {
    ValidateServiceName(options.ServiceName);

    std::filesystem::path resolvedExecutable = executablePath.empty()
        ? CurrentExecutablePath()
        : executablePath;
    std::wstring commandLine = BuildServiceCommandLine(resolvedExecutable, options.Arguments);
    std::wstring displayName = EffectiveDisplayName(options);
    std::vector<wchar_t> dependencies = BuildServiceDependencies(options.Dependencies);

    LPCWSTR dependenciesValue = dependencies.empty() ? nullptr : dependencies.data();
    LPCWSTR createAccount = options.AccountName.empty() ? nullptr : options.AccountName.c_str();
    LPCWSTR changeAccount = options.AccountName.empty() ? L"LocalSystem" : options.AccountName.c_str();
    LPCWSTR password = options.Password.empty() ? nullptr : options.Password.c_str();

    ServiceHandle scm = OpenServiceControlManager(SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    ServiceHandle service(CreateServiceW(
        scm.Get(),
        options.ServiceName.c_str(),
        displayName.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        ToServiceStartType(options.StartMode),
        SERVICE_ERROR_NORMAL,
        commandLine.c_str(),
        nullptr,
        nullptr,
        dependenciesValue,
        createAccount,
        password));

    if (!service) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_EXISTS) {
            ThrowServiceError("CreateServiceW", error);
        }

        service = ServiceHandle(OpenServiceW(scm.Get(), options.ServiceName.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS));
        if (!service) {
            ThrowServiceError("OpenServiceW");
        }

        if (!ChangeServiceConfigW(
                service.Get(),
                SERVICE_WIN32_OWN_PROCESS,
                ToServiceStartType(options.StartMode),
                SERVICE_ERROR_NORMAL,
                commandLine.c_str(),
                nullptr,
                nullptr,
                dependenciesValue,
                changeAccount,
                password,
                displayName.c_str())) {
            ThrowServiceError("ChangeServiceConfigW");
        }
    }

    SetServiceDescription(service.Get(), options.Description);
    SetDelayedAutoStart(service.Get(), options.StartMode);
}

void WindowsServiceHost::Uninstall(std::wstring serviceName, std::chrono::seconds stopTimeout) {
    ValidateServiceName(serviceName);

    ServiceHandle scm = OpenServiceControlManager(SC_MANAGER_CONNECT);
    ServiceHandle service(OpenServiceW(scm.Get(), serviceName.c_str(), DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return;
        }
        ThrowServiceError("OpenServiceW", error);
    }

    StopServiceHandle(service.Get(), stopTimeout);
    if (!DeleteService(service.Get())) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_MARKED_FOR_DELETE) {
            ThrowServiceError("DeleteService", error);
        }
    }
}

void WindowsServiceHost::Start(std::wstring serviceName) {
    ValidateServiceName(serviceName);

    ServiceHandle scm = OpenServiceControlManager(SC_MANAGER_CONNECT);
    ServiceHandle service(OpenServiceW(scm.Get(), serviceName.c_str(), SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) {
        ThrowServiceError("OpenServiceW");
    }

    SERVICE_STATUS_PROCESS status = QueryServiceStatusProcess(service.Get());
    if (status.dwCurrentState == SERVICE_RUNNING) {
        return;
    }

    if (!StartServiceW(service.Get(), 0, nullptr)) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            ThrowServiceError("StartServiceW", error);
        }
    }
    WaitForServiceState(service.Get(), SERVICE_RUNNING, std::chrono::seconds(30));
}

void WindowsServiceHost::Stop(std::wstring serviceName, std::chrono::seconds timeout) {
    ValidateServiceName(serviceName);

    ServiceHandle scm = OpenServiceControlManager(SC_MANAGER_CONNECT);
    ServiceHandle service(OpenServiceW(scm.Get(), serviceName.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        ThrowServiceError("OpenServiceW");
    }
    StopServiceHandle(service.Get(), timeout);
}

WebApplication::WebApplication()
    : impl_(std::make_unique<Impl>(WebApplicationOptions{})) {
}

WebApplication::WebApplication(WebApplicationOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {
}

WebApplication::~WebApplication() {
    Stop();
}

WebApplication::WebApplication(WebApplication&& other) noexcept = default;

WebApplication& WebApplication::operator=(WebApplication&& other) noexcept {
    if (this != &other) {
        Stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

WebApplicationBuilder WebApplication::CreateBuilder() {
    return WebApplicationBuilder();
}

WebApplicationBuilder WebApplication::CreateBuilder(WebApplicationOptions options) {
    return WebApplicationBuilder(std::move(options));
}

WebApplication WebApplication::Create() {
    return WebApplication();
}

WebApplication WebApplication::Create(WebApplicationOptions options) {
    return WebApplication(std::move(options));
}

WebApplication& WebApplication::Use(Middleware middleware) {
    impl_->middlewares.push_back(std::move(middleware));
    return *this;
}

WebApplication& WebApplication::UseProblemDetails() {
    return UseProblemDetails(impl_->options.ProblemDetails);
}

WebApplication& WebApplication::UseProblemDetails(ProblemDetailsOptions options) {
    impl_->options.ProblemDetails = options;
    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        return ApplyProblemDetails(context, next(context), options);
    });
    return *this;
}

WebApplication& WebApplication::UseStaticFiles(StaticFileOptions options) {
    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        auto result = TryServeStaticFile(options, context);
        if (result) {
            return *result;
        }
        return next(context);
    });
    return *this;
}

WebApplication& WebApplication::UseResponseCompression(ResponseCompressionOptions options) {
    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        HttpResult result = next(context);
        if (result.HasResponse()) {
            ApplyResponseCompression(context.Request(), result, options);
            return result;
        }
        ApplyResponseCompression(context, options);
        return Results::Empty();
    });
    return *this;
}

WebApplication& WebApplication::UseAuthentication(AuthenticationOptions options) {
    ValidateAuthenticationOptions(options);
    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        AuthenticateRequest(context, options);
        return next(context);
    });
    return *this;
}

WebApplication& WebApplication::UseAuthorization(AuthorizationOptions options) {
    ValidateAuthorizationOptions(options);
    impl_->authorizationOptions = std::move(options);
    impl_->authorizationConfigured = true;
    return *this;
}

WebApplication& WebApplication::UseCors(CorsOptions options) {
    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        const HttpRequest& request = context.Request();
        if (request.Header("Origin").empty()) {
            return next(context);
        }

        bool isPreflight = EqualsIgnoreCase(request.Method(), "OPTIONS") &&
            !request.Header("Access-Control-Request-Method").empty();
        if (isPreflight) {
            return CreateCorsPreflightResult(options, request);
        }

        ApplyCorsHeaders(context.Response().Headers(), options, request);
        HttpResult result = next(context);
        ApplyCorsHeaders(result, options, request);
        return result;
    });
    return *this;
}

WebApplication& WebApplication::UseSecurityHeaders(SecurityHeadersOptions options) {
    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        ApplySecurityHeaders(context.Response().Headers(), options);
        HttpResult result = next(context);
        ApplySecurityHeaders(result, options);
        return result;
    });
    return *this;
}

WebApplication& WebApplication::UseForwardedHeaders(ForwardedHeadersOptions options) {
    if (options.ForwardedFor) {
        ValidateHeaderName(options.ForwardedForHeaderName, "ForwardedHeadersOptions::ForwardedForHeaderName");
    }
    if (options.ForwardedProto) {
        ValidateHeaderName(options.ForwardedProtoHeaderName, "ForwardedHeadersOptions::ForwardedProtoHeaderName");
    }
    if (options.ForwardedHost) {
        ValidateHeaderName(options.ForwardedHostHeaderName, "ForwardedHeadersOptions::ForwardedHostHeaderName");
    }

    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        if (!IsKnownProxy(context.Request().RemoteAddress(), options)) {
            return next(context);
        }

        bool applied = false;
        if (options.ForwardedFor) {
            auto forwardedFor = SelectForwardedValue(context.Request().Header(options.ForwardedForHeaderName), options.ForwardLimit);
            if (forwardedFor && !ContainsUnsafeHeaderValueCharacters(*forwardedFor)) {
                std::string address = NormalizeForwardedAddress(*forwardedFor);
                if (!address.empty()) {
                    context.Request().RemoteAddress(std::move(address));
                    applied = true;
                }
            }
        }

        if (options.ForwardedProto) {
            auto forwardedProto = SelectForwardedValue(context.Request().Header(options.ForwardedProtoHeaderName), options.ForwardLimit);
            if (forwardedProto && !ContainsUnsafeHeaderValueCharacters(*forwardedProto)) {
                std::string scheme = NormalizeForwardedScheme(*forwardedProto);
                if (!scheme.empty()) {
                    context.Request().Scheme(std::move(scheme));
                    applied = true;
                }
            }
        }

        if (options.ForwardedHost) {
            auto forwardedHost = SelectForwardedValue(context.Request().Header(options.ForwardedHostHeaderName), options.ForwardLimit);
            if (forwardedHost && !forwardedHost->empty() && !ContainsUnsafeHeaderValueCharacters(*forwardedHost)) {
                context.Request().Host(*forwardedHost);
                applied = true;
            }
        }

        if (!options.AppliedItemKey.empty()) {
            context.Items()[options.AppliedItemKey] = applied;
        }
        return next(context);
    });
    return *this;
}

WebApplication& WebApplication::UseRequestId(RequestIdOptions options) {
    ValidateHeaderName(options.HeaderName, "RequestIdOptions::HeaderName");
    if (options.AddResponseHeader) {
        ValidateHeaderName(options.ResponseHeaderName, "RequestIdOptions::ResponseHeaderName");
    }

    impl_->middlewares.push_back([options = std::move(options)](HttpContext& context, RequestDelegate next) {
        std::string traceIdentifier = context.TraceIdentifier();
        if (options.TrustIncomingHeader) {
            std::string incoming = Trim(context.Request().Header(options.HeaderName));
            if (!incoming.empty()) {
                traceIdentifier = std::move(incoming);
                context.TraceIdentifier(traceIdentifier);
            }
        }

        if (!options.ItemKey.empty()) {
            context.Items()[options.ItemKey] = traceIdentifier;
        }
        if (options.AddResponseHeader) {
            SetHeaderIfMissing(context.Response().Headers(), options.ResponseHeaderName, traceIdentifier);
        }
        return next(context);
    });
    return *this;
}

WebApplication& WebApplication::UseRateLimiter(RateLimitOptions options) {
    ValidateRateLimitOptions(options);
    auto limiter = std::make_shared<FixedWindowRateLimiter>();
    impl_->middlewares.push_back([limiter, options = std::move(options)](HttpContext& context, RequestDelegate next) {
        if (options.PermitLimit == 0 || options.Window.count() == 0) {
            return next(context);
        }

        RateLimitDecision decision = limiter->Check(context, options);
        if (!options.ItemKey.empty()) {
            context.Items()[options.ItemKey] = decision.Key;
        }

        if (decision.Allowed) {
            AddRateLimitHeaders(context.Response().Headers(), options, decision);
            return next(context);
        }

        HttpResult result = Results::Problem(StatusReason(options.RejectedStatusCode), "Rate limit exceeded.", options.RejectedStatusCode);
        AddRateLimitHeaders(result, options, decision);
        return result;
    });
    return *this;
}

WebApplication& WebApplication::UseRequestLogging(RequestLoggingOptions options) {
    Logger logger = impl_->options.Log;
    impl_->middlewares.push_back([options, logger](HttpContext& context, RequestDelegate next) {
        auto started = std::chrono::steady_clock::now();
        auto buildMessage = [&](int status) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            std::string target = context.Request().Path();
            if (options.IncludeQueryString && !context.Request().QueryString().empty()) {
                target += context.Request().QueryString();
            }

            std::string message = context.Request().Method() + " " + target + " -> " +
                std::to_string(status) + " in " + std::to_string(elapsed.count()) + " ms";
            if (options.IncludeRemoteAddress && !context.Request().RemoteAddress().empty()) {
                message += " from " + context.Request().RemoteAddress();
            }
            if (options.IncludeTraceIdentifier && !context.TraceIdentifier().empty()) {
                message += " traceId=" + context.TraceIdentifier();
            }
            return message;
        };

        try {
            HttpResult result = next(context);
            int status = result.HasResponse() ? result.StatusCode() : context.Response().StatusCode();
            LogLevel level = status >= 500 ? options.FailureLevel : options.SuccessLevel;
            Log(logger, level, buildMessage(status));
            return result;
        } catch (...) {
            Log(logger, options.FailureLevel, buildMessage(500));
            throw;
        }
    });
    return *this;
}

WebApplication& WebApplication::UseMetrics(MetricsOptions options) {
    auto metrics = impl_->metrics;
    impl_->middlewares.push_back([metrics, options](HttpContext& context, RequestDelegate next) {
        metrics->RecordStarted();
        auto started = std::chrono::steady_clock::now();
        try {
            HttpResult result = next(context);
            int status = result.HasResponse() ? result.StatusCode() : context.Response().StatusCode();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            metrics->RecordCompleted(context, status, elapsed, options);
            return result;
        } catch (...) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            metrics->RecordCompleted(context, 500, elapsed, options);
            throw;
        }
    });
    return *this;
}

WebApplication& WebApplication::MapMetrics(MetricsEndpointOptions options) {
    if (options.Path.empty()) {
        options.Path = "/metrics";
    }
    auto metrics = impl_->metrics;
    return MapGet(options.Path, [this, metrics, options = std::move(options)] {
        System::Text::Json::JsonObject payload = metrics->Snapshot(options);
        payload.Add("isRunning", System::Text::Json::JsonNode::Create(IsRunning()));
        payload.Add("isStopping", System::Text::Json::JsonNode::Create(IsStopping()));
        payload.Add("httpSysActiveRequests", System::Text::Json::JsonNode::Create(static_cast<long long>(ActiveRequestCount())));
        return Results::Json(payload);
    });
}

WebApplication& WebApplication::AddHealthCheck(std::string name, HealthCheck check) {
    name = Trim(std::move(name));
    if (name.empty()) {
        throw std::invalid_argument("Health check name cannot be empty.");
    }
    if (!check) {
        throw std::invalid_argument("Health check callback cannot be empty.");
    }
    for (const auto& existing : impl_->healthChecks) {
        if (EqualsIgnoreCase(existing.first, name)) {
            throw std::invalid_argument("Health check '" + name + "' is already registered.");
        }
    }
    impl_->healthChecks.emplace_back(std::move(name), std::move(check));
    return *this;
}

WebApplication& WebApplication::MapHealthChecks(HealthCheckOptions options) {
    if (options.Path.empty()) {
        options.Path = "/health";
    }
    return MapGet(options.Path, [this, options = std::move(options)] {
        return CreateHealthCheckResult(impl_->healthChecks, options);
    });
}

WebApplication& WebApplication::UseHealthChecks(HealthCheckOptions options) {
    return MapHealthChecks(std::move(options));
}

WebApplication& WebApplication::MapOpenApi(OpenApiOptions options) {
    if (options.Path.empty()) {
        options.Path = "/openapi.json";
    }
    return MapGet(options.Path, [this, options = std::move(options)] {
        return Results::Json(CreateOpenApiDocument(impl_->endpoints, options, impl_->authorizationOptions));
    }).ExcludeFromDescription();
}

WebApplication& WebApplication::MapSwaggerUi(SwaggerUiOptions options) {
    if (options.Path.empty()) {
        options.Path = "/docs";
    }
    if (options.OpenApiPath.empty()) {
        options.OpenApiPath = "/openapi.json";
    }
    return MapGet(options.Path, [options = std::move(options)] {
        return Results::Content(CreateSwaggerUiHtml(options), "text/html; charset=utf-8");
    }).ExcludeFromDescription();
}

WebApplication& WebApplication::OnStarted(ApplicationCallback callback) {
    if (!callback) {
        throw std::invalid_argument("OnStarted callback cannot be empty.");
    }
    impl_->startedCallbacks.push_back(std::move(callback));
    return *this;
}

WebApplication& WebApplication::OnStopping(ApplicationCallback callback) {
    if (!callback) {
        throw std::invalid_argument("OnStopping callback cannot be empty.");
    }
    impl_->stoppingCallbacks.push_back(std::move(callback));
    return *this;
}

WebApplication& WebApplication::OnStopped(ApplicationCallback callback) {
    if (!callback) {
        throw std::invalid_argument("OnStopped callback cannot be empty.");
    }
    impl_->stoppedCallbacks.push_back(std::move(callback));
    return *this;
}

WebApplication& WebApplication::AddHostedService(HostedServiceOptions service) {
    service.Name = Trim(std::move(service.Name));
    if (!service.Start && !service.Stop) {
        throw std::invalid_argument("Hosted service must provide Start or Stop callback.");
    }

    std::lock_guard lock(impl_->mutex);
    if (impl_->running.load() || impl_->server) {
        throw std::logic_error("Hosted services must be registered before the application starts.");
    }

    HostedServiceRegistration registration;
    registration.Name = std::move(service.Name);
    registration.Start = std::move(service.Start);
    registration.Stop = std::move(service.Stop);
    impl_->hostedServices.push_back(std::move(registration));
    return *this;
}

WebApplication& WebApplication::AddBackgroundService(BackgroundServiceOptions service) {
    service.Name = Trim(std::move(service.Name));
    if (!service.Execute) {
        throw std::invalid_argument("Background service must provide Execute callback.");
    }

    struct BackgroundServiceRuntime {
        std::string Name;
        BackgroundServiceCallback Execute;
        bool StopApplicationOnException = true;
        std::jthread Thread;
        std::mutex Mutex;
    };

    auto runtime = std::make_shared<BackgroundServiceRuntime>();
    runtime->Name = std::move(service.Name);
    runtime->Execute = std::move(service.Execute);
    runtime->StopApplicationOnException = service.StopApplicationOnException;

    HostedServiceOptions hosted;
    hosted.Name = runtime->Name;
    hosted.Start = [runtime](WebApplication& app) {
        std::lock_guard lock(runtime->Mutex);
        if (runtime->Thread.joinable()) {
            return;
        }

        runtime->Thread = std::jthread([runtime, &app](std::stop_token token) {
            try {
                runtime->Execute(app, token);
            } catch (const std::exception& ex) {
                Log(app.Options().Log, LogLevel::Error, "Background service '" + runtime->Name + "' failed: " + ex.what());
                if (runtime->StopApplicationOnException && !token.stop_requested()) {
                    app.Stop();
                }
            } catch (...) {
                Log(app.Options().Log, LogLevel::Error, "Background service '" + runtime->Name + "' failed with an unknown exception.");
                if (runtime->StopApplicationOnException && !token.stop_requested()) {
                    app.Stop();
                }
            }
        });
    };
    hosted.Stop = [runtime](WebApplication&) {
        std::jthread threadToJoin;
        {
            std::lock_guard lock(runtime->Mutex);
            if (!runtime->Thread.joinable()) {
                return;
            }
            runtime->Thread.request_stop();
            if (runtime->Thread.get_id() == std::this_thread::get_id()) {
                runtime->Thread.detach();
                return;
            }
            threadToJoin = std::move(runtime->Thread);
        }
        if (threadToJoin.joinable()) {
            threadToJoin.join();
        }
    };
    return AddHostedService(std::move(hosted));
}

WebApplication& WebApplication::MapMethods(std::initializer_list<std::string> methods, std::string pattern, EndpointHandler handler) {
    return MapMethods(std::vector<std::string>(methods.begin(), methods.end()), std::move(pattern), std::move(handler));
}

WebApplication& WebApplication::MapMethods(std::vector<std::string> methods, std::string pattern, EndpointHandler handler) {
    for (std::string& method : methods) {
        method = ToUpperAscii(method);
    }
    RouteEndpoint endpoint;
    endpoint.Methods = std::move(methods);
    endpoint.Pattern = NormalizeRoutePath(std::move(pattern));
    endpoint.Segments = CompileRoutePattern(endpoint.Pattern);
    endpoint.Handler = std::move(handler);
    impl_->endpoints.push_back(std::move(endpoint));
    impl_->lastEndpointIndex = impl_->endpoints.size() - 1;
    impl_->lastEndpointIsFallback = false;
    return *this;
}

WebApplication& WebApplication::MapFallback(EndpointHandler handler) {
    RouteEndpoint endpoint;
    endpoint.Methods.clear();
    endpoint.Pattern = "{*path}";
    endpoint.Segments = CompileRoutePattern(endpoint.Pattern);
    endpoint.Handler = std::move(handler);
    endpoint.IsFallback = true;
    impl_->fallback = std::move(endpoint);
    impl_->lastEndpointIndex.reset();
    impl_->lastEndpointIsFallback = true;
    return *this;
}

WebApplication& WebApplication::RequireAuthorization(std::string policy) {
    policy = Trim(std::move(policy));
    if (policy.empty()) {
        return RequireAuthorization(std::initializer_list<std::string>{});
    }
    return RequireAuthorization({ policy });
}

WebApplication& WebApplication::RequireAuthorization(std::initializer_list<std::string> policies) {
    RouteEndpoint& endpoint = impl_->LastEndpoint("RequireAuthorization");
    endpoint.RequireAuthorization = true;
    endpoint.AllowAnonymous = false;
    for (std::string policy : policies) {
        policy = Trim(std::move(policy));
        if (!policy.empty() && !ContainsTokenIgnoreCase(endpoint.AuthorizationPolicies, policy)) {
            endpoint.AuthorizationPolicies.push_back(std::move(policy));
        }
    }
    return *this;
}

WebApplication& WebApplication::AllowAnonymous() {
    RouteEndpoint& endpoint = impl_->LastEndpoint("AllowAnonymous");
    endpoint.AllowAnonymous = true;
    return *this;
}

WebApplication& WebApplication::ExcludeFromDescription() {
    impl_->LastEndpoint("ExcludeFromDescription").ExcludeFromDescription = true;
    return *this;
}

WebApplication& WebApplication::WithName(std::string name) {
    impl_->LastEndpoint("WithName").Name = Trim(std::move(name));
    return *this;
}

WebApplication& WebApplication::WithTags(std::initializer_list<std::string> tags) {
    return WithTags(std::vector<std::string>(tags.begin(), tags.end()));
}

WebApplication& WebApplication::WithTags(std::vector<std::string> tags) {
    RouteEndpoint& endpoint = impl_->LastEndpoint("WithTags");
    endpoint.Tags.clear();
    for (std::string& tag : tags) {
        tag = Trim(std::move(tag));
        if (!tag.empty()) {
            endpoint.Tags.push_back(std::move(tag));
        }
    }
    return *this;
}

WebApplication& WebApplication::WithSummary(std::string summary) {
    impl_->LastEndpoint("WithSummary").Summary = Trim(std::move(summary));
    return *this;
}

WebApplication& WebApplication::WithDescription(std::string description) {
    impl_->LastEndpoint("WithDescription").Description = Trim(std::move(description));
    return *this;
}

WebApplication& WebApplication::WithParameter(OpenApiParameter parameter) {
    parameter.Name = Trim(std::move(parameter.Name));
    parameter.In = Trim(std::move(parameter.In));
    parameter.SchemaType = Trim(std::move(parameter.SchemaType));
    if (!parameter.Name.empty() && !parameter.In.empty()) {
        impl_->LastEndpoint("WithParameter").OpenApiParameters.push_back(std::move(parameter));
    }
    return *this;
}

WebApplication& WebApplication::WithQueryParameter(std::string name, std::string schemaType, bool required, std::string description) {
    OpenApiParameter parameter;
    parameter.Name = std::move(name);
    parameter.In = "query";
    parameter.SchemaType = std::move(schemaType);
    parameter.Required = required;
    parameter.Description = std::move(description);
    return WithParameter(std::move(parameter));
}

WebApplication& WebApplication::WithHeaderParameter(std::string name, std::string schemaType, bool required, std::string description) {
    OpenApiParameter parameter;
    parameter.Name = std::move(name);
    parameter.In = "header";
    parameter.SchemaType = std::move(schemaType);
    parameter.Required = required;
    parameter.Description = std::move(description);
    return WithParameter(std::move(parameter));
}

WebApplication& WebApplication::Produces(int statusCode, std::string contentType, std::string description) {
    OpenApiResponse response;
    response.StatusCode = statusCode;
    response.ContentType = std::move(contentType);
    response.Description = std::move(description);
    impl_->LastEndpoint("Produces").OpenApiResponses.push_back(std::move(response));
    return *this;
}

WebApplication& WebApplication::Accepts(std::string contentType, bool required, std::string description) {
    OpenApiRequestBody requestBody;
    requestBody.ContentType = std::move(contentType);
    requestBody.Required = required;
    requestBody.Description = std::move(description);
    impl_->LastEndpoint("Accepts").OpenApiRequestBodies.push_back(std::move(requestBody));
    return *this;
}

void WebApplication::Start() {
    if (!impl_) {
        throw std::logic_error("Cannot start a moved-from WebApplication.");
    }

    std::unique_lock lifecycleLock(impl_->lifecycleMutex);
    std::vector<ApplicationCallback> startedCallbacks;
    Logger logger;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->running.load()) {
            return;
        }
        impl_->stopping.store(false);
        impl_->server = std::make_unique<HttpSysServer>(
            impl_->options.HttpSys,
            impl_->options.Log,
            impl_->options.DetailedErrors,
            impl_->options.ProblemDetails);
        try {
            impl_->server->Start([this](HttpContext& context) {
                return impl_->Dispatch(context);
            });
        } catch (...) {
            impl_->server.reset();
            impl_->running.store(false);
            impl_->stopping.store(false);
            throw;
        }
        impl_->running.store(true);
        startedCallbacks = impl_->startedCallbacks;
        logger = impl_->options.Log;
    }
    RegisterRunningApplication(this);
    try {
        impl_->StartHostedServices(*this);
    } catch (...) {
        lifecycleLock.unlock();
        Stop();
        throw;
    }
    RunApplicationCallbacks(startedCallbacks, logger, "OnStarted");
}

void WebApplication::Stop() {
    if (!impl_) {
        return;
    }

    std::unique_lock lifecycleLock(impl_->lifecycleMutex);
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running.load() && !impl_->server) {
            return;
        }
    }
    if (impl_->stopping.exchange(true)) {
        return;
    }

    std::vector<ApplicationCallback> stoppingCallbacks;
    std::vector<ApplicationCallback> stoppedCallbacks;
    Logger logger;
    {
        std::lock_guard lock(impl_->mutex);
        stoppingCallbacks = impl_->stoppingCallbacks;
        stoppedCallbacks = impl_->stoppedCallbacks;
        logger = impl_->options.Log;
    }
    RunApplicationCallbacks(stoppingCallbacks, logger, "OnStopping");

    std::unique_ptr<HttpSysServer> server;
    {
        std::lock_guard lock(impl_->mutex);
        server = std::move(impl_->server);
    }

    UnregisterRunningApplication(this);
    if (server) {
        server->Stop();
    }
    impl_->StopHostedServices(*this);
    impl_->NotifyStopped();
    RunApplicationCallbacks(stoppedCallbacks, logger, "OnStopped");
}

void WebApplication::WaitForShutdown() {
    std::unique_lock lock(impl_->mutex);
    impl_->stopped.wait(lock, [this] {
        return !impl_->running.load();
    });
}

void WebApplication::Run() {
    Start();
    WaitForShutdown();
}

bool WebApplication::IsRunning() const noexcept {
    return impl_ && impl_->running.load();
}

bool WebApplication::IsStopping() const noexcept {
    return impl_ && impl_->stopping.load();
}

std::uint64_t WebApplication::ActiveRequestCount() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->server ? impl_->server->ActiveRequestCount() : 0;
}

const WebApplicationOptions& WebApplication::Options() const noexcept {
    return impl_->options;
}

}  // namespace Capi::Web
