#include "JsonInternal.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace System::Text::Json {

namespace {

void AppendUtf8(std::string& output, unsigned int codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        throw JsonException("Invalid Unicode code point");
    }
}

int HexValue(char ch) noexcept {
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

std::string FormatDouble(double value) {
    if (!std::isfinite(value)) {
        throw JsonException("JSON does not support NaN or infinity");
    }

    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

class JsonParser final {
public:
    JsonParser(std::string_view json, JsonDocumentOptions options)
        : json_(json), options_(std::move(options)) {
    }

    std::shared_ptr<JsonValue> Parse() {
        SkipTrivia();
        auto value = ParseValue(0);
        SkipTrivia();
        if (!End()) {
            Throw("Expected end of JSON input");
        }
        return value;
    }

private:
    bool End() const noexcept {
        return position_ >= json_.size();
    }

    char Peek() const noexcept {
        return End() ? '\0' : json_[position_];
    }

    bool StartsWith(std::string_view token) const noexcept {
        return json_.substr(position_, token.size()) == token;
    }

    void Advance(std::size_t count = 1) {
        for (std::size_t i = 0; i < count && position_ < json_.size(); ++i) {
            const char ch = json_[position_++];
            if (ch == '\n') {
                ++line_;
                bytePositionInLine_ = 0;
            } else {
                ++bytePositionInLine_;
            }
        }
    }

    [[noreturn]] void Throw(const std::string& message) const {
        throw JsonException(message, line_, bytePositionInLine_);
    }

    void SkipTrivia() {
        while (!End()) {
            const char ch = Peek();
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                Advance();
                continue;
            }

            if (ch == '/' && options_.CommentHandling == JsonCommentHandling::Skip) {
                if (StartsWith("//")) {
                    Advance(2);
                    while (!End() && Peek() != '\n') {
                        Advance();
                    }
                    continue;
                }
                if (StartsWith("/*")) {
                    Advance(2);
                    while (!End() && !StartsWith("*/")) {
                        Advance();
                    }
                    if (End()) {
                        Throw("Unterminated JSON comment");
                    }
                    Advance(2);
                    continue;
                }
            }

            break;
        }
    }

    std::shared_ptr<JsonValue> ParseValue(int depth) {
        if (options_.MaxDepth > 0 && depth > options_.MaxDepth) {
            Throw("The JSON value depth exceeds JsonDocumentOptions.MaxDepth");
        }

        SkipTrivia();
        if (End()) {
            Throw("Expected JSON value");
        }

        switch (Peek()) {
        case '{':
            return ParseObject(depth + 1);
        case '[':
            return ParseArray(depth + 1);
        case '"':
            return MakeStringValue(ParseString());
        case 't':
            ConsumeLiteral("true");
            return MakeBooleanValue(true);
        case 'f':
            ConsumeLiteral("false");
            return MakeBooleanValue(false);
        case 'n':
            ConsumeLiteral("null");
            return MakeNullValue();
        default:
            if (Peek() == '-' || std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                return MakeNumberValue(ParseNumber());
            }
            Throw("Expected JSON value");
        }
    }

    void ConsumeLiteral(std::string_view literal) {
        if (!StartsWith(literal)) {
            Throw("Invalid JSON literal");
        }
        Advance(literal.size());
    }

    std::shared_ptr<JsonValue> ParseObject(int depth) {
        auto object = MakeObjectValue();
        Advance();
        SkipTrivia();
        if (Peek() == '}') {
            Advance();
            return object;
        }

        while (true) {
            SkipTrivia();
            if (Peek() != '"') {
                Throw("Expected JSON property name");
            }
            std::string name = ParseString();
            SkipTrivia();
            if (Peek() != ':') {
                Throw("Expected ':' after JSON property name");
            }
            Advance();
            object->objectValues.emplace_back(std::move(name), ParseValue(depth));
            SkipTrivia();

            if (Peek() == '}') {
                Advance();
                return object;
            }
            if (Peek() != ',') {
                Throw("Expected ',' or '}' in JSON object");
            }
            Advance();
            SkipTrivia();
            if (Peek() == '}' && !options_.AllowTrailingCommas) {
                Throw("Trailing commas are not allowed in JSON objects");
            }
        }
    }

    std::shared_ptr<JsonValue> ParseArray(int depth) {
        auto array = MakeArrayValue();
        Advance();
        SkipTrivia();
        if (Peek() == ']') {
            Advance();
            return array;
        }

        while (true) {
            array->arrayValues.push_back(ParseValue(depth));
            SkipTrivia();
            if (Peek() == ']') {
                Advance();
                return array;
            }
            if (Peek() != ',') {
                Throw("Expected ',' or ']' in JSON array");
            }
            Advance();
            SkipTrivia();
            if (Peek() == ']' && !options_.AllowTrailingCommas) {
                Throw("Trailing commas are not allowed in JSON arrays");
            }
        }
    }

    std::string ParseString() {
        if (Peek() != '"') {
            Throw("Expected JSON string");
        }
        Advance();

        std::string value;
        while (!End()) {
            const char ch = Peek();
            if (ch == '"') {
                Advance();
                return value;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                Throw("Control characters must be escaped in JSON strings");
            }
            if (ch != '\\') {
                value.push_back(ch);
                Advance();
                continue;
            }

            Advance();
            if (End()) {
                Throw("Unterminated JSON escape sequence");
            }

            const char escaped = Peek();
            Advance();
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u':
                AppendEscapedUnicode(value);
                break;
            default:
                Throw("Invalid JSON escape sequence");
            }
        }

        Throw("Unterminated JSON string");
    }

    unsigned int ParseHexQuad() {
        unsigned int codePoint = 0;
        for (int i = 0; i < 4; ++i) {
            if (End()) {
                Throw("Unterminated Unicode escape sequence");
            }
            const int digit = HexValue(Peek());
            if (digit < 0) {
                Throw("Invalid Unicode escape sequence");
            }
            codePoint = (codePoint << 4) | static_cast<unsigned int>(digit);
            Advance();
        }
        return codePoint;
    }

    void AppendEscapedUnicode(std::string& value) {
        unsigned int codePoint = ParseHexQuad();
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (!StartsWith("\\u")) {
                Throw("Expected low surrogate after high surrogate");
            }
            Advance(2);
            const unsigned int low = ParseHexQuad();
            if (low < 0xDC00 || low > 0xDFFF) {
                Throw("Invalid low surrogate in Unicode escape sequence");
            }
            codePoint = 0x10000 + (((codePoint - 0xD800) << 10) | (low - 0xDC00));
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            Throw("Low surrogate cannot appear without a high surrogate");
        }
        AppendUtf8(value, codePoint);
    }

    std::string ParseNumber() {
        const std::size_t start = position_;
        if (Peek() == '-') {
            Advance();
        }

        if (End()) {
            Throw("Invalid JSON number");
        }
        if (Peek() == '0') {
            Advance();
        } else if (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            while (!End() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                Advance();
            }
        } else {
            Throw("Invalid JSON number");
        }

        if (!End() && Peek() == '.') {
            Advance();
            if (End() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                Throw("Invalid JSON number");
            }
            while (!End() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                Advance();
            }
        }

        if (!End() && (Peek() == 'e' || Peek() == 'E')) {
            Advance();
            if (!End() && (Peek() == '+' || Peek() == '-')) {
                Advance();
            }
            if (End() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                Throw("Invalid JSON number");
            }
            while (!End() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                Advance();
            }
        }

        return std::string(json_.substr(start, position_ - start));
    }

    std::string_view json_;
    JsonDocumentOptions options_;
    std::size_t position_ = 0;
    std::size_t line_ = 0;
    std::size_t bytePositionInLine_ = 0;
};

