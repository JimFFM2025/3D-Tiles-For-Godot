#ifndef CURL_HTTP_CLIENT
#define CURL_HTTP_CLIENT

#include "godot_cpp/classes/os.hpp"
#include <sstream>
#if defined(CESIUM_GD_EXT)

#ifdef _WIN32
//Link to windows libraries
#pragma comment(lib, "Ws2_32.Lib")
#pragma comment(lib, "Wldap32.Lib")
#pragma comment(lib, "Crypt32.Lib")
#endif

#include "godot_cpp/variant/string.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include <godot_cpp/templates/vector.hpp>
#include "godot_cpp/classes/http_client.hpp"
using namespace godot;
#elif defined(CESIUM_GD_MODULE)
#include "core/templates/vector.h"
#endif

#include "BRThreadPool.h"
#include "DataUri.h"
#include "RedactCredentials.h"
#include <curl/curl.h>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>
#include <string_view>

using CesiumHeader_t = std::pair<std::string, std::string>;
/// Called with the status code, the body and the response's headers, in the order received.
using HighLevelResponseCallback_t = std::function<void(int32_t, const PackedByteArray&, const std::vector<CesiumHeader_t>&)>;

/// @brief One reusable curl easy handle per thread. A handle is never shared between threads, and
/// it is reset before each request so options from an earlier request (method, headers, ...) never
/// leak into the next one. Reusing it keeps connections alive between requests on the same thread.
class ThreadCurlHandle {
public:
	~ThreadCurlHandle() {
		if (this->m_handle != nullptr) {
			curl_easy_cleanup(this->m_handle);
		}
	}

	static CURL* acquire() {
		thread_local ThreadCurlHandle threadHandle;
		if (threadHandle.m_handle == nullptr) {
			threadHandle.m_handle = curl_easy_init();
		} else {
			curl_easy_reset(threadHandle.m_handle);
		}
		return threadHandle.m_handle;
	}

private:
	CURL* m_handle = nullptr;
};

/// @brief Wrapper around libcurl that runs blocking requests on a small thread pool.
/// send_request may be called from any thread.
template<uint32_t N_MAX_HANDLES>
class CurlHttpClient {
public:
	CurlHttpClient() {
		// curl_global_init is not thread-safe; do it once and never clean up, since handles owned by
		// other threads may outlive any single client.
		static std::once_flag s_curlInit;
		std::call_once(s_curlInit, [] { curl_global_init(CURL_GLOBAL_ALL); });
	}

	void init_client(size_t maxThreads) {
		std::lock_guard<std::mutex> lock(this->m_headersMutex);
		if (this->m_threadPool.size() > 0) {
			return; // Already initialized (callers may call this more than once).
		}
		this->m_threadPool.init(maxThreads);
		std::string systemInfo = OS::get_singleton()->get_name().utf8().get_data();
		auto architecture = OS::get_singleton()->get_processor_name().utf8().get_data();
		std::stringstream stream;
		stream << "Godot3DTiles/1.0 (" << systemInfo << "; " << architecture << ")";	
		this->m_defaultHeaders.emplace_back("User-Agent", stream.str());
	}

	void send_get(const char* url, const HighLevelResponseCallback_t& callback, const std::vector<CesiumHeader_t>& headers) {
		this->send_request(url, HTTPClient::METHOD_GET, callback, headers);
	}

	void send_get_same_thread(const char *url, const HighLevelResponseCallback_t &callback, const std::vector<CesiumHeader_t> &headers) {
		this->send_request_same_thread(url, HTTPClient::METHOD_GET, callback, headers);
	}

	/// Sends body too, with any method but GET and HEAD.
	void send_request(const char* url, HTTPClient::Method method, const HighLevelResponseCallback_t& callback, const std::vector<CesiumHeader_t>& headers, const std::vector<std::byte>& body = {}) {
		// Everything the request needs is captured by value: this returns before the request runs.
		this->m_threadPool.enqueue([this, urlCopy = std::string(url), method, headers, body, callback] {
			long responseCode = 0;
			std::vector<CesiumHeader_t> responseHeaders;
			PackedByteArray packedData = this->pull_url(urlCopy.c_str(), method, &responseCode, headers, &responseHeaders, body);
			callback(responseCode, packedData, responseHeaders);
		});
	}

	void send_request_same_thread(const char *url, HTTPClient::Method method, const HighLevelResponseCallback_t &callback, const std::vector<CesiumHeader_t> &headers) {
		long responseCode = 0;
		std::vector<CesiumHeader_t> responseHeaders;
		PackedByteArray packedData = this->pull_url(url, method, &responseCode, headers, &responseHeaders);
		callback(responseCode, packedData, responseHeaders);
	}

