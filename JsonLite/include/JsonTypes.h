#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace System::Text::Json {

enum class JsonValueKind {
    Undefined,
    Object,
    Array,
    String,
    Number,
    True,
    False,
    Null,
};

enum class JsonTokenType {
    None,
    StartObject,
    EndObject,
    StartArray,
    EndArray,
    PropertyName,
    String,
    Number,
    True,
    False,
    Null,
};

enum class JsonCommentHandling {
    Disallow,
    Skip,
};

struct JsonDocumentOptions {
    JsonCommentHandling CommentHandling = JsonCommentHandling::Disallow;
    bool AllowTrailingCommas = false;
    int MaxDepth = 64;
};

struct JsonSerializerOptions {
    bool WriteIndented = false;
    bool PropertyNameCaseInsensitive = false;
    std::string IndentChars = "  ";
    std::string NewLine = "\n";
};

struct JsonWriterOptions {
    bool Indented = false;
    bool SkipValidation = false;
    std::string IndentChars = "  ";
    std::string NewLine = "\n";
};

class JsonException : public std::runtime_error {
public:
    JsonException(const std::string& message, std::size_t line = 0, std::size_t bytePositionInLine = 0);

    std::size_t LineNumber() const noexcept;
    std::size_t BytePositionInLine() const noexcept;

private:
    std::size_t line_;
    std::size_t bytePositionInLine_;
};

}  // namespace System::Text::Json
