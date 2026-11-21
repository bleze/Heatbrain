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
| 5. Notifications (Discord) + web UI | next |
| 6. Docker image + GitHub Actions, deploy to Synology | |

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
`maxTemp` and a schedule of target temperatures. Rooms without a sensor use `priceOnly`
(run the N cheapest hours per day, with no-heat windows). Every tick (default 5 min) the
planner decides each room, checking in this order:

| # | Rule | Reason code |
|---|---|---|
| 1 | At/above `maxTemp`: OFF, even with manual ON | `ABOVE_MAX` |
| 2 | Manual override (OFF still heats below `frostTemp`) | `MANUAL_ON`, `MANUAL_OFF`, `FROST_PROTECTION` |
| 3 | Temperature missing or older than 30 min: 10 min ON / 50 min OFF | `STALE_DATA_LIMP` |
| 4 | Below `minTemp`: ON whatever the price | `BELOW_MIN` |
| 5 | Expensive slot: coast down to target − 1.0 °C | `PEAK_AVOID`, `BELOW_COMFORT_DURING_PEAK` |
| 6 | Expensive slot within 3 h and clearly cheaper now: heat to target + 1.0 °C | `PREHEAT_BEFORE_PEAK` |
| 7 | Among the cheapest slots: heat to target + 0.5 °C | `CHEAP_BOOST` |
| 8 | Thermostat with ±0.3 °C hysteresis | `BELOW_TARGET`, `TARGET_REACHED`, `HOLD_ON`, `HOLD_OFF` |

"Expensive" = top quarter of the next 24 h *and* 15% above the median; "cheapest" = bottom
quarter *and* 10% below the median, so flat-price days fall back to a plain thermostat.
All margins are configurable per room (`strategy`).

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

The web UI will show per room: temperature over time with the target band, ON periods as
bars coloured by reason, and price as a secondary series, plus a filterable event log.

## Notifications

Pluggable notifiers selected in config: `discord` (webhook; first implementation), later
`ntfy`, `telegram`, `pushover`, generic `webhook`.

Alerts: plug unreachable, plug state differs from commanded state, Netatmo re-login
required, Netatmo module offline / low battery, stale temperature (room in limp mode),
tomorrow's prices missing by ~15:00, room below minimum despite heating (open window /
radiator off), heatbrain restarted.

Rules: alert only after a problem persists (debounce), send a "resolved" message on
recovery, repeat ongoing problems at most every few hours, optional quiet hours for
non-urgent alerts. Active problems also show as a banner in the web UI and in the room's
decision log.

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
