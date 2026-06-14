#include "JsonSerializer.h"

#include "JsonInternal.h"

namespace System::Text::Json {

std::string JsonSerializer::Serialize(const JsonElement& element, const JsonSerializerOptions& options) {
    return element.value_ ? RenderJsonValue(*element.value_, options) : "null";
}

std::string JsonSerializer::Serialize(const JsonNode& node, const JsonSerializerOptions& options) {
    return node.ToJsonString(options);
}

std::string JsonSerializer::Serialize(std::nullptr_t, const JsonSerializerOptions&) {
    return "null";
}

std::string JsonSerializer::Serialize(bool value, const JsonSerializerOptions&) {
    return value ? "true" : "false";
}

std::string JsonSerializer::Serialize(int value, const JsonSerializerOptions&) {
    return std::to_string(value);
}

std::string JsonSerializer::Serialize(long long value, const JsonSerializerOptions&) {
    return std::to_string(value);
}

std::string JsonSerializer::Serialize(double value, const JsonSerializerOptions& options) {
    return JsonNode::Create(value).ToJsonString(options);
}

std::string JsonSerializer::Serialize(std::string_view value, const JsonSerializerOptions&) {
    return EscapeJsonString(value);
}

JsonElement JsonSerializer::DeserializeElement(std::string_view json, const JsonDocumentOptions& options) {
    return JsonDocument::Parse(json, options)->RootElement();
}

JsonNode JsonSerializer::DeserializeNode(std::string_view json, const JsonDocumentOptions& options) {
    return JsonNode::Parse(json, options);
}

}  // namespace System::Text::Json
