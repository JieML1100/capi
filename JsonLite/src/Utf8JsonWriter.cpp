#include "Utf8JsonWriter.h"

#include "JsonInternal.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <ostream>
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

JsonSerializerOptions ToSerializerOptions(const JsonWriterOptions& options) {
    JsonSerializerOptions serializerOptions;
    serializerOptions.WriteIndented = options.Indented;
    serializerOptions.IndentChars = options.IndentChars;
    serializerOptions.NewLine = options.NewLine;
    return serializerOptions;
}

}  // namespace

Utf8JsonWriter::Utf8JsonWriter(JsonWriterOptions options)
    : options_(std::move(options)) {
}

Utf8JsonWriter::Utf8JsonWriter(std::ostream& stream, JsonWriterOptions options)
    : options_(std::move(options)), stream_(&stream) {
}

void Utf8JsonWriter::WriteStartObject() {
    BeforeValue();
    output_.push_back('{');
    stack_.push_back({ContainerType::Object});
}

void Utf8JsonWriter::WriteStartObject(std::string_view propertyName) {
    WritePropertyNameBeforeValue(propertyName);
    WriteStartObject();
}

void Utf8JsonWriter::WriteEndObject() {
    if (stack_.empty() || stack_.back().type != ContainerType::Object) {
        throw JsonException("The current JSON container is not an object");
    }
    if (!options_.SkipValidation && stack_.back().propertyNameWritten) {
        throw JsonException("Cannot close a JSON object while a property has no value");
    }

    const bool hadProperties = !stack_.back().first;
    stack_.pop_back();
    if (options_.Indented && hadProperties) {
        output_ += options_.NewLine;
        WriteIndentedNewLine();
    }
    output_.push_back('}');
    AfterValue();
}

void Utf8JsonWriter::WriteStartArray() {
    BeforeValue();
    output_.push_back('[');
    stack_.push_back({ContainerType::Array});
}

void Utf8JsonWriter::WriteStartArray(std::string_view propertyName) {
    WritePropertyNameBeforeValue(propertyName);
    WriteStartArray();
}

void Utf8JsonWriter::WriteEndArray() {
    if (stack_.empty() || stack_.back().type != ContainerType::Array) {
        throw JsonException("The current JSON container is not an array");
    }

    const bool hadItems = !stack_.back().first;
    stack_.pop_back();
    if (options_.Indented && hadItems) {
        output_ += options_.NewLine;
        WriteIndentedNewLine();
    }
    output_.push_back(']');
    AfterValue();
}

void Utf8JsonWriter::WritePropertyName(std::string_view propertyName) {
    EnsureCanWritePropertyName();

    auto& state = stack_.back();
    if (!state.first) {
        output_.push_back(',');
    }
    if (options_.Indented) {
        output_ += options_.NewLine;
        WriteIndentedNewLine();
    }

    output_ += EscapeJsonString(propertyName);
    output_.push_back(':');
    if (options_.Indented) {
        output_.push_back(' ');
    }
    state.propertyNameWritten = true;
}

void Utf8JsonWriter::WriteString(std::string_view propertyName, std::string_view value) {
    WritePropertyNameBeforeValue(propertyName);
    WriteStringValue(value);
}

void Utf8JsonWriter::WriteNumber(std::string_view propertyName, int value) {
    WritePropertyNameBeforeValue(propertyName);
    WriteNumberValue(value);
}

void Utf8JsonWriter::WriteNumber(std::string_view propertyName, long long value) {
    WritePropertyNameBeforeValue(propertyName);
    WriteNumberValue(value);
}

void Utf8JsonWriter::WriteNumber(std::string_view propertyName, double value) {
    WritePropertyNameBeforeValue(propertyName);
    WriteNumberValue(value);
}

void Utf8JsonWriter::WriteBoolean(std::string_view propertyName, bool value) {
    WritePropertyNameBeforeValue(propertyName);
    WriteBooleanValue(value);
}

void Utf8JsonWriter::WriteNull(std::string_view propertyName) {
    WritePropertyNameBeforeValue(propertyName);
    WriteNullValue();
}

