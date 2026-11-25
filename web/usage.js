"use strict";
// Electricity use: the household's measured use (Eloverblik, a day behind) with the part heatbrain's
// radiators drew in the same intervals. One axis, stacked: radiators at the bottom, the rest on top.
// Short ranges show power (kW) per quarter or hour; long ones energy (kWh) per day, week or month.
(() => {
  const GRAIN_KEY = "heatbrain-usage-grain";
  const U = { days: 7, data: null, loadedAt: 0, grain: null };
  try { U.grain = localStorage.getItem(GRAIN_KEY); } catch { /* per-browser convenience only */ }
  const kwh = (v) => `${v.toFixed(v >= 10 ? 0 : 1)} kWh`;
  const kw = (v) => `${v.toFixed(v >= 10 ? 1 : 2)} kW`;
  const allTime = () => U.days === 0;
  const long = () => allTime() || U.days >= 90;  // where days / weeks / months can be chosen

  // The bucket size for the current range.
  function grain() {
    if (!allTime() && U.days <= 2) return "quarter";
    if (!allTime() && U.days <= 7) return "hour";
    if (!long()) return "day";
    return ["day", "week", "month"].includes(U.grain) ? U.grain : allTime() ? "month" : "day";
  }

  async function load(force = false) {
    if (!force && Date.now() - U.loadedAt < 300_000 && U.data) return render();
    const want = U.days;
    try {
      const data = await api(`/api/usage?days=${want}`);
      if (want !== U.days) return;  // a newer range was chosen meanwhile: its answer wins
      U.data = data;
      U.loadedAt = Date.now();
      render();
    } catch { /* optional feature */ }
  }

  function tile(host, iconName, label, value, sub, tone = "") {
    const t = el("div", { class: `tile ${tone}` }, host);
    const k = el("div", { class: "k" }, t);
    k.appendChild(icon(iconName));
    k.appendChild(document.createTextNode(label));
    el("div", { class: "v" }, t, value);
    if (sub) el("div", { class: "s" }, t, sub);
  }

  // Calendar keys in the configured time zone (one formatter: thousands of readings).
  let keyFmt = null, keyTz = null;
  const dayKey = (t) => {
    if (keyTz !== state.tz) { keyFmt = new Intl.DateTimeFormat("en-CA", { timeZone: state.tz }); keyTz = state.tz; }
    return keyFmt.format(t * 1000);  // "2026-11-21"
  };
  const weekKey = (t) => {  // the Monday of that week
    const d = new Date(`${dayKey(t)}T00:00:00Z`);
    d.setUTCDate(d.getUTCDate() - ((d.getUTCDay() + 6) % 7));
    return d.toISOString().slice(0, 10);
  };
  const monthKey = (t) => dayKey(t).slice(0, 7);

  function buckets(d) {
    const g = grain();
    const key = g === "quarter" ? (t) => t : g === "hour" ? (t) => Math.floor(t / 3600)
      : g === "day" ? dayKey : g === "week" ? weekKey : monthKey;
    const out = [];
    d.meter.forEach(([start, secs, total], i) => {
      const heat = d.heating[i]?.[2] || 0;
      const k = key(start), last = out[out.length - 1];
      if (last && last.key === k) { last.total += total; last.heat += heat; last.secs += secs; }
      else out.push({ key: k, start, secs, total, heat });
    });
    return out;
  }

  function renderGrainSwitch() {
    const host = $("usage-grain");
    host.hidden = !long();
    host.querySelectorAll("button").forEach((b) => b.classList.toggle("active", b.dataset.grain === grain()));
  }

  function render() {
    const d = U.data;
    const card = $("usage");
    if (!d || !d.meter?.length) { card.hidden = true; return; }
    card.hidden = false;
    const uh = $("usage-help");
    if (uh && !uh.childElementCount && typeof helpButton === "function") helpButton(uh, "meter", $("usage-tiles"));
    renderGrainSwitch();

    const total = d.meter.reduce((a, m) => a + m[2], 0);
    const heat = d.heating.reduce((a, h) => a + h[2], 0);
    const tiles = $("usage-tiles");
    tiles.replaceChildren();
    const span = allTime()
      ? `measured since ${new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, month: "short", year: "numeric" }).format(d.from * 1000)}`
      : `measured, last ${U.days} days`;
    tile(tiles, "home", "Household", kwh(total), span, "tone-violet");
    tile(tiles, "flame", "Radiators", kwh(Math.min(heat, total)), total > 0 ? `${Math.round((Math.min(heat, total) / total) * 100)} % of the household` : "", "tone-orange");
    tile(tiles, "bolt", "Everything else", kwh(Math.max(0, total - heat)), "", "tone-amber");
    tile(tiles, "clock", "Meter data until", dayHm(d.to), "Eloverblik is about a day behind", "tone-blue");
    renderChart(d, buckets(d));
  }

  // Local-calendar ticks: Mondays for a month, month starts beyond that; thinned to fit the width.
  function calendarTicks(t0, t1, width) {
    const fmt = new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, year: "numeric", month: "numeric",
      day: "numeric", weekday: "short", hour: "numeric", hourCycle: "h23" });
    const monthFmt = new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, month: "short" });
    const monthYearFmt = new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, month: "short", year: "numeric" });
    const parts = (t) => Object.fromEntries(fmt.formatToParts(t * 1000).map((p) => [p.type, p.value]));
    const weekly = t1 - t0 <= 45 * 86400;
    const ticks = [];
    // Walk the days (in steps of an hour around midnight, so DST shifts can't skip one).
    for (let t = t0 - (t0 % 3600); t <= t1; t += 3600) {
      const p = parts(t);
      if (Number(p.hour) !== 0) continue;  // "00" or "0", depending on the browser
      if (weekly ? p.weekday === "Mon" : Number(p.day) === 1) {
        const january = Number(p.month) === 1;
        const label = weekly ? `${Number(p.day)}/${Number(p.month)}` : (january ? monthYearFmt : monthFmt).format(t * 1000);
        ticks.push({ t, label, major: !weekly && january });
        t += 20 * 3600;  // skip ahead within the day
      }
    }
    const room = Math.max(1, Math.floor(width / 56));  // ~56 px per label
    const every = Math.ceil(ticks.length / room);
    return ticks.filter((_, i) => i % every === 0);
  }

  const UNIT = { quarter: "kW", hour: "kW", day: "kWh per day", week: "kWh per week", month: "kWh per month" };

  function renderChart(d, bars) {
    const host = $("usage-chart");
    host.replaceChildren();
    const W = host.clientWidth, H = 170, m = { l: 40, r: 8, t: 10, b: 22 };
    const g = grain();
    const energy = g === "day" || g === "week" || g === "month";  // bills compare energy; hours show power
    const kwOf = (b, v) => v / (b.secs / 3600);
    const val = (b, v) => (energy ? v : kwOf(b, v));
    const max = Math.max(0.5, ...bars.map((b) => val(b, b.total)));
    const yTicks = niceTicks(0, max, 3);
    const yMax = yTicks[yTicks.length - 1];
    const t0 = d.from, t1 = d.to;
    const x = (t) => m.l + ((t - t0) / (t1 - t0)) * (W - m.l - m.r);
    const y = (v) => m.t + (1 - v / yMax) * (H - m.t - m.b);
    const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, height: H, role: "img", "aria-label": "Household electricity use and the radiators' part" }, host);
    // Quarters and hours (2 or 7 days): night shaded behind the bars, sunrise and sunset marked on top.
    const sun = !energy ? daylight(t0, t1) : null;
    if (sun) drawNights(svg, x, nights(t0, t1, sun), m.t, H - m.b);
    for (const v of yTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
      el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, svg, v.toFixed(v < 10 ? 1 : 0));
    }
    el("text", { class: "panel-label", x: m.l, y: m.t - 1 }, svg, UNIT[g]);
    // Up to a week: the usual time axis. Longer: calendar ticks (weeks or months), never thousands.
    drawTimeAxis(svg, x, !allTime() && U.days <= 7 ? timeTicks(t0, t1, W - m.l - m.r) : calendarTicks(t0, t1, W - m.l - m.r), m.t, H - m.b, H - 6);

    const heatOf = (b) => Math.min(val(b, b.heat), val(b, b.total));
    const dense = bars.length && (W - m.l - m.r) / bars.length < 2.5;
    if (dense) {
      // Too many for bars: stepped areas (radiators at the bottom, the rest stacked on top).
      let top = "", mid = "";
      for (const b of bars) {
        const x0 = x(b.start).toFixed(1), x1 = x(b.start + b.secs).toFixed(1);
        top += `${top ? "L" : "M"}${x0},${y(val(b, b.total)).toFixed(1)}L${x1},${y(val(b, b.total)).toFixed(1)}`;
        mid += `${mid ? "L" : "M"}${x0},${y(heatOf(b)).toFixed(1)}L${x1},${y(heatOf(b)).toFixed(1)}`;
      }
      const xs = x(bars[0].start).toFixed(1), xe = x(bars[bars.length - 1].start + bars[bars.length - 1].secs).toFixed(1);
      const midBack = mid.replace(/^M/, "L").split("L").filter(Boolean).reverse().map((p) => `L${p}`).join("");
      el("path", { class: "use-rest", d: `${top}${midBack}Z` }, svg);
      el("path", { class: "use-heat", d: `${mid}L${xe},${y(0)}L${xs},${y(0)}Z` }, svg);
    } else {
      for (const b of bars) {
        const x0 = x(b.start), x1 = x(b.start + b.secs);
        const w = Math.max(1, x1 - x0 - (x1 - x0 > 4 ? 2 : 0));  // 2px gap when there's room
        const heatV = heatOf(b), restV = Math.max(0, val(b, b.total) - heatV);
        if (heatV > 0) el("rect", { class: "use-heat", x: x0, y: y(heatV), width: w, height: y(0) - y(heatV) }, svg);
        if (restV > 0) el("rect", { class: "use-rest", x: x0, y: y(heatV + restV), width: w, height: y(heatV) - y(heatV + restV) }, svg);
      }
    }

    if (sun) drawSunMarks(svg, x, sun, t0, t1, m.t);

    // One hover layer for both drawings: the bucket under the pointer.
    const dayTitle = new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, weekday: "short", day: "numeric", month: "short", year: "numeric" });
    const weekTitle = new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, day: "numeric", month: "short", year: "numeric" });
    const monthTitle = new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, month: "long", year: "numeric" });
    const hit = el("rect", { x: m.l, y: m.t, width: W - m.l - m.r, height: H - m.t - m.b, fill: "transparent" }, svg);
    hit.addEventListener("pointermove", (evt) => {
      const box = svg.getBoundingClientRect();
      const t = t0 + ((evt.clientX - box.left) * (W / box.width) - m.l) / (W - m.l - m.r) * (t1 - t0);
      const b = bars.find((bb) => t >= bb.start && t < bb.start + bb.secs);
      if (!b) return hideTooltip();
      const rows = [
        { label: "Household", value: energy ? kwh(b.total) : `${kw(kwOf(b, b.total))} · ${kwh(b.total)}`, color: "var(--ink)" },
        { label: "Radiators (estimated)", value: energy ? kwh(b.heat) : `${kw(kwOf(b, b.heat))} · ${kwh(b.heat)}`, color: "var(--flame-1)", box: true },
        { label: "Everything else", value: kwh(Math.max(0, b.total - b.heat)), color: "var(--ink-2)", box: true },
      ];
      if (b.heat > b.total * 1.05) rows.push({ label: "Note", value: "radiators > measured: is a radiator's power set too high?" });
      const sr = sun && sunRow(t, sun);
      if (sr) rows.push(sr);
      const title = g === "month" ? monthTitle.format(b.start * 1000)
        : g === "week" ? `Week of ${weekTitle.format(b.start * 1000)}`
        : g === "day" ? dayTitle.format(b.start * 1000)
        : `${dayHm(b.start)}–${hm(b.start + b.secs)}`;
      showTooltip(evt, title, rows);
    });
    hit.addEventListener("pointerleave", hideTooltip);
    el("line", { class: "axis", x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }, svg);
  }

  document.querySelectorAll("#usage-range button").forEach((b) =>
    b.addEventListener("click", () => {
      document.querySelectorAll("#usage-range button").forEach((x) => x.classList.toggle("active", x === b));
      U.days = Number(b.dataset.days);
      load(true);
    }));
  document.querySelectorAll("#usage-grain button").forEach((b) =>
    b.addEventListener("click", () => {
      U.grain = b.dataset.grain;
      try { localStorage.setItem(GRAIN_KEY, U.grain); } catch { /* fine */ }
      render();  // same data, other buckets
    }));

  window.Usage = { load, render };
})();