void RenderJsonValueCore(const JsonValue& value, const JsonSerializerOptions& options, std::string& output, int depth);

void AppendIndent(const JsonSerializerOptions& options, std::string& output, int depth) {
    for (int i = 0; i < depth; ++i) {
        output += options.IndentChars;
    }
}

void RenderArray(const JsonValue& value, const JsonSerializerOptions& options, std::string& output, int depth) {
    output.push_back('[');
    if (!value.arrayValues.empty()) {
        for (std::size_t index = 0; index < value.arrayValues.size(); ++index) {
            if (index > 0) {
                output.push_back(',');
            }
            if (options.WriteIndented) {
                output += options.NewLine;
                AppendIndent(options, output, depth + 1);
            }
            RenderJsonValueCore(*value.arrayValues[index], options, output, depth + 1);
        }
        if (options.WriteIndented) {
            output += options.NewLine;
            AppendIndent(options, output, depth);
        }
    }
    output.push_back(']');
}

void RenderObject(const JsonValue& value, const JsonSerializerOptions& options, std::string& output, int depth) {
    output.push_back('{');
    if (!value.objectValues.empty()) {
        for (std::size_t index = 0; index < value.objectValues.size(); ++index) {
            if (index > 0) {
                output.push_back(',');
            }
            if (options.WriteIndented) {
                output += options.NewLine;
                AppendIndent(options, output, depth + 1);
            }
            output += EscapeJsonString(value.objectValues[index].first);
            output.push_back(':');
            if (options.WriteIndented) {
                output.push_back(' ');
            }
            RenderJsonValueCore(*value.objectValues[index].second, options, output, depth + 1);
        }
        if (options.WriteIndented) {
            output += options.NewLine;
            AppendIndent(options, output, depth);
        }
    }
    output.push_back('}');
}

