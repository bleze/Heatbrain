# heatbrain

Price- and temperature-aware heating control for Tasmota smart plugs.

heatbrain fetches electricity prices (spot + grid tariffs + taxes) and room temperatures,
decides per room when the radiators should run, and switches Tasmota plugs accordingly.
It runs as a single C++ process with a web UI, in Docker on a NAS or locally while developing.

## Status

| Milestone | State |
|---|---|
| 1. Price model (configurable providers) + `prices` command | done |
| 2. Tasmota plugs + on-plug fail-safe (`discover`, `plugs`, `plug`) | done |
| 3. Netatmo temperatures (OAuth, rotating tokens) + `temps` | done |
| 4. Planner + scheduler loop + history (`plan`, `run`, `history`, `override`) | done |
| 5. Notifications (ntfy, Discord) + web UI with settings, help, costs | done |
| 6. Docker image + GitHub Actions, deploy to Synology | next |

## Architecture

```
src/core/       Pure logic, no IO: time/DST, tariffs, schedules, price totals, (planner)
src/net/        HTTP client + file cache (public APIs are rate limited)
src/providers/  Pluggable data sources, selected by "type" in config
src/app/        Config loading, CLI, (scheduler, web server)
```

Nothing in `core` knows about Denmark. Country-specific data sources live in `providers`
and are registered in `providers/price_model.cpp`.

### Price components

The price per kWh is a list of components in `heatbrain.json`, summed, then surcharges
(VAT) applied in order:

| type | What it is |
|---|---|
| `energidataservice.spot` | Day-ahead spot price for DK1/DK2 (15-minute resolution) |
| `energidataservice.tariff` | Danish grid tariffs from the Datahub price list (by GLN + charge codes) |
| `schedule` | Time-of-day bands with seasons/weekdays written in config: works for any grid company |
| `fixed` | Constant per kWh: taxes, supplier markup |

Adding a country means adding a spot provider (ENTSO-E covers all of Europe) and
describing its tariffs with `schedule`.

## Fail-safe

The radiators have analog thermostats set high, so heatbrain is effectively the thermostat.
A crash while a relay is ON must never mean "heat forever". All of this was verified on
Tasmota 14.6.0 (Sonoff Basic R2), including that re-sending ON restarts the PulseTime
countdown:

1. **Dead-man switch on the plug.** Every ON is sent with Tasmota `PulseTime` (~20 min):
   the relay turns itself OFF unless heatbrain keeps refreshing it.
2. **Limp mode on the plug.** A Tasmota rule watchdog (`RuleTimer`, re-armed by every
   heatbrain command) runs a bounded duty cycle (e.g. 10 min ON / 50 min OFF) when
   heatbrain goes silent: frost protection, never runaway heat.
3. **No heating on stale data.** If a room's temperature is older than ~30 min, heatbrain
   uses the same bounded profile instead of trusting old readings.
4. **Hard max temperature per room**, enforced by heatbrain; the analog thermostat stays as
   the last-resort cap.
5. `PowerOnState 0`: after a power cut, plugs start OFF until heatbrain takes over.

### Firmware and fail-safe test (Settings → Plugs)

- **Test fail-safe**: switches the relay on with a 20 s auto-off, refreshes it once (the
  countdown must restart) and checks the relay turns itself off; then restores the plug's
  settings. The result is recorded per firmware version.
- **Upgrade**: shown when GitHub has a newer Tasmota release. Only for dry-run plugs, one
  at a time: backs up the plug's settings, starts the over-the-air upgrade from
  ota.tasmota.com (same build variant; 1 MB devices pass through the minimal build), waits
  for the plug to come back on the new version, then runs the fail-safe test.
- A **live** plug is not switched if its firmware differs from the last version that passed
  the test, or if the test failed.
- **Restart** restarts the plug.

### Plug commands

```powershell
heatbrain discover 192.168.1.0/24   # find Tasmota devices
heatbrain plugs                     # status + fail-safe check of configured plugs
heatbrain plug bedroom provision    # back up settings, install fail-safe, disable device timers
heatbrain plug bedroom on|off       # ON refused unless the fail-safe is installed (--force)
heatbrain plug bedroom restore      # put back the latest backup (data/plugs/<id>/)
```

Plugs default to `"mode": "dryrun"`: the planner will only switch plugs set to `"live"`.
Only provision a plug once heatbrain runs continuously; otherwise it enters limp mode.

