#include "net/http.hpp"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#include "core/log.hpp"

namespace hb {

namespace fs = std::filesystem;

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) {
        if (!out.empty()) out += '&';
        out += urlEncode(k) + "=" + urlEncode(v);
    }
    return out;
}

HttpResponse HttplibClient::send(const HttpRequest& req) {
    const std::string& url = req.url;
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) throw HttpError("Invalid URL: " + url);
    const auto pathStart = url.find('/', schemeEnd + 3);
    const std::string origin = url.substr(0, pathStart);
    const std::string path = pathStart == std::string::npos ? "/" : url.substr(pathStart);

    httplib::Client client(origin);
    client.set_connection_timeout(connectTimeout_);
    client.set_read_timeout(readTimeout_);
    client.set_follow_location(true);
    // Callers pass fully encoded URLs. Without this, httplib decodes and re-encodes the
    // query and turns %3B back into ';', which Tasmota treats as a command separator
    // (a Rule1 containing "Backlog a; b" was silently truncated at the ';').
    client.set_path_encode(false);
    client.set_default_headers({{"User-Agent", "heatbrain/0.1"}});

    httplib::Headers headers;
    for (const auto& [k, v] : req.headers) headers.emplace(k, v);

    httplib::Result res;
    if (req.method == "GET") {
        res = client.Get(path, headers);
    } else if (req.method == "POST") {
        res = client.Post(path, headers, req.body, req.contentType);
    } else {
        throw HttpError("Unsupported HTTP method " + req.method);
    }
    if (!res) {
        throw HttpError(std::format("{} {} failed: {}", req.method, origin, httplib::to_string(res.error())));
    }
    HttpResponse out{res->status, res->body, std::nullopt};
    if (res->has_header("Retry-After")) {
        int v = 0;
        const std::string h = res->get_header_value("Retry-After");
        if (std::from_chars(h.data(), h.data() + h.size(), v).ec == std::errc{}) out.retryAfterSeconds = v;
    }
    return out;
}

std::optional<int> retryAfter(const HttpResponse& res) {
    if (res.retryAfterSeconds) return res.retryAfterSeconds;
    static const std::regex pattern(R"(again in (\d+) second)");
    std::smatch m;
    if (std::regex_search(res.body, m, pattern)) return std::stoi(m[1].str());
    return std::nullopt;
}

CachingHttpClient::CachingHttpClient(HttpClient& inner, fs::path cacheDir)
    : inner_(inner), dir_(std::move(cacheDir)), sleep_([](std::chrono::seconds s) {
          std::this_thread::sleep_for(s);
      }) {
    fs::create_directories(dir_);
}

HttpResponse CachingHttpClient::fetchWithRetry(const std::string& url) {
    constexpr int kMaxAttempts = 3;
    constexpr int kMaxWaitSeconds = 120;
    for (int attempt = 1;; ++attempt) {
        HttpResponse res = inner_.get(url);
        if (res.status != 429 || attempt == kMaxAttempts) return res;
        const int wait = std::clamp(retryAfter(res).value_or(30), 1, kMaxWaitSeconds);
        log::info("Rate limited, retrying in {} s ({})", wait, url.substr(0, url.find('?')));
        sleep_(std::chrono::seconds{wait});
    }
}

fs::path CachingHttpClient::pathFor(const std::string& url) const {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a: stable across platforms and runs
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return dir_ / std::format("{:016x}.cache", h);
}

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFileAtomic(const fs::path& p, const std::string& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << data;
    }
    fs::rename(tmp, p);
}

}  // namespace

CachedBody CachingHttpClient::get(const std::string& url, std::chrono::seconds maxAge) {
    const fs::path file = pathFor(url);
    std::error_code ec;
    const bool cached = fs::exists(file, ec);
    if (cached) {
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, ec);
        if (!ec && age < maxAge) return {readFile(file), false};
    }

    try {
        log::debug("HTTP GET {}", url);
        // With an old copy to fall back on, don't block waiting out a rate limit.
        HttpResponse res = cached ? inner_.get(url) : fetchWithRetry(url);
        if (res.status < 200 || res.status >= 300) {
            throw HttpError(std::format("HTTP {} from {}: {}", res.status, url.substr(0, 60),
                                        res.body.substr(0, 200)));
        }
        writeFileAtomic(file, res.body);
        return {std::move(res.body), false};
    } catch (const std::exception& e) {
        if (!cached) throw;
        log::warn("{} - serving cached copy", e.what());
        return {readFile(file), true};
    }
}

std::string urlEncode(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

}  // namespace hb
