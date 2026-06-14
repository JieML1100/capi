#pragma once

#include <string>

#include "Convert.h"
#include "json.h"

namespace JsonCompat {

using Json = JsonLib::json;

inline bool IsValidUtf8(const std::string& text) noexcept {
	const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
	const size_t length = text.size();
	size_t index = 0;
	while (index < length) {
		const unsigned char ch = bytes[index];
		if (ch <= 0x7F) {
			++index;
			continue;
		}
		if (ch >= 0xC2 && ch <= 0xDF) {
			if (index + 1 >= length || (bytes[index + 1] & 0xC0) != 0x80) {
				return false;
			}
			index += 2;
			continue;
		}
		if (ch == 0xE0) {
			if (index + 2 >= length || bytes[index + 1] < 0xA0 || bytes[index + 1] > 0xBF ||
				(bytes[index + 2] & 0xC0) != 0x80) {
				return false;
			}
			index += 3;
			continue;
		}
		if ((ch >= 0xE1 && ch <= 0xEC) || (ch >= 0xEE && ch <= 0xEF)) {
			if (index + 2 >= length || (bytes[index + 1] & 0xC0) != 0x80 || (bytes[index + 2] & 0xC0) != 0x80) {
				return false;
			}
			index += 3;
			continue;
		}
		if (ch == 0xED) {
			if (index + 2 >= length || bytes[index + 1] < 0x80 || bytes[index + 1] > 0x9F ||
				(bytes[index + 2] & 0xC0) != 0x80) {
				return false;
			}
			index += 3;
			continue;
		}
		if (ch == 0xF0) {
			if (index + 3 >= length || bytes[index + 1] < 0x90 || bytes[index + 1] > 0xBF ||
				(bytes[index + 2] & 0xC0) != 0x80 || (bytes[index + 3] & 0xC0) != 0x80) {
				return false;
			}
			index += 4;
			continue;
		}
		if (ch >= 0xF1 && ch <= 0xF3) {
			if (index + 3 >= length || (bytes[index + 1] & 0xC0) != 0x80 ||
				(bytes[index + 2] & 0xC0) != 0x80 || (bytes[index + 3] & 0xC0) != 0x80) {
				return false;
			}
			index += 4;
			continue;
		}
		if (ch == 0xF4) {
			if (index + 3 >= length || bytes[index + 1] < 0x80 || bytes[index + 1] > 0x8F ||
				(bytes[index + 2] & 0xC0) != 0x80 || (bytes[index + 3] & 0xC0) != 0x80) {
				return false;
			}
			index += 4;
			continue;
		}
		return false;
	}
	return true;
}

inline void NormalizeStringEncoding(Json& value) {
	if (value.is_string()) {
		std::string text = value.get<std::string>();
		if (!text.empty() && !IsValidUtf8(text)) {
			value = Convert::AnsiToUtf8(text);
		}
		return;
	}
	if (value.is_array()) {
		for (auto& item : value) {
			NormalizeStringEncoding(item);
		}
		return;
	}
	if (value.is_object()) {
		for (auto& item : value.items()) {
			NormalizeStringEncoding(item.value());
		}
	}
}

inline Json Parse(const std::string& text) {
	try {
		return Json::parse(text);
	}
	catch (const Json::parse_error&) {
		const std::string fallback = Convert::AnsiToUtf8(text);
		if (fallback != text) {
			return Json::parse(fallback);
		}
		throw;
	}
}

inline bool TryParse(const std::string& text, Json& outJson) noexcept {
	try {
		outJson = Parse(text);
		return true;
	}
	catch (...) {
		return false;
	}
}

inline std::string Dump(Json value, int indent = -1, char indentChar = ' ', bool ensureAscii = false) {
	NormalizeStringEncoding(value);
	return value.dump(indent, indentChar, ensureAscii, Json::error_handler_t::replace);
}

}