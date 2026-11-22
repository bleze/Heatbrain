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
  const svg = el("svg", { class: cls, "aria-hidden": "true" });
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

// ---------------------------------------------------------------------------------------
// Price chart (single series: no legend, the title names it)
// ---------------------------------------------------------------------------------------
function renderPriceChart() {
  const host = $("price-chart");
  const s = state.status;
  const pts = s.prices;
  host.replaceChildren();
  if (!pts.length) { $("price-summary").textContent = "no price data yet"; return; }

  const now = s.now;
  const t0 = pts[0][0], t1 = pts[pts.length - 1][0] + 900;
  const W = host.clientWidth, H = 150, m = { l: 40, r: 8, t: 8, b: 22 };
  const vals = pts.map((p) => p[1]);
  const yTicks = niceTicks(Math.min(0, ...vals), Math.max(...vals), 3);
  const yMin = yTicks[0], yMax = yTicks[yTicks.length - 1];
  const x = (t) => m.l + ((t - t0) / (t1 - t0)) * (W - m.l - m.r);
  const y = (v) => m.t + (1 - (v - yMin) / (yMax - yMin)) * (H - m.t - m.b);

  const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, height: H, role: "img", "aria-label": "Electricity price per 15 minutes" }, host);
  for (const v of yTicks) {
    el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
    el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, svg, v.toFixed(1));
  }
  drawTimeAxis(svg, x, timeTicks(t0, t1, W - m.l - m.r), m.t, H - m.b, H - 6);
  const defs = el("defs", {}, svg);
  const grad = el("linearGradient", { id: "price-fill", x1: 0, y1: 0, x2: 0, y2: 1 }, defs);
  el("stop", { offset: "0", "stop-color": "#fab005", "stop-opacity": 0.35 }, grad);
  el("stop", { offset: "1", "stop-color": "#fab005", "stop-opacity": 0.02 }, grad);
  const line = stepPath(pts, x, y, t1, 900);
  el("path", { class: "area", d: `${line}V${y(yMin)}H${x(pts[0][0])}Z` }, svg);
  el("path", { class: "line", d: line }, svg);
  if (now >= t0 && now <= t1) {
    el("line", { class: "now", x1: x(now), x2: x(now), y1: m.t, y2: H - m.b }, svg);
    el("text", { class: "now-label", x: x(now) + 4, y: m.t + 10 }, svg, "now");
  }

  // Summary line
  const today = pts.filter((p) => dayShort(p[0]) === dayShort(now));
  const cur = pts[lastAtOrBefore(pts, now)];
  const pieces = [];
  if (cur) pieces.push(`now ${price(cur[1])} ${s.currency}/kWh`);
  if (today.length) {
    const lo = today.reduce((a, b) => (b[1] < a[1] ? b : a));
    const hi = today.reduce((a, b) => (b[1] > a[1] ? b : a));
    pieces.push(`today low ${price(lo[1])} at ${hm(lo[0])}`, `high ${price(hi[1])} at ${hm(hi[0])}`);
  }
  $("price-summary").textContent = pieces.join(" · ");

  addCrosshair(svg, x, t0, t1, m.t, H - m.b, W, (t, evt) => {
    const i = lastAtOrBefore(pts, t);
    if (i < 0) return hideTooltip();
    showTooltip(evt, `${dayHm(pts[i][0])}–${hm(pts[i][0] + 900)}`, [{ label: `${s.currency}/kWh`, value: price(pts[i][1]) }]);
  });
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
  const next = plan.slice(1, 3).map((s) => `${planLabel(s)} ${hm(s.from)}`);
  const text = `Now: ${planLabel(plan[0])}${next.length ? ` · then ${next.join(" · ")}` : ""}`;
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
  if (s.humidity != null) el("small", {}, hero, `${Math.round(s.humidity)} % humidity`);
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

let awayFormOpen = false;
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