## Rooms and the planner

Each room has plugs, a sensor (`"netatmo:Bedroom"`, by module name or id), `minTemp`,
`maxTemp` and a **daily program** of switch points, each running until the next one (the
last continues past midnight): **heat to X** (price-aware), **keep above X** (a floor only:
no pre-heating or boosting) or **off** (only frost protection, e.g. the 17-21 peak tariff or
the night). Older configs with `schedule` bands and `noHeat` windows are converted
automatically. Rooms without a sensor use `priceOnly`
(run the N cheapest hours per day, with no-heat windows). Every tick (default 5 min) the
planner decides each room, checking in this order:

| # | Rule | Reason code |
|---|---|---|
| 1 | At/above `maxTemp`: OFF, even with manual ON | `ABOVE_MAX` |
| 2 | Manual override (OFF still heats below `frostTemp`) | `MANUAL_ON`, `MANUAL_OFF`, `FROST_PROTECTION` |
| 3 | Program says off: only frost protection | `SCHEDULED_OFF` |
| 4 | Away period: keep above the away temperature | `AWAY_OFF` |
| 5 | Temperature missing or older than 30 min: 10 min ON / 50 min OFF | `STALE_DATA_LIMP` |
| 6 | Keep-above period: heat only below the floor | `KEEP_ABOVE`, `KEEP_SATISFIED` |
| 7 | Below `minTemp`: ON whatever the price | `BELOW_MIN` |
| 8 | Expensive slot: coast down to target − 1.0 °C | `PEAK_AVOID`, `BELOW_COMFORT_DURING_PEAK` |
| 9 | Expensive slot within 3 h and clearly cheaper now: heat to target + 1.0 °C (×0.5–1.5 by outdoor temperature) | `PREHEAT_BEFORE_PEAK` |
| 10 | Among the cheapest slots: heat to target + 0.5 °C | `CHEAP_BOOST` |
| 11 | Thermostat with ±0.3 °C hysteresis | `BELOW_TARGET`, `TARGET_REACHED`, `HOLD_ON`, `HOLD_OFF` |

"Expensive" = top quarter of the next 24 h *and* 15% above the median; "cheapest" = bottom
quarter *and* 10% below the median, so flat-price days fall back to a plain thermostat.
All margins are configurable per room (`strategy`). The outdoor module (configurable, or
the first Netatmo outdoor module) scales pre-heating: colder outside, more stored heat.

**Away mode** is a planned period (from – until, set from the dashboard's Away button,
stored in `data/away.json`): every room only keeps above the chosen temperature, price-only
rooms stay off, and it starts and ends by itself.

