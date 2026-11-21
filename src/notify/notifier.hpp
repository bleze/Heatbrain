#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/alerts.hpp"
#include "net/http.hpp"

namespace hb {

// A channel that delivers alerts to a human. Selected by "type" in notifications.channels.
class Notifier {
public:
    virtual ~Notifier() = default;
    virtual std::string name() const = 0;
    // Throws on failure.
    virtual void send(const Alert& alert) = 0;
    // Free-form message (startup, test).
    virtual void sendText(const std::string& title, const std::string& text) = 0;
};

// Discord channel webhook (Edit Channel -> Integrations -> Webhooks).
class DiscordNotifier final : public Notifier {
public:
    DiscordNotifier(HttpClient& http, std::string webhookUrl);
    std::string name() const override { return "discord"; }
    void send(const Alert& alert) override;
    void sendText(const std::string& title, const std::string& text) override;

private:
    void post(const nlohmann::json& payload);
    HttpClient& http_;
    std::string url_;
};

nlohmann::json discordPayload(const Alert& alert);

// ntfy (https://ntfy.sh, or self-hosted): push notifications to the ntfy phone app.
// No account needed; the topic name acts as the shared secret.
class NtfyNotifier final : public Notifier {
public:
    NtfyNotifier(HttpClient& http, std::string server, std::string topic, std::string token);
    std::string name() const override { return "ntfy"; }
    void send(const Alert& alert) override;
    void sendText(const std::string& title, const std::string& text) override;

private:
    void post(const nlohmann::json& payload);
    HttpClient& http_;
    std::string server_, topic_, token_;
};

// JSON publish payload (title/message/priority/tags) for an alert.
nlohmann::json ntfyPayload(const Alert& alert, const std::string& topic);

struct NotificationConfig {
    std::vector<std::unique_ptr<Notifier>> channels;
    AlertPolicy policy;
    bool notifyOnStart = true;
};

// Parses the config's "notifications" object. Channels without a URL are skipped.
NotificationConfig loadNotifications(const nlohmann::json& cfg, HttpClient& http);

}  // namespace hb
