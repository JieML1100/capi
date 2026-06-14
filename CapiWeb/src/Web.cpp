#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <Capi/Web.h>

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <http.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "httpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace Capi::Web {

namespace {

constexpr HTTPAPI_VERSION HttpApiVersion{ 2, 0 };

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
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

std::vector<std::uint8_t> StringToBytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(text.data());
    return std::vector<std::uint8_t>(first, first + text.size());
}

System::Text::Json::JsonObject CreateProblemObject(std::string title, std::string detail, int statusCode) {
    System::Text::Json::JsonObject problem;
    problem.Add("title", System::Text::Json::JsonNode::Create(title));
    problem.Add("status", System::Text::Json::JsonNode::Create(statusCode));
    if (!detail.empty()) {
        problem.Add("detail", System::Text::Json::JsonNode::Create(detail));
    }
    return problem;
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

    auto bytes = ReadFileBytes(*path);
    HttpResult result = Results::File(std::move(bytes), contentType);
    if (!options.CacheControl.empty()) {
        result.Header("Cache-Control", options.CacheControl);
    }
    return result;
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

bool MatchEndpoint(const RouteEndpoint& endpoint, const HttpRequest& request, ValueMap& routeValues) {
    if (!EndpointAllowsMethod(endpoint, request.Method())) {
        return false;
    }

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
    HttpSysServer(HttpSysOptions options, Logger logger)
        : options_(std::move(options)), logger_(std::move(logger)) {
    }

    ~HttpSysServer() {
        Stop();
    }

    void Start(std::function<HttpResult(HttpContext&)> handler) {
        if (running_.load()) {
            return;
        }
        handler_ = std::move(handler);
        for (std::string& prefix : options_.UrlPrefixes) {
            prefix = NormalizeUrlPrefix(std::move(prefix));
        }
        UrlReservationManager::Apply(options_.UrlPrefixes, options_.UrlReservation);

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
            CleanupHandles();
            return;
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
        Log(logger_, LogLevel::Information, "HTTP.sys server stopped.");
    }

    bool IsRunning() const noexcept {
        return running_.load();
    }

private:
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
        request.body_ = ReadBody(source);
        return context;
    }

    HttpResult ExecuteHandler(HttpContext& context) {
        try {
            return handler_(context);
        } catch (const std::length_error& ex) {
            return Results::Problem("Payload Too Large", ex.what(), 413);
        } catch (const System::Text::Json::JsonException& ex) {
            return Results::Problem("Invalid JSON", ex.what(), 400);
        } catch (const std::exception& ex) {
            return Results::Problem("Internal Server Error", ex.what(), 500);
        } catch (...) {
            return Results::Problem("Internal Server Error", "Unknown exception.", 500);
        }
    }

    void HandleRequest(const HTTP_REQUEST& source) {
        HttpContext context;
        try {
            context = BuildContext(source);
            HttpResult result = ExecuteHandler(context);
            result.Apply(context.Response());
        } catch (const std::exception& ex) {
            context.response_ = HttpResponse();
            Results::Problem("Internal Server Error", ex.what(), 500).Apply(context.Response());
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
    std::function<HttpResult(HttpContext&)> handler_;
    std::unique_ptr<HttpApiScope> apiScope_;
    HTTP_SERVER_SESSION_ID serverSessionId_ = 0;
    HTTP_URL_GROUP_ID urlGroupId_ = 0;
    HANDLE requestQueue_ = nullptr;
    std::atomic<bool> running_{ false };
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
const HeaderCollection& HttpRequest::Headers() const noexcept { return headers_; }
const ValueMap& HttpRequest::Query() const noexcept { return query_; }
const ValueMap& HttpRequest::RouteValues() const noexcept { return routeValues_; }

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
std::unordered_map<std::string, std::any>& HttpContext::Items() noexcept { return items_; }
const std::unordered_map<std::string, std::any>& HttpContext::Items() const noexcept { return items_; }

bool HttpResult::HasResponse() const noexcept { return hasResponse_; }
int HttpResult::StatusCode() const noexcept { return statusCode_; }
const HeaderCollection& HttpResult::Headers() const noexcept { return headers_; }
const std::vector<std::uint8_t>& HttpResult::Body() const noexcept { return body_; }

HttpResult& HttpResult::Header(std::string name, std::string value) {
    hasResponse_ = true;
    headers_.Set(std::move(name), std::move(value));
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

HttpResult Results::NotFound(std::string detail) {
    return Problem("Not Found", std::move(detail), 404);
}

HttpResult Results::Problem(std::string title, std::string detail, int statusCode) {
    return Json(CreateProblemObject(std::move(title), std::move(detail), statusCode), statusCode);
}

HttpResult Results::Redirect(std::string location, bool permanent) {
    HttpResult result = StatusCode(permanent ? 301 : 302);
    result.headers_.Set("Location", std::move(location));
    return result;
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

WebApplicationBuilder& WebApplicationBuilder::ConfigureHttpSys(std::function<void(HttpSysOptions&)> configure) {
    configure(options_.HttpSys);
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
    std::optional<RouteEndpoint> fallback;
    std::unique_ptr<HttpSysServer> server;
    mutable std::mutex mutex;
    std::condition_variable stopped;
    std::atomic<bool> running{ false };

    HttpResult Dispatch(HttpContext& context) {
        RequestDelegate terminal = [this](HttpContext& current) {
            for (const RouteEndpoint& endpoint : endpoints) {
                ValueMap routeValues;
                if (MatchEndpoint(endpoint, current.Request(), routeValues)) {
                    current.request_.routeValues_ = std::move(routeValues);
                    return endpoint.Handler(current);
                }
            }
            if (fallback && EndpointAllowsMethod(*fallback, current.Request().Method())) {
                return fallback->Handler(current);
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

}  // namespace

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
    return *this;
}

void WebApplication::Start() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running.load()) {
        return;
    }
    impl_->server = std::make_unique<HttpSysServer>(impl_->options.HttpSys, impl_->options.Log);
    impl_->server->Start([this](HttpContext& context) {
        return impl_->Dispatch(context);
    });
    impl_->running.store(true);
    RegisterRunningApplication(this);
}

void WebApplication::Stop() {
    if (!impl_) {
        return;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running.load() && !impl_->server) {
            return;
        }
    }
    UnregisterRunningApplication(this);
    if (impl_->server) {
        impl_->server->Stop();
        impl_->server.reset();
    }
    impl_->NotifyStopped();
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

const WebApplicationOptions& WebApplication::Options() const noexcept {
    return impl_->options;
}

}  // namespace Capi::Web
