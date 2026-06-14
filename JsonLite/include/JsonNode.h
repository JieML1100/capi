#pragma once

#include "JsonElement.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace System::Text::Json {

class JsonObject;
class JsonArray;

class JsonNode {
public:
    JsonNode();

    static JsonNode Parse(std::string_view json, const JsonDocumentOptions& options = {});
    static JsonNode CreateNull();
    static JsonNode Create(bool value);
    static JsonNode Create(int value);
    static JsonNode Create(long long value);
    static JsonNode Create(double value);
    static JsonNode Create(const char* value);
    static JsonNode Create(std::string_view value);

    JsonValueKind ValueKind() const noexcept;
    JsonElement AsElement() const;
    JsonObject AsObject() const;
    JsonArray AsArray() const;
    std::string ToJsonString(const JsonSerializerOptions& options = {}) const;
    std::string ToString() const;

protected:
    explicit JsonNode(std::shared_ptr<JsonValue> value);

    std::shared_ptr<JsonValue> value_;

    friend class JsonObject;
    friend class JsonArray;
    friend class JsonSerializer;
};

class JsonObject final : public JsonNode {
public:
    JsonObject();

    std::size_t Count() const;
    bool ContainsKey(std::string_view propertyName) const;
    JsonNode operator[](std::string_view propertyName) const;
    JsonNode GetValue(std::string_view propertyName) const;
    bool TryGetValue(std::string_view propertyName, JsonNode& value) const;

    void Add(std::string_view propertyName, const JsonNode& value);
    void Set(std::string_view propertyName, const JsonNode& value);
    bool Remove(std::string_view propertyName);
    void Clear();

    std::vector<std::pair<std::string, JsonNode>> Properties() const;

private:
    explicit JsonObject(std::shared_ptr<JsonValue> value);

    friend class JsonNode;
};

class JsonArray final : public JsonNode {
public:
    JsonArray();

    std::size_t Count() const;
    JsonNode operator[](std::size_t index) const;
    JsonNode GetValue(std::size_t index) const;

    void Add(const JsonNode& value);
    void Add(const char* value);
    void Add(std::string_view value);
    void Add(int value);
    void Add(long long value);
    void Add(double value);
    void Add(bool value);
    void AddNull();
    void RemoveAt(std::size_t index);
    void Clear();

    std::vector<JsonNode> Items() const;

private:
    explicit JsonArray(std::shared_ptr<JsonValue> value);

    friend class JsonNode;
};

}  // namespace System::Text::Json
