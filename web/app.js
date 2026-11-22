"use strict";

// ---------------------------------------------------------------------------------------
// Reason codes from the planner -> human label + heating category (colour).
// Three categories only: the validated palette is safe for 3 colours in any adjacency.
// ---------------------------------------------------------------------------------------
const REASONS = {
  BELOW_TARGET: ["Below target", "comfort"],
  HOLD_ON: ["Heating to target", "comfort"],
  BELOW_COMFORT_DURING_PEAK: ["Too cold to coast", "comfort"],
  PREHEAT_BEFORE_PEAK: ["Pre-heating before peak", "price"],
  CHEAP_BOOST: ["Cheap-price boost", "price"],
  CHEAP_HOURS: ["Cheapest hours", "price"],
  BELOW_MIN: ["Below minimum", "safety"],
  FROST_PROTECTION: ["Frost protection", "safety"],
  STALE_DATA_LIMP: ["No temperature: limp mode", "safety"],
  MANUAL_ON: ["Manual on", "safety"],
  NO_PRICES_FALLBACK: ["No prices: fallback", "safety"],
  PEAK_AVOID: ["Avoiding peak price", null],
  TARGET_REACHED: ["Target reached", null],
  HOLD_OFF: ["Within target band", null],
  ABOVE_MAX: ["Above maximum", null],
  MANUAL_OFF: ["Manual off", null],
  NOT_CHEAP_HOURS: ["Waiting for cheap hours", null],
  AVOID_WINDOW: ["No-heat window", null],
  AWAY_OFF: ["Away", null],
  SCHEDULED_OFF: ["Off by program", null],
  KEEP_ABOVE: ["Keeping above the floor", "comfort"],
  KEEP_SATISFIED: ["Above the floor", null],
  OFFLINE_SCHEDULE: ["Wi-Fi off: night schedule", "price"],
  WINDOW_OPEN: ["Window open? Paused", null],
};
const CATEGORIES = {
  comfort: "Heating: comfort",
  price: "Heating: price-driven",
  safety: "Heating: safety / manual",
};
const reasonLabel = (code) => (REASONS[code] || [code])[0];
const reasonCat = (code) => (REASONS[code] || [null, null])[1];
const catColor = (cat) => `var(--cat-${cat})`;

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------
const state = { status: null, selected: null, hours: 24, history: null, tz: "Europe/Copenhagen" };
const $ = (id) => document.getElementById(id);

async function api(path, opts) {
  const res = await fetch(path, opts);
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body.error || `HTTP ${res.status}`);
  return body;
}

// ---------------------------------------------------------------------------------------
// Formatting (always in the house's time zone, not the browser's)
// ---------------------------------------------------------------------------------------
const fmtCache = {};
function fmt(opts) {
  const key = JSON.stringify(opts) + state.tz;
  return (fmtCache[key] ||= new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, ...opts }));
}
const hm = (ts) => fmt({ hour: "2-digit", minute: "2-digit", hourCycle: "h23" }).format(ts * 1000);
const dayHm = (ts) => fmt({ weekday: "short", hour: "2-digit", minute: "2-digit", hourCycle: "h23" }).format(ts * 1000);
const dayShort = (ts) => fmt({ weekday: "short", day: "numeric" }).format(ts * 1000);
const localHour = (ts) => Number(fmt({ hour: "2-digit", hourCycle: "h23" }).format(ts * 1000));
const temp = (v) => (v == null ? "–" : `${v.toFixed(1)}°`);
const price = (v) => (v == null ? "–" : v.toFixed(2));

function el(tag, attrs = {}, parent, text) {
  const svg = ["svg", "g", "path", "rect", "line", "text", "polyline", "defs", "linearGradient", "stop", "use", "pattern", "circle"].includes(tag);
  const node = svg ? document.createElementNS("http://www.w3.org/2000/svg", tag) : document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) if (v != null) node.setAttribute(k, v);
  if (text != null) node.textContent = text; // untrusted strings: textContent only
  if (parent) parent.appendChild(node);
  return node;
}

function icon(name, cls = "icon") {
  const svg = el("svg", { class: cls, "aria-hidden": "true", "data-icon": name });  // data-icon: for per-icon colours in CSS
  const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
  use.setAttribute("href", `#i-${name}`);
  svg.appendChild(use);
  return svg;
}

function badge(name, tone) {
  const b = el("span", { class: `badge badge-${tone}` });
  b.appendChild(icon(name));
  return b;
}

let toastTimer;
function toast(message, isError = false) {
  const t = $("toast");
  t.textContent = message;
  t.className = `toast${isError ? " error" : ""}`;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, isError ? 8000 : 3500);
}

// Binary search: index of the last element with a[i][0] <= t, or -1.
function lastAtOrBefore(arr, t) {
  let lo = 0, hi = arr.length - 1, ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (arr[mid][0] <= t) { ans = mid; lo = mid + 1; } else hi = mid - 1;
  }
  return ans;
}

// ---------------------------------------------------------------------------------------
// Tooltip
// ---------------------------------------------------------------------------------------
const tooltip = $("tooltip");
function showTooltip(evt, title, rows) {
  tooltip.replaceChildren();
  el("div", { class: "t-time" }, tooltip, title);
  for (const r of rows) {
    const row = el("div", { class: "t-row" }, tooltip);
    const key = el("span", { class: "t-key" }, row);
    const swatch = el("i", {}, key);
    if (r.color) swatch.style.background = r.color;
    if (r.box) { swatch.style.height = "8px"; swatch.style.width = "8px"; swatch.style.borderRadius = "2px"; }
    key.appendChild(document.createTextNode(r.label));
    el("b", {}, row, r.value);
  }
  tooltip.hidden = false;
  const pad = 14, w = tooltip.offsetWidth, h = tooltip.offsetHeight;
  let x = evt.clientX + pad, y = evt.clientY + pad;
  if (x + w > window.innerWidth - 8) x = evt.clientX - w - pad;
  if (y + h > window.innerHeight - 8) y = evt.clientY - h - pad;
  tooltip.style.left = `${Math.max(8, x)}px`;
  tooltip.style.top = `${Math.max(8, y)}px`;
}
const hideTooltip = () => { tooltip.hidden = true; };

// ---------------------------------------------------------------------------------------
// Time axis shared by the charts
// ---------------------------------------------------------------------------------------
function timeTicks(t0, t1, widthPx) {
  const pxPerHour = widthPx / ((t1 - t0) / 3600);
  const step = [1, 2, 3, 6, 12, 24].find((h) => h * pxPerHour >= 64) || 24; // labels ~64px apart
  const ticks = [];
  for (let t = Math.ceil(t0 / 3600) * 3600; t <= t1; t += 3600) {
    const h = localHour(t);
    if (h % step === 0) ticks.push({ t, label: h === 0 ? dayShort(t) : `${String(h).padStart(2, "0")}:00`, major: h === 0 });
  }
  return ticks;
}

function drawTimeAxis(g, x, ticks, top, bottom, labelY) {
  for (const tk of ticks) {
    el("line", { class: "grid", x1: x(tk.t), x2: x(tk.t), y1: top, y2: bottom, opacity: tk.major ? 1 : 0.6 }, g);
    if (labelY != null) el("text", { x: x(tk.t), y: labelY, "text-anchor": "middle" }, g, tk.label);
  }
}

function niceTicks(min, max, count) {
  const span = max - min || 1;
  const raw = span / count;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw);
  // Ticks must enclose the data: the domain is [first tick, last tick].
  const out = [];
  const end = Math.ceil(max / step - 1e-9) * step;
  for (let v = Math.floor(min / step + 1e-9) * step; v <= end + 1e-9; v += step) out.push(+v.toFixed(6));
  return out;
}

// Step path for [t, value] series where each value holds until the next point.
function stepPath(points, x, y, tEnd, maxGap) {
  let d = "";
  for (let i = 0; i < points.length; i++) {
    const [t, v] = points[i];
    if (v == null) continue;
    const next = i + 1 < points.length ? points[i + 1][0] : tEnd;
    const end = next - t > maxGap ? t + maxGap : next;
    const prevOk = i > 0 && points[i - 1][1] != null && t - points[i - 1][0] <= maxGap;
    d += `${prevOk ? "L" : "M"}${x(t).toFixed(1)},${y(v).toFixed(1)}H${x(end).toFixed(1)}`;
  }
  return d;
}

