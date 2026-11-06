#include "NetworkAssetAccessor.h"
#include "../Models/LocalAssetResponse.h"
#include "../Models/LocalAssetRequest.h"
#include "../Utils/RedactCredentials.h"
#include "CesiumAsync/AsyncSystem.h"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

#if defined (CESIUM_GD_EXT)
#include <godot_cpp/classes/http_client.hpp>
using namespace godot;
#elif defined(CESIUM_GD_MODULE)
#include "core/io/http_client.h"
#endif

using namespace CesiumAsync;

constexpr uint32_t REQ_DELAY_MICROS = 1000;
constexpr uint32_t EXPECTED_RESPONSE_SIZE_BYTES = 1024 * 120; //120kB expected response
constexpr std::string_view COMPLETED_REQ_EVENT_NAME = "request_completed";

using FutureResult_t = std::shared_ptr<CesiumAsync::IAssetRequest>;

NetworkAssetAccessor::NetworkAssetAccessor()
{
	constexpr size_t maxThreadsPerClient = 16;
	this->m_httpClient.init_client(maxThreadsPerClient);
	// Set all the default headers
	this->m_httpClient.add_default_header({"x-cesium-client", "3D Tiles For Godot"});
	this->m_httpClient.add_default_header({"x-cesium-client-version", "1.0"});
	String godotBuildInfo = Engine::get_singleton()->get_version_info().get("string", "");
	this->m_httpClient.add_default_header({"x-cesium-client-engine", godotBuildInfo.utf8().get_data()});
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> NetworkAssetAccessor::get(const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url, const std::vector<THeader>& headers /*= {}*/)
{
	return asyncSystem.runInWorkerThread([=, this]() {
		return process_request(HTTPClient::METHOD_GET, "GET", asyncSystem, url, headers);
	});
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> NetworkAssetAccessor::request(const CesiumAsync::AsyncSystem& asyncSystem, const std::string& verb, const std::string& url, const std::vector<THeader>& headers /*= std::vector<THeader>()*/, const std::span<const std::byte>& contentPayload /*= {}*/)
{
	using HttpMethod_t = HTTPClient::Method;

	constexpr std::array<const char*, 9> STR_METHODS = {
		"GET",
		"POST",
		"PUT",
		"CONNECT",
		"HEAD",
		"DELETE",
		"OPTIONS",
		"TRACE",
		"PATCH"
	};

	constexpr std::array<HttpMethod_t, 9> ENUM_METHODS = {
		HttpMethod_t::METHOD_GET,
		HttpMethod_t::METHOD_POST,
		HttpMethod_t::METHOD_PUT,
		HttpMethod_t::METHOD_CONNECT,
		HttpMethod_t::METHOD_HEAD,
		HttpMethod_t::METHOD_DELETE,
		HttpMethod_t::METHOD_OPTIONS,
		HttpMethod_t::METHOD_TRACE,
		HttpMethod_t::METHOD_PATCH
	};
	static_assert(STR_METHODS.size() == ENUM_METHODS.size());

	const auto found = std::find(STR_METHODS.begin(), STR_METHODS.end(), verb);
	if (found == STR_METHODS.end()) {
		ERR_PRINT(String("Request method with name ") + String(verb.c_str()) + " not found!");
		// Always resolve with error status - reject() crashes with LIBASYNC_NO_EXCEPTIONS
		return asyncSystem.createFuture<FutureResult_t>([=](CesiumAsync::Promise<FutureResult_t> p_promise) {
			std::string contentType = "text/plain";
			CesiumAsync::HttpHeaders errorHeaders = { { "content-type", contentType } };
			auto errorResponse = std::make_unique<LocalAssetResponse>(
				400, // Bad Request
				contentType,
				errorHeaders,
				PackedByteArray()
			);
			auto errorRequest = std::make_shared<LocalAssetRequest>(
				verb,
				url,
				errorHeaders,
				std::move(errorResponse)
			);
			p_promise.resolve(errorRequest);
		});
	}

	const HttpMethod_t method = ENUM_METHODS[found - STR_METHODS.begin()];
	// The caller only lends the payload for this call, and the request runs later.
	std::vector<std::byte> payload(contentPayload.begin(), contentPayload.end());
	return asyncSystem.runInWorkerThread([=, this, payload = std::move(payload)]() {
		return process_request(method, verb, asyncSystem, url, headers, payload);
	});
}

void NetworkAssetAccessor::tick() noexcept
{
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> NetworkAssetAccessor::process_request(HTTPClient::Method method, const std::string& verb, const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url, const std::vector<THeader>& headers /*= {}*/, const std::vector<std::byte>& payload /*= {}*/)
{
	CesiumAsync::Promise<FutureResult_t> p_promise = asyncSystem.createPromise<FutureResult_t>();
	CesiumAsync::Future<FutureResult_t> future = p_promise.getFuture();
	this->m_httpClient.send_request(
			url.c_str(),
			method,
			[verb, url, headers, p_promise = std::move(p_promise)](int32_t responseCode, const PackedByteArray &body, const std::vector<THeader> &responseHeaderList) mutable {
				// For file:// URLs, libcurl returns response code 0 on success (no HTTP layer).
				// Treat code 0 with a non-empty body as a successful response.
				const bool isFileUrl = url.rfind("file://", 0) == 0;
				if (isFileUrl && responseCode == 0 && body.size() > 0) {
					responseCode = HTTPClient::ResponseCode::RESPONSE_OK;
				}

				// Log errors but always resolve - Cesium Native handles error status codes
				// Using reject() causes crashes with LIBASYNC_NO_EXCEPTIONS
				if (responseCode >= HTTPClient::ResponseCode::RESPONSE_BAD_REQUEST || (responseCode == 0 && !isFileUrl) /* Invalid request will yield 0 */) {
					const String errorMessage = String("The underlying request failed with code: ") + itos(responseCode);
					const char *strPtr = reinterpret_cast<const char *>(body.ptr());
					String bodyStr = strPtr;
					ERR_PRINT(errorMessage + String("\nURL: ") + String::utf8(redact_credentials(url).c_str()) + String("\nFailed request's body: ") + bodyStr);
					if (responseCode == HTTPClient::ResponseCode::RESPONSE_UNAUTHORIZED) {
						ERR_PRINT("Access denied - check CesiumION token!");
					}
				}

				// Pass the server's headers on: the request cache (CachingAssetAccessor) only stores
				// responses whose Cache-Control, Expires, ETag or Last-Modified headers allow it. A
				// header sent more than once becomes one comma-separated value, as HTTP allows.
				CesiumAsync::HttpHeaders responseHeaders;
				for (const THeader &header : responseHeaderList) {
					auto [existing, inserted] = responseHeaders.emplace(header);
					if (!inserted) {
						existing->second += ", " + header.second;
					}
				}
				const std::string contentType =
						responseHeaders.try_emplace("content-type", "application/octet-stream").first->second;

				auto assetResponse = std::make_unique<LocalAssetResponse>(
						responseCode,
						contentType,
						responseHeaders,
						body);

				auto assetRequest = std::make_shared<LocalAssetRequest>(
						verb,
						url,
						CesiumAsync::HttpHeaders(headers.begin(), headers.end()),
						std::move(assetResponse));
				p_promise.resolve(assetRequest);
			},
			headers,
			payload
	);
	return future;
}
