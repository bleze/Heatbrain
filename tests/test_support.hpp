#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "net/http.hpp"

namespace hb::test {

inline std::string readFixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(HEATBRAIN_TEST_FIXTURES) / name, std::ios::binary);
    if (!in) throw std::runtime_error("missing fixture " + name);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A unique temporary directory, removed on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() / ("heatbrain-test-" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class FakeHttp final : public HttpClient {
public:
    std::function<HttpResponse(const std::string&)> handler;           // by URL
    std::function<HttpResponse(const HttpRequest&)> requestHandler;    // full request, wins if set
    int calls = 0;
    std::string lastUrl;
    std::vector<HttpRequest> requests;

    HttpResponse send(const HttpRequest& req) override {
        ++calls;
        lastUrl = req.url;
        requests.push_back(req);
        return requestHandler ? requestHandler(req) : handler(req.url);
    }
};

}  // namespace hb::test