function addCrosshair(svg, x, t0, t1, top, bottom, W, onMove) {
  const cross = el("line", { class: "cross", y1: top, y2: bottom, visibility: "hidden" }, svg);
  const hit = el("rect", { x: 0, y: 0, width: W, height: bottom + 4, fill: "transparent" }, svg);
  const toT = (evt) => {
    const r = svg.getBoundingClientRect();
    const px = ((evt.clientX - r.left) / r.width) * W;
    return t0 + ((px - x(t0)) / (x(t1) - x(t0))) * (t1 - t0);
  };
  hit.addEventListener("pointermove", (evt) => {
    const t = Math.min(t1, Math.max(t0, toT(evt)));
    cross.setAttribute("x1", x(t)); cross.setAttribute("x2", x(t));
    cross.setAttribute("visibility", "visible");
    onMove(t, evt);
  });
  hit.addEventListener("pointerleave", () => { cross.setAttribute("visibility", "hidden"); hideTooltip(); });
}

// ---------------------------------------------------------------------------------------
// Problems banner
// ---------------------------------------------------------------------------------------
function renderProblems() {
  const host = $("problems");
  const problems = state.status.problems || [];
  host.hidden = problems.length === 0;
  host.replaceChildren();
  const order = { critical: 0, warning: 1, info: 2 };
  for (const p of [...problems].sort((a, b) => order[a.severity] - order[b.severity])) {
    const div = el("div", { class: `problem ${p.severity}` }, host);
    const label = el("span", { class: "label" }, div);
    label.appendChild(icon(p.severity === "info" ? "info" : "alert"));
    label.appendChild(document.createTextNode(p.severity === "critical" ? "Action needed" : p.severity === "warning" ? "Problem" : "Note"));
    el("span", {}, div, p.message);
  }
}

// ---------------------------------------------------------------------------------------
// Plan for the next 24 h (from the server: the planner's intent per period)
// ---------------------------------------------------------------------------------------
const PLAN = {
  heat: (t) => (t == null ? "heat (cheap hours)" : `heat to ${temp(t)} as needed`),
  preheat: (t) => `pre-heat (target ${temp(t)})`,
  coast: () => "coast - expensive",
  boost: () => "cheap boost",
  keep: (t) => `keep above ${temp(t)}`,
  idle: () => "warm enough, no heat",
  away: (t) => `away - keep above ${temp(t)}`,
  booked: (t) => `booked - ${temp(t)}`,
  off: () => "off",
  night: () => "Wi-Fi off - heating by night schedule",
  nightoff: () => "Wi-Fi off - off",
};
const planLabel = (s) => (PLAN[s.kind] || (() => s.kind))(s.target);

function planStrip(parent, plan) {
  const t0 = plan[0].from, t1 = plan[plan.length - 1].to;
  const wrap = el("div", { class: "plan" }, parent);
  const head = el("div", { class: "plan-head" }, wrap);
  el("span", {}, head, "Next 24 h");
  if (typeof helpButton === "function") helpButton(head, "plan", wrap);
  const bar = el("div", { class: "planbar" }, wrap);
  for (const s of plan) {
    el("div", { class: `pl-${s.kind}`, style: `flex:${s.to - s.from}`, title: `${hm(s.from)}-${hm(s.to)}: ${planLabel(s)}` }, bar);
  }
  const axis = el("div", { class: "plan-axis" }, wrap);
  el("span", {}, axis, "now");
  el("span", {}, axis, hm(t0 + (t1 - t0) / 2));
  el("span", {}, axis, hm(t1));
  // The next two changes, in words
  // time first ("17:00 off · 21:00 heat to 20.0°"); "as needed" only for now
  const next = plan.slice(1, 3).map((s) => `${hm(s.from)} ${planLabel(s).replace(" as needed", "")}`);
  const text = `Now: ${planLabel(plan[0])}${next.length ? ` · ${next.join(" · ")}` : ""}`;
  el("div", { class: "plan-next", title: text }, wrap, text);
}

// ---------------------------------------------------------------------------------------
// Away / holiday mode
// ---------------------------------------------------------------------------------------
// The outdoor sensor as a card next to the rooms.
function outdoorCard(host) {
  const o = state.status?.outdoor;
  if (!o) return;
  const s = (state.status.sensors || []).find((x) => x.name === o.name && x.kind === "outdoor") || {};
  const card = el("article", { class: `card room outdoor-card${state.selected === "__outdoor" ? " selected" : ""}`, tabindex: 0 }, host);
  card.addEventListener("click", () => selectRoom("__outdoor"));
  card.addEventListener("keydown", (e) => { if (e.key === "Enter") selectRoom("__outdoor"); });
  const top = el("div", { class: "room-top" }, card);
  const name = el("div", { class: "room-name" }, top);
  name.appendChild(badge("snow", "cool"));
  name.appendChild(document.createTextNode("Outside"));
  el("span", { class: "muted" }, top, o.name);
  const hero = el("div", { class: "hero" }, card, temp(o.temperature));
  if (s.humidity != null) {
    const hum = el("small", { class: "hum", title: "Humidity" }, hero);  // like the room cards
    hum.appendChild(icon("drop"));
    hum.appendChild(document.createTextNode(`${Math.round(s.humidity)} %`));
  }
  el("div", { class: "detail" }, card, "Colder outside means more pre-heating before expensive or off periods.");
  const chips = el("div", { class: "plugs" }, el("div", { class: "room-foot" }, card));
  const chip = (cls, iconName, text) => { const c = el("span", { class: `chip ${cls}` }, chips); c.appendChild(icon(iconName)); c.appendChild(document.createTextNode(text)); };
  if (s.battery != null) chip(s.battery <= 20 ? "error" : s.battery <= 40 ? "warn" : "", "battery", `battery ${s.battery} %`);
  if (s.ageMinutes != null) chip("", "clock", `${s.ageMinutes} min ago`);
}

// A spinbox like the ones in the settings: [ - value unit + ]
function makeStepper({ value, step = 1, min, max, unit, label }) {
  const wrap = el("span", { class: `stepper${unit ? " has-unit" : ""}` });
  const input = el("input", { type: "number", step, min, max, inputmode: "decimal", "aria-label": label });
  input.value = value;
  const bump = (dir) => {
    let v = Math.round(((Number(input.value) || 0) + dir * step) / step) * step;
    if (min != null) v = Math.max(min, v);
    if (max != null) v = Math.min(max, v);
    input.value = v.toFixed((String(step).split(".")[1] || "").length);
  };
  const minus = el("button", { type: "button", class: "step", "aria-label": "Decrease", tabindex: -1 }, wrap);
  minus.appendChild(icon("minus"));
  wrap.appendChild(input);
  if (unit) el("span", { class: "unit" }, wrap, unit);
  const plus = el("button", { type: "button", class: "step", "aria-label": "Increase", tabindex: -1 }, wrap);
  plus.appendChild(icon("plus"));
  minus.addEventListener("click", () => bump(-1));
  plus.addEventListener("click", () => bump(1));
  return { wrap, input };
}

let awayFormOpen = null;  // null: follow whether an away period is in place
// Wi-Fi off period: a quiet note while it runs (or is about to), and any plug that missed its schedule.
function renderWifiOff() {
  const bar = $("wifi-bar");
  const w = state.status?.wifiOff;
  const now = state.status?.now || Date.now() / 1000;
  const soon = w && w.start - now < 30 * 60;
  bar.replaceChildren();
  bar.hidden = !w || !(w.active || soon || w.errors.length);
  if (bar.hidden) return;
  bar.appendChild(badge("wifi", "gray"));
  const t = el("div", { class: "grow" }, bar);
  el("b", {}, t, w.late ? `Wi-Fi is off longer than planned (until ${hm(w.end)})`
    : w.active ? `Wi-Fi off until ${hm(w.end)}` : `Wi-Fi goes off at ${hm(w.start)}`);
  const plugs = w.armedPlugs.length
    ? `${w.armedPlugs.join(", ")} follow${w.armedPlugs.length === 1 ? "s" : ""} the night schedule loaded onto the plug${w.scheduleUntil ? ` (it reaches until ${hm(w.scheduleUntil)}; heatbrain takes over as soon as the Wi-Fi is back)` : ""}`
    : "Live plugs get their night schedule shortly before";
  el("div", { class: "muted" }, t, `${plugs}. Expected connection problems are not reported until ${hm(w.end + 20 * 60)}.`);
  for (const e of w.errors) el("div", { class: "wifi-error" }, t, `Not loaded: ${e} - this plug runs limp mode while the Wi-Fi is off.`);
}

