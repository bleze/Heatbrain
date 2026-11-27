"use strict";
// Help texts: used by the "?" buttons in Settings and by the Help page (one source).
// Each entry: title, body (paragraphs), optional example(s) and list items.
const HELP = {
  start: {
    title: "Getting started",
    list: [
      "Settings → Heaters: scan your network, add your Tasmota plugs and enter each radiator's power (W).",
      "Settings → Sensors: connect Netatmo, VELUX ACTIVE or a Shelly H&T so rooms get their temperature.",
      "Settings → Rooms: add a room, pick its plug(s) and sensor, and set its daily program.",
      "Leave plugs in dry-run for a day or two and watch what heatbrain would do on the dashboard.",
      "Happy? Install the fail-safe on a plug, test it, then switch the plug to live - straight away: with the fail-safe installed, a dry-run plug switches itself off and runs limp mode, because heatbrain does not refresh it.",
      "Settings → Notifications: set up phone notifications so you hear about problems.",
    ],
  },
  dryrun: {
    title: "Dry-run and live",
    body: [
      "In dry-run heatbrain decides everything, logs it and shows it on the dashboard - but never switches the plug. Use it to check the decisions before you trust heatbrain with your radiators. Costs are then shown as what heating would have cost.",
      "In live heatbrain switches the plug. A live plug is only switched when its fail-safe is installed and has passed the fail-safe test on the firmware it runs.",
    ],
  },
  failsafe: {
    title: "The fail-safe",
    body: [
      "Your radiators have their own thermostats turned up high, so heatbrain is the thermostat. The fail-safe makes sure a crash, a network problem or a power cut never means heating forever:",
    ],
    list: [
      "Auto-off: the plug switches itself off 20 minutes after heatbrain last told it to stay on. heatbrain refreshes it every few minutes while heating.",
      "Limp mode: if heatbrain is silent for 30 minutes, the plug runs a gentle cycle by itself (10 min on, 50 min off) - frost protection, never full heat.",
      "After a power cut the plug starts off and waits for heatbrain.",
      "The plug's own timers are switched off so they cannot fight heatbrain.",
      "Backups: before installing the fail-safe and before a firmware upgrade, heatbrain saves the settings it changes (timers, power-on state, Rule1, auto-off) in data/plugs/<plug>/. 'Remove fail-safe' puts back the newest backup from before the fail-safe - the plug then works on its own timers again, like before heatbrain. A live plug must be set to dry-run first.",
    ],
  },
  firmware: {
    title: "Firmware upgrades",
    body: [
      "When a newer Tasmota release exists, the plug shows 'x.y available' and an Upgrade button (dry-run plugs only, one at a time). heatbrain backs up the plug's settings, starts the upgrade, waits for the plug to come back on the new version and then runs the fail-safe test. The plug must not lose power during the 2-5 minutes.",
    ],
  },
  program: {
    title: "Daily program",
    body: [
      "A room's day is a list of switch points. Each one runs until the next; the last one continues past midnight until the first. So there are never gaps or overlaps.",
    ],
    example: "07:00 heat to 19° · 17:00 off · 21:00 heat to 20° · 23:00 keep above 15° → warm in the day, nothing during the expensive peak, warm before bed, cool but not cold at night.",
  },
  modes: {
    title: "Heat to, keep above, off",
    list: [
      "Heat to X: keeps the room around X and is clever about prices - pre-heats before expensive hours, lets the room cool a little while it is expensive, and adds a little extra in the cheapest hours.",
      "Keep above X: only heats when the room falls below X. No pre-heating or boosting, and the price does not matter. Good for nights or rooms you rarely use.",
      "Off: never heats - only frost protection still does (5 °C by default). Use it for the 17-21 peak tariff or for sleeping.",
      "Off, stay above X: still never heats during the period, but stores heat just before it, so the room coasts through and is still above X when it ends. How much depends on how long the period is, the forecast outdoor temperature and how fast the room cools (learned; until then a cautious guess). It heats along the same curve as a warm-up, reaching the level about half an hour before the period starts, whatever the price. It never stores more than the room's max allows (max − 0.1): when that does not last through the period, the period still stays off, and the program row, the plan and the chart say how far it is expected to fall - raise the max, lower the floor or shorten the period. Choose plain Off for no floor. Good for the peak tariff in rooms you use in the evening; for an empty room, a plain off saves more.",
    ],
  },
  limits: {
    title: "Never below, never above, frost protection",
    list: [
      "Never below: in heat periods the room always heats below this, whatever the price. It limits how far coasting during expensive hours can go.",
      "Never above: heatbrain switches off above this, even when you pressed 'Heat 1 h'.",
      "Frost protection: even 'off', away or a manual 'Off 2 h' heats below this, so pipes cannot freeze.",
    ],
  },
  strategy: {
    title: "How eagerly to chase cheap prices",
    body: [
      "These settings decide how much comfort heatbrain trades for price in heat periods. 'Expensive' means the top quarter of the next 24 hours and clearly above the day's middle price; 'cheapest' the bottom quarter and clearly below it. On days with flat prices nothing is expensive or cheap and heatbrain is a plain thermostat.",
    ],
    list: [
      "Peak coasting (default 1.0 °C): while it is expensive, heating pauses until the room is this much below target. Example: target 19 °C, 18:00 is expensive → no heating at 18.6 °C; at 17.9 °C it heats again (just back above 18 °C).",
      "Pre-heating: when an expensive period is coming and the price now is at least 15 % lower, heatbrain heats a little above target so the room can coast through it. Until it has learned how fast the room warms and cools, that is 1 °C above target, looking 3 hours ahead - more when it will be cold outside during the expensive hours (up to ×1.5), less when mild (down to ×0.5).",
      "Once it has learned (a few days of history), pre-heating is worked out instead: just enough that the room coasts through the whole expensive stretch without falling more than the peak coasting allows, never above the room's max - and started only as late as heating that far allows, because stored heat leaks away. It then looks up to 8 hours ahead, so a slow room (a concrete floor) can start in time. On a mild evening that can mean no pre-heating at all.",
      "Cheap boost (default 0.5 °C): in the cheapest hours, heat up to target + this - heat stored when it costs least.",
      "Heating starts at target - 0.1 °C. A radiator keeps warming the room after it switches off, so heatbrain learns each room's after-heat (the median rise after its last 7 switch-offs) and stops that much before target + 0.1 - below the target, if the after-heat is large. Each start heats at least 15 minutes, so the relay never clicks on and off every few minutes. The line then runs close to the target instead of swinging around it.",
      "Readings older than 30 minutes are not trusted. For up to 3 hours heatbrain then estimates the temperature from the last reading - cooling towards outdoors, warming while it heated since, at the rates it has learned for the room - and heats to the target on that estimate (no price boosts on a guess). Without learned rates, or after 3 hours, the room runs limp mode until fresh readings arrive: as much as it usually heats in such a period (learned from the last week, at most 45 minutes an hour), or 10 minutes an hour until that is known.",
    ],
  },
  priceOnly: {
    title: "Price-only rooms",
    body: [
      "For rooms without a temperature sensor (like a shed): choose 'No sensor (cheapest hours)' as the room's sensor. heatbrain then heats in the N cheapest whole hours of each day and never in the no-heat windows. Whole hours, so the relay switches at most once an hour. Choose a sensor later and the room gets a daily program instead.",
    ],
  },
  sensors: {
    title: "Netatmo and the outdoor temperature",
    body: [
      "Indoor modules give each room its temperature; heatbrain reads them every control step (Netatmo itself updates about every 10 minutes). If a module goes offline, its last reading is used for up to 30 minutes so a restart passes quietly.",
      "The outdoor module scales pre-heating: when it is cold outside rooms cool faster during off periods, so heatbrain stores a little more heat beforehand.",
    ],
  },
  velux: {
    title: "VELUX ACTIVE room climate",
    body: [
      "VELUX ACTIVE measures temperature, humidity and CO₂ in its wall switch (and in separate climate sensors, if you have them), one reading per VELUX room. heatbrain reads them from the VELUX cloud, the same place the VELUX app gets them from, at each control step. It only reads: it never opens or closes windows.",
      "Connect with your VELUX ACTIVE app login. The password is used once to log in and is not stored; heatbrain keeps only the login token. If VELUX later rejects it, heatbrain warns you and you log in again here.",
      "This uses the VELUX app's own connection, not an official public interface, so a VELUX update could break it. The VELUX gateway also needs internet: during Wi-Fi-off periods the readings pause, just as for a Netatmo module.",
      "Mount the wall switch away from heaters, direct sun and electronics - it is a room thermometer like any other.",
    ],
  },
  shelly: {
    title: "Shelly H&T",
    body: [
      "The Shelly H&T (Gen3) runs on batteries and sleeps between readings, so heatbrain cannot ask it - it tells heatbrain. It needs two Actions on the Shelly: when the temperature changes, call heatbrain's address with the temperature; when the humidity changes, with the humidity. heatbrain puts them there itself, no Shelly app needed: press the Shelly's button once to wake it (it is only on the Wi-Fi while awake), then Find devices -> Scan (top of the Heaters and Sensors tabs), give it a name and press Set up. If the scan misses it, Settings -> Sensors -> Shelly H&T sets it up by its address. Everything stays on your network: no Shelly cloud needed.",
      "By hand instead (its app or its web page, Actions/Webhooks): When Temperature changes, call http://<heatbrain>/api/push/shelly?id=shed&name=Shed&temp=${ev.tC} - and When Humidity changes, the same with &hum=${ev.rh}. Add &battery=${status[\"devicepower:0\"].battery.percent} to both for its battery level. The scan recognises such a Shelly as added, and Set up again keeps it the same sensor.",
      "It only reports when a value changes by its threshold (Set up makes it 0.5°), so a steady room can be quiet for a long time. heatbrain treats no news as no change for up to 3 hours; after that the reading counts as stale, like any sensor that has gone quiet. The last readings are kept across restarts.",
      "During Wi-Fi-off periods the sensor cannot reach heatbrain; it catches up once the Wi-Fi is back.",
    ],
  },
  airQuality: {
    title: "Air quality (CO₂)",
    body: [
      "Netatmo indoor modules and the VELUX sensors measure CO₂, the gas we breathe out. It is the best everyday measure of how fresh the air in a room is: outdoors is about 420 ppm, below 1000 is fine, from 1000 it gets stuffy, and above 1400 most people sleep worse, get headaches or tire - time to air the room.",
      "heatbrain keeps the CO₂ with the other readings (Settings -> Sensors shows it per module, with the 1000 and notify levels as dashed lines), shows it on a room's card from 1000 ppm, and tells you when it has been above the level you choose (Settings -> Climate) for 20 minutes. During quiet hours it waits; a high night comes as one message in the morning, with when it was high and how high.",
      "Bedrooms with the door and vents closed often reach 1500-2500 ppm by morning. A vent or a window slightly open usually fixes it - airing costs some heat, but a short, wide airing (5-10 minutes) loses much less than a window on the latch all day.",
    ],
  },
  insulation: {
    title: "How well a room keeps its heat",
    body: [
      "From a room's own history heatbrain learns how fast it cools towards the outdoor temperature and how fast it warms at full power. With the heater's power that gives the heat that leaks out per degree between inside and outside: the heat loss in W/°C. 58 W/°C means that at 19 °C inside and 2 °C outside about 1000 W escapes - all the time.",
      "Hours in which a room got warmer without being heated are left out: something else heated it (sun, PCs, people, a warm room next door), so they tell nothing about how fast it loses heat. Heat that only slows the cooling still counts, so a room with a gaming PC on most evenings looks a little better insulated than it is.",
      "With the floor area set (Settings -> Rooms) it is also given per m², so rooms of different sizes compare fairly. Heat escapes through the outer walls, ceiling, windows and floor, so rooms with a high ceiling or many outer walls naturally come out somewhat higher. As a rough guide: around 1 W/°C per m² or less is well insulated, 1.5-2.5 is typical for older houses, much more points to a weak spot.",
      "heatbrain tells you when a room loses clearly more heat per m² than your other rooms (worth checking the ceiling, windows and vents), and when a room starts losing heat clearly faster than its own normal over the last week (a window on the latch, a vent left open, wet insulation). Sun, people, electronics and warm neighbouring rooms affect the figures, so they settle over a week or two and are guides, not an energy rating.",
    ],
  },
  forecast: {
    title: "Weather forecast",
    body: [
      "heatbrain fetches the hourly outdoor temperature for the next two days from Open-Meteo, a free weather service without an account. Only the position is sent, rounded to about a kilometre - by default where your Netatmo station is.",
      "The forecast is corrected to your outdoor sensor (the difference measured now fades out over six hours) and used for: how much to pre-heat before expensive hours (the weather during those hours counts, not the weather now), how long a booking needs to warm up, and the 'Next 24 h' plan.",
    ],
    example: "Mild now (10 °C) but 0 °C forecast for 17-21: the room is pre-heated for 0 °C. Freezing now but mild this evening: less pre-heating.",
  },
  openWindow: {
    title: "Open windows",
    body: [
      "When a room's temperature falls 1 °C or more within half an hour, a window or door is probably open. heatbrain then pauses that room's heating for 30 minutes and tells you (a notification, and on the dashboard). Frost protection and a manual ON still heat.",
      "After the pause the room heats normally again; only a new fast fall pauses it once more. The cooling during a pause is left out when heatbrain learns how fast the room cools.",
    ],
  },
  prices: {
    title: "What a kWh costs",
    body: [
      "The price per 15 minutes is the sum of its parts - spot price, grid tariff, Energinet's tariffs, electricity tax and your supplier's markup - with VAT on top. The defaults fit EWII on Radius in DK2; enter your supplier's markup from your contract.",
      "Under the room chart, on the same time axis, the Electricity card shows every known price ahead (tomorrow's from about 13:00) and the power mix of your price area per hour: wind, solar, the power plants (gas, biomass, coal and waste together - the fuels are only split later) and imports, from Energinet's open data. Hover for each share and the CO2 per kWh. It is for information: heatbrain plans by price. Either panel can be switched off in Settings → System.",
    ],
  },
  notifications: {
    title: "Notifications",
    body: [
      "Problems (a plug unreachable, a sensor offline or low on battery, missing prices, a room that does not warm up) are sent after they have lasted a while - one missed reading does not wake you. You get a quiet 'resolved' when a problem is over, reminders every few hours while it lasts, and nothing but critical alerts during quiet hours.",
      "Also: a weekly cost summary on Monday morning, a monthly report on the 1st (cost, the average price you paid against the market, the dearest day, and the energy against the month before), a heads-up when tomorrow is unusually expensive or cheap, and one message when a new Tasmota release is out for your plugs (checked daily; you start the upgrade yourself).",
    ],
  },
  away: {
    title: "Away mode",
    body: [
      "Plan a period (from - until) and every room only keeps above the temperature you choose; price-only rooms stay off. It starts and ends by itself - set it the evening before an early flight. Manual overrides and frost protection still work. The Away button at the top opens and closes the panel; it stays lit while a period is planned or running.",
    ],
  },
  bookings: {
    title: "Bookings",
    body: [
      "For a room you only use now and then: press Book at the top, choose the room and say when you need it and how warm - say 22 deg on Saturday 16:00-23:00. The room heats to that temperature from early enough before the start until the end, also through expensive hours (someone is there), then follows its program again. A booking wins over the program and away mode; manual buttons and the room's maximum still apply.",
      "How early it starts is learned from the room's own history: how fast it cools towards the outdoor temperature and how fast it warms when heated. From the room's current temperature and the weather that gives the warm-up time, with a margin - a few hours for a radiator, a day or more for floor heating in concrete. Until heatbrain has seen the room heat for a while, it uses the room's warm-up setting (Settings -> Rooms). Once warming has started it does not stop again.",
      "All bookings are listed in the Bookings card above the rooms, soonest first, with when warming starts - end or cancel them there. If a booking says the room may not reach the temperature in time, book earlier or check the room's maximum.",
    ],
  },
  plan: {
    title: "The next 24 hours",
    body: [
      "Each room card shows what heatbrain intends for the coming day, worked out from the program and the known prices: heat, pre-heat before an expensive period, coast while it is expensive, a cheap boost, warm up for the next period, keep above, away or off. In a keep-above period, 'warm enough, no heat' means the room is expected to stay above the floor by itself: worked out from its current temperature and how fast it cools (learned from its history). Whether it actually heats depends on the room's temperature at the time.",
      "Warm up for the next period: when the program steps up (say from keep above 16 at night to 21.5 at 07:00), the room starts early, so it is warm when the period begins rather than starting to heat then. The target rises along a curve - in the chart an S from 16 to 21.5 - worked out backwards from 07:00: how warm the room must be at each moment so that heating at full power still reaches 21.5 by 06:30. Once heatbrain has learned how fast the room warms, the curve is the room's own heating curve for the weather forecast; until then it is a straight ramp over the room's warm-up time (Settings -> Rooms; 0 turns early starts off). Towards a heat period, expensive hours still coast and cheap ones still pre-heat, against the curve; towards a keep-above period, like the period itself, price does not matter. It never starts early out of an off period, and during the Wi-Fi off period the night schedule heats the warm-up at full power.",
      "Off periods have no target. In the room chart, the hours ahead show instead how the room is expected to coast through them (a grey dashed line, from where heating leaves it, cooling at its learned rate), and an off period with 'stay above' shows that floor as a thin solid line marked ≥ 20° (or whatever the floor is). Heat stored before such a period shows as pre-heat.",
    ],
  },
  wifiOff: {
    title: "Wi-Fi off period",
    body: [
      "If you switch the Wi-Fi off at night, set the period here - with other times for the weekend if you sleep in (the nights ending Saturday and Sunday morning). Shortly before it starts, heatbrain loads a schedule onto each live plug and the plug switches on its own clock - no network needed. The schedule reaches 3 hours past the planned end, so a late Wi-Fi does not mean limp mode; as soon as readings arrive again, heatbrain takes over.",
      "Below the setting you see when the Wi-Fi was really off on recent nights, from the gaps in the Netatmo readings, and whether the period covered it. 'Use these times' fills in a period that covers them.",
      "There are no temperatures while the Wi-Fi is off (the Netatmo station uses Wi-Fi too), so the schedule is decided in advance: price-only rooms get their cheapest hours, 'off' periods stay off, and 'heat to' / 'keep above' periods heat as much as that room actually heated in such periods over the last week (15 min per hour until there is enough history), placed in the cheapest quarters - and only from when the room, cooling unheated from its last temperature at its learned rate towards the forecast outdoor temperature, would get down to the period's temperature. A warm room in a 'keep above' night therefore stays silent. For a night with no switching at all (a bedroom, where the relay click and the radiator's ticking can wake you), make it an 'off' period: nothing switches then, only frost protection below 5 °C.",
      "The fail-safe stays in charge: each planned ON ends with its own OFF and an auto-off just longer than it, and limp mode only starts if heatbrain has not taken over 30 minutes after the schedule ends. Plug and sensor outages during the period (and 20 minutes after) are not reported.",
      "A plug whose clock is not synchronised, or whose rule slots 2 and 3 hold your own rules, gets no schedule - you are told, and it runs limp mode for the night.",
      "The plugs' own Wi-Fi: with the network gone, a plug keeps searching for it all night. So heatbrain also tells each plug to switch its Wi-Fi off a minute after the period starts and on again two minutes before it ends - a little less power, and no radio while you sleep. Dry-run plugs with the fail-safe installed do it too (they switch nothing else). It needs Tasmota 15.4 or newer: older versions could crash when the Wi-Fi was switched off, so their Wi-Fi stays on (the log says so). Nothing is lost if the morning switch-on were missed: a plug's Wi-Fi always comes back when it restarts, so unplugging it for a moment is enough.",
    ],
  },
  meter: {
    title: "Your meter (Eloverblik)",
    body: [
      "Eloverblik (Energinet's DataHub) has your electricity meter's readings, whatever your supplier. Log in on eloverblik.dk with MitID, go to Data sharing and create a token, then paste it here and save. heatbrain fetches the readings every 6 hours (or with 'Fetch now'); they arrive about a day late, so the dashboard shows the past, not today.",
      "The dashboard's 'Electricity use' card shows the household's measured use per quarter or hour, with the part heatbrain's radiators drew in the same intervals (live plugs that were on × their power) at the bottom. If the radiators ever look bigger than the measured total, a radiator's power is probably set too high under Settings → Heaters.",
    ],
  },
  phone: {
    title: "heatbrain on your phone",
    body: [
      "Home-screen app: open heatbrain in Safari on the iPhone (Chrome on Android) and use Share → Add to Home Screen. It then opens full screen with its own icon, like an app.",
      "Widget: with the free iPhone app Scriptable, heatbrain gets a home-screen widget showing each room's temperature and target, a flame when it heats, the price now and the cheapest hour ahead. iOS refreshes widgets by itself, usually every 10-15 minutes; tapping it opens heatbrain. It needs to reach heatbrain: at home that's your network; away from home only through Tailscale (free; Synology package + iPhone app), whose address you can add before copying the script. Never forward a router port to heatbrain - it has no login. When nothing answers, the widget shows the last data it got.",
    ],
  },
  thermostats: {
    title: "Thermostats (DEVIreg Smart)",
    body: [
      "A thermostat regulates the floor heating by itself; heatbrain only sets its temperature: the room's target normally, a little lower while electricity is expensive (and 17-21), a little higher before an expensive period and in the cheapest hours, and frost protection when the program says off. In Live, heatbrain switches the thermostat to manual mode, so the DEVI app's own schedule no longer applies; in Dry-run it only shows what it would set.",
      "There is no fail-safe to install: if heatbrain or the network is gone, the thermostat keeps the last temperature it was given. The thermostat's wall sensor is available as a temperature source (devi:<name>), and its floor probe as devi:<name>-floor; you can also use a Netatmo module in the room.",
      "The connection goes through Danfoss's servers (as the DEVI app does), so it needs the internet. Pair once with a one-time code from the DEVI app (Settings -> Share house).",
    ],
  },
  transfer: {
    title: "Moving heatbrain (export and import)",
    body: [
      "Export downloads one file with everything heatbrain needs: the settings including passwords and the Netatmo login, plug backups and fail-safe test results, the away period, manual overrides and - if you tick it - the history behind the graphs and costs. Import on the other heatbrain (Settings → System) and it carries on where this one stopped.",
      "Run only one heatbrain at a time: stop this one as soon as the other has imported. Netatmo replaces its login every time it is used, so two copies would log each other out - and two copies would both switch the plugs.",
      "The file contains passwords: keep it private and delete it when the move is done.",
    ],
  },
  heatingHours: {
    title: "Heating hours",
    body: [
      "How long each room's radiator was on, per day - stacked by room, so the bar's height is the whole house. Over the year it shows the heating season: tall bars in winter, next to nothing in summer. Weeks and months show the average per day, so a short month compares fairly with a long one. Click a room's name to leave it out (the colours stay with their rooms).",
      "Plugs count the time the plug was on; a thermostat that reports its own heating time counts that instead. Rooms without a temperature sensor are left out, as in Heating cost. A grey stub means no data that day (heatbrain wasn't running), not a day without heat; today's bar is lighter until the day is over.",
      "Underneath: the outdoor sensor's day - the band from the day's low to its high, the line its average. Beside it each whole day is a dot, its hours against the outdoor average. The dashed line through the days that heated shows how many hours a degree colder adds, and where it reaches zero is roughly the outdoor temperature at which the house needs no heating. If that line gets flatter from one winter to the next, the house holds its heat better.",
      "Each day is summed up once it is over and kept for good (the detailed history goes after 3 years), so the chart can compare one winter with the next.",
    ],
  },
  costs: {
    title: "Heating cost",
    body: [
      "Plugs: estimated as the heating power × the time the plug was on × the real price in each 15-minute slot. The Sonoff Basic cannot measure power, and a radiator's own thermostat may cut out now and then, so the real figure can be a little lower. Rooms without a temperature sensor (price-only) are left out: their radiator's own thermostat decides when it really heats, so plug-on time would overstate it a lot. Once the room has a sensor it is counted. Thermostats (DEVIreg Smart) report how long they actually heat, so their energy is that time × the power set for the thermostat under Settings -> Heaters. 'Saved by timing' is what the same energy would have cost at each day's average price, minus what it actually cost. It assumes the same amount of heat with and without heatbrain, spread evenly over the day - a plain thermostat - so it is a guide to whether the timing pays off, not an exact amount: pre-heating uses a little more energy, coasting a little less, and the heat a room needs varies through the day.",
    ],
  },
};

