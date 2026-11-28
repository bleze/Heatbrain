#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/time.hpp"
#include "net/http.hpp"

namespace hb {

// Outdoor network cameras (VIVOTEK), shown on the dashboard's outdoor card. heatbrain fetches
// their still picture itself, so the browser never needs the camera's address or password, and
// a camera that stops answering is reported like any other device.
//
// "cameras": [ { "id": "garden", "name": "Garden", "host": "192.168.1.61",
//                "user": "viewer", "password": "..." } ]     user/password: when it asks for a login
struct CameraConfig {
    std::string id;
    std::string name;  // empty: the camera's own name (asked once it answers), else the id
    std::string host;  // address, optionally with :port
    std::string user;
    std::string password;
};
std::vector<CameraConfig> parseCameras(const nlohmann::json& cfg);

// The camera's still picture (VIVOTEK: the same on every model).
std::string snapshotUrl(const CameraConfig& c);

// A VIVOTEK camera found on the network.
struct DiscoveredCamera {
    std::string host;
    std::string model, firmware, hostName;  // empty when it asks for a login
    bool needsLogin = false;  // to view it
    bool openAdmin = false;   // its settings can be changed without a password (no root password)
};
// Scans a /24 ("192.168.1.0/24", "192.168.1" or any address in it) in parallel.
std::vector<DiscoveredCamera> discoverCameras(std::string_view subnet, HttpClient& http);

struct CameraPicture {
    std::shared_ptr<const std::string> jpeg;
    Instant at;  // when it was fetched
};

struct CameraStatus {
    std::string id, name;              // name: as shown (set, else the camera's own, else the id)
    std::string ownName, model, firmware;  // as the camera tells (empty until it has answered)
    bool ok = false;
    std::string error;
    std::optional<Instant> lastPicture;  // the last time it answered with a picture
};

// The cameras' latest pictures, shared by everyone looking. Thread-safe; one fetch at a time per
// camera, so several viewers (and the health check) do not ask a slow camera at once.
class CameraHub {
public:
    using Clock = std::function<Instant()>;
    explicit CameraHub(HttpClient& http, Clock clock);
    static CameraHub& instance();  // the program's, over the LAN with short timeouts

    void configure(std::vector<CameraConfig> cameras);
    std::vector<CameraConfig> cameras() const;  // names as shown

    // The picture, fetched again when older than maxAge. Throws with a readable reason (also
    // kept for status()); std::out_of_range for an unknown camera.
    CameraPicture picture(const std::string& id, std::chrono::milliseconds maxAge);
    std::vector<CameraStatus> status() const;

private:
    struct Entry {
        CameraConfig cfg;
        std::mutex fetch;  // held while asking the camera
        std::optional<CameraPicture> last;
        std::string error;
        std::string ownName, model, firmware;  // from the camera itself
        std::optional<Instant> infoAt;         // when that was asked (again hourly, and after not answering)
        std::string shownName() const;
    };
    void askInfo(Entry& e, Instant now);  // with e.fetch held
    std::shared_ptr<Entry> entry(const std::string& id) const;

    HttpClient& http_;
    Clock clock_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Entry>> entries_;
};

}  // namespace hb