// Away: the header button opens and closes the panel (lit while a period is planned or running).
// The panel shows the period with "end"/"cancel", or the form. The form keeps what was typed
// across the 30 s refresh.
let awayDraft = null;
function renderAway() {
  const bar = $("away-bar"), headerBtn = $("away-btn");
  const away = state.status?.away;
  headerBtn.hidden = !state.status?.rooms?.length;
  if (awayFormOpen === null) awayFormOpen = !!away;  // a period in place: shown until closed
  headerBtn.setAttribute("aria-pressed", String(!!awayFormOpen));
  headerBtn.classList.toggle("lit", !!away);
  headerBtn.title = away ? (away.active ? `Away until ${dayHm(away.until)}` : `Away planned from ${dayHm(away.from)}`) : "Plan an away period";
  bar.replaceChildren();
  bar.hidden = !awayFormOpen;
  if (!awayFormOpen) return;
  bar.appendChild(badge("plane", "violet"));
  const t = el("div", { class: "grow" }, bar);
  if (away) {
    if (away.active) {
      el("b", {}, t, `Away until ${dayHm(away.until)}`);
      el("div", { class: "muted" }, t, `Every room keeps above ${temp(away.keepAbove)}C; programs resume afterwards.`);
    } else {
      el("b", {}, t, `Away planned: ${dayHm(away.from)} → ${dayHm(away.until)}`);
      el("div", { class: "muted" }, t, `Starts by itself; every room will keep above ${temp(away.keepAbove)}C.`);
    }
    const end = el("button", { type: "button", class: away.active ? "primary" : "away-cancel" }, bar);
    end.appendChild(icon(away.active ? "home" : "trash"));
    end.appendChild(document.createTextNode(away.active ? "I'm home - end now" : "Cancel plan"));
    end.addEventListener("click", async () => {
      end.disabled = true;
      try {
        await api("/api/away", { method: "DELETE" });
        toast(away.active ? "Welcome home - programs are back" : "Away plan cancelled");
        awayFormOpen = false;
        refreshAfterTick();
      } catch (e) { toast(e.message, true); end.disabled = false; }
    });
    return;
  }
  const ga = el("b", {}, t, "Going away? ");
  if (typeof helpButton === "function") helpButton(ga, "away", t);
  el("div", { class: "muted" }, t, "Plan it ahead: it starts and ends by itself. Every room only keeps above the temperature you choose.");
  const local = (d) => new Date(d.getTime() - d.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
  if (!awayDraft) {
    const f = new Date(); f.setMinutes(0, 0, 0); f.setHours(f.getHours() + 1);
    const u = new Date(); u.setDate(u.getDate() + 2); u.setHours(18, 0, 0, 0);
    awayDraft = { from: local(f), until: local(u), keep: 12 };
  }
  const d = awayDraft;
  const dt = (label, key) => {
    const l = el("label", { class: "field" }, bar);
    el("span", {}, l, label);
    const i = el("input", { type: "datetime-local", "aria-label": `Away ${label.toLowerCase()}`, value: d[key] }, l);
    i.addEventListener("input", () => { d[key] = i.value; });
  };
  dt("From", "from");
  dt("Until", "until");
  const keepLabel = el("label", { class: "field" }, bar);
  el("span", {}, keepLabel, "Keep above");
  const { wrap: keepBox, input: keep } = makeStepper({ value: d.keep, step: 0.5, min: 5, max: 20, unit: "°C", label: "Keep above" });
  keep.addEventListener("input", () => { d.keep = Number(keep.value); });
  keep.addEventListener("change", () => { d.keep = Number(keep.value); });
  keepLabel.appendChild(keepBox);
  const start = el("button", { type: "button", class: "primary" }, bar);
  start.appendChild(icon("plane"));
  start.appendChild(document.createTextNode("Save away period"));
  start.addEventListener("click", async () => {
    const fromTs = Math.floor(new Date(d.from).getTime() / 1000);
    const untilTs = Math.floor(new Date(d.until).getTime() / 1000);
    start.disabled = true;
    try {
      await api("/api/away", { method: "POST", headers: { "Content-Type": "application/json" },
                               body: JSON.stringify({ from: fromTs, until: untilTs, keepAbove: Number(keep.value) }) });
      awayDraft = null;  // the panel now shows the saved period
      toast(fromTs * 1000 > Date.now() ? "Away mode planned" : "Away mode started");
      refreshAfterTick();
    } catch (e) { toast(e.message, true); start.disabled = false; }
  });
}
$("away-btn").addEventListener("click", () => {
  awayFormOpen = !awayFormOpen;
  if (location.hash.startsWith("#settings") || location.hash.startsWith("#help")) { location.hash = ""; awayFormOpen = true; }
  renderAway();
  if (awayFormOpen) window.scrollTo({ top: 0, behavior: "smooth" });
});

// Book: the header button opens a panel to book a room (preselected: the room whose chart is open).
// The room warms up early by itself and follows its program again afterwards.
let bookOpen = false;
let bookDraft = null;
const pad2 = (n) => String(n).padStart(2, "0");
const isoDate = (d) => `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
const bookableRooms = () => (state.status?.rooms || []).filter((r) => !r.priceOnly);

function renderBook() {
  const bar = $("book-bar"), headerBtn = $("book-btn");
  const rooms = bookableRooms();
  headerBtn.hidden = !rooms.length;
  headerBtn.setAttribute("aria-pressed", String(bookOpen));
  bar.replaceChildren();
  bar.hidden = !bookOpen || !rooms.length;
  if (bar.hidden) return;
  if (!bookDraft || !rooms.some((r) => r.id === bookDraft.room)) {
    let target = 22;
    try { target = Number(localStorage.getItem("hb.bookingTarget")) || 22; } catch { /* private mode */ }
    const room = rooms.find((r) => r.id === state.selected) || rooms[0];
    bookDraft = { room: room.id, date: isoDate(new Date()), from: "16:00", until: "23:00", target };
  }
  const d = bookDraft;
  const roomOf = () => rooms.find((r) => r.id === d.room);
  const maxFor = () => (roomOf()?.maxTemp ?? 30) - 0.5;  // never above the room's maximum
  d.target = Math.min(d.target, maxFor());
  bar.appendChild(badge("calendar", "violet"));
  const t = el("div", { class: "grow" }, bar);
  const ga = el("b", {}, t, d.id ? "Change the booking " : "Book a room ");
  if (typeof helpButton === "function") helpButton(ga, "bookings", t);
  el("div", { class: "muted" }, t, "It starts heating early enough by itself and goes back to the program afterwards. An end before the start means the next day.");
  const field = (label) => { const l = el("label", { class: "field" }, bar); el("span", {}, l, label); return l; };
  const sel = el("select", { "aria-label": "Room" }, field("Room"));
  for (const r of rooms) {
    const o = el("option", { value: r.id }, sel, r.name);
    if (r.id === d.room) o.selected = true;
  }
  sel.addEventListener("change", () => { d.room = sel.value; renderBook(); });
  for (const [label, key, attrs] of [["Day", "date", { type: "date" }], ["From", "from", { type: "time", step: 900 }], ["Until", "until", { type: "time", step: 900 }]]) {
    const i = el("input", { ...attrs, value: d[key], "aria-label": label }, field(label));
    i.addEventListener("input", () => { d[key] = i.value; });
  }
  const { wrap: tBox, input: tIn } = makeStepper({ value: d.target, step: 0.5, min: 5, max: maxFor(), unit: "°C", label: "Temperature" });
  const syncT = () => { d.target = Number(tIn.value); };
  tIn.addEventListener("input", syncT);
  tIn.addEventListener("change", syncT);
  field("Temperature").appendChild(tBox);
  const save = el("button", { type: "button", class: "primary" }, bar);
  save.appendChild(icon("check"));
  save.appendChild(document.createTextNode(d.id ? "Save changes" : "Book"));
  const err = el("div", { class: "hint", style: "color:var(--critical);flex-basis:100%" }, bar);
  save.addEventListener("click", async () => {
    syncT();
    const room = roomOf();
    const from = new Date(`${d.date}T${d.from}`);
    const until = new Date(`${d.date}T${d.until}`);
    if (until <= from) until.setDate(until.getDate() + 1);
    if (room && d.target > room.maxTemp - 0.5) { err.textContent = `${room.name} never heats above ${temp(room.maxTemp)}: choose at most ${temp(room.maxTemp - 0.5)}`; return; }
    save.disabled = true;
    err.textContent = "";
    try {
      const body = JSON.stringify({ room: d.room, from: Math.floor(from / 1000), until: Math.floor(until / 1000), target: d.target });
      const headers = { "Content-Type": "application/json" };
      if (d.id) {
        await api(`/api/bookings/${encodeURIComponent(d.id)}`, { method: "PUT", headers, body });
        const sent = JSON.parse(body);
        bookingEdited.set(d.id, { room: sent.room, from: sent.from, until: sent.until, target: sent.target });
      }
      else await api(`/api/rooms/${encodeURIComponent(d.room)}/bookings`, { method: "POST", headers, body });
      try { localStorage.setItem("hb.bookingTarget", String(d.target)); } catch { /* private mode */ }
      toastMaybe(d.id ? "Booking changed" : `${room?.name || d.room} booked`);
      bookOpen = false;
      bookDraft = null;
      renderBook();
      renderBookingsCard();
      refreshAfterTick();
    } catch (e) { err.textContent = e.message; save.disabled = false; }
  });
}
$("book-btn").addEventListener("click", () => {
  bookOpen = !bookOpen;
  if (bookDraft?.id) bookDraft = null;  // the button books anew; editing starts from a booking's Edit
  if (location.hash.startsWith("#settings") || location.hash.startsWith("#help")) { location.hash = ""; bookOpen = true; }
  renderBook();
  if (bookOpen) window.scrollTo({ top: 0, behavior: "smooth" });
});

const bookingRemoved = new Set();  // cancelled here, still in the last status
const bookingEdited = new Map();   // id -> {room, from, until, target} changed here, not yet in the status

// A booking's Edit: the Book panel, filled in with it.
function editBooking(b) {
  const f = new Date(b.from * 1000), u = new Date(b.until * 1000);
  const hmLocal = (d) => `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  bookDraft = { id: b.id, room: b.room, date: isoDate(f), from: hmLocal(f), until: hmLocal(u), target: b.target };
  bookOpen = true;
  renderBook();
  window.scrollTo({ top: 0, behavior: "smooth" });
}