// How heatbrain decides, in rule order (for the Help page).
const RULES = [
  ["Above 'never above'", "off - even with a manual ON"],
  ["Manual override (Heat 1 h / Off 2 h)", "on / off (off still protects against frost)"],
  ["Program says off", "off, except frost protection"],
  ["Away period", "keep above the away temperature"],
  ["No fresh temperature", "heat to target on an estimate from the last reading (up to 3 h, once learned); else limp mode: what the room usually heats (10 min an hour until learned)"],
  ["Keep-above period", "heat only below the floor"],
  ["Below 'never below'", "heat, whatever the price"],
  ["Expensive now", "coast down to target - peak coasting"],
  ["Expensive soon, cheaper now", "pre-heat to target + boost (more when cold outside)"],
  ["Among the cheapest hours", "heat to target + cheap boost"],
  ["Otherwise", "thermostat: on at target - 0.1, off early for the after-heat"],
];

function helpContent(keys) {
  const box = el("div", { class: "helpbox" });
  const list = Array.isArray(keys) ? keys : [keys];
  for (const key of list) {
    const h = HELP[key];
    if (!h) continue;
    if (list.length > 1) el("h4", {}, box, h.title);
    fillHelp(box, h);
  }
  return box;
}

function fillHelp(box, h) {
  for (const p of h.body || []) el("p", {}, box, p);
  if (h.list) {
    const ul = el("ul", {}, box);
    for (const item of h.list) el("li", {}, ul, item);
  }
  if (h.example) {
    const ex = el("p", { class: "help-example" }, box);
    el("b", {}, ex, "Example: ");
    ex.appendChild(document.createTextNode(h.example));
  }
}

