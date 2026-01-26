#pragma once

#include <atomic>
#include <functional>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

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
    using ExtendedProgressCallback = std::function<void(size_t downloaded, size_t total, size_t bytesPerSecond)>;

    struct DownloadControl {
            std::atomic<bool> *shouldCancel {nullptr};
            std::atomic<bool> *isPaused {nullptr};
            size_t bandwidthLimit {0}; // bytes per second, 0 = unlimited
    };

    struct StreamingDownloadResult {
            int status_code {0};
            size_t bytesDownloaded {0};
            size_t totalBytes {0};
            bool wasCancelled {false};
            std::string error;
    };

    nlohmann::json decode(const Response &response);

    std::string build_kv_string(std::initializer_list<std::pair<const std::string, std::string>> items, char sep = '&');

    Response
    get(const std::string &url,
        std::initializer_list<std::pair<std::string, std::string>> headers = {},
        const cpr::Parameters &params = {},
        bool follow_redirects = true,
        int max_redirects = 10);

    Response
    get(const std::string &url,
        std::span<const std::pair<std::string, std::string>> headers,
        const cpr::Parameters &params = {},
        bool follow_redirects = true,
        int max_redirects = 10);

    Response rateLimitedGet(
        const std::string &url,
        const std::vector<std::pair<std::string, std::string>> &headers = {}
    );

    BinaryResponse get_binary(
        const std::string &url,
        std::initializer_list<std::pair<const std::string, std::string>> headers = {},
        cpr::Parameters params = {},
        bool follow_redirects = true,
        int max_redirects = 10
    );

    Response post(
        const std::string &url,
        std::initializer_list<std::pair<const std::string, std::string>> headers = {},
        const std::string &jsonBody = std::string(),
        std::initializer_list<std::pair<const std::string, std::string>> form = {},
        bool follow_redirects = true,
        int max_redirects = 10
    );

    Response post(
        const std::string &url,
        std::span<const std::pair<std::string, std::string>> headers,
        const std::string &jsonBody = std::string(),
        std::initializer_list<std::pair<const std::string, std::string>> form = {},
        bool follow_redirects = true,
        int max_redirects = 10
    );

    Response rateLimitedPost(
        const std::string &url,
        const std::vector<std::pair<std::string, std::string>> &headers,
        const std::string &body = {}
    );

    bool download(
        const std::string &url,
        const std::string &output_path,
        std::initializer_list<std::pair<const std::string, std::string>> headers = {},
        ProgressCallback progress_cb = nullptr
    );

    StreamingDownloadResult download_streaming(
        const std::string &url,
        const std::string &output_path,
        std::vector<std::pair<std::string, std::string>> headers = {},
        size_t resumeOffset = 0,
        ExtendedProgressCallback progress_cb = nullptr,
        DownloadControl control = {}
    );

    class DownloadSession {
        private:
            class Impl;
            std::unique_ptr<Impl> m_impl;

        public:
            DownloadSession();
            ~DownloadSession();

            DownloadSession(const DownloadSession &) = delete;
            DownloadSession &operator=(const DownloadSession &) = delete;
            DownloadSession(DownloadSession &&) noexcept;
            DownloadSession &operator=(DownloadSession &&) noexcept;

            void set_url(const std::string &url);
            void set_headers(std::initializer_list<std::pair<const std::string, std::string>> headers);
            void set_follow_redirects(bool follow, int max_redirects = 10);
            bool download_to_file(const std::string &output_path, ProgressCallback progress_cb = nullptr);
    };

    class RateLimiter {
        public:
            using Clock = std::chrono::steady_clock;
            using Duration = std::chrono::milliseconds;

            static RateLimiter &instance();

            void configure(int maxRequests, Duration windowSize);

            void acquire();

            bool tryAcquire();

            int available() const;

            int maxRequests() const { return m_maxRequests; }
            Duration windowSize() const { return m_windowSize; }

            // Temporarily back off after receiving a 429
            // Adds extra delay before next request
            void backoff(Duration duration = std::chrono::seconds(2));

        private:
            RateLimiter();

            void pruneOldRequests() const;

            mutable std::mutex m_mutex;
            std::condition_variable m_cv;

            int m_maxRequests = 50;
            Duration m_windowSize = std::chrono::milliseconds(1000);

            mutable std::deque<Clock::time_point> m_requestTimestamps;
            Clock::time_point m_backoffUntil = Clock::time_point::min();
    };

    // Wraps an HTTP request with rate limiting and retry logic for 429s
    template <typename Func>
    auto rateLimitedRequest(Func &&requestFunc, int maxRetries = 3) -> decltype(requestFunc()) {
        auto &limiter = RateLimiter::instance();

        for (int attempt = 0; attempt <= maxRetries; ++attempt) {
            limiter.acquire();

            auto response = requestFunc();

            // Check if response indicates rate limiting
            if constexpr (std::is_same_v<decltype(response), Response>) {
                if (response.status_code == 429) {
                    if (attempt < maxRetries) {
                        // Exponential backoff: 1s, 2s, 4s
                        auto delay = std::chrono::seconds(1 << attempt);
                        limiter.backoff(std::chrono::duration_cast<RateLimiter::Duration>(delay));
                        continue;
                    }
                }
            }

            return response;
        }

        // Should not reach here, but return last attempt
        return requestFunc();
    }

} // namespace HttpClient