// Every booking in one card above the rooms (only when there are any), soonest first.
function renderBookingsCard() {
  const card = $("bookings");
  // Until the server's next step reflects a change, show it as made: cancelled ones gone, edited
  // ones with their new room, time and temperature.
  const fromServer = (state.status?.rooms || []).flatMap((r) => (r.bookings || []).map((b) => ({ ...b, room: r.id, roomName: r.name })));
  for (const id of [...bookingRemoved]) if (!fromServer.some((b) => b.id === id)) bookingRemoved.delete(id);
  for (const [id, e] of [...bookingEdited]) {
    const b = fromServer.find((x) => x.id === id);
    if (!b || (b.room === e.room && b.from === e.from && b.until === e.until && b.target === e.target)) bookingEdited.delete(id);
  }
  const roomName = (id) => (state.status?.rooms || []).find((r) => r.id === id)?.name || id;
  const all = fromServer.filter((b) => !bookingRemoved.has(b.id))
    .map((b) => (bookingEdited.has(b.id) ? { ...b, ...bookingEdited.get(b.id), roomName: roomName(bookingEdited.get(b.id).room) } : b))
    .sort((a, b) => a.from - b.from);
  card.hidden = !all.length;
  card.replaceChildren();
  if (!all.length) return;
  const head = el("div", { class: "card-head" }, card);
  const h2 = el("h2", {}, head);
  h2.appendChild(badge("calendar", "violet"));
  h2.appendChild(document.createTextNode("Bookings"));
  if (typeof helpButton === "function") helpButton(h2, "bookings", head);
  const list = el("div", { class: "bk-list" }, card);
  for (const b of all) bookingRow(list, b);
}

// One booking: room and time, when warming starts (and on what basis), the target, and end/cancel.
function bookingRow(host, b) {
  const now = Date.now() / 1000;
  const underway = b.active && now >= b.from, warming = b.active && !underway;
  const row = el("div", { class: `bk-row${b.active ? " active" : ""}` }, host);
  row.appendChild(badge(underway || warming ? "flame" : "calendar", underway || warming ? "warm" : "violet"));
  const main = el("div", { class: "bk-main" }, row);
  const what = el("div", { class: "bk-what" }, main);
  el("b", {}, what, b.roomName);
  what.appendChild(document.createTextNode(` · ${dayHm(b.from)}–${hm(b.until)}`));
  const line = (iconName, tone, text, title) => {
    const l = el("div", { class: `bk-line tone-${tone}`, title: title || "" }, main);
    l.appendChild(icon(iconName));
    l.appendChild(document.createTextNode(text));
  };
  const h = b.warmupMinutes / 60;
  const dur = h >= 10 ? `${Math.round(h)} h` : `${Math.round(h * 2) / 2} h`;
  const how = b.basis === "learned" ? `about ${dur}, from how fast the room has warmed before`
    : `${dur} as set for the room, until heatbrain has learned how fast it warms`;
  if (underway) line("flame", "orange", `Booked until ${hm(b.until)}, then back to the program`);
  else if (warming) line("flame", "orange", `Warming up since ${dayHm(b.warmupFrom)} (${how})`);
  else line("clock", "violet", `Starts warming ${dayHm(b.warmupFrom)} (${how})`);
  if (b.reachable === false) line("alert", "red", `May not reach ${temp(b.target)} in time`);
  const tgt = el("span", { class: "chip bk-target" }, row);
  tgt.appendChild(icon("thermo"));
  tgt.appendChild(document.createTextNode(temp(b.target)));
  const ed = el("button", { type: "button", class: "ghost bk-edit", title: "Change the time, temperature or room" }, row);
  ed.appendChild(icon("edit"));
  ed.appendChild(document.createTextNode("Edit"));
  ed.addEventListener("click", () => editBooking(b));
  const x = el("button", { type: "button", class: "ghost bk-x", title: b.active ? "End the booking now" : "Cancel the booking" }, row);
  x.appendChild(icon(b.active ? "undo" : "trash"));
  x.appendChild(document.createTextNode(b.active ? "End" : "Cancel"));
  x.addEventListener("click", async () => {
    x.disabled = true;
    try {
      await api(`/api/bookings/${encodeURIComponent(b.id)}`, { method: "DELETE" });
      toastMaybe(b.active ? "Booking ended - back to the program" : "Booking cancelled");
      bookingRemoved.add(b.id);
      if (bookDraft?.id === b.id) { bookOpen = false; bookDraft = null; renderBook(); }  // its change panel goes too
      renderBookingsCard();
      refreshAfterTick();
    } catch (err) { alertInline(err); x.disabled = false; }
  });
}

