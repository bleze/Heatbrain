"use strict";
// Help texts: used by the "?" buttons in Settings and by the Help page (one source).
// Each entry: title, body (paragraphs), optional example(s) and list items.
const HELP = {
  start: {
    title: "Getting started",
    list: [
      "Settings → Plugs: scan your network, add your Tasmota plugs and enter each radiator's power (W).",
      "Settings → Sensors: connect Netatmo so rooms get their temperature.",
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
      "Pre-heat (default 1.0 °C): when an expensive period starts within the look-ahead and the price now is at least 15 % lower, heat up to target + this, so the room can coast. Colder outside → more (up to ×1.5), mild → less (down to ×0.5). Example: 15:00 costs 2.30, 17:00 costs 3.40, 0 °C outside → heat to about 20.3 °C before 17:00.",
      "Peak look-ahead (default 180 min): how far ahead heatbrain looks for an expensive period to pre-heat for.",
      "Cheap boost (default 0.5 °C): in the cheapest hours, heat up to target + this - heat stored when it costs least.",
      "Hysteresis (default 0.3 °C): heating starts at target - 0.3 and stops at target + 0.3, so the relay does not click on and off every few minutes.",
      "Stale after (default 30 min): older temperature readings are not trusted; the room then runs limp mode (a gentle on/off cycle) until fresh readings arrive.",
      "Limp mode ON / OFF (default 10 / 50 min): the cycle used when the temperature is unknown. 0 minutes ON means stay off.",
    ],
  },
  priceOnly: {
    title: "Price-only rooms",
    body: [
      "For rooms without a temperature sensor (like a shed): heatbrain heats in the N cheapest whole hours of each day and never in the no-heat windows. Whole hours, so the relay switches at most once an hour.",
    ],
  },
  sensors: {
    title: "Netatmo and the outdoor temperature",
    body: [
      "Indoor modules give each room its temperature; heatbrain reads them every control step (Netatmo itself updates about every 10 minutes). If a module goes offline, its last reading is used for up to 30 minutes so a restart passes quietly.",
      "The outdoor module scales pre-heating: when it is cold outside rooms cool faster during off periods, so heatbrain stores a little more heat beforehand.",
    ],
  },
  prices: {
    title: "What a kWh costs",
    body: [
      "The price per 15 minutes is the sum of its parts - spot price, grid tariff, Energinet's tariffs, electricity tax and your supplier's markup - with VAT on top. The defaults fit EWII on Radius in DK2; enter your supplier's markup from your contract.",
    ],
  },
  notifications: {
    title: "Notifications",
    body: [
      "Problems (a plug unreachable, a sensor offline or low on battery, missing prices, a room that does not warm up) are sent after they have lasted a while - one missed reading does not wake you. You get a quiet 'resolved' when a problem is over, reminders every few hours while it lasts, and nothing but critical alerts during quiet hours.",
      "Also: a weekly cost summary on Monday morning, a heads-up when tomorrow is unusually expensive or cheap, and one message when a new Tasmota release is out for your plugs (checked daily; you start the upgrade yourself).",
    ],
  },
  away: {
    title: "Away mode",
    body: [
      "Plan a period (from - until) and every room only keeps above the temperature you choose; price-only rooms stay off. It starts and ends by itself - set it the evening before an early flight. Manual overrides and frost protection still work.",
    ],
  },
  plan: {
    title: "The next 24 hours",
    body: [
      "Each room card shows what heatbrain intends for the coming day, worked out from the program and the known prices: heat, pre-heat before an expensive period, coast while it is expensive, a cheap boost, keep above, away or off. Whether it actually heats depends on the room's temperature at the time.",
    ],
  },
  wifiOff: {
    title: "Wi-Fi off period",
    body: [
      "If you switch the Wi-Fi off at night, set the period here - with other times for the weekend if you sleep in (the nights ending Saturday and Sunday morning). Shortly before it starts, heatbrain loads a schedule onto each live plug and the plug switches on its own clock - no network needed. The schedule reaches 3 hours past the planned end, so a late Wi-Fi does not mean limp mode; as soon as readings arrive again, heatbrain takes over.",
      "Below the setting you see when the Wi-Fi was really off on recent nights, from the gaps in the Netatmo readings, and whether the period covered it. 'Use these times' fills in a period that covers them.",
      "There are no temperatures while the Wi-Fi is off (the Netatmo station uses Wi-Fi too), so the schedule is decided in advance: price-only rooms get their cheapest hours, 'off' periods stay off, and 'heat to' / 'keep above' periods heat as much as that room actually heated in such periods over the last week (15 min per hour until there is enough history), placed in the cheapest quarters.",
      "The fail-safe stays in charge: each planned ON ends with its own OFF and an auto-off just longer than it, and limp mode only starts if heatbrain has not taken over 30 minutes after the schedule ends. Plug and sensor outages during the period (and 20 minutes after) are not reported.",
      "A plug whose clock is not synchronised, or whose rule slots 2 and 3 hold your own rules, gets no schedule - you are told, and it runs limp mode for the night.",
    ],
  },
  meter: {
    title: "Your meter (Eloverblik)",
    body: [
      "Eloverblik (Energinet's DataHub) has your electricity meter's readings, whatever your supplier. Log in on eloverblik.dk with MitID, go to Data sharing and create a token, then paste it here and save. heatbrain fetches the readings every 6 hours (or with 'Fetch now'); they arrive about a day late, so the dashboard shows the past, not today.",
      "The dashboard's 'Electricity use' card shows the household's measured use per quarter or hour, with the part heatbrain's radiators drew in the same intervals (live plugs that were on × their power) at the bottom. If the radiators ever look bigger than the measured total, a radiator's power is probably set too high under Settings → Plugs.",
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
  costs: {
    title: "Heating cost",
    body: [
      "Estimated: radiator power × the time the plug was on × the real price in each 15-minute slot. The Sonoff Basic cannot measure power, and a radiator's own thermostat may cut out now and then, so the real figure can be a little lower. 'Saved by timing' compares with paying each day's average price for the same energy.",
    ],
  },
};

// How heatbrain decides, in rule order (for the Help page).
const RULES = [
  ["Above 'never above'", "off - even with a manual ON"],
  ["Manual override (Heat 1 h / Off 2 h)", "on / off (off still protects against frost)"],
  ["Program says off", "off, except frost protection"],
  ["Away period", "keep above the away temperature"],
  ["No fresh temperature", "limp mode: 10 min on / 50 min off"],
  ["Keep-above period", "heat only below the floor"],
  ["Below 'never below'", "heat, whatever the price"],
  ["Expensive now", "coast down to target - peak coasting"],
  ["Expensive soon, cheaper now", "pre-heat to target + boost (more when cold outside)"],
  ["Among the cheapest hours", "heat to target + cheap boost"],
  ["Otherwise", "thermostat: target ± hysteresis"],
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
                           "data-help": [].concat(key).join(",") }, parent, "?");
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
  const order = ["start", "program", "modes", "limits", "strategy", "priceOnly", "plan", "dryrun", "failsafe", "firmware",
                 "sensors", "prices", "meter", "away", "wifiOff", "notifications", "costs", "transfer"];
  const add = (key) => {
    const sec = el("section", { id: `help-${key}` }, card);
    el("h3", {}, sec, HELP[key].title);
    sec.appendChild(helpContent(key));
    const a = el("a", { href: `#help-${key}` }, toc, HELP[key].title);
    a.addEventListener("click", (e) => { e.preventDefault(); sec.scrollIntoView({ behavior: "smooth" }); });
  };
  add("start");
  // The rules, right after getting started
  const rules = el("section", { id: "help-rules" }, card);
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
