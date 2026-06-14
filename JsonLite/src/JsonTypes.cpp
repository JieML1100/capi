#include "JsonTypes.h"

namespace System::Text::Json {

namespace {

std::string BuildMessage(const std::string& message, std::size_t line, std::size_t bytePositionInLine) {
    return message + " LineNumber: " + std::to_string(line)
        + " | BytePositionInLine: " + std::to_string(bytePositionInLine) + ".";
}

}  // namespace

JsonException::JsonException(const std::string& message, std::size_t line, std::size_t bytePositionInLine)
    : std::runtime_error(BuildMessage(message, line, bytePositionInLine)),
      line_(line),
      bytePositionInLine_(bytePositionInLine) {
}

std::size_t JsonException::LineNumber() const noexcept {
    return line_;
}

std::size_t JsonException::BytePositionInLine() const noexcept {
    return bytePositionInLine_;
}

}  // namespace System::Text::Json
