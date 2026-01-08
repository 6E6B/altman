#pragma once

#include <string>
#include <map>
#include <vector>
#include <initializer_list>
#include <sstream>
#include <fstream>
#include <functional>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include "core/logging.hpp"

namespace HttpClient {
	struct Response {
		int status_code;
		std::string text;
		std::map<std::string, std::string> headers;
		std::string final_url;
	};

	struct BinaryResponse {
		int status_code;
		std::vector<uint8_t> data;
		std::map<std::string, std::string> headers;
		std::string final_url;
	};

	using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

	inline std::string build_kv_string(
		std::initializer_list<std::pair<const std::string, std::string>> items,
		char sep = '&'
	) {
		std::ostringstream ss;
		bool first = true;
		for (const auto& kv : items) {
			if (!first) ss << sep;
			first = false;
			ss << kv.first << '=' << kv.second;
		}
		return ss.str();
	}

	inline Response get_impl(
		const std::string& url,
		const cpr::Header& hdr,
		const cpr::Parameters& params,
		bool follow_redirects,
		int max_redirects
	) {
		auto r = cpr::Get(
			cpr::Url{url},
			hdr,
			params,
			cpr::Redirect(follow_redirects ? max_redirects : 0L)
		);

		std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());

		return {
			static_cast<int>(r.status_code),
			r.text,
			hdrs,
			r.url.str()
		};
	}

	inline Response get(
		const std::string& url,
		std::initializer_list<std::pair<std::string, std::string>> headers = {},
		const cpr::Parameters& params = {},
		bool follow_redirects = true,
		int max_redirects = 10
	) {
		cpr::Header hdr;

		for (const auto& [k, v] : headers)
			hdr.emplace(k, v);

		return get_impl(url, hdr, params, follow_redirects, max_redirects);
	}


	inline Response get(
		const std::string& url,
		std::span<const std::pair<std::string, std::string>> headers,
		const cpr::Parameters& params = {},
		bool follow_redirects = true,
		int max_redirects = 10
	) {
		cpr::Header hdr;

		for (const auto& [k, v] : headers)
			hdr.emplace(k, v);

		return get_impl(
			url,
			hdr,
			params,
			follow_redirects,
			max_redirects
		);
	}

    inline BinaryResponse get_binary(
        const std::string& url,
        std::initializer_list<std::pair<const std::string, std::string>> headers = {},
        cpr::Parameters params = {},
        bool follow_redirects = true,
        int max_redirects = 10
    ) {
        auto r = cpr::Get(
            cpr::Url{url},
            cpr::Header{headers},
            params,
            cpr::Redirect(follow_redirects ? max_redirects : 0L)
        );

        std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());

        std::vector<uint8_t> data(r.text.begin(), r.text.end());

        return {
            static_cast<int>(r.status_code),
            std::move(data),
            hdrs,
            r.url.str()
        };
    }

    inline Response post(
        const std::string& url,
        std::initializer_list<std::pair<const std::string, std::string>> headers = {},
        const std::string& jsonBody = std::string(),
        std::initializer_list<std::pair<const std::string, std::string>> form = {},
        bool follow_redirects = true,
        int max_redirects = 10
    ) {
        cpr::Header h{headers};
        cpr::Response r;

        if (!jsonBody.empty()) {
            h["Content-Type"] = "application/json";
            r = cpr::Post(
                cpr::Url{url},
                h,
                cpr::Body{jsonBody},
                cpr::Redirect(follow_redirects ? max_redirects : 0L)
            );
        } else if (form.size() > 0) {
            std::string body = build_kv_string(form);
            h["Content-Type"] = "application/x-www-form-urlencoded";
            r = cpr::Post(
                cpr::Url{url},
                h,
                cpr::Body{body},
                cpr::Redirect(follow_redirects ? max_redirects : 0L)
            );
        } else {
            r = cpr::Post(
                cpr::Url{url},
                h,
                cpr::Redirect(follow_redirects ? max_redirects : 0L)
            );
        }

        std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
        return {
            static_cast<int>(r.status_code),
            r.text,
            hdrs,
            r.url.str()
        };
    }

    inline bool download(
        const std::string& url,
        const std::string& output_path,
        std::initializer_list<std::pair<const std::string, std::string>> headers = {},
        ProgressCallback progress_cb = nullptr
    ) {
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            LOG_ERROR(std::format("Failed to create file: {}", output_path));
            return false;
        }

        cpr::ProgressCallback cpr_progress;
        if (progress_cb) {
            cpr_progress = cpr::ProgressCallback{
                [progress_cb](cpr::cpr_pf_arg_t downloadTotal, cpr::cpr_pf_arg_t downloadNow,
                             cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
                    progress_cb(static_cast<size_t>(downloadNow), static_cast<size_t>(downloadTotal));
                    return true;
                }
            };
        }

        cpr::Response r;
        if (progress_cb) {
            r = cpr::Download(
                file,
                cpr::Url{url},
                cpr::Header{headers},
                cpr_progress
            );
        } else {
            r = cpr::Download(
                file,
                cpr::Url{url},
                cpr::Header{headers}
            );
        }

        file.close();

        if (r.status_code != 200) {
            LOG_ERROR(std::format("Download failed: HTTP {}", r.status_code));
            std::filesystem::remove(output_path);
            return false;
        }

        if (r.error.code != cpr::ErrorCode::OK) {
            LOG_ERROR(std::format("Download error: {}", r.error.message));
            std::filesystem::remove(output_path);
            return false;
        }

        return true;
    }

    class DownloadSession {
    private:
        cpr::Session session_;

    public:
        DownloadSession() = default;

        void set_url(const std::string& url) {
            session_.SetUrl(cpr::Url{url});
        }

        void set_headers(std::initializer_list<std::pair<const std::string, std::string>> headers) {
            session_.SetHeader(cpr::Header{headers});
        }

        void set_follow_redirects(bool follow, int max_redirects = 10) {
            session_.SetRedirect(cpr::Redirect(follow ? max_redirects : 0L));
        }

        bool download_to_file(const std::string& output_path, ProgressCallback progress_cb = nullptr) {
            std::ofstream file(output_path, std::ios::binary);
            if (!file) {
                LOG_ERROR(std::format("Failed to create file: {}", output_path));
                return false;
            }

            if (progress_cb) {
                session_.SetProgressCallback(cpr::ProgressCallback{
                    [progress_cb](cpr::cpr_pf_arg_t downloadTotal, cpr::cpr_pf_arg_t downloadNow,
                                 cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
                        progress_cb(static_cast<size_t>(downloadNow), static_cast<size_t>(downloadTotal));
                        return true;
                    }
                });
            }

            cpr::Response r = session_.Download(file);
            file.close();

            if (r.status_code != 200) {
                LOG_ERROR(std::format("Download failed: HTTP {}", r.status_code));
                std::filesystem::remove(output_path);
                return false;
            }

            if (r.error.code != cpr::ErrorCode::OK) {
                LOG_ERROR(std::format("Download error: {}", r.error.message));
                std::filesystem::remove(output_path);
                return false;
            }

            return true;
        }
    };

    inline nlohmann::json decode(const Response& response) {
        if (response.text.empty()) {
            LOG_ERROR("Cannot decode empty response");
            return nlohmann::json::object();
        }

        if (const auto e = std::exception_ptr{}) {
            // No exception active
        }

        nlohmann::json result;
        try {
            result = nlohmann::json::parse(response.text);
        } catch (const std::exception& e) {
            LOG_ERROR(std::format("Failed to parse JSON response: {}", e.what()));
            return nlohmann::json::object();
        }

        return result;
    }
}