#include "JsonNode.h"

#include "JsonInternal.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace System::Text::Json {

namespace {

std::string FormatDouble(double value) {
    if (!std::isfinite(value)) {
        throw JsonException("JSON does not support NaN or infinity");
    }

    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

JsonValue& RequireNodeValue(const std::shared_ptr<JsonValue>& value) {
    if (!value) {
        throw JsonException("The JsonNode is null");
    }
    return *value;
}

JsonValue& RequireObjectValue(const std::shared_ptr<JsonValue>& nodeValue) {
    auto& value = RequireNodeValue(nodeValue);
    if (value.kind != JsonValueKind::Object) {
        throw JsonException("The JsonNode is not a JsonObject");
    }
    return value;
}

JsonValue& RequireArrayValue(const std::shared_ptr<JsonValue>& nodeValue) {
    auto& value = RequireNodeValue(nodeValue);
    if (value.kind != JsonValueKind::Array) {
        throw JsonException("The JsonNode is not a JsonArray");
    }
    return value;
}

std::size_t FindPropertyIndex(const JsonValue& object, std::string_view propertyName) {
    for (std::size_t index = 0; index < object.objectValues.size(); ++index) {
        if (object.objectValues[index].first == propertyName) {
            return index;
        }
    }
    return (std::numeric_limits<std::size_t>::max)();
}

}  // namespace

JsonNode::JsonNode()
    : value_(MakeNullValue()) {
}

JsonNode::JsonNode(std::shared_ptr<JsonValue> value)
    : value_(std::move(value)) {
    if (!value_) {
        value_ = MakeNullValue();
    }
}

JsonNode JsonNode::Parse(std::string_view json, const JsonDocumentOptions& options) {
    return JsonNode(ParseJsonValue(json, options));
}

JsonNode JsonNode::CreateNull() {
    return JsonNode(MakeNullValue());
}

JsonNode JsonNode::Create(bool value) {
    return JsonNode(MakeBooleanValue(value));
}

JsonNode JsonNode::Create(int value) {
    return JsonNode(MakeNumberValue(std::to_string(value)));
}

JsonNode JsonNode::Create(long long value) {
    return JsonNode(MakeNumberValue(std::to_string(value)));
}

JsonNode JsonNode::Create(double value) {
    return JsonNode(MakeNumberValue(FormatDouble(value)));
}

JsonNode JsonNode::Create(const char* value) {
    return JsonNode(MakeStringValue(value == nullptr ? std::string{} : std::string(value)));
}

JsonNode JsonNode::Create(std::string_view value) {
    return JsonNode(MakeStringValue(std::string(value)));
}

JsonValueKind JsonNode::ValueKind() const noexcept {
    return value_ ? value_->kind : JsonValueKind::Undefined;
}

JsonElement JsonNode::AsElement() const {
    return JsonElement(value_);
}

JsonObject JsonNode::AsObject() const {
    if (ValueKind() != JsonValueKind::Object) {
        throw JsonException("The JsonNode is not a JsonObject");
    }
    return JsonObject(value_);
}

JsonArray JsonNode::AsArray() const {
    if (ValueKind() != JsonValueKind::Array) {
        throw JsonException("The JsonNode is not a JsonArray");
    }
    return JsonArray(value_);
}

std::string JsonNode::ToJsonString(const JsonSerializerOptions& options) const {
    return value_ ? RenderJsonValue(*value_, options) : "null";
}

std::string JsonNode::ToString() const {
    return ToJsonString();
}

JsonObject::JsonObject()
    : JsonNode(MakeObjectValue()) {
}

JsonObject::JsonObject(std::shared_ptr<JsonValue> value)
    : JsonNode(std::move(value)) {
}

std::size_t JsonObject::Count() const {
    return RequireObjectValue(value_).objectValues.size();
}

bool JsonObject::ContainsKey(std::string_view propertyName) const {
    return FindPropertyIndex(RequireObjectValue(value_), propertyName)
        != (std::numeric_limits<std::size_t>::max)();
}

JsonNode JsonObject::operator[](std::string_view propertyName) const {
    return GetValue(propertyName);
}

JsonNode JsonObject::GetValue(std::string_view propertyName) const {
    JsonNode value;
    if (!TryGetValue(propertyName, value)) {
        throw JsonException("The requested JSON property was not found");
    }
    return value;
}

bool JsonObject::TryGetValue(std::string_view propertyName, JsonNode& value) const {
    const auto& object = RequireObjectValue(value_);
    for (auto it = object.objectValues.rbegin(); it != object.objectValues.rend(); ++it) {
        if (it->first == propertyName) {
            value = JsonNode(it->second);
            return true;
        }
    }
    return false;
}

void JsonObject::Add(std::string_view propertyName, const JsonNode& value) {
    auto& object = RequireObjectValue(value_);
    if (FindPropertyIndex(object, propertyName) != (std::numeric_limits<std::size_t>::max)()) {
        throw JsonException("An item with the same property name has already been added");
    }
    object.objectValues.emplace_back(std::string(propertyName), CloneJsonValue(value.value_));
}

void JsonObject::Set(std::string_view propertyName, const JsonNode& value) {
    auto& object = RequireObjectValue(value_);
    const std::size_t index = FindPropertyIndex(object, propertyName);
    if (index == (std::numeric_limits<std::size_t>::max)()) {
        object.objectValues.emplace_back(std::string(propertyName), CloneJsonValue(value.value_));
    } else {
        object.objectValues[index].second = CloneJsonValue(value.value_);
    }
}

bool JsonObject::Remove(std::string_view propertyName) {
    auto& object = RequireObjectValue(value_);
    const std::size_t index = FindPropertyIndex(object, propertyName);
    if (index == (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }
    object.objectValues.erase(object.objectValues.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void JsonObject::Clear() {
    RequireObjectValue(value_).objectValues.clear();
}

std::vector<std::pair<std::string, JsonNode>> JsonObject::Properties() const {
    const auto& object = RequireObjectValue(value_);
    std::vector<std::pair<std::string, JsonNode>> result;
    result.reserve(object.objectValues.size());
    for (const auto& property : object.objectValues) {
        result.emplace_back(property.first, JsonNode(property.second));
    }
    return result;
}

JsonArray::JsonArray()
    : JsonNode(MakeArrayValue()) {
}

JsonArray::JsonArray(std::shared_ptr<JsonValue> value)
    : JsonNode(std::move(value)) {
}

std::size_t JsonArray::Count() const {
    return RequireArrayValue(value_).arrayValues.size();
}

JsonNode JsonArray::operator[](std::size_t index) const {
    return GetValue(index);
}

JsonNode JsonArray::GetValue(std::size_t index) const {
    const auto& array = RequireArrayValue(value_);
    if (index >= array.arrayValues.size()) {
        throw JsonException("JSON array index is out of range");
    }
    return JsonNode(array.arrayValues[index]);
}

void JsonArray::Add(const JsonNode& value) {
    RequireArrayValue(value_).arrayValues.push_back(CloneJsonValue(value.value_));
}

void JsonArray::Add(const char* value) {
    Add(JsonNode::Create(value));
}

void JsonArray::Add(std::string_view value) {
    Add(JsonNode::Create(value));
}

void JsonArray::Add(int value) {
    Add(JsonNode::Create(value));
}

void JsonArray::Add(long long value) {
    Add(JsonNode::Create(value));
}

void JsonArray::Add(double value) {
    Add(JsonNode::Create(value));
}

void JsonArray::Add(bool value) {
    Add(JsonNode::Create(value));
}

void JsonArray::AddNull() {
    Add(JsonNode::CreateNull());
}

void JsonArray::RemoveAt(std::size_t index) {
    auto& array = RequireArrayValue(value_);
    if (index >= array.arrayValues.size()) {
        throw JsonException("JSON array index is out of range");
    }
    array.arrayValues.erase(array.arrayValues.begin() + static_cast<std::ptrdiff_t>(index));
}

void JsonArray::Clear() {
    RequireArrayValue(value_).arrayValues.clear();
}

std::vector<JsonNode> JsonArray::Items() const {
    const auto& array = RequireArrayValue(value_);
    std::vector<JsonNode> result;
    result.reserve(array.arrayValues.size());
    for (const auto& item : array.arrayValues) {
        result.push_back(JsonNode(item));
    }
    return result;
}

}  // namespace System::Text::Json
