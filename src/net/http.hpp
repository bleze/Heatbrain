#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hb {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::optional<int> retryAfterSeconds;  // from a Retry-After header, if any
};

class HttpError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct HttpRequest {
    std::string method = "GET";  // GET or POST
    std::string url;              // fully encoded
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string contentType;
};

// Minimal HTTP client interface so providers can be tested with fakes.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    // Throws HttpError on transport failure. Non-2xx statuses are returned, not thrown.
    virtual HttpResponse send(const HttpRequest& req) = 0;

    HttpResponse get(const std::string& url) { return send(HttpRequest{"GET", url, {}, {}, {}}); }
};

// application/x-www-form-urlencoded body from key/value pairs.
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields);

// Real client (cpp-httplib + OpenSSL).
// Stateless and thread-safe: each request uses its own connection.
class HttplibClient final : public HttpClient {
public:
    explicit HttplibClient(std::chrono::milliseconds connectTimeout = std::chrono::seconds{10},
                           std::chrono::milliseconds readTimeout = std::chrono::seconds{30})
        : connectTimeout_(connectTimeout), readTimeout_(readTimeout) {}

    HttpResponse send(const HttpRequest& req) override;

private:
    std::chrono::milliseconds connectTimeout_;
    std::chrono::milliseconds readTimeout_;
};

struct CachedBody {
    std::string body;
    bool stale = false;  // true when the fetch failed and an older copy was served
};

// File-backed cache in front of an HttpClient. Public data APIs are rate limited
// (Energi Data Service allows only a few requests per minute), so every provider
// goes through this. On fetch failure an expired copy is served if one exists.
// HTTP 429 without any cached copy: wait as long as the server asks (up to a limit) and retry.
class CachingHttpClient {
public:
    using Sleeper = std::function<void(std::chrono::seconds)>;

    CachingHttpClient(HttpClient& inner, std::filesystem::path cacheDir);

    CachedBody get(const std::string& url, std::chrono::seconds maxAge);

    // For tests: replace the real sleep.
    void setSleeper(Sleeper s) { sleep_ = std::move(s); }

private:
    std::filesystem::path pathFor(const std::string& url) const;
    HttpResponse fetchWithRetry(const std::string& url);

    HttpClient& inner_;
    std::filesystem::path dir_;
    Sleeper sleep_;
};

// Seconds to wait from a 429 response: Retry-After header, or Energi Data Service's
// "Try again in N seconds" message.
std::optional<int> retryAfter(const HttpResponse& res);

std::string urlEncode(std::string_view s);

}  // namespace hb
