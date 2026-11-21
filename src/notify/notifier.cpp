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
        } else {
            throw std::runtime_error("Notification channel: unknown type '" + type + "' (known: discord)");
        }
    }
    return nc;
}

}  // namespace hb
