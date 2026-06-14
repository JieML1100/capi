#pragma once

#include "JsonElement.h"

#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

namespace System::Text::Json {

class JsonDocument final {
public:
    JsonDocument() = default;
    explicit JsonDocument(std::shared_ptr<const JsonValue> root);

    static std::shared_ptr<JsonDocument> Parse(std::string_view json, const JsonDocumentOptions& options = {});
    static std::shared_ptr<JsonDocument> Parse(std::istream& stream, const JsonDocumentOptions& options = {});
    static std::shared_ptr<JsonDocument> ParseFile(const std::string& path, const JsonDocumentOptions& options = {});

    JsonElement RootElement() const;
    std::string ToString(const JsonSerializerOptions& options = {}) const;

private:
    std::shared_ptr<const JsonValue> root_;
};

}  // namespace System::Text::Json