void RenderJsonValueCore(const JsonValue& value, const JsonSerializerOptions& options, std::string& output, int depth) {
    switch (value.kind) {
    case JsonValueKind::Object:
        RenderObject(value, options, output, depth);
        break;
    case JsonValueKind::Array:
        RenderArray(value, options, output, depth);
        break;
    case JsonValueKind::String:
        output += EscapeJsonString(value.stringValue);
        break;
    case JsonValueKind::Number:
        output += value.numberText.empty() ? "0" : value.numberText;
        break;
    case JsonValueKind::True:
        output += "true";
        break;
    case JsonValueKind::False:
        output += "false";
        break;
    case JsonValueKind::Null:
    case JsonValueKind::Undefined:
        output += "null";
        break;
    }
}

}  // namespace

std::shared_ptr<JsonValue> MakeNullValue() {
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValueKind::Null;
    return value;
}

std::shared_ptr<JsonValue> MakeBooleanValue(bool value) {
    auto jsonValue = std::make_shared<JsonValue>();
    jsonValue->kind = value ? JsonValueKind::True : JsonValueKind::False;
    jsonValue->boolValue = value;
    return jsonValue;
}

std::shared_ptr<JsonValue> MakeStringValue(std::string value) {
    auto jsonValue = std::make_shared<JsonValue>();
    jsonValue->kind = JsonValueKind::String;
    jsonValue->stringValue = std::move(value);
    return jsonValue;
}

std::shared_ptr<JsonValue> MakeNumberValue(std::string value) {
    auto jsonValue = std::make_shared<JsonValue>();
    jsonValue->kind = JsonValueKind::Number;
    jsonValue->numberText = std::move(value);
    return jsonValue;
}

std::shared_ptr<JsonValue> MakeArrayValue() {
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValueKind::Array;
    return value;
}

std::shared_ptr<JsonValue> MakeObjectValue() {
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValueKind::Object;
    return value;
}

std::shared_ptr<JsonValue> CloneJsonValue(const std::shared_ptr<const JsonValue>& value) {
    if (!value) {
        return {};
    }

    auto clone = std::make_shared<JsonValue>();
    clone->kind = value->kind;
    clone->stringValue = value->stringValue;
    clone->numberText = value->numberText;
    clone->boolValue = value->boolValue;
    for (const auto& item : value->arrayValues) {
        clone->arrayValues.push_back(CloneJsonValue(item));
    }
    for (const auto& property : value->objectValues) {
        clone->objectValues.emplace_back(property.first, CloneJsonValue(property.second));
    }
    return clone;
}

std::shared_ptr<JsonValue> ParseJsonValue(std::string_view json, const JsonDocumentOptions& options) {
    return JsonParser(json, options).Parse();
}

std::string RenderJsonValue(const JsonValue& value, const JsonSerializerOptions& options) {
    std::string output;
    RenderJsonValueCore(value, options, output, 0);
    return output;
}

std::string EscapeJsonString(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                std::ostringstream stream;
                stream << "\\u" << std::uppercase << std::hex << std::setfill('0') << std::setw(4)
                       << static_cast<unsigned int>(static_cast<unsigned char>(ch));
                escaped += stream.str();
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

}  // namespace System::Text::Json
