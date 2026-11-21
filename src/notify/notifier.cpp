#include "notify/notifier.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <stdexcept>

#include "app/rooms.hpp"
#include "core/log.hpp"

namespace hb {

using nlohmann::json;
using namespace std::chrono;

DiscordNotifier::DiscordNotifier(HttpClient& http, std::string webhookUrl)
    : http_(http), url_(std::move(webhookUrl)) {
    if (!url_.starts_with("https://")) throw std::runtime_error("discord: webhookUrl must start with https://");
}

json discordPayload(const Alert& a) {
    int color = 0x3498DB;  // info: blue
    std::string title;
    switch (a.kind) {
        case Alert::Kind::Raised:
            title = a.severity == Severity::Critical ? "🚨 Action needed" : "⚠️ Problem";
            color = a.severity == Severity::Critical ? 0xE74C3C : (a.severity == Severity::Warning ? 0xF39C12 : 0x3498DB);
            break;
        case Alert::Kind::Reminder:
            title = "⏰ Still a problem";
            color = a.severity == Severity::Critical ? 0xE74C3C : 0xF39C12;
            break;
        case Alert::Kind::Resolved:
            title = "✅ Resolved";
            color = 0x2ECC71;
            break;
    }
    return json{{"username", "heatbrain"},
                {"embeds",
                 {{{"title", title},
                   {"description", a.message},
                   {"color", color},
                   {"footer", {{"text", std::format("{} · {}", a.key, severityName(a.severity))}}},
                   {"timestamp", formatIsoUtc(a.since)}}}}};
}

void DiscordNotifier::post(const json& payload) {
    HttpRequest req;
    req.method = "POST";
    req.url = url_;
    req.contentType = "application/json";
    req.body = payload.dump();
    const auto res = http_.send(req);
    if (res.status < 200 || res.status >= 300) {
        throw HttpError(std::format("Discord webhook: HTTP {} {}", res.status, res.body.substr(0, 200)));
    }
}

void DiscordNotifier::send(const Alert& alert) { post(discordPayload(alert)); }

void DiscordNotifier::sendText(const std::string& title, const std::string& text) {
    post(json{{"username", "heatbrain"},
              {"embeds", {{{"title", title}, {"description", text}, {"color", 0x3498DB}}}}});
}

NtfyNotifier::NtfyNotifier(HttpClient& http, std::string server, std::string topic, std::string token)
    : http_(http), server_(std::move(server)), topic_(std::move(topic)), token_(std::move(token)) {
    while (!server_.empty() && server_.back() == '/') server_.pop_back();
    if (!server_.starts_with("https://") && !server_.starts_with("http://")) {
        throw std::runtime_error("ntfy: server must start with https:// (or http:// for a local server)");
    }
    if (topic_.size() < 8 || topic_.find_first_of("/ ?#") != std::string::npos) {
        throw std::runtime_error("ntfy: topic must be at least 8 characters without spaces or / ? #");
    }
}

json ntfyPayload(const Alert& a, const std::string& topic) {
    int priority = 3;
    std::string title;
    json tags = json::array();
    switch (a.kind) {
        case Alert::Kind::Raised:
            priority = a.severity == Severity::Critical ? 5 : (a.severity == Severity::Warning ? 4 : 3);
            title = a.severity == Severity::Critical ? "heatbrain: action needed" : "heatbrain: problem";
            tags.push_back(a.severity == Severity::Critical ? "rotating_light" : "warning");
            break;
        case Alert::Kind::Reminder:
            priority = a.severity == Severity::Critical ? 5 : 3;
            title = "heatbrain: still a problem";
            tags.push_back("alarm_clock");
            break;
        case Alert::Kind::Resolved:
            priority = 2;
            title = "heatbrain: resolved";
            tags.push_back("white_check_mark");
            break;
    }
    return json{{"topic", topic}, {"title", title}, {"message", a.message}, {"priority", priority}, {"tags", tags}};
}

void NtfyNotifier::post(const json& payload) {
    HttpRequest req;
    req.method = "POST";
    req.url = server_ + "/";  // JSON publishing: the topic is in the body (UTF-8 safe titles)
    req.contentType = "application/json";
    req.body = payload.dump();
    if (!token_.empty()) req.headers.emplace_back("Authorization", "Bearer " + token_);
    const auto res = http_.send(req);
    if (res.status < 200 || res.status >= 300) {
        throw HttpError(std::format("ntfy: HTTP {} {}", res.status, res.body.substr(0, 200)));
    }
}

void NtfyNotifier::send(const Alert& alert) { post(ntfyPayload(alert, topic_)); }

void NtfyNotifier::sendText(const std::string& title, const std::string& text) {
    post(json{{"topic", topic_}, {"title", title}, {"message", text}, {"tags", {"fire"}}});
}

NotificationConfig loadNotifications(const json& cfg, HttpClient& http) {
    NotificationConfig nc;
    if (!cfg.contains("notifications")) return nc;
    const auto& n = cfg.at("notifications");
    nc.policy.repeat = minutes{static_cast<int>(n.value("repeatHours", 6.0) * 60)};
    if (n.contains("quietHours")) nc.policy.quietHours = parseWindow(n["quietHours"]);
    nc.notifyOnStart = n.value("notifyOnStart", true);
    for (const auto& c : n.value("channels", json::array())) {
        const std::string type = c.at("type").get<std::string>();
        if (type == "discord") {
            const std::string url = c.value("webhookUrl", "");
            if (url.empty()) {
                log::debug("Notifications: discord channel has no webhookUrl yet - skipped");
                continue;
            }
            nc.channels.push_back(std::make_unique<DiscordNotifier>(http, url));
        } else if (type == "ntfy") {
            const std::string topic = c.value("topic", "");
            if (topic.empty()) continue;  // not set up yet
            nc.channels.push_back(std::make_unique<NtfyNotifier>(http, c.value("server", std::string("https://ntfy.sh")),
                                                                 topic, c.value("token", "")));
        } else {
            throw std::runtime_error("Notification channel: unknown type '" + type + "' (known: ntfy, discord)");
        }
    }
    return nc;
}

}  // namespace hb
