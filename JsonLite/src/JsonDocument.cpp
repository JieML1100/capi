#include "JsonDocument.h"

#include "JsonInternal.h"

#include <fstream>
#include <sstream>

namespace System::Text::Json {

namespace {

std::string ReadStreamFully(std::istream& stream) {
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string ReadFileFully(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw JsonException("Could not open JSON file '" + path + "'");
    }
    return ReadStreamFully(stream);
}

}  // namespace

JsonDocument::JsonDocument(std::shared_ptr<const JsonValue> root)
    : root_(std::move(root)) {
}

std::shared_ptr<JsonDocument> JsonDocument::Parse(std::string_view json, const JsonDocumentOptions& options) {
    return std::make_shared<JsonDocument>(ParseJsonValue(json, options));
}

std::shared_ptr<JsonDocument> JsonDocument::Parse(std::istream& stream, const JsonDocumentOptions& options) {
    return Parse(ReadStreamFully(stream), options);
}

std::shared_ptr<JsonDocument> JsonDocument::ParseFile(const std::string& path, const JsonDocumentOptions& options) {
    return Parse(ReadFileFully(path), options);
}

JsonElement JsonDocument::RootElement() const {
    return JsonElement(root_);
}

std::string JsonDocument::ToString(const JsonSerializerOptions& options) const {
    if (!root_) {
        return {};
    }
    return RenderJsonValue(*root_, options);
}

}  // namespace System::Text::Json
