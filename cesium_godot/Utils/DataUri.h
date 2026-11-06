#ifndef DATA_URI_H
#define DATA_URI_H

#if defined(CESIUM_GD_EXT)
#include "godot_cpp/classes/marshalls.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/string.hpp"
using namespace godot;
#elif defined(CESIUM_GD_MODULE)
#include "core/io/marshalls.h"
#endif

#include <string>
#include <string_view>

/// Whether url is a data: URI (RFC 2397), which carries its content in the URL itself. Cesium ion's
/// credits show their logos this way.
inline bool is_data_uri(std::string_view url) {
	return url.rfind("data:", 0) == 0;
}

/// Decodes a base64 data: URI ("data:[<media type>];base64,<data>") into out. Returns false for
/// any other data: URI.
inline bool decode_data_uri(std::string_view url, PackedByteArray& out) {
	constexpr std::string_view SCHEME = "data:";
	constexpr std::string_view BASE64 = ";base64";
	const size_t comma = url.find(',');
	if (!is_data_uri(url) || comma == std::string_view::npos) {
		return false;
	}
	const std::string_view metadata = url.substr(SCHEME.size(), comma - SCHEME.size());
	if (metadata.size() < BASE64.size() || metadata.substr(metadata.size() - BASE64.size()) != BASE64) {
		return false;
	}
	const std::string data(url.substr(comma + 1));
	out = Marshalls::get_singleton()->base64_to_raw(String(data.c_str()));
	return true;
}

#endif // DATA_URI_H