void Utf8JsonWriter::WriteStringValue(std::string_view value) {
    BeforeValue();
    output_ += EscapeJsonString(value);
    AfterValue();
}

void Utf8JsonWriter::WriteNumberValue(int value) {
    BeforeValue();
    output_ += std::to_string(value);
    AfterValue();
}

void Utf8JsonWriter::WriteNumberValue(long long value) {
    BeforeValue();
    output_ += std::to_string(value);
    AfterValue();
}

void Utf8JsonWriter::WriteNumberValue(double value) {
    BeforeValue();
    output_ += FormatDouble(value);
    AfterValue();
}

void Utf8JsonWriter::WriteBooleanValue(bool value) {
    BeforeValue();
    output_ += value ? "true" : "false";
    AfterValue();
}

void Utf8JsonWriter::WriteNullValue() {
    BeforeValue();
    output_ += "null";
    AfterValue();
}

void Utf8JsonWriter::WriteRawValue(std::string_view json) {
    BeforeValue();
    output_ += json;
    AfterValue();
}

void Utf8JsonWriter::WriteElementValue(const JsonElement& element) {
    if (!element.value_) {
        WriteNullValue();
        return;
    }
    WriteRawValue(RenderJsonValue(*element.value_, ToSerializerOptions(options_)));
}

void Utf8JsonWriter::WriteNodeValue(const JsonNode& node) {
    WriteRawValue(node.ToJsonString(ToSerializerOptions(options_)));
}

void Utf8JsonWriter::Flush() const {
    if (stream_ == nullptr || flushedLength_ >= output_.size()) {
        return;
    }

    stream_->write(output_.data() + static_cast<std::ptrdiff_t>(flushedLength_), static_cast<std::streamsize>(output_.size() - flushedLength_));
    stream_->flush();
    flushedLength_ = output_.size();
}

void Utf8JsonWriter::Reset() {
    output_.clear();
    stack_.clear();
    rootWritten_ = false;
    flushedLength_ = 0;
}

std::string Utf8JsonWriter::GetString() const {
    return output_;
}

void Utf8JsonWriter::Save(std::ostream& stream) const {
    stream << output_;
}

void Utf8JsonWriter::Save(const std::string& path) const {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw JsonException("Could not open JSON file '" + path + "' for writing");
    }
    Save(stream);
}

void Utf8JsonWriter::WritePropertyNameBeforeValue(std::string_view propertyName) {
    WritePropertyName(propertyName);
}

void Utf8JsonWriter::BeforeValue() {
    EnsureCanWriteValue();

    if (stack_.empty()) {
        return;
    }

    auto& state = stack_.back();
    if (state.type == ContainerType::Array) {
        if (!state.first) {
            output_.push_back(',');
        }
        if (options_.Indented) {
            output_ += options_.NewLine;
            WriteIndentedNewLine();
        }
    }
}

void Utf8JsonWriter::AfterValue() {
    if (stack_.empty()) {
        rootWritten_ = true;
        return;
    }

    auto& state = stack_.back();
    state.first = false;
    if (state.type == ContainerType::Object) {
        state.propertyNameWritten = false;
    }
}

void Utf8JsonWriter::WriteIndentedNewLine() {
    for (std::size_t index = 0; index < stack_.size(); ++index) {
        output_ += options_.IndentChars;
    }
}

void Utf8JsonWriter::EnsureCanWriteValue() const {
    if (options_.SkipValidation) {
        return;
    }

    if (stack_.empty()) {
        if (rootWritten_) {
            throw JsonException("A JSON payload can only contain one root value");
        }
        return;
    }

    const auto& state = stack_.back();
    if (state.type == ContainerType::Object && !state.propertyNameWritten) {
        throw JsonException("Cannot write a JSON value inside an object without a property name");
    }
}

void Utf8JsonWriter::EnsureCanWritePropertyName() const {
    if (options_.SkipValidation) {
        return;
    }

    if (stack_.empty() || stack_.back().type != ContainerType::Object) {
        throw JsonException("Property names can only be written inside JSON objects");
    }
    if (stack_.back().propertyNameWritten) {
        throw JsonException("The previous JSON property has no value");
    }
}

}  // namespace System::Text::Json