// Which help boxes are open, so they survive a re-render (the plugs tab redraws when plug
// status arrives, the dashboard every refresh). A button is identified by its view, its key and
// its position among buttons with the same key.
const HELP_OPEN = new Set();
function helpId(b) {
  const same = [...document.querySelectorAll(".help-btn")].filter((x) => x.dataset.help === b.dataset.help);
  return `${location.hash}|${b.dataset.help}|${same.indexOf(b)}`;
}

// A small "?" button that shows the help text in place, below `anchor`.
function helpButton(parent, key, anchor) {
  const first = Array.isArray(key) ? key[0] : key;
  const b = el("button", { type: "button", class: "help-btn", title: `About: ${HELP[first]?.title || first}`, "aria-label": "Help",
                           "data-help": [].concat(key).join(",") }, parent);
  b.appendChild(icon("question"));
  let box = null;
  const show = () => {
    // A "?" in a collapsed section's header: open the section, or the text would be hidden inside it
    const details = b.closest("summary")?.parentElement;
    if (details && !details.open) details.open = true;
    box = helpContent(key);
    (anchor || parent).after(box);
    b.classList.add("open");
  };
  const hide = () => { box?.remove(); box = null; b.classList.remove("open"); };
  b.addEventListener("click", (e) => {
    e.preventDefault();
    e.stopPropagation();
    const id = helpId(b);
    if (box) { hide(); HELP_OPEN.delete(id); } else { show(); HELP_OPEN.add(id); }
  });
  // Once the surrounding render is in the page: reopen if it was open before the redraw.
  requestAnimationFrame(() => { if (!box && b.isConnected && HELP_OPEN.has(helpId(b))) show(); });
  return b;
}

