#include "sensors/shelly.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <future>
#include <stdexcept>

#include "core/log.hpp"

namespace hb {

using json = nlohmann::json;

namespace {

HttpRequest rpcRequest(std::string_view host, std::string_view method, const json& params) {
    HttpRequest r;
    r.method = "POST";
    r.url = std::format("http://{}/rpc", host);
    r.contentType = "application/json";
    r.body = json{{"id", 1}, {"method", method}, {"params", params}}.dump();
    return r;
}

// One RPC call; returns its "result". Throws with a readable message.
json rpc(HttpClient& http, std::string_view host, std::string_view method, const json& params = json::object()) {
    HttpResponse res;
    try {
        res = http.send(rpcRequest(host, method, params));
    } catch (const HttpError&) {
        throw std::runtime_error(std::format(
            "The Shelly at {} does not answer. A battery Shelly sleeps: press its button once to wake it and try again.", host));
    }
    if (res.status == 401) {
        throw std::runtime_error(std::format(
            "The Shelly at {} is password protected. Turn off its password (on its web page, http://{}) and try again.", host, host));
    }
    json j;
    try {
        j = json::parse(res.body);
    } catch (const std::exception&) {
        throw std::runtime_error(std::format("The Shelly at {} answered {} with HTTP {}", host, method, res.status));
    }
    if (j.contains("error")) {
        throw std::runtime_error(std::format("The Shelly at {} refused {}: {}", host, method,
                                             j["error"].value("message", j["error"].dump())));
    }
    if (res.status != 200 || !j.contains("result")) {
        throw std::runtime_error(std::format("The Shelly at {} answered {} with HTTP {}", host, method, res.status));
    }
    return j["result"];
}

std::string str(const json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

std::string slugOf(std::string_view s) {
    std::string out;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
        else if (ok) out += c;
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.substr(0, 40);
}

bool isHeatbrainUrl(const json& u) {
    return u.is_string() && u.get<std::string>().find("/api/push/shelly") != std::string::npos;
}

// A query parameter of a heatbrain push URL, decoded ("%20" and "+").
std::string queryParam(std::string_view url, std::string_view key) {
    const auto q = url.find('?');
    if (q == std::string_view::npos) return {};
    std::string_view rest = url.substr(q + 1);
    while (!rest.empty()) {
        const auto amp = rest.find('&');
        const std::string_view kv = rest.substr(0, amp);
        const auto eq = kv.find('=');
        if (eq != std::string_view::npos && kv.substr(0, eq) == key) {
            std::string out;
            const std::string_view v = kv.substr(eq + 1);
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '+') {
                    out += ' ';
                } else if (v[i] == '%' && i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1])) &&
                           std::isxdigit(static_cast<unsigned char>(v[i + 2]))) {
                    out += static_cast<char>(std::stoi(std::string(v.substr(i + 1, 2)), nullptr, 16));
                    i += 2;
                } else {
                    out += v[i];
                }
            }
            return out;
        }
        if (amp == std::string_view::npos) break;
        rest = rest.substr(amp + 1);
    }
    return {};
}

}  // namespace

void applyShellyHooks(DiscoveredShelly& d, std::string_view body) {
    try {
        const json j = json::parse(body);
        const json& r = j.contains("result") ? j["result"] : j;
        if (!r.contains("hooks")) return;
        for (const auto& h : r["hooks"]) {
            if (!h.contains("urls")) continue;
            for (const auto& u : h["urls"]) {
                if (!isHeatbrainUrl(u)) continue;
                const std::string url = u.get<std::string>();
                if (d.pushId.empty()) d.pushId = queryParam(url, "id");
                if (d.pushName.empty()) d.pushName = queryParam(url, "name");
            }
        }
    } catch (const std::exception&) {
        // not readable: treated as not set up
    }
}

