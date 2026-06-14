#pragma once

#include "JsonDocument.h"
#include "JsonNode.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace System::Text::Json {

struct JsonValue {
    JsonValueKind kind = JsonValueKind::Null;
    std::string stringValue;
    std::string numberText;
    bool boolValue = false;
    std::vector<std::shared_ptr<JsonValue>> arrayValues;
    std::vector<std::pair<std::string, std::shared_ptr<JsonValue>>> objectValues;
};

std::shared_ptr<JsonValue> MakeNullValue();
std::shared_ptr<JsonValue> MakeBooleanValue(bool value);
std::shared_ptr<JsonValue> MakeStringValue(std::string value);
std::shared_ptr<JsonValue> MakeNumberValue(std::string value);
std::shared_ptr<JsonValue> MakeArrayValue();
std::shared_ptr<JsonValue> MakeObjectValue();
std::shared_ptr<JsonValue> CloneJsonValue(const std::shared_ptr<const JsonValue>& value);

std::shared_ptr<JsonValue> ParseJsonValue(std::string_view json, const JsonDocumentOptions& options);
std::string RenderJsonValue(const JsonValue& value, const JsonSerializerOptions& options);
std::string EscapeJsonString(std::string_view value);

}  // namespace System::Text::Json
