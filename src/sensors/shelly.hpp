#pragma once

// Finding Shelly sensors on the network and setting them up without the Shelly app.
//
// Gen2+ Shellys (Plus, Pro, Gen3, Gen4) answer GET /shelly with who they are and take RPC calls on
// /rpc. A battery H&T sleeps between readings, so it only answers while awake (just after it joined
// the Wi-Fi, or for a short while after its button is pressed). Setting one up installs two webhooks
// on it (Webhook.Create) that call heatbrain's /api/push/shelly when the temperature or humidity
// changes - the same as adding the two Actions by hand in the Shelly app. Both also send the battery level.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/http.hpp"

namespace hb {

struct DiscoveredShelly {
    std::string host;
    std::string id;     // "shellyhtg3-84fce63ad204"
    std::string model;  // "S3SN-0U12A"
    std::string app;    // "HTG3", "PlusHT", ...
    std::string name;   // the name given on the device, if any
    std::string firmware;
    int gen = 0;
    bool auth = false;    // password protected: heatbrain cannot set it up
    bool sensor = false;  // has a temperature sensor
    std::optional<double> temperature, humidity;
    std::optional<int> battery;
    // Already reporting to heatbrain (its webhooks call /api/push/shelly): under this id and name.
    std::string pushId, pushName;
};

// Parses a GET /shelly answer; nullopt when it is not a Shelly.
std::optional<DiscoveredShelly> parseShellyInfo(std::string_view host, std::string_view body);
// Adds temperature/humidity/battery from a Shelly.GetStatus result.
void applyShellyStatus(DiscoveredShelly& d, std::string_view body);
// Finds heatbrain's webhooks in a Webhook.List result and notes the id/name they report under.
void applyShellyHooks(DiscoveredShelly& d, std::string_view body);

// Probes every address of a /24 network ("192.168.1.0/24" or "192.168.1") for Shellys.
std::vector<DiscoveredShelly> discoverShelly(std::string_view subnet, HttpClient& http);

// Added to both webhook URLs: the battery level, read by the device from its own status when it sends.
inline constexpr std::string_view kShellyBatteryToken = R"(&battery=${status["devicepower:0"].battery.percent})";

// heatbrain's push address for a sensor; base is how the Shelly reaches heatbrain ("http://192.168.1.10:8090").
std::string shellyPushUrl(std::string_view base, std::string_view id, std::string_view name, std::string_view suffix);

struct ShellySetupResult {
    DiscoveredShelly device;
    int removed = 0;  // heatbrain webhooks that were there before
};

// Installs heatbrain's two webhooks on the Shelly at host (replacing any earlier ones from heatbrain)
// and returns its current readings. A Shelly already reporting to heatbrain keeps its id (and its name
// when none is given), so it stays the same sensor - also one set up by hand. Throws std::runtime_error with a readable message.
ShellySetupResult setupShelly(HttpClient& http, std::string_view host, std::string_view base, std::string_view name);

// Adds the battery level to heatbrain's webhooks on the Shelly at host, where they don't send it yet
// (set up before heatbrain asked for it). Leaves everything else as it is. For the moment a battery
// Shelly is awake anyway: while it reports to heatbrain. Returns how many webhooks it changed; throws
// std::runtime_error when the Shelly can't be reached or refuses.
int addShellyBattery(HttpClient& http, std::string_view host);

}  // namespace hb