std::optional<DiscoveredShelly> parseShellyInfo(std::string_view host, std::string_view body) {
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!j.is_object()) return std::nullopt;
    DiscoveredShelly d;
    d.host = std::string(host);
    if (j.contains("gen") && j["gen"].is_number_integer()) {
        // Gen2+: {"id":"shellyhtg3-...","mac":"...","model":"S3SN-0U12A","gen":3,"fw_id":"...","ver":"1.4.2","app":"HTG3","auth_en":false,"name":null}
        if (!j.contains("id") || !j.contains("model")) return std::nullopt;
        d.gen = j["gen"].get<int>();
        d.id = str(j, "id");
        d.model = str(j, "model");
        d.app = str(j, "app");
        d.name = str(j, "name");
        d.firmware = str(j, "ver");
        d.auth = j.value("auth_en", false);
    } else if (j.contains("type") && j.contains("mac")) {
        // Gen1: {"type":"SHHT-1","mac":"...","auth":false,"fw":"..."}
        d.gen = 1;
        d.model = str(j, "type");
        d.id = "shelly-" + str(j, "mac");
        d.firmware = str(j, "fw");
        d.auth = j.value("auth", false);
    } else {
        return std::nullopt;
    }
    return d;
}

void applyShellyStatus(DiscoveredShelly& d, std::string_view body) {
    try {
        const json j = json::parse(body);
        const json& s = j.contains("result") ? j["result"] : j;
        if (s.contains("temperature:0")) {
            d.sensor = true;
            const auto& t = s["temperature:0"];
            if (t.contains("tC") && t["tC"].is_number()) d.temperature = t["tC"].get<double>();
        }
        if (s.contains("humidity:0")) {
            const auto& h = s["humidity:0"];
            if (h.contains("rh") && h["rh"].is_number()) d.humidity = h["rh"].get<double>();
        }
        if (s.contains("devicepower:0") && s["devicepower:0"].contains("battery")) {
            const auto& b = s["devicepower:0"]["battery"];
            if (b.contains("percent") && b["percent"].is_number()) d.battery = b["percent"].get<int>();
        }
    } catch (const std::exception&) {
        // no readings, still a Shelly
    }
}