// ---------------------------------------------------------------------------------------
// Room cards
// ---------------------------------------------------------------------------------------
let renderRooms = function () {
  const host = $("rooms");
  host.replaceChildren();
  if (!state.status.rooms.length) {
    const empty = el("div", { class: "card empty-state" }, host);
    empty.appendChild(badge("home", "violet"));
    el("div", { style: "font-weight:600;font-size:16px" }, empty, "No rooms yet");
    const p = el("div", {}, empty, "Add plugs and rooms in ");
    el("a", { href: "#settings" }, p, "Settings");
    host.style.gridTemplateColumns = "1fr";
    return;
  }
  host.style.gridTemplateColumns = "";
  for (const r of state.status.rooms) {
    const mood = r.on ? "heating" : "idle";
    const card = el("article", { class: `card room ${mood}${state.selected === r.id ? " selected" : ""}`, tabindex: 0 }, host);
    card.addEventListener("click", () => selectRoom(r.id));
    card.addEventListener("keydown", (e) => { if (e.key === "Enter") selectRoom(r.id); });

    const top = el("div", { class: "room-top" }, card);
    const name = el("div", { class: "room-name" }, top);
    name.appendChild(r.on ? badge("flame", "warm") : badge("snow", "cool"));
    name.appendChild(document.createTextNode(r.name));
    const chip = el("span", { class: `state${r.on ? " on" : ""}` }, top);
    const cat = r.on ? reasonCat(r.reason) : null;
    chip.appendChild(icon(r.on ? "flame" : "power"));
    chip.appendChild(document.createTextNode(r.on ? "Heating" : "Off"));

    const inp = r.inputs || {};
    // Price-only rooms have no temperature: the current price is what drives them.
    const hero = el("div", { class: "hero" }, card, r.priceOnly ? price(inp.price) : temp(inp.temperature));
    // Only the unit for price-only rooms: the target and mode are in the reason just below.
    if (r.priceOnly) el("small", {}, hero, `${state.status.currency}/kWh · no sensor`);
    if (!r.priceOnly && r.humidity != null) {
      const hum = el("small", { class: "hum", title: "Humidity" }, hero);
      hum.appendChild(icon("drop"));
      hum.appendChild(document.createTextNode(`${Math.round(r.humidity)} %`));
    }

    const reason = el("div", { class: "reason" }, card);
    if (cat) el("span", { class: "swatch", style: `background:${catColor(cat)}` }, reason);
    reason.appendChild(document.createTextNode(reasonLabel(r.reason)));
    el("div", { class: "detail" }, card, r.detail);
    // Only when something outranks the override (e.g. above max); otherwise the reason says it.
    if (r.override && !["MANUAL_ON", "MANUAL_OFF"].includes(r.reason)) {
      el("div", { class: "override-note" }, card, `Manual ${r.override.on ? "ON" : "OFF"} until ${hm(r.override.until)}`);
    }

    // Pinned to the bottom so plan, plugs and buttons line up across cards of different text length
    const foot = el("div", { class: "room-foot" }, card);
    if (r.plan?.length) planStrip(foot, r.plan);
    // Only what's worth knowing: a plug in dry-run, on its night schedule, or with a problem. Live and
    // fine is the normal state - the card's on/off badge already tells it.
    const plugs = el("div", { class: "plugs" }, foot);
    for (const p of r.plugs || []) {
      let text = null, cls = "", ic = "plug";
      if (p.thermostat && !p.error) {
        // A thermostat: always worth showing what it is (or would be) set to.
        text = `${p.id} · ${p.mode === "live" ? "set to" : "would set"} ${Number(p.setpoint).toFixed(1)}°${p.floor != null ? ` · floor ${p.floor.toFixed(1)}°` : ""}`;
        ic = "thermo"; cls = p.mode === "live" ? "" : "dryrun";
      } else if (p.error) { text = `${p.id} · ${p.error}`; cls = "error"; ic = "alert"; }
      else if (p.schedule) { text = `${p.id} · night schedule`; ic = "wifi"; }
      else if (p.mode !== "live") { text = `${p.id} · dry-run`; cls = "dryrun"; }
      if (!text) continue;
      const chipEl = el("span", { class: `chip ${cls}`, title: p.error || text }, plugs);
      chipEl.appendChild(icon(ic));
      el("span", { class: "chip-text" }, chipEl, text);
    }

    const actions = el("div", { class: "actions" }, foot);
    const act = (label, iconName, cls, fn) => {
      const b = el("button", { type: "button", class: cls }, actions);
      b.appendChild(icon(iconName));
      b.appendChild(document.createTextNode(label));
      b.addEventListener("click", (e) => { e.stopPropagation(); fn(b); });
    };
    act("Heat 1 h", "flame", "act-heat", (b) => setOverride(r.id, true, 60, b));
    act("Off 2 h", "power", "act-off", (b) => setOverride(r.id, false, 120, b));
    if (r.override) act("Back to auto", "undo", "", (b) => clearOverride(r.id, b));
  }
}

// After a change (booking, away, override) the server re-plans at once, but the control step takes a
// few seconds (sensors, plugs): ask again until the status of that new step is in, at most ~20 s.
let tickWait = null;
function refreshAfterTick() {
  const before = state.status?.now;
  if (tickWait) clearTimeout(tickWait);
  let tries = 0;
  const poll = async () => {
    tickWait = null;
    await refreshStatus();
    if (state.status?.now === before && ++tries < 14) tickWait = setTimeout(poll, 1500);
  };
  tickWait = setTimeout(poll, 1200);
}

function toastMaybe(msg) { if (typeof toast === "function") toast(msg); else $("updated").textContent = msg; }

// Room cards, then the outdoor card (kept as a separate step so it is added exactly once).
const renderRoomCards = renderRooms;
renderRooms = function () { renderRoomCards(); if (state.status?.rooms?.length) outdoorCard($("rooms")); };

async function setOverride(id, on, minutes, button) {
  button.disabled = true;
  try {
    await api(`/api/rooms/${encodeURIComponent(id)}/override`, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ on, minutes }),
    });
    refreshAfterTick();
  } catch (e) { alertInline(e); } finally { button.disabled = false; }
}

async function clearOverride(id, button) {
  button.disabled = true;
  try {
    await api(`/api/rooms/${encodeURIComponent(id)}/override`, { method: "DELETE" });
    refreshAfterTick();
  } catch (e) { alertInline(e); } finally { button.disabled = false; }
}

function alertInline(e) { $("updated").textContent = `error: ${e.message}`; }

// ---------------------------------------------------------------------------------------
// Room detail: temperature + heating strip + price, one shared time axis, no dual axes.
// ---------------------------------------------------------------------------------------
async function selectRoom(id, scroll = true) {
  state.selected = id;
  renderRooms();
  $("detail").hidden = false;
  await loadHistory();
  if (scroll) $("detail").scrollIntoView({ behavior: "smooth", block: "nearest" });
}

async function loadHistory() {
  if (!state.selected) return;
  $("room-chart").classList.add("loading"); // refetch keeps the frame
  try {
    if (state.selected === "__outdoor") {
      const o = state.status?.outdoor;
      if (!o) return;
      const sh = await api(`/api/sensor-history?key=${encodeURIComponent(o.key)}&hours=${state.hours}`);
      state.history = { room: { id: "__outdoor", name: `Outside · ${o.name}`, sensorOnly: true, priceOnly: false, minTemp: 0, maxTemp: 0 },
                        from: sh.from, to: sh.to, temperatures: sh.temperatures, decisions: [], events: [], prices: sh.prices, wifi: sh.wifi };
    } else {
      state.history = await api(`/api/rooms/${encodeURIComponent(state.selected)}/history?hours=${state.hours}`);
    }
    if (!state.mixAt || Date.now() - state.mixAt > 5 * 60_000) {
      try { state.mix = await api("/api/power-mix"); state.mixAt = Date.now(); } catch { /* optional */ }
    }
    renderDetail();
  } catch (e) { alertInline(e); } finally { $("room-chart").classList.remove("loading"); }
}

function heatingSegments(decisions, tick, tEnd) {
  const segs = [];
  for (let i = 0; i < decisions.length; i++) {
    const [t, on, reason] = decisions[i];
    const next = i + 1 < decisions.length ? decisions[i + 1][0] : Math.min(tEnd, t + tick);
    const end = next - t > 3 * tick ? t + tick : next; // controller was down: don't bridge
    if (!on) continue;
    const cat = reasonCat(reason) || "comfort";
    const last = segs[segs.length - 1];
    if (last && last.cat === cat && Math.abs(last.end - t) < 1) last.end = end;
    else segs.push({ start: t, end, cat, reason });
  }
  return segs;
}

