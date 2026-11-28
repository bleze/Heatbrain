#include "app/cameras.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <future>
#include <set>
#include <stdexcept>

#include "core/base64.hpp"
#include "core/log.hpp"

namespace hb {

using json = nlohmann::json;
using namespace std::chrono;

std::vector<CameraConfig> parseCameras(const json& cfg) {
    std::vector<CameraConfig> out;
    if (!cfg.contains("cameras") || cfg["cameras"].is_null()) return out;
    if (!cfg["cameras"].is_array()) throw std::runtime_error("cameras must be a list");
    std::set<std::string> ids;
    for (const auto& cj : cfg["cameras"]) {
        CameraConfig c;
        c.id = cj.value("id", "");
        if (c.id.empty() || !ids.insert(c.id).second) throw std::runtime_error("camera id missing or duplicated: '" + c.id + "'");
        c.name = cj.value("name", "");
        c.host = cj.value("host", "");
        const std::string label = c.name.empty() ? c.id : c.name;
        if (c.host.empty()) throw std::runtime_error("camera " + label + ": address missing");
        if (c.host.find_first_of("/ ?#@") != std::string::npos) {
            throw std::runtime_error("camera " + label + ": address must be an IP or host name (and :port), not a URL");
        }
        c.user = cj.value("user", "");
        c.password = cj.value("password", "");
        out.push_back(std::move(c));
    }
    return out;
}

std::string snapshotUrl(const CameraConfig& c) { return "http://" + c.host + "/cgi-bin/viewer/video.jpg"; }

namespace {
// "name='value'" lines, as VIVOTEK's getparam.cgi answers.
std::map<std::string, std::string> params(const std::string& body) {
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t end = std::min(body.find('\n', pos), body.size());
        const std::string line = body.substr(pos, end - pos);
        const auto eq = line.find("='");
        const auto close = line.rfind('\'');
        if (eq != std::string::npos && close != std::string::npos && close > eq + 1) out[line.substr(0, eq)] = line.substr(eq + 2, close - eq - 2);
        pos = end + 1;
    }
    return out;
}
}  // namespace