std::vector<DiscoveredShelly> discoverShelly(std::string_view subnet, HttpClient& http) {
    // Accept "a.b.c", "a.b.c.0/24" or "a.b.c.x".
    std::string prefix(subnet.substr(0, subnet.find('/')));
    const int dots = static_cast<int>(std::ranges::count(prefix, '.'));
    if (dots == 3) prefix = prefix.substr(0, prefix.rfind('.'));
    else if (dots != 2) throw std::runtime_error("Subnet must look like 192.168.1.0/24");
    if (subnet.find('/') != std::string_view::npos && !subnet.ends_with("/24")) {
        throw std::runtime_error("Only /24 networks are supported");
    }

    auto probe = [&http, &prefix](int i) -> std::optional<DiscoveredShelly> {
        const std::string host = std::format("{}.{}", prefix, i);
        try {
            const auto res = http.get("http://" + host + "/shelly");
            if (res.status != 200) return std::nullopt;
            auto d = parseShellyInfo(host, res.body);
            if (!d) return std::nullopt;
            if (d->gen >= 2 && !d->auth) {
                try {
                    const auto st = http.get("http://" + host + "/rpc/Shelly.GetStatus");
                    if (st.status == 200) applyShellyStatus(*d, st.body);
                    const auto hooks = http.get("http://" + host + "/rpc/Webhook.List");
                    if (hooks.status == 200) applyShellyHooks(*d, hooks.body);
                } catch (const std::exception&) {
                    // gone back to sleep: listed without readings
                }
            }
            return d;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    std::vector<DiscoveredShelly> found;
    constexpr int kBatch = 64;
    for (int start = 1; start <= 254; start += kBatch) {
        std::vector<std::future<std::optional<DiscoveredShelly>>> batch;
        for (int i = start; i < start + kBatch && i <= 254; ++i) {
            batch.push_back(std::async(std::launch::async, probe, i));
        }
        for (auto& f : batch) {
            if (auto d = f.get()) found.push_back(std::move(*d));
        }
    }
    return found;
}

std::string shellyPushUrl(std::string_view base, std::string_view id, std::string_view name, std::string_view suffix) {
    std::string b(base);
    while (!b.empty() && b.back() == '/') b.pop_back();
    return std::format("{}/api/push/shelly?id={}&name={}{}", b, urlEncode(id), urlEncode(name), suffix);
}

ShellySetupResult setupShelly(HttpClient& http, std::string_view host, std::string_view base, std::string_view name) {
    if (!(base.starts_with("http://") || base.starts_with("https://"))) {
        throw std::runtime_error("heatbrain's address must start with http:// or https://");
    }
    HttpResponse info;
    try {
        info = http.get(std::format("http://{}/shelly", host));
    } catch (const HttpError&) {
        throw std::runtime_error(std::format(
            "The Shelly at {} does not answer. A battery Shelly sleeps: press its button once to wake it and try again.", host));
    }
    auto d = parseShellyInfo(host, info.body);
    if (info.status != 200 || !d) throw std::runtime_error(std::format("{} is not a Shelly", host));
    if (d->gen < 2) throw std::runtime_error("Only Shelly Gen2 and newer (Plus, Gen3, Gen4) can be set up from heatbrain");
    if (d->auth) {
        throw std::runtime_error(std::format(
            "The Shelly at {} is password protected. Turn off its password (on its web page, http://{}) and try again.", host, host));
    }

    const json status = rpc(http, host, "Shelly.GetStatus");
    applyShellyStatus(*d, status.dump());
    if (!d->sensor) throw std::runtime_error(std::format("The Shelly at {} ({}) has no temperature sensor", host, d->model));

    const json hooks = rpc(http, host, "Webhook.List");
    applyShellyHooks(*d, hooks.dump());

    std::string label(name);
    if (label.empty()) label = !d->pushName.empty() ? d->pushName : !d->name.empty() ? d->name : d->id;
    d->name = label;
    // Already reporting to heatbrain: the same id, so it stays the same sensor (and a room keeps it).
    // Otherwise the device's own id: unique and stays the same when the name changes.
    std::string id = slugOf(d->pushId);
    if (id.empty()) id = slugOf(d->id);
    if (id.empty()) id = slugOf(label);
    if (id.empty()) id = "shelly";
    d->id = id;
    d->pushId = id;
    d->pushName = label;

    // Replace what heatbrain set up before (a new name or a new heatbrain address).
    ShellySetupResult out;
    if (hooks.contains("hooks")) {
        for (const auto& h : hooks["hooks"]) {
            bool ours = false;
            if (h.contains("urls")) {
                for (const auto& u : h["urls"]) ours = ours || isHeatbrainUrl(u);
            }
            if (ours && h.contains("id")) {
                rpc(http, host, "Webhook.Delete", json{{"id", h["id"]}});
                ++out.removed;
            }
        }
    }
    // Each report also carries the battery level: the device fills in ${status[...]} from its own status
    // before sending (Shelly's webhook URL tokens). On USB power it is null, which heatbrain ignores.
    rpc(http, host, "Webhook.Create",
        json{{"cid", 0}, {"enable", true}, {"event", "temperature.change"}, {"name", "heatbrain temperature"},
             {"urls", json::array({shellyPushUrl(base, id, label, std::string("&temp=${ev.tC}").append(kShellyBatteryToken))})}});
    rpc(http, host, "Webhook.Create",
        json{{"cid", 0}, {"enable", true}, {"event", "humidity.change"}, {"name", "heatbrain humidity"},
             {"urls", json::array({shellyPushUrl(base, id, label, std::string("&hum=${ev.rh}").append(kShellyBatteryToken))})}});
    // Report every half degree (best effort: not every model has the setting).
    try {
        rpc(http, host, "Temperature.SetConfig", json{{"id", 0}, {"config", {{"report_thr_C", 0.5}}}});
    } catch (const std::exception& e) {
        log::info("Shelly {}: report threshold left as it was ({})", host, e.what());
    }
    out.device = std::move(*d);
    return out;
}

int addShellyBattery(HttpClient& http, std::string_view host) {
    const json hooks = rpc(http, host, "Webhook.List");
    int changed = 0;
    if (!hooks.contains("hooks")) return 0;
    for (const auto& h : hooks["hooks"]) {
        if (!h.contains("id") || !h.contains("urls") || !h["urls"].is_array()) continue;
        json urls = json::array();
        bool update = false;
        for (const auto& u : h["urls"]) {
            if (isHeatbrainUrl(u) && u.get<std::string>().find("battery=") == std::string::npos) {
                urls.push_back(u.get<std::string>().append(kShellyBatteryToken));
                update = true;
            } else {
                urls.push_back(u);
            }
        }
        if (!update) continue;
        rpc(http, host, "Webhook.Update", json{{"id", h["id"]}, {"urls", urls}});
        ++changed;
    }
    return changed;
}

}  // namespace hb