// The electricity card under the room chart: the price and the power mix of the price area, on
// the same time axis as the room chart above (so the hours line up), each with its own "now".
// Either can be switched off in Settings -> System; with both off the card is hidden.
function renderElectricity({ t0, t1, now, prices, W, m, ticks }) {
  const card = $("electricity"), host = $("electricity-chart");
  const show = state.status?.dashboard || {};
  const mix = show.powerMix === false ? [] : (state.mix?.hours || []).filter((r) => r[0] + 3600 > t0 && r[0] < t1);
  const showPrice = show.price !== false && prices.length > 0;
  card.hidden = !showPrice && !mix.length;
  host.replaceChildren();
  $("price-summary").textContent = showPrice ? priceSummary() : "";
  if (card.hidden) return;
  $("electricity-title").textContent = showPrice && mix.length ? "Electricity price and power mix" : showPrice ? "Electricity price" : "Power mix";

  const panelH = 90;
  const priceTop = showPrice ? 18 : 0;
  const mixTop = (showPrice ? priceTop + panelH + 30 : 18), mixH = mix.length ? panelH : 0;
  const bottom = mix.length ? mixTop + mixH : priceTop + panelH;
  const H = bottom + 24;
  const x = (t) => m.l + ((t - t0) / (t1 - t0)) * (W - m.l - m.r);
  const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, height: H, role: "img", "aria-label": "Electricity price and power mix" }, host);
  if (t1 > now + 60) el("rect", { x: x(now), y: 18, width: x(t1) - x(now), height: bottom - 18, fill: "var(--grid)", opacity: 0.35 }, svg);

  if (showPrice) {
    el("text", { class: "panel-label", x: m.l, y: priceTop - 8 }, svg, `Price ${state.status?.currency || ""}/kWh`);
    const pv = prices.map((p) => p[1]);
    const pTicks = niceTicks(Math.min(0, ...pv), Math.max(...pv), 2);
    const plo = pTicks[0], phi = pTicks[pTicks.length - 1];
    const yP = (v) => priceTop + (1 - (v - plo) / (phi - plo)) * panelH;
    for (const v of pTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: yP(v), y2: yP(v) }, svg);
      el("text", { x: m.l - 6, y: yP(v) + 4, "text-anchor": "end" }, svg, v.toFixed(1));
    }
    drawTimeAxis(svg, x, ticks, priceTop, priceTop + panelH, mix.length ? null : H - 6);
    const grad = el("linearGradient", { id: "price-fill", x1: 0, y1: 0, x2: 0, y2: 1 }, el("defs", {}, svg));
    el("stop", { offset: "0", "stop-color": "#fab005", "stop-opacity": 0.35 }, grad);
    el("stop", { offset: "1", "stop-color": "#fab005", "stop-opacity": 0.02 }, grad);
    const line = stepPath(prices, x, yP, t1, 900);
    // The fill as an attribute: url(#id) in the stylesheet would resolve against style.css.
    el("path", { d: `${line}V${yP(plo)}H${x(prices[0][0])}Z`, fill: "url(#price-fill)", stroke: "none" }, svg);
    el("path", { class: "line", d: line, "stroke-width": 1.5 }, svg);
  }
  if (mix.length) {
    // A 100 % column per hour; the label row doubles as its legend (direct labels, no extra legend).
    const lbl = el("text", { class: "panel-label", x: m.l, y: mixTop - 6 }, svg, `Power mix ${state.mix.area || ""}`);
    let lx = m.l + lbl.getComputedTextLength() + 14;
    for (const [key, name] of MIX_PARTS) {
      el("rect", { x: lx, y: mixTop - 14, width: 9, height: 9, rx: 2, style: `fill: var(--mix-${key})` }, svg);
      const t = el("text", { class: "panel-note", x: lx + 13, y: mixTop - 6 }, svg, name);
      lx += 13 + t.getComputedTextLength() + 12;
    }
    // A % axis and the time grid. Columns stack from the bottom (wind first), so the share of
    // wind - and wind + solar - reads straight off the axis.
    const yM = (share) => mixTop + (1 - share) * mixH;
    for (const v of [0, 0.5, 1]) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: yM(v), y2: yM(v) }, svg);
      el("text", { x: m.l - 6, y: yM(v) + 4, "text-anchor": "end" }, svg, `${v * 100}%`);
    }
    drawTimeAxis(svg, x, ticks, mixTop, mixTop + mixH, H - 6);
    for (const r of mix) {
      const parts = mixParts(r);
      const total = parts.reduce((a, p) => a + p.mw, 0);
      if (total <= 0) continue;
      const x0 = x(Math.max(r[0], t0)), x1 = x(Math.min(r[0] + 3600, t1, now));  // the hour so far
      let share = 0;
      for (const p of parts) {
        const part = p.mw / total;
        if (part > 0) el("rect", { x: x0, y: yM(share + part), width: Math.max(1, x1 - x0), height: part * mixH, style: `fill: var(--mix-${p.key})` }, svg);
        share += part;
      }
    }
  }
  const nowT = state.status?.now;
  if (nowT && nowT >= t0 && nowT <= t1) {
    el("line", { class: "now", x1: x(nowT), x2: x(nowT), y1: showPrice ? priceTop : mixTop, y2: bottom }, svg);
  }
  addCrosshair(svg, x, t0, t1, showPrice ? priceTop : mixTop, bottom, W, (t, evt) => {
    const rows = [];
    const pi = showPrice ? lastAtOrBefore(prices, t) : -1;
    if (pi >= 0 && t - prices[pi][0] < 900) rows.push({ label: `${state.status?.currency || ""}/kWh`, value: price(prices[pi][1]) });
    const mr = mix.find((r) => t >= r[0] && t < r[0] + 3600);
    if (mr) {
      const parts = mixParts(mr);
      const total = parts.reduce((a, p) => a + p.mw, 0);
      if (total > 0) {
        for (const [p, [, name]] of parts.map((p, i) => [p, MIX_PARTS[i]]).reverse()) {
          rows.push({ label: name, value: `${Math.round((p.mw / total) * 100)} %`, color: `var(--mix-${p.key})`, box: true });
        }
        if (mr[6] != null) rows.push({ label: "CO₂", value: `${Math.round(mr[6])} g/kWh` });
      }
    }
    if (!rows.length) return hideTooltip();
    showTooltip(evt, dayHm(t), rows);
  });
}

// The power mix lane: [epoch, offshore wind, onshore wind, solar, power plants, imports, co2] per hour.
const MIX_PARTS = [["wind", "Wind"], ["solar", "Solar"], ["plants", "Power plants"], ["imports", "Imports"]];
function mixParts(r) {
  return [{ key: "wind", mw: (r[1] || 0) + (r[2] || 0) }, { key: "solar", mw: r[3] || 0 },
          { key: "plants", mw: r[4] || 0 }, { key: "imports", mw: r[5] || 0 }];
}

// "now 3.26 DKK/kWh · today low 2.21 at 23:45 · high 3.41 at 20:00", from the status' prices.
function priceSummary() {
  const s = state.status;
  const pts = s?.prices || [];
  if (!pts.length) return "";
  const today = pts.filter((p) => dayShort(p[0]) === dayShort(s.now));
  const cur = pts[lastAtOrBefore(pts, s.now)];
  const pieces = [];
  if (cur) pieces.push(`now ${price(cur[1])}`);
  if (today.length) {
    const lo = today.reduce((a, b) => (b[1] < a[1] ? b : a));
    const hi = today.reduce((a, b) => (b[1] > a[1] ? b : a));
    pieces.push(`today low ${price(lo[1])} at ${hm(lo[0])}`, `high ${price(hi[1])} at ${hm(hi[0])}`);
  }
  return pieces.join(" · ");
}

function renderLegend(room, segs, wifi) {
  const host = $("legend");
  host.replaceChildren();
  if (wifi) {
    const o = el("span", {}, host); el("i", { class: "key-wifi-on" }, o); o.appendChild(document.createTextNode("Wi-Fi on"));
    const s = el("span", {}, host); el("i", { class: "key-wifi-off" }, s); s.appendChild(document.createTextNode("Wi-Fi off"));
    const p = el("span", {}, host); el("i", { class: "key-wifi-planned" }, p); p.appendChild(document.createTextNode("Wi-Fi off: planned"));
  }
  if (!room.priceOnly) {
    const a = el("span", {}, host); el("i", { class: "key-line" }, a); a.appendChild(document.createTextNode("Temperature"));
    if (state.history?.temperatures?.some((p, i, arr) => i > 0 && p[0] - arr[i - 1][0] > 1800)) {
      const e = el("span", {}, host); el("i", { class: "key-estimate" }, e); e.appendChild(document.createTextNode("Estimate (no readings)"));
    }
    if (room.sensorOnly) return;  // a sensor: no target, no heating
    const b = el("span", {}, host); el("i", { class: "key-dash" }, b); b.appendChild(document.createTextNode("Target"));
  }
  for (const [cat, label] of Object.entries(CATEGORIES)) {
    const s = el("span", {}, host);
    el("i", { class: "swatch", style: `background:${catColor(cat)}` }, s);
    s.appendChild(document.createTextNode(label));
  }
}

