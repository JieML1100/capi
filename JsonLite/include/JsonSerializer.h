#pragma once

#include "JsonDocument.h"
#include "JsonNode.h"
#include "Utf8JsonWriter.h"

#include <string>
#include <string_view>

namespace System::Text::Json {

class JsonSerializer final {
public:
    JsonSerializer() = delete;

    static std::string Serialize(const JsonElement& element, const JsonSerializerOptions& options = {});
    static std::string Serialize(const JsonNode& node, const JsonSerializerOptions& options = {});
    static std::string Serialize(std::nullptr_t, const JsonSerializerOptions& options = {});
    static std::string Serialize(bool value, const JsonSerializerOptions& options = {});
    static std::string Serialize(int value, const JsonSerializerOptions& options = {});
    static std::string Serialize(long long value, const JsonSerializerOptions& options = {});
    static std::string Serialize(double value, const JsonSerializerOptions& options = {});
    static std::string Serialize(std::string_view value, const JsonSerializerOptions& options = {});

    static JsonElement DeserializeElement(std::string_view json, const JsonDocumentOptions& options = {});
    static JsonNode DeserializeNode(std::string_view json, const JsonDocumentOptions& options = {});
};

}  // namespace System::Text::Json