	void add_default_header(const CesiumHeader_t& header) {
		std::lock_guard<std::mutex> lock(this->m_headersMutex);
		this->m_defaultHeaders.emplace_back(header);
	}

private:
	static void configure_http_method(CURL* handle, HTTPClient::Method method) {
		// pull_url sets any body afterwards; without one, make sure curl does not try to read one
		// (from stdin).
		switch (method) {
			case HTTPClient::METHOD_GET:
				break;
			case HTTPClient::METHOD_HEAD:
				curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
				break;
			case HTTPClient::METHOD_POST:
				curl_easy_setopt(handle, CURLOPT_POST, 1L);
				curl_easy_setopt(handle, CURLOPT_POSTFIELDS, "");
				curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, 0L);
				break;
			case HTTPClient::METHOD_PUT:
				curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
				break;
			case HTTPClient::METHOD_DELETE:
				curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
				break;
			case HTTPClient::METHOD_OPTIONS:
				curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "OPTIONS");
				break;
			case HTTPClient::METHOD_TRACE:
				curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "TRACE");
				break;
			case HTTPClient::METHOD_CONNECT:
				curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "CONNECT");
				break;
			case HTTPClient::METHOD_PATCH:
				curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PATCH");
				break;
			default:
				break;
		}
	}

	PackedByteArray pull_url(const char *url, HTTPClient::Method method, long *outStatus, const std::vector<CesiumHeader_t> &headers, std::vector<CesiumHeader_t> *outHeaders, const std::vector<std::byte> &body = {}) {
		*outStatus = 0;
		outHeaders->clear();
		PackedByteArray buffer;
		if (is_data_uri(url)) {
			// curl has no data: scheme, so decode the content here.
			if (decode_data_uri(url, buffer)) {
				*outStatus = HTTPClient::RESPONSE_OK;
			} else {
				ERR_PRINT("Unsupported data: URI; only base64 content is supported");
				*outStatus = HTTPClient::RESPONSE_BAD_REQUEST;
			}
			return buffer;
		}
		CURL* handle = ThreadCurlHandle::acquire();
		if (handle == nullptr) {
			ERR_PRINT(String("Could not create a curl handle for: ") + String::utf8(redact_credentials(url).c_str()));
			return buffer;
		}

		curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(handle, CURLOPT_URL, url);
		curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &CurlHttpClient::write_callback);
		curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
		curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &CurlHttpClient::header_callback);
		curl_easy_setopt(handle, CURLOPT_HEADERDATA, outHeaders);
		curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
		configure_http_method(handle, method);
		if (!body.empty() && method != HTTPClient::METHOD_GET && method != HTTPClient::METHOD_HEAD) {
			// With CURLOPT_CUSTOMREQUEST (PUT, PATCH, ...) curl sends this body with that method.
			curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
			curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS, reinterpret_cast<const char*>(body.data()));
		}

		curl_slist* curlHeaders = nullptr;
		{
			std::lock_guard<std::mutex> lock(this->m_headersMutex);
			for (const CesiumHeader_t& h : this->m_defaultHeaders) {
				curlHeaders = curl_slist_append(curlHeaders, (h.first + ": " + h.second).c_str());
			}
		}
		for (const CesiumHeader_t& h : headers) {
			curlHeaders = curl_slist_append(curlHeaders, (h.first + ": " + h.second).c_str());
		}
		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);

		CURLcode code = curl_easy_perform(handle);
		if (code == CURLE_OK) {
			curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, outStatus);
		} else {
			ERR_PRINT(String("Could not make request to: ") + String::utf8(redact_credentials(url).c_str()) + String(" error: ") + itos(code) + " (" + curl_easy_strerror(code) + ")");
			buffer = PackedByteArray();
			outHeaders->clear();
		}
		// The handle keeps a pointer to the header list until its next reset, so clear it first.
		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, nullptr);
		curl_slist_free_all(curlHeaders);
		return buffer;
	}

	/// Collects the response's headers. After a redirect curl reports each response's headers in
	/// turn, every set starting with its status line, so a status line starts the list over and only
	/// the final response's headers remain.
	static size_t header_callback(char *line, size_t size, size_t nitems, void *userp) {
		auto *headers = reinterpret_cast<std::vector<CesiumHeader_t>*>(userp);
		const size_t length = size * nitems;
		const std::string_view text(line, length);
		if (text.rfind("HTTP/", 0) == 0) {
			headers->clear();
			return length;
		}
		const size_t colon = text.find(':');
		if (colon != std::string_view::npos) { // Not the blank line that ends the headers.
			headers->emplace_back(trim(text.substr(0, colon)), trim(text.substr(colon + 1)));
		}
		return length;
	}

	static std::string trim(std::string_view text) {
		constexpr std::string_view SPACE = " \t\r\n";
		const size_t first = text.find_first_not_of(SPACE);
		if (first == std::string_view::npos) {
			return std::string();
		}
		return std::string(text.substr(first, text.find_last_not_of(SPACE) - first + 1));
	}

	static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
		auto *buffer = reinterpret_cast<PackedByteArray*>(userp);
		const size_t realSize = size * nmemb;
		const int64_t previousSize = buffer->size();
		if (buffer->resize(previousSize + realSize) != OK) {
			return 0; // Tells curl the write failed.
		}
		memcpy(buffer->ptrw() + previousSize, contents, realSize);
		return realSize;
	}

	// Declared before the pool so the pool (whose workers use these) is destroyed first.
	std::mutex m_headersMutex;
	std::vector<CesiumHeader_t> m_defaultHeaders;
	//Have a thread pool for some batches of requests
	BRThreadPool m_threadPool;
};

#endif // HIGH_LEVEL_HTTP_CLIENT