function renderDetail() {
  const h = state.history;
  const room = h.room;
  $("detail-title").textContent = room.name;
  const tick = state.status?.tickSeconds || 300;
  const host = $("room-chart");
  host.replaceChildren();

  // Ahead of now: every known price (tomorrow's from about 13:00), at most as far ahead as the
  // chart looks back - the room chart is also where the coming prices are shown.
  const t0 = h.from;
  const lastPrice = h.prices.length ? h.prices[h.prices.length - 1][0] + 900 : h.to;
  const t1 = Math.max(h.to, Math.min(h.to + (h.to - h.from), lastPrice));
  const temps = h.temperatures, decisions = h.decisions;
  const prices = h.prices.filter((p) => p[0] >= t0 - 900 && p[0] <= t1);
  const targets = decisions.map((d) => [d[0], d[3]]);
  // Ahead of now the target comes from the room's plan, so the line doesn't seem to stop at the last
  // decision (an off period has no target: a gap, then the line where heating resumes).
  const roomPlan = state.status?.rooms?.find((r) => r.id === room.id)?.plan || [];
  const lastDecision = decisions.length ? decisions[decisions.length - 1][0] : h.to;
  const step = Math.min(900, tick);  // within stepPath's gap limit, so the steps join up
  for (const seg of roomPlan) {
    for (let t = Math.max(seg.from, lastDecision + step); t < seg.to && t <= t1; t += step) {
      targets.push([t, seg.kind === "off" || seg.kind === "nightoff" ? null : seg.target ?? null]);
    }
  }
  const segs = heatingSegments(decisions, tick, h.to);
  const wifi = h.wifi && (h.wifi.seen.length || h.wifi.planned.length) ? h.wifi : null;
  renderLegend(room, segs, wifi);

  const W = host.clientWidth, m = { l: 40, r: 8 };
  const showTemp = !room.priceOnly;
  const tempTop = 18, tempH = showTemp ? 190 : 0;
  const showStrip = !room.sensorOnly;
  const wifiTop = tempTop + tempH + (showTemp ? 34 : 0), wifiH = wifi ? 10 : 0;
  const stripTop = wifiTop + (wifi ? wifiH + 30 : 0), stripH = showStrip ? 12 : 0;
  const bottom = showStrip ? stripTop + stripH : wifi ? wifiTop + wifiH : tempTop + tempH;
  const H = bottom + 24;
  const x = (t) => m.l + ((t - t0) / (t1 - t0)) * (W - m.l - m.r);
  const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, height: H, role: "img", "aria-label": `${room.name}: temperature and heating` }, host);
  const ticks = timeTicks(t0, t1, W - m.l - m.r);

  // The future: a faint wash with a label, so planned prices aren't mistaken for history.
  if (t1 > h.to + 60) {
    el("rect", { x: x(h.to), y: tempTop, width: x(t1) - x(h.to), height: bottom - tempTop, fill: "var(--grid)", opacity: 0.35 }, svg);
    // Centred in the shaded area, above the plot: the hours ahead show the plan and known prices.
    if (x(t1) - x(h.to) > 90) el("text", { class: "future-label", x: (x(h.to) + x(t1)) / 2, y: tempTop - 5, "text-anchor": "middle" }, svg, "planned");
  }

  // Temperature panel
  let yT = null;
  if (showTemp) {
    el("text", { class: "panel-label", x: m.l, y: 12 }, svg, "Temperature °C");
    const vals = [...temps.map((p) => p[1]), ...targets.map((p) => p[1]).filter((v) => v != null)];
    if (!vals.length) vals.push(room.minTemp, room.maxTemp);
    const yTicks = niceTicks(Math.min(...vals) - 0.5, Math.max(...vals) + 0.5, 4);
    const lo = yTicks[0], hi = yTicks[yTicks.length - 1];
    yT = (v) => tempTop + (1 - (v - lo) / (hi - lo)) * tempH;
    // Heating periods as a faint wash behind the line
    for (const s of segs) {
      el("rect", { x: x(s.start), y: tempTop, width: Math.max(1, x(s.end) - x(s.start)), height: tempH, fill: catColor(s.cat), "fill-opacity": 0.1 }, svg);
    }
    for (const v of yTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: yT(v), y2: yT(v) }, svg);
      el("text", { x: m.l - 6, y: yT(v) + 4, "text-anchor": "end" }, svg, v.toFixed(v % 1 ? 1 : 0));
    }
    drawTimeAxis(svg, x, ticks, tempTop, tempTop + tempH, null);
    el("path", { class: "target", d: stepPath(targets, x, yT, t1, 3 * tick) }, svg);
    // Temperature line, broken where readings are missing for > 30 min. Each gap is bridged by a
    // dashed straight line from the last reading before to the first after: an estimate, not data
    // (e.g. a Wi-Fi off night, when the station can't upload).
    let d = "", est = "";
    temps.forEach(([t, v], i) => {
      const gap = i === 0 || t - temps[i - 1][0] > 1800;
      if (gap && i > 0) {
        const [pt, pv] = temps[i - 1];
        est += `M${x(pt).toFixed(1)},${yT(pv).toFixed(1)}L${x(t).toFixed(1)},${yT(v).toFixed(1)}`;
      }
      d += `${gap ? "M" : "L"}${x(t).toFixed(1)},${yT(v).toFixed(1)}`;
    });
    if (est) el("path", { class: "line-estimate", d: est }, svg);
    el("path", { class: "line", d }, svg);
    if (!temps.length) el("text", { class: "empty", x: W / 2, y: tempTop + tempH / 2, "text-anchor": "middle" }, svg, "No temperatures recorded yet");
  }

  // Wi-Fi lane, read like the rest of the UI: on = a lit bar, off = the dark stripes "off" has in the
  // program; a planned off period is a dashed outline, so plan and reality can be compared.
  if (wifi) {
    // Dark stripes like .tl-off. CSS variables only work in style, not in SVG presentation attributes.
    const pat = el("pattern", { id: "wifi-off", width: 6, height: 6, patternUnits: "userSpaceOnUse", patternTransform: "rotate(45)" },
                   el("defs", {}, svg));
    el("rect", { width: 6, height: 6, style: "fill: var(--surface-2)" }, pat);
    el("rect", { width: 3, height: 6, style: "fill: var(--grid)" }, pat);
    el("text", { class: "panel-label", x: m.l, y: wifiTop - 4 }, svg, "Wi-Fi");
    el("rect", { x: m.l, y: wifiTop, width: W - m.l - m.r, height: wifiH, fill: "var(--grid)", rx: 3, opacity: 0.35 }, svg);
    const span = (a, b) => [x(Math.max(a, t0)), x(Math.min(b, t1))];
    // On: from the first reading in the chart until now, minus what the sensors saw as off.
    const [o0, o1] = span(temps.length ? Math.max(t0, temps[0][0]) : t0, h.to);
    if (o1 > o0) el("rect", { class: "wifi-on", x: o0, y: wifiTop, width: o1 - o0, height: wifiH, rx: 3 }, svg);
    // Off: what the sensors saw, and planned periods still ahead. The pattern goes in as an attribute:
    // url(#id) in the stylesheet would resolve against style.css.
    const off = [...wifi.seen.map(([a, b]) => [a, b]), ...wifi.planned.filter(([, b]) => b > h.to).map(([a, b]) => [Math.max(a, h.to), b])];
    for (const [a, b] of off) {
      const [x0, x1] = span(a, b);
      if (x1 > x0) el("rect", { x: x0, y: wifiTop, width: Math.max(2, x1 - x0), height: wifiH, fill: "url(#wifi-off)" }, svg);
    }
    for (const [a, b] of wifi.planned) {
      const [x0, x1] = span(a, b);
      if (x1 > x0) el("rect", { class: "wifi-planned", x: x0 + 0.5, y: wifiTop + 0.5, width: Math.max(2, x1 - x0 - 1), height: wifiH - 1, rx: 3 }, svg);
    }
  }

  // Heating strip: the exact on/off periods, coloured by why
  if (showStrip) el("text", { class: "panel-label", x: m.l, y: stripTop - 4 }, svg, "Heating");
  if (showStrip) el("rect", { x: m.l, y: stripTop, width: W - m.l - m.r, height: stripH, fill: "var(--grid)", rx: 3, opacity: 0.5 }, svg);
  for (const s of segs) {
    const x0 = x(s.start) + 1, w = Math.max(2, x(s.end) - x(s.start) - 2); // 2px surface gap between fills
    el("rect", { x: x0, y: stripTop, width: w, height: stripH, rx: 3, fill: catColor(s.cat) }, svg);
  }

  // Planned heating ahead (lighter): what the planner intends for the coming hours.
  const plan = (state.status?.rooms || []).find((x) => x.id === room.id)?.plan || [];
  for (const s of plan) {
    const cat = { heat: "comfort", keep: "comfort", booked: "comfort", away: "safety", preheat: "price", boost: "price", night: "price" }[s.kind];
    if (!cat) continue;  // off / coast: nothing planned
    const a = Math.max(s.from, h.to), b = Math.min(s.to, t1);
    if (b <= a) continue;
    el("rect", { x: x(a) + 1, y: stripTop, width: Math.max(2, x(b) - x(a) - 2), height: stripH, rx: 3,
                 fill: catColor(cat), "fill-opacity": 0.35 }, svg);
  }

  // Time labels under the last panel (price and power mix have their own card below).
  if (showStrip) drawTimeAxis(svg, x, ticks, stripTop, stripTop + stripH, H - 6);
  else drawTimeAxis(svg, x, ticks, tempTop, tempTop + tempH, H - 6);
  const nowT = state.status?.now;
  if (nowT && nowT >= t0 && nowT <= t1) {
    el("line", { class: "now", x1: x(nowT), x2: x(nowT), y1: tempTop, y2: bottom }, svg);
    el("text", { class: "now-label", x: x(nowT), y: tempTop - 5, "text-anchor": "middle" }, svg, "now");
  }

  // One crosshair, one tooltip listing every series at that time
  addCrosshair(svg, x, t0, t1, tempTop, bottom, W, (t, evt) => {
    const rows = [];
    if (showTemp) {
      const i = lastAtOrBefore(temps, t);
      const near = i >= 0 && t - temps[i][0] < 1800 ? temps[i][1] : null;
      if (near == null && i >= 0 && i + 1 < temps.length && temps[i + 1][0] - temps[i][0] > 1800) {
        // Inside a gap: the straight line between the readings around it, clearly marked.
        const [a, av] = temps[i], [b, bv] = temps[i + 1];
        const v = av + ((bv - av) * (t - a)) / (b - a);
        rows.push({ label: "Temperature", value: `≈ ${temp(v)} (estimate, no readings)`, color: "var(--ink-2)" });
      } else {
        rows.push({ label: "Temperature", value: temp(near), color: "var(--ink)" });
      }
      const di = lastAtOrBefore(targets, t);
      rows.push({ label: "Target", value: temp(di >= 0 ? targets[di][1] : null), color: "var(--target)" });
    }
    const si = lastAtOrBefore(decisions, t);
    if (si >= 0 && t - decisions[si][0] <= 3 * tick) {
      const [, on, reason] = decisions[si];
      const cat = on ? reasonCat(reason) || "comfort" : null;
      rows.push({ label: on ? "Heating" : "Off", value: reasonLabel(reason), color: cat ? catColor(cat) : "var(--grid)", box: true });
    }
    if (wifi) {
      const p = wifi.planned.find(([a, b]) => t >= a && t < b);
      const s = wifi.seen.find(([a, b]) => t >= a && t < b);
      if (p || s) {
        const parts = [];
        if (s) parts.push(`seen ${hm(s[0])}–${s[2] ? "now" : hm(s[1])}`);
        if (p) parts.push(`planned ${hm(p[0])}–${hm(p[1])}`);
        rows.push({ label: "Wi-Fi off", value: parts.join(" · "), color: "var(--ink-2)", box: true });
      }
    }
    const pi = lastAtOrBefore(prices, t);
    if (pi >= 0 && t - prices[pi][0] < 900) rows.push({ label: `${state.status?.currency || ""}/kWh`, value: price(prices[pi][1]) });
    showTooltip(evt, dayHm(t), rows);
  });

  renderElectricity({ t0, t1, now: h.to, prices, W, m, ticks });

  $("events-log").hidden = !!room.sensorOnly;
  if (!room.sensorOnly) renderEvents(h);
}