**Wi-Fi off period** (Settings → System, `"wifiOff": {"from": "23:30", "to": "06:30"}`): for a
network that is switched off at night. Shortly before, heatbrain loads a schedule onto each live
plug (Tasmota Rule2/Rule3, `Time#Minute` triggers on the plug's own clock) and stretches the
fail-safe to match: auto-off just longer than the longest planned ON, watchdog until the end
plus 30 min. Without Wi-Fi there are no temperatures either, so heat/keep periods heat the
share of time the room actually heated in such periods over the last 7 days (25 % until there
is enough history), in the cheapest quarters; price-only rooms get their cheapest hours.
Expected plug/sensor outages are not reported during the period and 20 minutes after.

**Plan preview:** each room card shows the next 24 h worked out from the program and the
known prices (heat / pre-heat / coast / boost / keep / away / off).

### Decision log

Every tick stores one decision per room in SQLite (`data/history.db`, 90 days): ON/OFF,
whether it changed, reason code, plain-language detail, a snapshot of the inputs
(temperature and age, target, price, median) and what happened at each plug. Temperatures
and prices are stored too, for the web UI's graphs.

```powershell
heatbrain plan                       # what every room would do now, and why
heatbrain run                        # the control loop
heatbrain history bedroom            # switch events of the last 24 h  [--hours 72] [--all]
heatbrain override bedroom on 60     # manual ON for 60 min (picked up by a running 'run')
heatbrain override bedroom clear
```

## Web UI

`heatbrain run` serves a web UI on port 8080 (`web` in config; no login, keep it on your
home network): problems banner, price chart for today and tomorrow, a card per room
(temperature, decision and reason, plugs, "Heat 1 h" / "Off 2 h" / "Back to auto"), and
per room a chart of temperature + target, heating periods coloured by reason category
(comfort / price-driven / safety-manual) and price, with the hours ahead, plus the switch
event table. The outdoor card opens the same chart for the outdoor temperature. Overrides
from the UI are applied immediately. **Settings** edits everything in the config (rooms and
programs, plugs incl. fail-safe and firmware upgrades, sensors, prices, notifications,
system) with validation and backups; **Help** explains every feature, and "?" buttons show
the same text next to the settings. Light/dark/system theme. The frontend is plain HTML/JS in `web/` (no build step); in dev builds it is served
straight from the source folder, so edits only need a browser refresh.

## Heating cost

The dashboard estimates what each radiator costs: on-time from the decision log × the
radiator's wattage (set per plug in Settings → Plugs) × the actual price in each 15-minute
slot. It shows cost, energy, the average price paid against each day's average price, and
what the timing saved. Dry-run plugs show what they *would* have cost. The Sonoff Basic
has no power metering, so these are estimates; plugs with metering (POW R3, S31, ...)
could report measured energy later.

## Notifications

Pluggable notifiers selected in config: **`ntfy`** (phone push without an account: install
the ntfy app and subscribe to a random topic; optional token for self-hosted servers) and
`discord` (channel webhook). Set up and test from Settings → Notifications.

Digests: a weekly cost summary on Monday at 08:00 and a heads-up at 14:00 when tomorrow is
at least 20 % more expensive or cheaper than today (both optional).

Alerts: plug unreachable, plug state differs from commanded state, Netatmo re-login
required, Netatmo module offline / low battery, stale temperature (room in limp mode),
tomorrow's prices missing by ~15:00, room below minimum despite heating (open window /
radiator off), heatbrain restarted.

Rules: alert only after a problem persists (debounce), send a "resolved" message on
recovery, repeat ongoing problems at most every few hours, optional quiet hours for
non-urgent alerts. Active problems also show as a banner in the web UI and in the room's
decision log.

## Deployment (Synology DS1821+ / any Docker host)

Every push to `main` builds the image with GitHub Actions (unit tests run inside the
Docker build) and publishes `ghcr.io/bleze/heatbrain:latest` (plus a `sha-…` tag).

1. **Registry access.** GHCR packages start private. Either make the `heatbrain` package
   public (GitHub → Packages → heatbrain → Package settings), or on the NAS log in with a
   personal access token that has `read:packages`:
   `sudo docker login ghcr.io -u bleze`.
2. **Data folder.** Create `/volume1/docker/heatbrain` (empty is fine: heatbrain creates a
   starter config on first run).
3. **Container Manager → Project → Create**, paste `deploy/docker-compose.yml`, start.
   The web UI is at `http://<nas-ip>:8080`.
4. **Move your setup.** On the old heatbrain: Settings → System → *Export* (tick "Include
   history" to keep graphs and costs). On the NAS: Settings → System → *Import* that file.
   It carries the config with passwords, the Netatmo login, plug backups and fail-safe test
   results, the away period and overrides. Delete the file afterwards: it contains secrets.
5. **Only one heatbrain at a time.** Stop the old one right after importing. Netatmo rotates
   its refresh token on every refresh, so two copies lock each other out - and both would
   switch the plugs.
6. Other commands run inside the container, e.g.
   `sudo docker exec heatbrain /app/bin/heatbrain plugs`.
7. **Going live, one plug at a time:** Settings → Plugs → *Install fail-safe*, *Test
   fail-safe*, then switch the plug to Live and save. *Remove fail-safe* undoes it.

Updating: Container Manager → Project → heatbrain → Action → Build (pulls `latest`), or
`sudo docker compose pull && sudo docker compose up -d` in the project folder.

## Development (Windows, Visual Studio 2026)

Open the folder in Visual Studio, select the **Windows Debug (local dev)** preset, and build.
vcpkg (bundled with VS) installs the dependencies on first configure.

From a terminal:

```powershell
./scripts/dev.ps1 build
./scripts/dev.ps1 test
./scripts/dev.ps1 run prices          # hourly prices today + tomorrow
./scripts/dev.ps1 run prices --slots  # 15-minute slots
```

Debug builds keep their data in `./data` (git-ignored): `heatbrain.json` is created from
`config/heatbrain.example.json` on first run, and API responses are cached in `data/cache`.
