#include "http.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "console/console.h"

namespace HttpClient {

    nlohmann::json decode(const Response &response) {
        if (response.text.empty()) {
            LOG_ERROR("Cannot decode empty response");
            return nlohmann::json::object();
        }

        nlohmann::json result;
        try {
            result = nlohmann::json::parse(response.text);
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to parse JSON response: {}", e.what());
            return nlohmann::json::object();
        }

        return result;
    }

    std::string build_kv_string(std::initializer_list<std::pair<const std::string, std::string>> items, char sep) {
        std::ostringstream ss;
        bool first = true;
        for (const auto &kv: items) {
            if (!first) {
                ss << sep;
            }
            first = false;
            ss << kv.first << '=' << kv.second;
        }
        return ss.str();
    }

    namespace {
        Response get_impl(
            const std::string &url,
            const cpr::Header &hdr,
            const cpr::Parameters &params,
            bool follow_redirects,
            int max_redirects
        ) {
            auto r = cpr::Get(cpr::Url {url}, hdr, params, cpr::Redirect(follow_redirects ? max_redirects : 0L));

            std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());

            return {static_cast<int>(r.status_code), r.text, hdrs, r.url.str()};
        }
    } // namespace

    Response
    get(const std::string &url,
        std::initializer_list<std::pair<std::string, std::string>> headers,
        const cpr::Parameters &params,
        bool follow_redirects,
        int max_redirects) {
        cpr::Header hdr;

        for (const auto &[k, v]: headers) {
            hdr.emplace(k, v);
        }

        return get_impl(url, hdr, params, follow_redirects, max_redirects);
    }

    Response
    get(const std::string &url,
        std::span<const std::pair<std::string, std::string>> headers,
        const cpr::Parameters &params,
        bool follow_redirects,
        int max_redirects) {
        cpr::Header hdr;

        for (const auto &[k, v]: headers) {
            hdr.emplace(k, v);
        }

        return get_impl(url, hdr, params, follow_redirects, max_redirects);
    }

    BinaryResponse get_binary(
        const std::string &url,
        std::initializer_list<std::pair<const std::string, std::string>> headers,
        cpr::Parameters params,
        bool follow_redirects,
        int max_redirects
    ) {
        auto r = cpr::Get(
            cpr::Url {url},
            cpr::Header {headers},
            params,
            cpr::Redirect(follow_redirects ? max_redirects : 0L)
        );

        std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());

        std::vector<uint8_t> data(r.text.begin(), r.text.end());

        return {static_cast<int>(r.status_code), std::move(data), hdrs, r.url.str()};
    }

    Response post(
        const std::string &url,
        std::initializer_list<std::pair<const std::string, std::string>> headers,
        const std::string &jsonBody,
        std::initializer_list<std::pair<const std::string, std::string>> form,
        bool follow_redirects,
        int max_redirects
    ) {
        cpr::Header h {headers};
        cpr::Response r;

        if (!jsonBody.empty()) {
            h["Content-Type"] = "application/json";
            r = cpr::Post(
                cpr::Url {url},
                h,
                cpr::Body {jsonBody},
                cpr::Redirect(follow_redirects ? max_redirects : 0L)
            );
        } else if (form.size() > 0) {
            std::string body = build_kv_string(form);
            h["Content-Type"] = "application/x-www-form-urlencoded";
            r = cpr::Post(cpr::Url {url}, h, cpr::Body {body}, cpr::Redirect(follow_redirects ? max_redirects : 0L));
        } else {
            r = cpr::Post(cpr::Url {url}, h, cpr::Redirect(follow_redirects ? max_redirects : 0L));
        }

        std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
        return {static_cast<int>(r.status_code), r.text, hdrs, r.url.str()};
    }

    Response post(
        const std::string &url,
        std::span<const std::pair<std::string, std::string>> headers,
        const std::string &jsonBody,
        std::initializer_list<std::pair<const std::string, std::string>> form,
        bool follow_redirects,
        int max_redirects
    ) {
        cpr::Header h;
        for (const auto &[k, v]: headers) {
            h.emplace(k, v);
        }

        cpr::Response r;

        if (!jsonBody.empty()) {
            h["Content-Type"] = "application/json";
            r = cpr::Post(
                cpr::Url {url},
                h,
                cpr::Body {jsonBody},
                cpr::Redirect(follow_redirects ? max_redirects : 0L)
            );
        } else if (form.size() > 0) {
            std::string body = build_kv_string(form);
            h["Content-Type"] = "application/x-www-form-urlencoded";
            r = cpr::Post(cpr::Url {url}, h, cpr::Body {body}, cpr::Redirect(follow_redirects ? max_redirects : 0L));
        } else {
            r = cpr::Post(cpr::Url {url}, h, cpr::Redirect(follow_redirects ? max_redirects : 0L));
        }

        std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
        return {static_cast<int>(r.status_code), r.text, hdrs, r.url.str()};
    }

    bool download(
        const std::string &url,
        const std::string &output_path,
        std::initializer_list<std::pair<const std::string, std::string>> headers,
        ProgressCallback progress_cb
    ) {
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            LOG_ERROR("Failed to create file: {}", output_path);
            return false;
        }

        cpr::ProgressCallback cpr_progress;
        if (progress_cb) {
            cpr_progress = cpr::ProgressCallback {
                [progress_cb](
                    cpr::cpr_pf_arg_t downloadTotal,
                    cpr::cpr_pf_arg_t downloadNow,
                    cpr::cpr_pf_arg_t,
                    cpr::cpr_pf_arg_t,
                    intptr_t
                ) -> bool {
                    progress_cb(static_cast<size_t>(downloadNow), static_cast<size_t>(downloadTotal));
                    return true;
                }
            };
        }

        cpr::Response r;
        if (progress_cb) {
            r = cpr::Download(file, cpr::Url {url}, cpr::Header {headers}, cpr_progress);
        } else {
            r = cpr::Download(file, cpr::Url {url}, cpr::Header {headers});
        }

        file.close();

        if (r.status_code != 200) {
            LOG_ERROR("Download failed: HTTP {}", r.status_code);
            std::filesystem::remove(output_path);
            return false;
        }

        if (r.error.code != cpr::ErrorCode::OK) {
            LOG_ERROR("Download error: {}", r.error.message);
            std::filesystem::remove(output_path);
            return false;
        }

        return true;
    }

    StreamingDownloadResult download_streaming(
        const std::string &url,
        const std::string &output_path,
        std::vector<std::pair<std::string, std::string>> headers,
        size_t resumeOffset,
        ExtendedProgressCallback progress_cb,
        DownloadControl control
    ) {
        StreamingDownloadResult result;

        std::ios::openmode mode = std::ios::binary;
        if (resumeOffset > 0) {
            mode |= std::ios::app;
        }

        std::ofstream file(output_path, mode);
        if (!file) {
            result.error = std::format("Failed to open file: {}", output_path);
            LOG_ERROR("{}", result.error);
            return result;
        }

        cpr::Header cprHeaders;
        for (const auto &[k, v]: headers) {
            cprHeaders.emplace(k, v);
        }

        if (resumeOffset > 0) {
            cprHeaders.emplace("Range", std::format("bytes={}-", resumeOffset));
        }

        struct DownloadState {
                std::ofstream *file;
                std::atomic<bool> *shouldCancel;
                std::atomic<bool> *isPaused;
                size_t bandwidthLimit;
                size_t resumeOffset;
                size_t bytesWritten {0};
                size_t bytesThisSecond {0};
                size_t totalBytes {0};
                std::chrono::steady_clock::time_point startTime;
                std::chrono::steady_clock::time_point secondStart;
                std::chrono::steady_clock::time_point lastProgressReport;
                ExtendedProgressCallback progressCb;
                bool cancelled {false};
        };

        DownloadState state {
            .file = &file,
            .shouldCancel = control.shouldCancel,
            .isPaused = control.isPaused,
            .bandwidthLimit = control.bandwidthLimit,
            .resumeOffset = resumeOffset,
            .startTime = std::chrono::steady_clock::now(),
            .secondStart = std::chrono::steady_clock::now(),
            .lastProgressReport = std::chrono::steady_clock::now(),
            .progressCb = progress_cb
        };

        cpr::WriteCallback writeCallback {[&state](const std::string_view &data, intptr_t) -> bool {
            if (state.shouldCancel && state.shouldCancel->load()) {
                state.cancelled = true;
                return false;
            }

            while (state.isPaused && state.isPaused->load()) {
                if (state.shouldCancel && state.shouldCancel->load()) {
                    state.cancelled = true;
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (state.bandwidthLimit > 0) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsedMs
                    = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.secondStart).count();

                if (elapsedMs >= 1000) {
                    state.bytesThisSecond = 0;
                    state.secondStart = now;
                }

                if (state.bytesThisSecond + data.size() > state.bandwidthLimit) {
                    const auto sleepMs = 1000 - elapsedMs;
                    if (sleepMs > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                    }
                    state.bytesThisSecond = 0;
                    state.secondStart = std::chrono::steady_clock::now();
                }
                state.bytesThisSecond += data.size();
            }

            state.file->write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!state.file->good()) {
                return false;
            }

            state.bytesWritten += data.size();

            if (state.progressCb) {
                const auto now = std::chrono::steady_clock::now();
                const auto sinceLastReport
                    = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastProgressReport).count();

                if (sinceLastReport >= 100) {
                    const auto elapsedSecs
                        = std::chrono::duration_cast<std::chrono::seconds>(now - state.startTime).count();
                    const size_t bytesPerSecond
                        = elapsedSecs > 0 ? state.bytesWritten / static_cast<size_t>(elapsedSecs) : 0;

                    const size_t totalDownloaded = state.resumeOffset + state.bytesWritten;
                    state.progressCb(totalDownloaded, state.totalBytes, bytesPerSecond);
                    state.lastProgressReport = now;
                }
            }

            return true;
        }};

        cpr::ProgressCallback progressCallback {
            [&state](cpr::cpr_pf_arg_t downloadTotal, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t)
                -> bool {
                if (downloadTotal > 0 && state.totalBytes == 0) {
                    state.totalBytes = state.resumeOffset + static_cast<size_t>(downloadTotal);
                }
                return !(state.shouldCancel && state.shouldCancel->load());
            }
        };

        cpr::Response r = cpr::Get(cpr::Url {url}, cprHeaders, writeCallback, progressCallback, cpr::Redirect(10L));

        file.close();

        result.status_code = static_cast<int>(r.status_code);
        result.wasCancelled = state.cancelled;
        result.bytesDownloaded = state.resumeOffset + state.bytesWritten;

        if (state.totalBytes == 0) {
            if (auto it = r.header.find("Content-Range"); it != r.header.end()) {
                if (auto slashPos = it->second.rfind('/'); slashPos != std::string::npos) {
                    try {
                        result.totalBytes = std::stoull(it->second.substr(slashPos + 1));
                    } catch (...) {
                        result.totalBytes = result.bytesDownloaded;
                    }
                }
            } else if (auto it = r.header.find("Content-Length"); it != r.header.end()) {
                try {
                    result.totalBytes = resumeOffset + std::stoull(it->second);
                } catch (...) {
                    result.totalBytes = result.bytesDownloaded;
                }
            } else {
                result.totalBytes = result.bytesDownloaded;
            }
        } else {
            result.totalBytes = state.totalBytes;
        }

        if (r.error.code != cpr::ErrorCode::OK && !state.cancelled) {
            result.error = r.error.message;
            LOG_ERROR("Download error: {}", result.error);
        }

        if (result.status_code != 200 && result.status_code != 206 && !state.cancelled) {
            if (result.error.empty()) {
                result.error = std::format("HTTP error: {}", result.status_code);
            }
            LOG_ERROR("Download failed: HTTP {}", result.status_code);
        }

        return result;
    }

    class DownloadSession::Impl {
        public:
            cpr::Session session;
    };

    DownloadSession::DownloadSession() : m_impl(std::make_unique<Impl>()) {
    }

    DownloadSession::~DownloadSession() = default;

    DownloadSession::DownloadSession(DownloadSession &&) noexcept = default;
    DownloadSession &DownloadSession::operator=(DownloadSession &&) noexcept = default;

    void DownloadSession::set_url(const std::string &url) {
        m_impl->session.SetUrl(cpr::Url {url});
    }

    void DownloadSession::set_headers(std::initializer_list<std::pair<const std::string, std::string>> headers) {
        m_impl->session.SetHeader(cpr::Header {headers});
    }

    void DownloadSession::set_follow_redirects(bool follow, int max_redirects) {
        m_impl->session.SetRedirect(cpr::Redirect(follow ? max_redirects : 0L));
    }

    bool DownloadSession::download_to_file(const std::string &output_path, ProgressCallback progress_cb) {
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            LOG_ERROR("Failed to create file: {}", output_path);
            return false;
        }

        if (progress_cb) {
            m_impl->session.SetProgressCallback(
                cpr::ProgressCallback {
                    [progress_cb](
                        cpr::cpr_pf_arg_t downloadTotal,
                        cpr::cpr_pf_arg_t downloadNow,
                        cpr::cpr_pf_arg_t,
                        cpr::cpr_pf_arg_t,
                        intptr_t
                    ) -> bool {
                        progress_cb(static_cast<size_t>(downloadNow), static_cast<size_t>(downloadTotal));
                        return true;
                    }
                }
            );
        }

        cpr::Response r = m_impl->session.Download(file);
        file.close();

        if (r.status_code != 200) {
            LOG_ERROR("Download failed: HTTP {}", r.status_code);
            std::filesystem::remove(output_path);
            return false;
        }

        if (r.error.code != cpr::ErrorCode::OK) {
            LOG_ERROR("Download error: {}", r.error.message);
            std::filesystem::remove(output_path);
            return false;
        }

        return true;
    }

    RateLimiter::RateLimiter() {
        m_maxRequests = 30;
        m_windowSize = std::chrono::milliseconds(1000);
    }

    RateLimiter &RateLimiter::instance() {
        static RateLimiter instance;
        return instance;
    }

    void RateLimiter::configure(int maxRequests, Duration windowSize) {
        std::lock_guard lock(m_mutex);
        m_maxRequests = maxRequests;
        m_windowSize = windowSize;
    }

    void RateLimiter::pruneOldRequests() const {
        auto now = Clock::now();
        auto cutoff = now - m_windowSize;

        while (!m_requestTimestamps.empty() && m_requestTimestamps.front() < cutoff) {
            m_requestTimestamps.pop_front();
        }
    }

    void RateLimiter::acquire() {
        std::unique_lock lock(m_mutex);

        while (true) {
            auto now = Clock::now();

            if (now < m_backoffUntil) {
                auto waitTime = m_backoffUntil - now;
                LOG_INFO("Rate limiter: backing off for {}ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(waitTime).count());
                m_cv.wait_for(lock, waitTime);
                continue;
            }

            pruneOldRequests();

            if (static_cast<int>(m_requestTimestamps.size()) < m_maxRequests) {
                m_requestTimestamps.push_back(now);
                return;
            }

            auto oldestExpiry = m_requestTimestamps.front() + m_windowSize;
            auto waitTime = oldestExpiry - now;

            if (waitTime > std::chrono::milliseconds(0)) {
                m_cv.wait_for(lock, waitTime);
            }
        }
    }

    bool RateLimiter::tryAcquire() {
        std::lock_guard lock(m_mutex);

        auto now = Clock::now();

        if (now < m_backoffUntil) {
            return false;
        }

        pruneOldRequests();

        if (static_cast<int>(m_requestTimestamps.size()) < m_maxRequests) {
            m_requestTimestamps.push_back(now);
            return true;
        }

        return false;
    }

    int RateLimiter::available() const {
        std::lock_guard lock(m_mutex);
        pruneOldRequests();
        return std::max(0, m_maxRequests - static_cast<int>(m_requestTimestamps.size()));
    }

    void RateLimiter::backoff(Duration duration) {
        std::lock_guard lock(m_mutex);
        auto newBackoff = Clock::now() + duration;

        if (newBackoff > m_backoffUntil) {
            m_backoffUntil = newBackoff;
            LOG_WARN("Rate limiter: 429 received, backing off for {}ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
        }

        m_cv.notify_all();
    }

    Response rateLimitedGet(
        const std::string &url,
        const std::vector<std::pair<std::string, std::string>> &headers
    ) {
        return rateLimitedRequest([&]() {
            return HttpClient::get(url, headers);
        });
    }

    Response rateLimitedPost(
        const std::string &url,
        const std::vector<std::pair<std::string, std::string>> &headers,
        const std::string &body
    ) {
        return rateLimitedRequest([&]() {
            return HttpClient::post(url, headers, body);
        });
    }

} // namespace HttpClient
