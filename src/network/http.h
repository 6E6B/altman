#pragma once

#include <string>
#include <map>
#include <vector>
#include <initializer_list>
#include <functional>
#include <span>
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

std::string build_kv_string(
    std::initializer_list<std::pair<const std::string, std::string>> items,
    char sep = '&'
);

Response get(
    const std::string& url,
    std::initializer_list<std::pair<std::string, std::string>> headers = {},
    const cpr::Parameters& params = {},
    bool follow_redirects = true,
    int max_redirects = 10
);

Response get(
    const std::string& url,
    std::span<const std::pair<std::string, std::string>> headers,
    const cpr::Parameters& params = {},
    bool follow_redirects = true,
    int max_redirects = 10
);

BinaryResponse get_binary(
    const std::string& url,
    std::initializer_list<std::pair<const std::string, std::string>> headers = {},
    cpr::Parameters params = {},
    bool follow_redirects = true,
    int max_redirects = 10
);

Response post(
    const std::string& url,
    std::initializer_list<std::pair<const std::string, std::string>> headers = {},
    const std::string& jsonBody = std::string(),
    std::initializer_list<std::pair<const std::string, std::string>> form = {},
    bool follow_redirects = true,
    int max_redirects = 10
);

bool download(
    const std::string& url,
    const std::string& output_path,
    std::initializer_list<std::pair<const std::string, std::string>> headers = {},
    ProgressCallback progress_cb = nullptr
);

class DownloadSession {
private:
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    DownloadSession();
    ~DownloadSession();

    DownloadSession(const DownloadSession&) = delete;
    DownloadSession& operator=(const DownloadSession&) = delete;
    DownloadSession(DownloadSession&&) noexcept;
    DownloadSession& operator=(DownloadSession&&) noexcept;

    void set_url(const std::string& url);
    void set_headers(std::initializer_list<std::pair<const std::string, std::string>> headers);
    void set_follow_redirects(bool follow, int max_redirects = 10);
    bool download_to_file(const std::string& output_path, ProgressCallback progress_cb = nullptr);
};

nlohmann::json decode(const Response& response);

}