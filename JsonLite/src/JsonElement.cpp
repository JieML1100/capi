#include "JsonElement.h"

#include "JsonInternal.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>

namespace System::Text::Json {

namespace {

bool IsIntegralNumberText(const std::string& text) {
    return text.find_first_of(".eE") == std::string::npos;
}

template <typename TInt>
bool TryParseInteger(const std::string& text, TInt& value) noexcept {
    if (!IsIntegralNumberText(text)) {
        return false;
    }

    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

}  // namespace

JsonElement::JsonElement(std::shared_ptr<const JsonValue> value)
    : value_(std::move(value)) {
}

JsonValueKind JsonElement::ValueKind() const noexcept {
    return value_ ? value_->kind : JsonValueKind::Undefined;
}

std::string JsonElement::GetRawText() const {
    return ToString();
}

std::string JsonElement::ToString() const {
    if (!value_) {
        return {};
    }
    return RenderJsonValue(*value_, JsonSerializerOptions{});
}

std::string JsonElement::GetString() const {
    return RequireKind(JsonValueKind::String).stringValue;
}

bool JsonElement::GetBoolean() const {
    const auto kind = ValueKind();
    if (kind == JsonValueKind::True) {
        return true;
    }
    if (kind == JsonValueKind::False) {
        return false;
    }
    throw JsonException("The JSON value is not a Boolean");
}

int JsonElement::GetInt32() const {
    int value = 0;
    if (!TryGetInt32(value)) {
        throw JsonException("The JSON number could not be represented as Int32");
    }
    return value;
}

long long JsonElement::GetInt64() const {
    long long value = 0;
    if (!TryGetInt64(value)) {
        throw JsonException("The JSON number could not be represented as Int64");
    }
    return value;
}

double JsonElement::GetDouble() const {
    double value = 0;
    if (!TryGetDouble(value)) {
        throw JsonException("The JSON number could not be represented as Double");
    }
    return value;
}

bool JsonElement::TryGetInt32(int& value) const noexcept {
    if (ValueKind() != JsonValueKind::Number) {
        return false;
    }
    return TryParseInteger(value_->numberText, value);
}

bool JsonElement::TryGetInt64(long long& value) const noexcept {
    if (ValueKind() != JsonValueKind::Number) {
        return false;
    }
    return TryParseInteger(value_->numberText, value);
}

bool JsonElement::TryGetDouble(double& value) const noexcept {
    if (ValueKind() != JsonValueKind::Number) {
        return false;
    }

    char* end = nullptr;
    value = std::strtod(value_->numberText.c_str(), &end);
    return end == value_->numberText.c_str() + value_->numberText.size();
}

int JsonElement::GetArrayLength() const {
    const auto& value = RequireKind(JsonValueKind::Array);
    return static_cast<int>(value.arrayValues.size());
}

JsonElement JsonElement::operator[](std::size_t index) const {
    const auto& value = RequireKind(JsonValueKind::Array);
    if (index >= value.arrayValues.size()) {
        throw JsonException("JSON array index is out of range");
    }
    return JsonElement(value.arrayValues[index]);
}

JsonElement JsonElement::GetProperty(std::string_view propertyName) const {
    JsonElement value;
    if (!TryGetProperty(propertyName, value)) {
        throw JsonException("The requested JSON property was not found");
    }
    return value;
}

bool JsonElement::TryGetProperty(std::string_view propertyName, JsonElement& value) const {
    const auto& object = RequireKind(JsonValueKind::Object);
    for (auto it = object.objectValues.rbegin(); it != object.objectValues.rend(); ++it) {
        if (it->first == propertyName) {
            value = JsonElement(it->second);
            return true;
        }
    }
    return false;
}

bool JsonElement::HasProperty(std::string_view propertyName) const {
    JsonElement value;
    return TryGetProperty(propertyName, value);
}

std::vector<JsonElement> JsonElement::EnumerateArray() const {
    const auto& value = RequireKind(JsonValueKind::Array);
    std::vector<JsonElement> result;
    result.reserve(value.arrayValues.size());
    for (const auto& item : value.arrayValues) {
        result.push_back(JsonElement(item));
    }
    return result;
}

std::vector<JsonElement::ObjectProperty> JsonElement::EnumerateObject() const {
    const auto& value = RequireKind(JsonValueKind::Object);
    std::vector<ObjectProperty> result;
    result.reserve(value.objectValues.size());
    for (const auto& property : value.objectValues) {
        result.push_back(ObjectProperty(property.first, JsonElement(property.second)));
    }
    return result;
}

const JsonValue& JsonElement::RequireValue() const {
    if (!value_) {
        throw JsonException("The JsonElement is undefined");
    }
    return *value_;
}

const JsonValue& JsonElement::RequireKind(JsonValueKind kind) const {
    const auto& value = RequireValue();
    if (value.kind != kind) {
        throw JsonException("The JSON value is not of the required kind");
    }
    return value;
}

}  // namespace System::Text::Json