function renderAway() {
  const bar = $("away-bar"), headerBtn = $("away-btn");
  const away = state.status?.away;
  headerBtn.hidden = !!away || !state.status?.rooms?.length;
  bar.replaceChildren();
  if (away) {
    bar.hidden = false;
    bar.appendChild(badge("plane", "violet"));
    const t = el("div", { class: "grow" }, bar);
    if (away.active) {
      el("b", {}, t, `Away until ${dayHm(away.until)}`);
      el("div", { class: "muted" }, t, `Every room keeps above ${temp(away.keepAbove)}C; programs resume afterwards.`);
    } else {
      el("b", {}, t, `Away planned: ${dayHm(away.from)} → ${dayHm(away.until)}`);
      el("div", { class: "muted" }, t, `Starts by itself; every room will keep above ${temp(away.keepAbove)}C.`);
    }
    const end = el("button", { type: "button", class: away.active ? "primary" : "" }, bar);
    end.appendChild(icon(away.active ? "home" : "trash"));
    end.appendChild(document.createTextNode(away.active ? "I'm home - end now" : "Cancel plan"));
    end.addEventListener("click", async () => {
      end.disabled = true;
      try {
        await api("/api/away", { method: "DELETE" });
        toast(away.active ? "Welcome home - programs are back" : "Away plan cancelled");
        setTimeout(refreshStatus, 1200);
      } catch (e) { toast(e.message, true); end.disabled = false; }
    });
    return;
  }
  bar.hidden = !awayFormOpen;
  if (!awayFormOpen) return;
  bar.appendChild(badge("plane", "violet"));
  const t = el("div", { class: "grow" }, bar);
  const ga = el("b", {}, t, "Going away? ");
  if (typeof helpButton === "function") helpButton(ga, "away", t);
  el("div", { class: "muted" }, t, "Plan it ahead: it starts and ends by itself. Every room only keeps above the temperature you choose.");
  const local = (d) => new Date(d.getTime() - d.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
  const fromLabel = el("label", { class: "field" }, bar);
  el("span", {}, fromLabel, "From");
  const from = el("input", { type: "datetime-local", "aria-label": "Away from" }, fromLabel);
  const f = new Date(); f.setMinutes(0, 0, 0); f.setHours(f.getHours() + 1);
  from.value = local(f);
  const untilLabel = el("label", { class: "field" }, bar);
  el("span", {}, untilLabel, "Until");
  const until = el("input", { type: "datetime-local", "aria-label": "Away until" }, untilLabel);
  const d = new Date(); d.setDate(d.getDate() + 2); d.setHours(18, 0, 0, 0);
  until.value = local(d);
  const keepLabel = el("label", { class: "field" }, bar);
  el("span", {}, keepLabel, "Keep above");
  const { wrap: keepBox, input: keep } = makeStepper({ value: 12, step: 0.5, min: 5, max: 20, unit: "°C", label: "Keep above" });
  keepLabel.appendChild(keepBox);
  const start = el("button", { type: "button", class: "primary" }, bar);
  start.appendChild(icon("plane"));
  start.appendChild(document.createTextNode("Save away period"));
  start.addEventListener("click", async () => {
    const fromTs = Math.floor(new Date(from.value).getTime() / 1000);
    const untilTs = Math.floor(new Date(until.value).getTime() / 1000);
    start.disabled = true;
    try {
      await api("/api/away", { method: "POST", headers: { "Content-Type": "application/json" },
                               body: JSON.stringify({ from: fromTs, until: untilTs, keepAbove: Number(keep.value) }) });
      awayFormOpen = false;
      toast(fromTs * 1000 > Date.now() ? "Away mode planned" : "Away mode started");
      setTimeout(refreshStatus, 1200);
    } catch (e) { toast(e.message, true); start.disabled = false; }
  });
  const cancel = el("button", { type: "button", class: "ghost" }, bar, "Cancel");
  cancel.addEventListener("click", () => { awayFormOpen = false; renderAway(); });
}
$("away-btn").addEventListener("click", () => {
  awayFormOpen = true;
  if (location.hash.startsWith("#settings")) location.hash = "";  // the panel lives on the dashboard
  renderAway();
  window.scrollTo({ top: 0, behavior: "smooth" });
});

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
    if (r.priceOnly) el("small", {}, hero, `${state.status.currency}/kWh · no sensor`);
    else if (inp.mode === "away") el("small", {}, hero, `away · keep above ${temp(inp.target)}`);
    else if (inp.mode === "booked") el("small", {}, hero, `booked · ${temp(inp.target)}`);
    else if (inp.mode === "off") el("small", {}, hero, `off ${inp.targetLabel || ""}`.trim());
    else if (inp.mode === "keep") el("small", {}, hero, `keep above ${temp(inp.target)}`);
    else if (inp.target != null) el("small", {}, hero, `target ${temp(inp.target)}`);

    const reason = el("div", { class: "reason" }, card);
    if (cat) el("span", { class: "swatch", style: `background:${catColor(cat)}` }, reason);
    reason.appendChild(document.createTextNode(reasonLabel(r.reason)));
    el("div", { class: "detail" }, card, r.detail);
    // Only when something outranks the override (e.g. above max); otherwise the reason says it.
    if (r.override && !["MANUAL_ON", "MANUAL_OFF"].includes(r.reason)) {
      el("div", { class: "override-note" }, card, `Manual ${r.override.on ? "ON" : "OFF"} until ${hm(r.override.until)}`);
    }

    renderBookings(card, r);

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
      const chipEl = el("span", { class: `chip ${cls}`, title: p.error || "" }, plugs);
      chipEl.appendChild(icon(ic));
      chipEl.appendChild(document.createTextNode(text));
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
    if (!r.priceOnly && bookingDraft?.room !== r.id) act("Book", "calendar", "", () => openBooking(r));
    if (bookingDraft?.room === r.id) bookingForm(foot, r);
  }
}

