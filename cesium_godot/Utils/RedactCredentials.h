#ifndef REDACT_CREDENTIALS_H
#define REDACT_CREDENTIALS_H

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

/// Query parameters whose values are credentials: Cesium ion's access token, Bing Maps' key and
/// the usual names for API keys.
constexpr std::array<std::string_view, 5> CREDENTIAL_PARAMETERS = {
	"access_token", "token", "key", "api_key", "apikey"
};

/// Returns text with the value of every credential query parameter in the URLs it holds replaced
/// by "<redacted>", so error messages and logs that quote a request's URL do not leak it.
inline std::string redact_credentials(std::string_view text) {
	constexpr std::string_view VALUE_END = "&# \t\r\n\"'<>";
	const auto same_name = [](std::string_view a, std::string_view b) {
		for (size_t i = 0; i < a.size(); i++) {
			if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) {
				return false;
			}
		}
		return true;
	};
	std::string result;
	result.reserve(text.size());
	size_t at = 0;
	while (at < text.size()) {
		const char c = text[at++];
		result += c;
		if (c != '?' && c != '&') {
			continue;
		}
		for (std::string_view name : CREDENTIAL_PARAMETERS) {
			if (text.size() - at > name.size() && text[at + name.size()] == '='
					&& same_name(text.substr(at, name.size()), name)) {
				result.append(text.substr(at, name.size() + 1));
				const size_t end = text.find_first_of(VALUE_END, at + name.size() + 1);
				at = end == std::string_view::npos ? text.size() : end;
				result += "<redacted>";
				break;
			}
		}
	}
	return result;
}

#endif // REDACT_CREDENTIALS_H
