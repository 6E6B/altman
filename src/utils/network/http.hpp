#pragma once

#include "core/logging.hpp"
#include <cpr/cpr.h>
#include <initializer_list>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace HttpClient {
	struct Response {
			int status_code;
			std::string text;
			std::map<std::string, std::string> headers;
	};

	inline std::string
		build_kv_string(std::initializer_list<std::pair<const std::string, std::string>> items, char sep = '&') {
		std::ostringstream ss;
		bool first = true;
		for (auto &kv : items) {
			if (!first) { ss << sep; }
			first = false;
			ss << kv.first << '=' << kv.second;
		}
		return ss.str();
	}

	inline Response
		get(const std::string &url,
			std::initializer_list<std::pair<const std::string, std::string>> headers = {},
			cpr::Parameters params = {}) {
		auto r = cpr::Get(
			cpr::Url {url},
			cpr::Header {headers},
			params // <-- directly pass it
		);
		std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
		return {r.status_code, r.text, hdrs};
	}

	inline Response post(
		const std::string &url,
		std::initializer_list<std::pair<const std::string, std::string>> headers = {},
		const std::string &jsonBody = std::string(),
		std::initializer_list<std::pair<const std::string, std::string>> form = {}
	) {
		cpr::Header h {headers};
		cpr::Response r;
		if (!jsonBody.empty()) {
			h["Content-Type"] = "application/json";
			r = cpr::Post(cpr::Url {url}, h, cpr::Body {jsonBody});
		} else if (form.size() > 0) {
			std::string body = build_kv_string(form);
			h["Content-Type"] = "application/x-www-form-urlencoded";
			r = cpr::Post(cpr::Url {url}, h, cpr::Body {body});
		} else {
			r = cpr::Post(cpr::Url {url}, h);
		}
		std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
		return {r.status_code, r.text, hdrs};
	}

	inline nlohmann::json decode(const Response &response) {
		try {
			return nlohmann::json::parse(response.text);
		} catch (const nlohmann::json::exception &e) {
			LOG_ERROR(std::string("Failed to parse JSON response: ") + e.what());
			return nlohmann::json::object();
		}
	}

	// ============================================================================
	// Extended request methods with additional header support
	// ============================================================================

	/**
	 * Make a GET request with additional headers map
	 * @param url The request URL
	 * @param headers Map of headers to include
	 * @param params URL parameters
	 * @return Response
	 */
	inline Response getWithHeaders(
		const std::string &url,
		const std::map<std::string, std::string> &headers,
		cpr::Parameters params = {}
	) {
		cpr::Header h;
		for (const auto &[key, value] : headers) { h[key] = value; }
		auto r = cpr::Get(cpr::Url {url}, h, params);
		std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
		return {r.status_code, r.text, hdrs};
	}

	/**
	 * Make a POST request with additional headers map
	 * @param url The request URL
	 * @param headers Map of headers to include
	 * @param jsonBody JSON body string
	 * @return Response
	 */
	inline Response postWithHeaders(
		const std::string &url,
		const std::map<std::string, std::string> &headers,
		const std::string &jsonBody = ""
	) {
		cpr::Header h;
		for (const auto &[key, value] : headers) { h[key] = value; }

		cpr::Response r;
		if (!jsonBody.empty()) {
			h["Content-Type"] = "application/json";
			r = cpr::Post(cpr::Url {url}, h, cpr::Body {jsonBody});
		} else {
			r = cpr::Post(cpr::Url {url}, h);
		}

		std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
		return {r.status_code, r.text, hdrs};
	}

	/**
	 * Fetch CSRF token from Roblox endpoint
	 * @param url The URL to fetch CSRF from
	 * @param cookie The .ROBLOSECURITY cookie
	 * @return CSRF token string, or empty on failure
	 */
	inline std::string fetchCSRFToken(const std::string &url, const std::string &cookie) {
		auto response = post(
			url,
			{
				{"Cookie", ".ROBLOSECURITY=" + cookie}
		}
		);
		auto it = response.headers.find("x-csrf-token");
		if (it != response.headers.end()) { return it->second; }
		return "";
	}

} // namespace HttpClient