// ---------------------------------------------------------------------------------------
// Room bookings: "22 deg on Saturday 16:00-23:00". The room warms up early by itself and
// follows its program again afterwards. The draft survives the 30 s re-render.
let bookingDraft = null;
const pad2 = (n) => String(n).padStart(2, "0");
const isoDate = (d) => `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;

function renderBookings(card, r) {
  for (const b of r.bookings || []) {
    const row = el("div", { class: `booking${b.active ? " active" : ""}` }, card);
    row.appendChild(icon("calendar"));
    const now = Date.now() / 1000;
    const text = b.active && now >= b.from ? `Booked until ${hm(b.until)} · ${temp(b.target)}`
      : b.active ? `Warming up for ${hm(b.from)}–${hm(b.until)} · ${temp(b.target)}`
      : `Booked ${dayHm(b.from)}–${hm(b.until)} · ${temp(b.target)}`;
    const span = el("span", {}, row, text);
    if (!(b.active && now >= b.from)) {
      const h = b.warmupMinutes / 60;
      const dur = h >= 10 ? `${Math.round(h)} h` : `${Math.round(h * 2) / 2} h`;
      const how = b.basis === "learned" ? `about ${dur}, from how fast the room has warmed before`
        : `${dur} as set for the room, until heatbrain has learned how fast it warms`;
      el("small", { class: "muted", style: "display:block;font-weight:400" }, span,
        b.active ? `Warming since ${dayHm(b.warmupFrom)} (${how})` : `Starts warming ${dayHm(b.warmupFrom)} (${how})`);
      if (b.reachable === false) el("small", { style: "display:block;color:var(--critical)" }, span, `May not reach ${temp(b.target)} in time`);
    }
    const x = el("button", { type: "button", class: "ghost", title: b.active ? "End the booking now" : "Cancel the booking", "aria-label": "Cancel booking" }, row);
    x.appendChild(icon(b.active ? "undo" : "trash"));
    x.addEventListener("click", async (e) => {
      e.stopPropagation();
      x.disabled = true;
      try {
        await api(`/api/bookings/${encodeURIComponent(b.id)}`, { method: "DELETE" });
        toastMaybe(b.active ? "Booking ended - back to the program" : "Booking cancelled");
        setTimeout(refreshStatus, 1500);
      } catch (err) { alertInline(err); x.disabled = false; }
    });
  }
}

function openBooking(r) {
  let target = 22;
  try { target = Number(localStorage.getItem("hb.bookingTarget")) || 22; } catch { /* private mode */ }
  bookingDraft = { room: r.id, date: isoDate(new Date()), from: "16:00", until: "23:00", target: Math.min(target, r.maxTemp - 0.5) };
  renderRooms();
}

function bookingForm(host, r) {
  const d = bookingDraft;
  const f = el("div", { class: "booking-form" }, host);
  f.addEventListener("click", (e) => e.stopPropagation());
  f.addEventListener("keydown", (e) => e.stopPropagation());
  const field = (label, attrs, key) => {
    const l = el("label", {}, f);
    el("span", { class: "muted" }, l, label);
    const i = el("input", { ...attrs, value: d[key] }, l);
    i.addEventListener("input", () => { d[key] = attrs.type === "number" ? Number(i.value) : i.value; });
    return i;
  };
  field("Day", { type: "date" }, "date");
  field("From", { type: "time", step: 900 }, "from");
  field("Until", { type: "time", step: 900 }, "until");
  field("Temperature", { type: "number", step: 0.5, min: 5, max: r.maxTemp - 0.5, style: "width:5.5em" }, "target");
  el("div", { class: "hint" }, f, "It starts heating early enough by itself and goes back to the program afterwards. An end before the start means the next day.");
  const row = el("div", { class: "actions" }, f);
  const save = el("button", { type: "button", class: "primary" }, row);
  save.appendChild(icon("check"));
  save.appendChild(document.createTextNode("Book"));
  save.addEventListener("click", async () => {
    const from = new Date(`${d.date}T${d.from}`);
    const until = new Date(`${d.date}T${d.until}`);
    if (until <= from) until.setDate(until.getDate() + 1);
    save.disabled = true;
    try {
      await api(`/api/rooms/${encodeURIComponent(r.id)}/bookings`, {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ from: Math.floor(from / 1000), until: Math.floor(until / 1000), target: d.target }),
      });
      try { localStorage.setItem("hb.bookingTarget", String(d.target)); } catch { /* private mode */ }
      bookingDraft = null;
      toastMaybe(`${r.name} booked`);
      setTimeout(refreshStatus, 1500);
      renderRooms();
    } catch (e) { alertInline(e); save.disabled = false; }
  });
  const cancel = el("button", { type: "button" }, row);
  cancel.appendChild(document.createTextNode("Cancel"));
  cancel.addEventListener("click", () => { bookingDraft = null; renderRooms(); });
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
    setTimeout(refreshStatus, 1500);
  } catch (e) { alertInline(e); } finally { button.disabled = false; }
}

async function clearOverride(id, button) {
  button.disabled = true;
  try {
    await api(`/api/rooms/${encodeURIComponent(id)}/override`, { method: "DELETE" });
    setTimeout(refreshStatus, 1500);
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

function renderLegend(room, segs, wifi) {
  const host = $("legend");
  host.replaceChildren();
  if (wifi) {
    const p = el("span", {}, host); el("i", { class: "key-wifi-planned" }, p); p.appendChild(document.createTextNode("Wi-Fi off: planned"));
    const s = el("span", {}, host); el("i", { class: "key-wifi-seen" }, s); s.appendChild(document.createTextNode("Wi-Fi off: seen"));
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

  // Show a third of the range ahead of now, as far as prices are known: you see what's coming.
  const t0 = h.from;
  const lastPrice = h.prices.length ? h.prices[h.prices.length - 1][0] + 900 : h.to;
  const t1 = Math.max(h.to, Math.min(h.to + (h.to - h.from) / 3, lastPrice));
  const temps = h.temperatures, decisions = h.decisions;
  const prices = h.prices.filter((p) => p[0] >= t0 - 900 && p[0] <= t1);
  const targets = decisions.map((d) => [d[0], d[3]]);
  const segs = heatingSegments(decisions, tick, h.to);
  const wifi = h.wifi && (h.wifi.seen.length || h.wifi.planned.length) ? h.wifi : null;
  renderLegend(room, segs, wifi);

  const W = host.clientWidth, m = { l: 40, r: 8 };
  const showTemp = !room.priceOnly;
  const tempTop = 18, tempH = showTemp ? 190 : 0;
  const showStrip = !room.sensorOnly;
  const wifiTop = tempTop + tempH + (showTemp ? 34 : 0), wifiH = wifi ? 10 : 0;
  const stripTop = wifiTop + (wifi ? wifiH + 30 : 0), stripH = showStrip ? 12 : 0;
  const priceTop = stripTop + stripH + (showStrip ? 30 : 0), priceH = 90;
  const H = priceTop + priceH + 24;
  const x = (t) => m.l + ((t - t0) / (t1 - t0)) * (W - m.l - m.r);
  const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, height: H, role: "img", "aria-label": `${room.name}: temperature, heating and price` }, host);
  const ticks = timeTicks(t0, t1, W - m.l - m.r);

  // The future: a faint wash with a label, so planned prices aren't mistaken for history.
  if (t1 > h.to + 60) {
    el("rect", { x: x(h.to), y: tempTop, width: x(t1) - x(h.to), height: priceTop + priceH - tempTop, fill: "var(--grid)", opacity: 0.35 }, svg);
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

  // Wi-Fi lane: planned periods as a hatched band, what the sensors saw as a solid bar on top.
  if (wifi) {
    const pat = el("pattern", { id: "wifi-hatch", width: 4, height: 4, patternUnits: "userSpaceOnUse", patternTransform: "rotate(45)" },
                   el("defs", {}, svg));
    // CSS variables only work in style, not in SVG presentation attributes.
    el("line", { x1: 0, y1: 0, x2: 0, y2: 4, style: "stroke: var(--muted); stroke-width: 1.5" }, pat);
    el("text", { class: "panel-label", x: m.l, y: wifiTop - 4 }, svg, "Wi-Fi off");
    el("rect", { x: m.l, y: wifiTop, width: W - m.l - m.r, height: wifiH, fill: "var(--grid)", rx: 3, opacity: 0.35 }, svg);
    const band = (a, b, attrs) => {
      const x0 = x(Math.max(a, t0)), x1 = x(Math.min(b, t1));
      if (x1 > x0) el("rect", { x: x0, y: wifiTop, width: Math.max(2, x1 - x0), height: wifiH, rx: 3, ...attrs }, svg);
    };
    // The pattern fill as an attribute: url(#id) in the stylesheet would resolve against style.css.
    for (const [a, b] of wifi.planned) band(a, b, { class: "wifi-planned", fill: "url(#wifi-hatch)" });
    for (const [a, b] of wifi.seen) band(a, b, { class: "wifi-seen" });
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

  // Price panel (its own y-scale: separate chart, shared x)
  el("text", { class: "panel-label", x: m.l, y: priceTop - 8 }, svg, `Price ${state.status?.currency || ""}/kWh`);
  if (prices.length) {
    const pv = prices.map((p) => p[1]);
    const pTicks = niceTicks(Math.min(0, ...pv), Math.max(...pv), 2);
    const plo = pTicks[0], phi = pTicks[pTicks.length - 1];
    const yP = (v) => priceTop + (1 - (v - plo) / (phi - plo)) * priceH;
    for (const v of pTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: yP(v), y2: yP(v) }, svg);
      el("text", { x: m.l - 6, y: yP(v) + 4, "text-anchor": "end" }, svg, v.toFixed(1));
    }
    drawTimeAxis(svg, x, ticks, priceTop, priceTop + priceH, H - 6);
    el("path", { class: "line", d: stepPath(prices, x, yP, t1, 900), "stroke-width": 1.5 }, svg);
  } else {
    drawTimeAxis(svg, x, ticks, priceTop, priceTop + priceH, H - 6);
  }
  const nowT = state.status?.now;
  if (nowT && nowT >= t0 && nowT <= t1) {
    el("line", { class: "now", x1: x(nowT), x2: x(nowT), y1: tempTop, y2: priceTop + priceH }, svg);
    el("text", { class: "now-label", x: x(nowT), y: tempTop - 5, "text-anchor": "middle" }, svg, "now");
  }

  // One crosshair, one tooltip listing every series at that time
  addCrosshair(svg, x, t0, t1, tempTop, priceTop + priceH, W, (t, evt) => {
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
      rows.push({ label: "Target", value: temp(di >= 0 ? targets[di][1] : null), color: "var(--muted)" });
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

  const eventsBlock = $("events").closest(".table-wrap");
  eventsBlock.hidden = !!room.sensorOnly;
  eventsBlock.previousElementSibling.hidden = !!room.sensorOnly;  // the "Switch events" heading
  if (!room.sensorOnly) renderEvents(h);
}

function renderEvents(h) {
  const body = $("events");
  body.replaceChildren();
  const events = [...h.events].reverse();
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
    if (!s.rooms) { $("updated").textContent = "waiting for the first control step…"; return; }
    state.status = s;
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
    renderRooms();
    renderPriceChart();
    window.Costs?.load();
    window.Usage?.load();
    if (state.selected) {
      if (!state.history) await loadHistory();
    } else if (s.rooms.length) {
      await selectRoom(s.rooms[0].id, false);
    }
  } catch (e) {
    $("updated").textContent = `offline: ${e.message}`;
  }
}

document.querySelectorAll(".range button").forEach((b) =>
  b.addEventListener("click", () => {
    document.querySelectorAll(".range button").forEach((x) => x.classList.toggle("active", x === b));
    state.hours = Number(b.dataset.hours);
    loadHistory();
  }));

let resizeTimer;
window.addEventListener("resize", () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { if (state.status) renderPriceChart(); window.Costs?.render(); window.Usage?.render(); if (state.history) renderDetail(); }, 150);
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
  if (view === "dashboard" && state.status) { renderPriceChart(); window.Costs?.render(); window.Usage?.render(); if (state.history) renderDetail(); }
}
window.addEventListener("hashchange", route);

refreshStatus().then(route);
setInterval(() => { if (!$("view-dashboard").hidden) refreshStatus(); }, 30_000);
setInterval(() => { if (!$("view-dashboard").hidden) loadHistory(); }, 60_000);