std::vector<DiscoveredCamera> discoverCameras(std::string_view subnet, HttpClient& http) {
    std::string prefix(subnet.substr(0, subnet.find('/')));
    const auto dots = std::ranges::count(prefix, '.');
    if (dots == 3) prefix = prefix.substr(0, prefix.rfind('.'));
    else if (dots != 2) throw std::runtime_error("Subnet must look like 192.168.1.0/24");
    if (subnet.find('/') != std::string_view::npos && !subnet.ends_with("/24")) throw std::runtime_error("Only /24 networks are supported");

    const auto probe = [&](int i) -> std::optional<DiscoveredCamera> {
        DiscoveredCamera d;
        d.host = std::format("{}.{}", prefix, i);
        try {
            const auto r = http.get("http://" + d.host + "/cgi-bin/viewer/getparam.cgi?system_info_modelname&system_info_firmwareversion&system_hostname");
            if (r.status == 401) {
                // VIVOTEK's refusal names the page that was asked for; other devices' do not.
                if (r.body.find("/cgi-bin/viewer/getparam.cgi") == std::string::npos) return std::nullopt;
                d.needsLogin = true;
                return d;
            }
            if (r.status != 200) return std::nullopt;
            const auto p = params(r.body);
            if (!p.contains("system_info_modelname")) return std::nullopt;
            d.model = p.at("system_info_modelname");
            d.firmware = p.contains("system_info_firmwareversion") ? p.at("system_info_firmwareversion") : "";
            d.hostName = p.contains("system_hostname") ? p.at("system_hostname") : "";
            const auto admin = http.get("http://" + d.host + "/cgi-bin/admin/getparam.cgi?system_hostname");
            d.openAdmin = admin.status == 200;
            return d;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    std::vector<DiscoveredCamera> found;
    constexpr int kBatch = 64;
    for (int start = 1; start <= 254; start += kBatch) {
        std::vector<std::future<std::optional<DiscoveredCamera>>> batch;
        for (int i = start; i < start + kBatch && i <= 254; ++i) batch.push_back(std::async(std::launch::async, probe, i));
        for (auto& f : batch) {
            if (auto d = f.get()) found.push_back(std::move(*d));
        }
    }
    return found;
}

CameraHub::CameraHub(HttpClient& http, Clock clock) : http_(http), clock_(std::move(clock)) {}

CameraHub& CameraHub::instance() {
    // Short timeouts: a camera that does not answer must not hold a dashboard request for long.
    static HttplibClient lan{seconds{3}, seconds{6}};
    static CameraHub hub(lan, [] { return floor<seconds>(system_clock::now()); });
    return hub;
}

void CameraHub::configure(std::vector<CameraConfig> cameras) {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Entry>> next;
    for (auto& c : cameras) {
        // The same camera (address and login): its last picture and state stay.
        const auto old = std::ranges::find_if(entries_, [&](const auto& e) {
            return e->cfg.id == c.id && e->cfg.host == c.host && e->cfg.user == c.user && e->cfg.password == c.password;
        });
        if (old != entries_.end()) {
            (*old)->cfg.name = c.name;  // the rest is the same
            next.push_back(*old);
        } else {
            auto e = std::make_shared<Entry>();
            e->cfg = std::move(c);
            next.push_back(std::move(e));
        }
    }
    entries_ = std::move(next);
}

std::vector<CameraConfig> CameraHub::cameras() const {
    std::lock_guard lock(mutex_);
    std::vector<CameraConfig> out;
    for (const auto& e : entries_) {
        out.push_back(e->cfg);
        out.back().name = e->shownName();
    }
    return out;
}

std::string CameraHub::Entry::shownName() const {
    if (!cfg.name.empty()) return cfg.name;
    if (!ownName.empty()) return ownName;
    return cfg.id;
}

namespace {
// The name a camera leaves at its factory default says nothing ("Network Camera").
bool defaultName(std::string n) {
    std::ranges::transform(n, n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return n.empty() || n.starts_with("network camera") || n.starts_with("networkcamera") || n.starts_with("ip cam") ||
           n.starts_with("vivotek");
}
}  // namespace

// The camera's own name, model and firmware (with the login, which the scan did not have).
void CameraHub::askInfo(Entry& e, Instant now) {
    e.infoAt = now;
    HttpRequest req{"GET", "http://" + e.cfg.host + "/cgi-bin/viewer/getparam.cgi?system_hostname&system_info_modelname&system_info_firmwareversion",
                    {}, {}, {}};
    if (!e.cfg.user.empty()) req.headers.emplace_back("Authorization", "Basic " + base64Encode(e.cfg.user + ":" + e.cfg.password));
    try {
        const auto r = http_.send(req);
        if (r.status != 200) return;
        const auto p = params(r.body);
        std::lock_guard lock(mutex_);
        if (const auto it = p.find("system_hostname"); it != p.end()) e.ownName = defaultName(it->second) ? "" : it->second;
        if (const auto it = p.find("system_info_modelname"); it != p.end()) e.model = it->second;
        if (const auto it = p.find("system_info_firmwareversion"); it != p.end()) e.firmware = it->second;
    } catch (const std::exception&) {
        // not important: asked again later
    }
}

std::shared_ptr<CameraHub::Entry> CameraHub::entry(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_) {
        if (e->cfg.id == id) return e;
    }
    throw std::out_of_range("unknown camera '" + id + "'");
}

CameraPicture CameraHub::picture(const std::string& id, milliseconds maxAge) {
    const auto e = entry(id);
    std::lock_guard fetching(e->fetch);  // a second viewer waits, then gets the same picture
    const Instant now = clock_();
    if (e->last && now - e->last->at < maxAge) return *e->last;

    HttpRequest req{"GET", snapshotUrl(e->cfg), {}, {}, {}};
    if (!e->cfg.user.empty()) req.headers.emplace_back("Authorization", "Basic " + base64Encode(e->cfg.user + ":" + e->cfg.password));
    std::string error;
    try {
        HttpResponse r = http_.send(req);
        if (r.status == 401) error = e->cfg.user.empty() ? "asks for a login: set user and password" : "login refused: check user and password";
        else if (r.status != 200) error = std::format("answered HTTP {}", r.status);
        else if (r.body.size() < 4 || static_cast<unsigned char>(r.body[0]) != 0xFF || static_cast<unsigned char>(r.body[1]) != 0xD8) {
            error = "did not send a picture";
        } else {
            CameraPicture p{std::make_shared<const std::string>(std::move(r.body)), now};
            {
                std::lock_guard lock(mutex_);
                e->last = p;
                e->error.clear();
            }
            if (!e->infoAt || now - *e->infoAt >= hours{6}) askInfo(*e, now);
            return p;
        }
    } catch (const std::exception& ex) {
        error = std::string("not answering (") + ex.what() + ")";
    }
    {
        std::lock_guard lock(mutex_);
        if (e->error != error) log::warn("Camera {}: {}", e->shownName(), error);
        e->error = error;
    }
    throw std::runtime_error(error);
}

std::vector<CameraStatus> CameraHub::status() const {
    std::lock_guard lock(mutex_);
    std::vector<CameraStatus> out;
    for (const auto& e : entries_) {
        CameraStatus s{e->cfg.id, e->shownName(), e->ownName, e->model, e->firmware, e->last.has_value() && e->error.empty(),
                       e->error, std::nullopt};
        if (e->last) s.lastPicture = e->last->at;
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace hb