function renderManual(host) {
  host.replaceChildren();
  const card = el("section", { class: "card manual" }, host);
  const head = el("h2", {}, card);
  head.appendChild(badge("info", "violet"));
  head.appendChild(document.createTextNode("How heatbrain works"));
  el("p", { class: "muted" }, card,
    "heatbrain switches your radiators' plugs on and off from the price of electricity, the temperature in each room and the program you set. Everything here is also available as '?' next to the settings.");
  const toc = el("nav", { class: "toc" }, card);
  // The sections side by side on wide screens, each a readable column; read left to right, row by row.
  const body = el("div", { class: "manual-sections" }, card);
  const order = ["start", "program", "modes", "limits", "strategy", "priceOnly", "plan", "dryrun", "failsafe", "firmware",
                 "thermostats", "sensors", "velux", "shelly", "forecast", "openWindow", "insulation", "airQuality", "prices", "meter", "away", "bookings", "wifiOff", "notifications", "phone", "costs", "heatingHours", "transfer"];
  const add = (key) => {
    const sec = el("section", { id: `help-${key}` }, body);
    el("h3", {}, sec, HELP[key].title);
    sec.appendChild(helpContent(key));
    const a = el("a", { href: `#help-${key}` }, toc, HELP[key].title);
    a.addEventListener("click", (e) => { e.preventDefault(); sec.scrollIntoView({ behavior: "smooth" }); });
  };
  add("start");
  // The rules, right after getting started
  const rules = el("section", { id: "help-rules" }, body);
  el("h3", {}, rules, "How heatbrain decides");
  el("p", {}, rules, "Every few minutes each room is decided by the first rule that applies:");
  const table = el("table", { class: "events" }, rules);
  const thead = el("tr", {}, el("thead", {}, table));
  el("th", {}, thead, "#"); el("th", {}, thead, "When"); el("th", {}, thead, "Then");
  const tbody = el("tbody", {}, table);
  RULES.forEach(([when, then], i) => {
    const tr = el("tr", {}, tbody);
    el("td", {}, tr, String(i + 1)); el("td", {}, tr, when); el("td", { class: "why" }, tr, then);
  });
  const ra = el("a", { href: "#help-rules" }, toc, "How heatbrain decides");
  ra.addEventListener("click", (e) => { e.preventDefault(); rules.scrollIntoView({ behavior: "smooth" }); });
  for (const key of order.slice(1)) add(key);
  const v = state.status?.version;
  if (v) el("p", { class: "muted", style: "margin-top:24px" }, card, `heatbrain version ${v}`);
}
