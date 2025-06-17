#pragma once

#include <string>
#include <map>
#include <initializer_list>
#include <sstream>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include "logging.hpp"

using namespace std;

namespace HttpClient {
    struct Response {
        int status_code;
        string text;
        map<string, string> headers;
    };

    inline string build_kv_string(
        initializer_list<pair<const string, string> > items,
        char sep = '&'
    ) {
        ostringstream ss;
        bool first = true;
        for (auto &kv: items) {
            if (!first) ss << sep;
            first = false;
            ss << kv.first << '=' << kv.second;
        }
        return ss.str();
    }

    inline Response get(
        const std::string &url,
        std::initializer_list<std::pair<const std::string, std::string> > headers = {},
        cpr::Parameters params = {}
    ) {
        auto r = cpr::Get(
            cpr::Url{url},
            cpr::Header{headers},
            params // <-- directly pass it
        );
        std::map<std::string, std::string> hdrs(r.header.begin(), r.header.end());
        return {r.status_code, r.text, hdrs};
    }

    inline Response post(
        const string &url,
        initializer_list<pair<const string, string> > headers = {},
        const string &jsonBody = string(),
        initializer_list<pair<const string, string> > form = {}
    ) {
        cpr::Header h{headers};
        cpr::Response r;
        if (!jsonBody.empty()) {
            h["Content-Type"] = "application/json";
            r = cpr::Post(
                cpr::Url{url},
                h,
                cpr::Body{jsonBody}
            );
        } else if (form.size() > 0) {
            string body = build_kv_string(form);
            h["Content-Type"] = "application/x-www-form-urlencoded";
            r = cpr::Post(
                cpr::Url{url},
                h,
                cpr::Body{body}
            );
        } else {
            r = cpr::Post(
                cpr::Url{url},
                h
            );
        }
        map<string, string> hdrs(r.header.begin(), r.header.end());
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
}
