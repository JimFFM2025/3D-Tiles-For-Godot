#ifndef REDACTING_LOG_SINK_H
#define REDACTING_LOG_SINK_H

#include "RedactCredentials.h"
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

/// Passes log messages on to other sinks with credentials redacted (see redact_credentials).
/// Cesium Native logs request URLs through spdlog's default logger, and Cesium ion's carry the
/// access token.
class RedactingLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
	explicit RedactingLogSink(std::vector<spdlog::sink_ptr> sinks) : m_sinks(std::move(sinks)) {}

	/// Puts a RedactingLogSink in front of the default logger's sinks, once. Call before anything
	/// logs: spdlog does not guard a logger's sink list.
	static void install() {
		std::vector<spdlog::sink_ptr>& sinks = spdlog::default_logger()->sinks();
		if (sinks.size() == 1 && std::dynamic_pointer_cast<RedactingLogSink>(sinks[0]) != nullptr) {
			return;
		}
		sinks = { std::make_shared<RedactingLogSink>(sinks) };
	}

protected:
	void sink_it_(const spdlog::details::log_msg& message) override {
		const std::string payload = redact_credentials(std::string_view(message.payload.data(), message.payload.size()));
		spdlog::details::log_msg redacted = message;
		redacted.payload = spdlog::string_view_t(payload.data(), payload.size());
		for (const spdlog::sink_ptr& sink : this->m_sinks) {
			if (sink->should_log(redacted.level)) {
				sink->log(redacted);
			}
		}
	}

	void flush_() override {
		for (const spdlog::sink_ptr& sink : this->m_sinks) {
			sink->flush();
		}
	}

private:
	std::vector<spdlog::sink_ptr> m_sinks;
};

#endif // REDACTING_LOG_SINK_H
