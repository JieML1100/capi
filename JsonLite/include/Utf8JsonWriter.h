#pragma once

#include "JsonElement.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace System::Text::Json {

class JsonNode;

class Utf8JsonWriter final {
public:
    explicit Utf8JsonWriter(JsonWriterOptions options = {});
    Utf8JsonWriter(std::ostream& stream, JsonWriterOptions options = {});

    void WriteStartObject();
    void WriteStartObject(std::string_view propertyName);
    void WriteEndObject();
    void WriteStartArray();
    void WriteStartArray(std::string_view propertyName);
    void WriteEndArray();
    void WritePropertyName(std::string_view propertyName);

    void WriteString(std::string_view propertyName, std::string_view value);
    void WriteNumber(std::string_view propertyName, int value);
    void WriteNumber(std::string_view propertyName, long long value);
    void WriteNumber(std::string_view propertyName, double value);
    void WriteBoolean(std::string_view propertyName, bool value);
    void WriteNull(std::string_view propertyName);

    void WriteStringValue(std::string_view value);
    void WriteNumberValue(int value);
    void WriteNumberValue(long long value);
    void WriteNumberValue(double value);
    void WriteBooleanValue(bool value);
    void WriteNullValue();
    void WriteRawValue(std::string_view json);
    void WriteElementValue(const JsonElement& element);
    void WriteNodeValue(const JsonNode& node);

    void Flush() const;
    void Reset();
    std::string GetString() const;
    void Save(std::ostream& stream) const;
    void Save(const std::string& path) const;

private:
    enum class ContainerType {
        Object,
        Array,
    };

    struct ContainerState {
        ContainerType type;
        bool first = true;
        bool propertyNameWritten = false;
    };

    void WritePropertyNameBeforeValue(std::string_view propertyName);
    void BeforeValue();
    void AfterValue();
    void WriteIndentedNewLine();
    void EnsureCanWriteValue() const;
    void EnsureCanWritePropertyName() const;

    JsonWriterOptions options_;
    std::ostream* stream_ = nullptr;
    std::string output_;
    std::vector<ContainerState> stack_;
    bool rootWritten_ = false;
    mutable std::size_t flushedLength_ = 0;
};

}  // namespace System::Text::Json
