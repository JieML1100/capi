#pragma once

#include "JsonTypes.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace System::Text::Json {

struct JsonValue;

class JsonElement final {
public:
    using ObjectProperty = std::pair<std::string, JsonElement>;

    JsonElement() = default;

    JsonValueKind ValueKind() const noexcept;
    std::string GetRawText() const;
    std::string ToString() const;

    std::string GetString() const;
    bool GetBoolean() const;
    int GetInt32() const;
    long long GetInt64() const;
    double GetDouble() const;

    bool TryGetInt32(int& value) const noexcept;
    bool TryGetInt64(long long& value) const noexcept;
    bool TryGetDouble(double& value) const noexcept;

    int GetArrayLength() const;
    JsonElement operator[](std::size_t index) const;
    JsonElement GetProperty(std::string_view propertyName) const;
    bool TryGetProperty(std::string_view propertyName, JsonElement& value) const;
    bool HasProperty(std::string_view propertyName) const;

    std::vector<JsonElement> EnumerateArray() const;
    std::vector<ObjectProperty> EnumerateObject() const;

private:
    explicit JsonElement(std::shared_ptr<const JsonValue> value);

    const JsonValue& RequireValue() const;
    const JsonValue& RequireKind(JsonValueKind kind) const;

    std::shared_ptr<const JsonValue> value_;

    friend class JsonDocument;
    friend class JsonSerializer;
    friend class Utf8JsonWriter;
    friend class JsonNode;
};

}  // namespace System::Text::Json