function renderEvents(h) {
  const body = $("events");
  body.replaceChildren();
  const events = [...h.events].reverse();
  $("events-count").textContent = events.length ? `${events.length} in this period` : "";
  if (!events.length) {
    const tr = el("tr", {}, body);
    el("td", { colspan: 7, class: "muted" }, tr, "No switch events in this period.");
    return;
  }
  for (const e of events) {
    const tr = el("tr", {}, body);
    el("td", {}, tr, dayHm(e.ts));
    const st = el("td", {}, tr);
    const cat = e.on ? reasonCat(e.reason) || "comfort" : null;
    // Same-size box on every row keeps the text aligned: filled = heating (by reason), hollow = off.
    if (cat) el("span", { class: "swatch", style: `background:${catColor(cat)};margin-right:6px` }, st);
    else el("span", { class: "swatch swatch-off", style: "margin-right:6px" }, st);
    st.appendChild(document.createTextNode(e.on ? "ON" : "OFF"));
    el("td", {}, tr, reasonLabel(e.reason));
    el("td", { class: "num" }, tr, temp(e.inputs?.temperature));
    el("td", { class: "num" }, tr, temp(e.inputs?.target));
    el("td", { class: "num" }, tr, price(e.inputs?.price));
    const why = el("td", { class: "why" }, tr, e.detail);
    for (const p of e.plugs || []) if (p.error) el("div", { style: "color:var(--critical)" }, why, `⚠️ ${p.id}: ${p.error}`);
  }
}

// ---------------------------------------------------------------------------------------
// Refresh loop
// ---------------------------------------------------------------------------------------
async function refreshStatus() {
  try {
    const s = await api("/api/status");
    // An update restarted the server under this open page: the old scripts may not understand
    // the new status, so load the new ones rather than half-render the dashboard.
    if (s.version && state.loadedVersion && s.version !== state.loadedVersion) { location.reload(); return; }
    if (s.version) state.loadedVersion ??= s.version;
    if (!s.rooms) { $("updated").textContent = "waiting for the first control step…"; retrySoon(); return; }
    const first = !state.status;
    state.status = s;
    // Settings drawn before the first status (just after a restart) lacked the sensor lists: draw them
    // again now, keeping any unsaved edits.
    if (first && !$("view-settings").hidden) window.Settings?.open();
    const ce = $("config-error");
    ce.hidden = !s.configError;
    if (s.configError) {
      ce.replaceChildren();
      const label = el("span", { class: "label" }, ce);
      label.appendChild(icon("alert"));
      label.appendChild(document.createTextNode("Configuration problem"));
      el("span", {}, ce, `${s.configError} `);
      el("a", { href: "#settings" }, ce, "Open settings");
    }
    state.tz = s.timezone || state.tz;
    $("updated").textContent = `updated ${hm(s.now)} · every ${Math.round(s.tickSeconds / 60)} min`;
    renderProblems();
    renderWifiOff();
    renderAway();
    renderBook();
    renderRooms();
    renderBookingsCard();
    window.Costs?.load();
    window.Usage?.load();
    if (state.selected) {
      if (!state.history) await loadHistory();
      else renderDetail();  // the planned part (target, heating ahead) comes from the status: redraw with it
    } else if (s.rooms.length) {
      await selectRoom(s.rooms[0].id, false);
    }
  } catch (e) {
    $("updated").textContent = `offline: ${e.message}`;
    if (!state.status) retrySoon();
  }
}
// Until the first status is in (heatbrain restarting), ask every 5 s, whatever view is open.
let retryTimer = null;
function retrySoon() {
  if (retryTimer) return;
  retryTimer = setTimeout(() => { retryTimer = null; refreshStatus(); }, 5000);
}

// The room chart's own range buttons only (cost and usage have theirs).
document.querySelectorAll("#detail-range button").forEach((b) =>
  b.addEventListener("click", () => {
    document.querySelectorAll("#detail-range button").forEach((x) => x.classList.toggle("active", x === b));
    state.hours = Number(b.dataset.hours);
    loadHistory();
  }));

let resizeTimer;
window.addEventListener("resize", () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { window.Costs?.render(); window.Usage?.render(); if (state.history) renderDetail(); }, 150);
});

// ---------------------------------------------------------------------------------------
// Views: #settings or the dashboard
// ---------------------------------------------------------------------------------------
function route() {
  const view = location.hash.startsWith("#settings") ? "settings" : location.hash.startsWith("#help") ? "help" : "dashboard";
  $("view-dashboard").hidden = view !== "dashboard";
  $("view-settings").hidden = view !== "settings";
  $("view-help").hidden = view !== "help";
  if (view === "help" && typeof renderManual === "function" && !$("view-help").childElementCount) renderManual($("view-help"));
  document.querySelectorAll(".nav a").forEach((a) => a.classList.toggle("active", a.dataset.view === view));
  if (view === "settings" && window.Settings) window.Settings.open();
  if (view === "dashboard" && state.status) { window.Costs?.render(); window.Usage?.render(); if (state.history) renderDetail(); }
  // The dashboard isn't refreshed while hidden: fetch now rather than show what it had (or
  // nothing, if the server was restarting when the page loaded) until the next 30 s tick.
  if (view === "dashboard" && routed) { refreshStatus(); loadHistory(); }
  routed = true;
}
let routed = false;
// Back to the tab (or the phone app) after a while: fetch fresh status and chart at once.
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible" && !$("view-dashboard").hidden && state.status) { refreshStatus(); loadHistory(); }
});
window.addEventListener("hashchange", route);

refreshStatus().then(route);
setInterval(() => { if (!$("view-dashboard").hidden) refreshStatus(); }, 30_000);
setInterval(() => { if (!$("view-dashboard").hidden) loadHistory(); }, 60_000);

// The switch log is long: collapsed by default, and it stays the way the viewer left it.
(() => {
  const log = $("events-log");
  try { log.open = localStorage.getItem("hb.eventsOpen") === "1"; } catch { /* private mode */ }
  log.addEventListener("toggle", () => { try { localStorage.setItem("hb.eventsOpen", log.open ? "1" : "0"); } catch { /* private mode */ } });
})();
