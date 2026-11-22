#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/window.hpp"

using namespace hb;
using namespace std::chrono;

namespace {
Instant t0() { return Instant{seconds{1'790'000'000}}; }
Instant at(int minute) { return t0() + minutes{minute}; }
}  // namespace

TEST_CASE("Open window: a fast fall pauses heating, a slow one does not") {
    const OpenWindowConfig cfg;  // 1 degree within 30 min, pause 30 min
    OpenWindowDetector d;
    CHECK_FALSE(d.update(at(0), 21.0, cfg));
    CHECK_FALSE(d.update(at(10), 20.8, cfg));
    CHECK_FALSE(d.update(at(10), 19.0, cfg));  // same reading time again: ignored
    const auto ev = d.update(at(20), 19.9, cfg);
    REQUIRE(ev);
    CHECK(ev->drop == Catch::Approx(1.1));
    CHECK(ev->over == minutes{20});
    CHECK(ev->until == at(50));
    CHECK(d.pausedAt(at(30)));
    CHECK_FALSE(d.pausedAt(at(50)));

    // Still cold after the pause: only a new fall counts.
    CHECK_FALSE(d.update(at(55), 19.5, cfg));
    CHECK_FALSE(d.pausedAt(at(56)));

    // A slow fall (1 degree over two hours) is just cooling.
    OpenWindowDetector slow;
    for (int m = 0; m <= 120; m += 10) CHECK_FALSE(slow.update(at(m), 21.0 - m / 120.0, cfg));
}

TEST_CASE("Open window detection can be switched off") {
    OpenWindowConfig off;
    off.enabled = false;
    OpenWindowDetector d;
    d.update(at(0), 21.0, off);
    CHECK_FALSE(d.update(at(10), 18.0, off));
    CHECK_FALSE(d.pausedAt(at(11)));
}
