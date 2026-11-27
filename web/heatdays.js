"use strict";
// Heating hours: how long each room's radiator was on, per day, week or month, stacked by room, with
// the outdoor temperature in its own panel underneath (same buckets, its own axis) and a scatter of
// heating hours against the outdoor temperature. Over a year: heating in winter, none in summer.
(() => {
  const GRAIN_KEY = "heatbrain-heatdays-grain";
  const OFF_KEY = "heatbrain-heatdays-off";
  const H = { days: 365, data: null, loadedAt: 0, grain: null, off: new Set() };
  try {
    H.grain = localStorage.getItem(GRAIN_KEY);
    H.off = new Set(JSON.parse(localStorage.getItem(OFF_KEY) || "[]"));
  } catch { /* per-browser conveniences only */ }
  const FULL_DAY = 20;  // hours of data for a day to count as a whole day (the scatter, "per day")
  const SOME_DATA = 1;  // less is the tail of the day before (the last decision runs on a little), not data
  const hrs = (v) => `${v.toFixed(v >= 100 ? 0 : 1)} h`;
  const kwh = (v) => `${v.toFixed(v >= 10 ? 0 : 1)} kWh`;
  const deg = (v) => `${v.toFixed(1)} °C`;

  async function load(force = false) {
    if (!force && Date.now() - H.loadedAt < 300_000 && H.data) return render();
    const want = H.days;
    $("heatdays-chart")?.classList.add("loading");
    try {
      const data = await api(`/api/heating-days?days=${want}`);
      if (want !== H.days) return;  // a newer range was chosen meanwhile: its answer wins
      H.data = data;
      H.loadedAt = Date.now();
      render();
    } catch { /* the dashboard shows connectivity problems already */ }
    finally { $("heatdays-chart")?.classList.remove("loading"); }
  }

  // Dates as "YYYY-MM-DD", stepped in UTC (calendar days, no time zone involved).
  const utc = (date) => new Date(`${date}T12:00:00Z`);
  const iso = (d) => d.toISOString().slice(0, 10);
  const addDays = (date, n) => { const d = utc(date); d.setUTCDate(d.getUTCDate() + n); return iso(d); };
  const weekOf = (date) => { const d = utc(date); d.setUTCDate(d.getUTCDate() - ((d.getUTCDay() + 6) % 7)); return iso(d); };
  const monthOf = (date) => `${date.slice(0, 7)}-01`;
  const fmt = (opts) => new Intl.DateTimeFormat("en-GB", { ...opts, timeZone: "UTC" });
  const dayTitle = fmt({ weekday: "short", day: "numeric", month: "short", year: "numeric" });
  const shortDay = fmt({ day: "numeric", month: "short" });
  const monthTitle = fmt({ month: "long", year: "numeric" });
  const monthShort = fmt({ month: "short" });
  const monthYear = fmt({ month: "short", year: "numeric" });

  // The span shown: from the first day with any data (or the range's start) to today.
  function span(d) {
    let first = null;
    for (const r of d.rooms) for (const x of r.days) if (x[3] >= SOME_DATA && (!first || x[0] < first)) first = x[0];
    if (!first) return null;
    const from = first > d.from ? first : d.from;
    const dates = [];
    for (let t = from; t <= d.today; t = addDays(t, 1)) dates.push(t);
    return dates;
  }

  function grain(n) {
    if (["day", "week", "month"].includes(H.grain)) return H.grain;
    return n <= 120 ? "day" : n <= 730 ? "week" : "month";
  }

  // Per bucket: hours on per room (summed), hours of data per room, days with data, outdoor temps.
  function buckets(d, dates, g) {
    const byRoom = d.rooms.map((r) => new Map(r.days.map((x) => [x[0], x])));
    const outdoor = new Map((d.outdoor?.days || []).map((x) => [x[0], x]));
    const keyOf = g === "day" ? (x) => x : g === "week" ? weekOf : monthOf;
    const out = [];
    for (const date of dates) {
      const k = keyOf(date);
      let b = out[out.length - 1];
      if (!b || b.key !== k) {
        b = { key: k, first: date, last: date, days: 0, on: d.rooms.map(() => 0), kWh: d.rooms.map(() => 0),
              cov: d.rooms.map(() => 0), roomDays: d.rooms.map(() => 0), temps: [], min: null, max: null, today: false };
        out.push(b);
      }
      b.last = date;
      b.days++;
      if (date === d.today) b.today = true;
      d.rooms.forEach((r, i) => {
        const x = byRoom[i].get(date);
        if (!x) return;
        b.on[i] += x[1]; b.kWh[i] += x[2]; b.cov[i] += x[3];
        if (x[3] >= SOME_DATA) b.roomDays[i]++;
      });
      const o = outdoor.get(date);
      if (o) {
        b.temps.push(o[1]);
        b.min = b.min == null ? o[2] : Math.min(b.min, o[2]);
        b.max = b.max == null ? o[3] : Math.max(b.max, o[3]);
      }
    }
    for (const b of out) b.avgTemp = b.temps.length ? b.temps.reduce((a, v) => a + v, 0) / b.temps.length : null;
    return out;
  }

  // A bucket's value for a room: hours on (a day), or the average per day with data (weeks, months).
  const perDay = (b, i, g) => (g === "day" ? b.on[i] : b.roomDays[i] ? b.on[i] / b.roomDays[i] : 0);
  const shown = (d) => d.rooms.map((r) => !H.off.has(r.id));
  const covered = (b) => b.cov.some((c) => c >= SOME_DATA);

  function tile(host, iconName, label, value, sub, tone = "") {
    const t = el("div", { class: `tile ${tone}` }, host);
    const k = el("div", { class: "k" }, t);
    k.appendChild(icon(iconName));
    k.appendChild(document.createTextNode(label));
    el("div", { class: "v" }, t, value);
    if (sub) el("div", { class: "s" }, t, sub);
  }

  function render() {
    const d = H.data;
    const card = $("heatdays");
    if (!card) return;
    if (!d || !d.rooms?.length) { card.hidden = true; return; }
    card.hidden = false;
    hideTooltip();  // its chart is about to be replaced
    const hh = $("heatdays-help");
    if (hh && !hh.childElementCount && typeof helpButton === "function") helpButton(hh, "heatingHours", $("heatdays-tiles"));
    const dates = span(d);
    const chart = $("heatdays-chart"), temps = $("heatdays-temps"), scatter = $("heatdays-scatter");
    if (!dates) {
      $("heatdays-tiles").replaceChildren();
      $("heatdays-legend").replaceChildren();
      $("heatdays-table").replaceChildren();
      temps.replaceChildren();
      scatter.replaceChildren();
      chart.replaceChildren(el("div", { class: "muted" }, null, "No heating recorded yet in this period."));
      return;
    }
    const g = grain(dates.length);
    document.querySelectorAll("#heatdays-grain button").forEach((b) => b.classList.toggle("active", b.dataset.grain === g));
    const bars = buckets(d, dates, g);
    const on = shown(d);
    renderTiles(d, bars, on);
    renderLegend(d);
    renderBars(d, bars, g, on);
    renderTemps(d, bars, g);
    renderScatter(d, dates, on);
    renderTable(d, bars, on);
  }

  // The rooms' colours: categorical slots in the config's room order, whatever is shown.
  const color = (i) => `var(--room-${(i % 8) + 1})`;

  function renderLegend(d) {
    const host = $("heatdays-legend");
    host.replaceChildren();
    d.rooms.forEach((r, i) => {
      const off = H.off.has(r.id);
      const b = el("button", { type: "button", class: `legend-toggle${off ? " off" : ""}`, "aria-pressed": String(!off),
                               title: off ? `Show ${r.name}` : `Hide ${r.name}` }, host);
      const sw = el("i", { class: "swatch" }, b);
      sw.style.background = off ? "transparent" : color(i);
      if (off) sw.style.boxShadow = `inset 0 0 0 1.5px ${color(i)}`;
      b.appendChild(document.createTextNode(r.name));
      b.addEventListener("click", () => {
        if (off) H.off.delete(r.id);
        else if (d.rooms.filter((x) => !H.off.has(x.id)).length > 1) H.off.add(r.id);  // keep one at least
        try { localStorage.setItem(OFF_KEY, JSON.stringify([...H.off])); } catch { /* fine */ }
        render();
      });
    });
    if (d.excluded?.length) el("span", { class: "muted" }, host, `Not counted: ${d.excluded.join(", ")} (no sensor)`);
  }

  function renderTiles(d, bars, on) {
    const tiles = $("heatdays-tiles");
    tiles.replaceChildren();
    let total = 0, energy = 0;
    const days = [];
    for (const b of bars) {
      d.rooms.forEach((_, i) => { if (on[i]) { total += b.on[i]; energy += b.kWh[i]; } });
    }
    // Whole days (not today) for "per day" and the busiest day.
    const byDay = new Map();
    d.rooms.forEach((r, i) => {
      if (!on[i]) return;
      for (const x of r.days) {
        if (x[0] === d.today || x[0] < bars[0].first) continue;
        const e = byDay.get(x[0]) || { on: 0, cov: 0 };
        e.on += x[1]; e.cov = Math.max(e.cov, x[3]);
        byDay.set(x[0], e);
      }
    });
    for (const [date, e] of byDay) if (e.cov >= FULL_DAY) days.push([date, e.on]);
    const span = `${shortDay.format(utc(bars[0].first))} – ${bars[bars.length - 1].today ? "today" : shortDay.format(utc(bars[bars.length - 1].last))}`;
    tile(tiles, "flame", "Heating hours", hrs(total), energy > 0 ? `${span} · ≈ ${kwh(energy)}` : span, "tone-orange");
    if (days.length) {
      const avg = days.reduce((a, x) => a + x[1], 0) / days.length;
      tile(tiles, "clock", "Per day", hrs(avg), `all rooms together · average of ${days.length} whole day${days.length === 1 ? "" : "s"}`, "tone-amber");
      const top = days.reduce((a, x) => (x[1] > a[1] ? x : a));
      tile(tiles, "gauge", "Most in a day", hrs(top[1]), `${dayTitle.format(utc(top[0]))} · all rooms together`, "tone-violet");
    }
    const temps = bars.flatMap((b) => b.temps);
    if (temps.length) {
      const coldest = bars.reduce((a, b) => (b.min != null && (a == null || b.min < a.min) ? b : a), null);
      tile(tiles, "thermo", "Outside", deg(temps.reduce((a, v) => a + v, 0) / temps.length),
        `average · coldest ${deg(coldest.min)}${d.outdoor?.name ? ` · ${d.outdoor.name}` : ""}`, "tone-blue");
    }
  }

  // Labels under the buckets: months (Jan with the year), or Mondays for a short range of days.
  function bucketLabels(bars, g, width) {
    const out = [];
    bars.forEach((b, i) => {
      const date = utc(b.key);
      if (g === "month") out.push({ i, label: b.key.slice(5, 7) === "01" || i === 0 ? monthYear.format(date) : monthShort.format(date), major: b.key.slice(5, 7) === "01" });
      else if (g === "week") { if (i === 0 || b.key.slice(0, 7) !== bars[i - 1].key.slice(0, 7)) out.push({ i, label: b.key.slice(5, 7) === "01" ? monthYear.format(date) : monthShort.format(date), major: b.key.slice(5, 7) === "01" }); }
      else if (bars.length <= 45) { if (date.getUTCDay() === 1 || bars.length <= 10) out.push({ i, label: shortDay.format(date) }); }
      else if (b.key.slice(8) === "01") out.push({ i, label: b.key.slice(5, 7) === "01" ? monthYear.format(date) : monthShort.format(date), major: b.key.slice(5, 7) === "01" });
    });
    // Thinned to fit, and never closer than a label's width (a range starting late in a month).
    const room = Math.max(1, Math.floor(width / 60));
    const every = Math.ceil(out.length / room);
    const bw = width / bars.length;
    const kept = [];
    for (const t of out.filter((_, k) => k % every === 0)) {
      if (kept.length && (t.i - kept[kept.length - 1].i) * bw < 52) {
        if (kept.length === 1 && kept[0].i === 0) kept.pop();  // drop the partial first one instead
        else continue;
      }
      kept.push(t);
    }
    return kept;
  }

  function bucketTitle(b, g) {
    if (g === "month") return `${monthTitle.format(utc(b.key))}${b.today ? " · so far" : ""}`;
    if (g === "week") return `Week of ${shortDay.format(utc(b.key))} ${b.key.slice(0, 4)}${b.today ? " · so far" : ""}`;
    return `${dayTitle.format(utc(b.key))}${b.today ? " · so far" : ""}`;
  }

  // The tooltip for a bucket, shared by both panels.
  function bucketRows(d, b, g, on) {
    const rows = [];
    let total = 0;
    d.rooms.forEach((r, i) => {
      if (!on[i]) return;
      const v = perDay(b, i, g);
      total += v;
      rows.push({ label: r.name, value: b.cov[i] < SOME_DATA ? "no data" : hrs(v), color: color(i), box: true });
    });
    rows.reverse();  // top of the stack first
    rows.unshift({ label: g === "day" ? "All rooms" : "All rooms, per day", value: covered(b) ? hrs(total) : "no data", color: "var(--ink)" });
    if (g !== "day" && covered(b)) {
      const sum = d.rooms.reduce((a, _, i) => a + (on[i] ? b.on[i] : 0), 0);
      rows.splice(1, 0, { label: `In the ${g}`, value: hrs(sum) });
    }
    if (b.avgTemp != null) rows.push({ label: "Outside", value: `${deg(b.avgTemp)} (${b.min.toFixed(0)} to ${b.max.toFixed(0)})`, color: "var(--ink-2)" });
    if (g === "day" && covered(b) && !b.today) {
      const most = Math.max(...b.cov);
      if (most < FULL_DAY) rows.push({ label: "Note", value: `data for ${most.toFixed(0)} of 24 h` });
    }
    return rows;
  }

  const M = { l: 40, r: 8 };
  const xBand = (W, n) => { const bw = (W - M.l - M.r) / n; return { bw, x: (i) => M.l + i * bw }; };

  function renderBars(d, bars, g, on) {
    const host = $("heatdays-chart");
    host.replaceChildren();
    const W = host.clientWidth, Ht = 190, m = { ...M, t: 14, b: 22 };
    const { bw, x } = xBand(W, bars.length);
    const stackOf = (b) => d.rooms.reduce((a, _, i) => a + (on[i] ? perDay(b, i, g) : 0), 0);
    const max = Math.max(1, ...bars.map(stackOf));
    const yTicks = niceTicks(0, max, 4);
    const yMax = yTicks[yTicks.length - 1];
    const y = (v) => m.t + (1 - v / yMax) * (Ht - m.t - m.b);
    const svg = el("svg", { viewBox: `0 0 ${W} ${Ht}`, height: Ht, role: "img", "aria-label": "Hours each radiator was on, per day, stacked by room" }, host);
    for (const v of yTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
      el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, svg, String(v));
    }
    el("text", { class: "panel-label", x: m.l, y: m.t - 4 }, svg, g === "day" ? "hours on per day" : `hours on per day (average per ${g})`);
    for (const t of bucketLabels(bars, g, W - m.l - m.r)) {
      el("line", { class: "grid", x1: x(t.i), x2: x(t.i), y1: m.t, y2: Ht - m.b, opacity: t.major ? 1 : 0.6 }, svg);
      el("text", { x: x(t.i) + 2, y: Ht - 6, "text-anchor": "start" }, svg, t.label);
    }
    const gap = bw > 6 ? 2 : bw > 3 ? 1 : 0;
    const barW = Math.max(1, Math.min(40, bw - gap));
    bars.forEach((b, k) => {
      const bx = x(k) + (bw - barW) / 2;
      if (!covered(b)) {
        // No data (heatbrain not running): a hatched stub, so it can't pass for a warm day.
        el("rect", { class: "hd-nodata", x: bx, y: y(0) - 4, width: barW, height: 4 }, svg);
        return;
      }
      let base = 0;
      const segs = d.rooms.map((_, i) => (on[i] ? perDay(b, i, g) : 0));
      const top = segs.reduce((a, v, i) => (v > 0 ? i : a), -1);
      segs.forEach((v, i) => {
        if (v <= 0) return;
        const y0 = y(base), y1 = y(base + v);
        // A 1px surface gap between stacked segments when there is height for it; rounded top end.
        const h = Math.max(0.5, y0 - y1 - (i === top ? 0 : (y0 - y1 > 3 ? 1 : 0)));
        const r = i === top ? Math.min(3, barW / 2, h) : 0;
        const yt = y0 - h;
        el("path", { class: `hd-seg${b.today ? " today" : ""}`, fill: color(i),
          d: r ? `M${bx},${y0}V${yt + r}Q${bx},${yt} ${bx + r},${yt}H${bx + barW - r}Q${bx + barW},${yt} ${bx + barW},${yt + r}V${y0}Z`
               : `M${bx},${y0}V${y0 - h}H${bx + barW}V${y0}Z` }, svg);
        base += v;
      });
    });
    el("line", { class: "axis", x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }, svg);
    hover(svg, W, bars, (evt, b) => showTooltip(evt, bucketTitle(b, g), bucketRows(d, b, g, on)), m, Ht, x, bw);
  }

  // The outdoor temperature per bucket: the day's (or the bucket's) low-high band and the average.
  function renderTemps(d, bars, g) {
    const host = $("heatdays-temps");
    host.replaceChildren();
    if (!bars.some((b) => b.avgTemp != null)) return;
    const W = host.clientWidth, Ht = 120, m = { ...M, t: 14, b: 8 };
    const { bw, x } = xBand(W, bars.length);
    const lo = Math.min(...bars.filter((b) => b.min != null).map((b) => b.min));
    const hi = Math.max(...bars.filter((b) => b.max != null).map((b) => b.max));
    const yTicks = niceTicks(Math.min(0, lo), Math.max(hi, 5), 3);
    const y0 = yTicks[0], y1 = yTicks[yTicks.length - 1];
    const y = (v) => m.t + (1 - (v - y0) / (y1 - y0)) * (Ht - m.t - m.b);
    const svg = el("svg", { viewBox: `0 0 ${W} ${Ht}`, height: Ht, role: "img", "aria-label": "Outdoor temperature" }, host);
    for (const v of yTicks) {
      el("line", { class: v === 0 ? "axis" : "grid", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
      el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, svg, `${v}°`);
    }
    el("text", { class: "panel-label", x: m.l, y: m.t - 4 }, svg, `outside${d.outdoor?.name ? ` · ${d.outdoor.name}` : ""} (°C, low–high and average)`);
    for (const t of bucketLabels(bars, g, W - m.l - m.r)) {
      el("line", { class: "grid", x1: x(t.i), x2: x(t.i), y1: m.t, y2: Ht - m.b, opacity: t.major ? 1 : 0.6 }, svg);
    }
    // Runs of buckets with a temperature: band and line, broken where there is none.
    let band = "", line = "", run = [];
    const flush = () => {
      if (!run.length) return;
      const pts = run.map((k) => [x(k) + bw / 2, bars[k]]);
      band += `M${pts.map(([px, b]) => `${px.toFixed(1)},${y(b.max).toFixed(1)}`).join("L")}` +
              `L${pts.reverse().map(([px, b]) => `${px.toFixed(1)},${y(b.min).toFixed(1)}`).join("L")}Z`;
      pts.reverse();
      line += `M${pts.map(([px, b]) => `${px.toFixed(1)},${y(b.avgTemp).toFixed(1)}`).join("L")}`;
      if (run.length === 1) el("circle", { class: "hd-temp-dot", cx: pts[0][0], cy: y(pts[0][1].avgTemp), r: 2.5 }, svg);
      run = [];
    };
    bars.forEach((b, k) => { if (b.avgTemp == null) flush(); else run.push(k); });
    flush();
    el("path", { class: "hd-temp-band", d: band }, svg);
    el("path", { class: "hd-temp-line", d: line }, svg);
    hover(svg, W, bars, (evt, b) => showTooltip(evt, bucketTitle(b, g), bucketRows(d, b, g, shown(d))), m, Ht, x, bw);
  }

  function hover(svg, W, bars, show, m, Ht, x, bw) {
    const cross = el("rect", { class: "hd-cross", x: 0, y: m.t, width: bw, height: Ht - m.t - m.b, visibility: "hidden" }, svg);
    const hit = el("rect", { x: m.l, y: m.t, width: W - m.l - m.r, height: Ht - m.t - m.b, fill: "transparent" }, svg);
    hit.addEventListener("pointermove", (evt) => {
      const box = svg.getBoundingClientRect();
      const px = (evt.clientX - box.left) * (W / box.width);
      const k = Math.min(bars.length - 1, Math.max(0, Math.floor((px - m.l) / bw)));
      cross.setAttribute("x", x(k));
      cross.setAttribute("visibility", "visible");
      show(evt, bars[k]);
    });
    hit.addEventListener("pointerleave", () => { cross.setAttribute("visibility", "hidden"); hideTooltip(); });
  }

  // Each whole day as a dot: the rooms' hours together against the day's average outdoor temperature.
  // A straight line through the days that heated shows where heating stops ("balance point").
  function renderScatter(d, dates, on) {
    const host = $("heatdays-scatter");
    host.replaceChildren();
    const outdoor = new Map((d.outdoor?.days || []).map((x) => [x[0], x[1]]));
    const byRoom = d.rooms.map((r) => new Map(r.days.map((x) => [x[0], x])));
    const firstDay = d.rooms.map((r) => r.days.find((x) => x[3] >= SOME_DATA)?.[0] ?? null);
    const pts = [];
    for (const date of dates) {
      if (date === d.today || !outdoor.has(date)) continue;
      let sum = 0, ok = true, any = false;
      d.rooms.forEach((_, i) => {
        if (!on[i] || !firstDay[i] || date < firstDay[i]) return;  // the room wasn't set up yet
        const x = byRoom[i].get(date);
        if (!x || x[3] < FULL_DAY) ok = false;
        else { sum += x[1]; any = true; }
      });
      if (ok && any) pts.push({ date, t: outdoor.get(date), h: sum });
    }
    const head = el("div", { class: "hd-scatter-head" }, host);
    el("div", { class: "panel-title" }, head, "Hours per day against the outdoor temperature");
    if (pts.length < 3) {
      el("div", { class: "muted hd-scatter-empty" }, host,
        d.outdoor ? "A dot for each whole day with an outdoor reading - a few days more and they show up here."
                  : "Needs an outdoor sensor (Settings → Sensors).");
      return;
    }
    const W = host.clientWidth, Ht = 300, m = { l: 40, r: 10, t: 10, b: 30 };
    const xs = pts.map((p) => p.t), hs = pts.map((p) => p.h);
    const xTicks = niceTicks(Math.min(...xs) - 0.5, Math.max(...xs) + 0.5, 5);
    const yTicks = niceTicks(0, Math.max(1, ...hs), 4);
    const xa = xTicks[0], xb = xTicks[xTicks.length - 1], yMax = yTicks[yTicks.length - 1];
    const x = (v) => m.l + ((v - xa) / (xb - xa)) * (W - m.l - m.r);
    const y = (v) => m.t + (1 - v / yMax) * (Ht - m.t - m.b);
    const svg = el("svg", { viewBox: `0 0 ${W} ${Ht}`, height: Ht, role: "img", "aria-label": "Heating hours per day against the outdoor temperature" }, host);
    for (const v of yTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
      el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, svg, `${v} h`);
    }
    for (const v of xTicks) {
      el("line", { class: "grid", x1: x(v), x2: x(v), y1: m.t, y2: Ht - m.b }, svg);
      el("text", { x: x(v), y: Ht - m.b + 14, "text-anchor": "middle" }, svg, `${v}°`);
    }
    el("text", { x: W - m.r, y: Ht - 2, "text-anchor": "end" }, svg, "outside, day average (°C)");
    el("line", { class: "axis", x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }, svg);

    // Least squares through the days that heated (a day without heat would bend the line).
    const fitPts = pts.filter((p) => p.h > 0.25);
    let note = null;
    if (fitPts.length >= 7 && Math.max(...fitPts.map((p) => p.t)) - Math.min(...fitPts.map((p) => p.t)) >= 4) {
      const n = fitPts.length, mx = fitPts.reduce((a, p) => a + p.t, 0) / n, my = fitPts.reduce((a, p) => a + p.h, 0) / n;
      const sxx = fitPts.reduce((a, p) => a + (p.t - mx) ** 2, 0), sxy = fitPts.reduce((a, p) => a + (p.t - mx) * (p.h - my), 0);
      const slope = sxy / sxx, icept = my - slope * mx;
      if (slope < 0) {
        const zero = -icept / slope;  // where the line reaches 0 h
        // Clipped to the plot along the line itself (where it leaves the top, and where it reaches 0 h).
        const ta = Math.max(xa, (yMax - icept) / slope), tb = Math.min(xb, zero);
        const at = (t) => icept + slope * t;
        if (tb > ta) el("line", { class: "hd-fit", x1: x(ta), y1: y(at(ta)), x2: x(tb), y2: y(at(tb)) }, svg);
        note = `About ${Math.abs(slope).toFixed(1)} h more a day per degree colder` +
               (zero > xa && zero < 30 ? ` · no heating above ≈ ${zero.toFixed(0)} °C` : "");
      }
    }
    for (const p of pts) {
      const c = el("circle", { class: "hd-dot", cx: x(p.t), cy: y(p.h), r: 4 }, svg);
      const t = el("circle", { cx: x(p.t), cy: y(p.h), r: 9, fill: "transparent" }, svg);  // a bigger hit target
      t.addEventListener("pointermove", (evt) => {
        c.classList.add("hover");
        showTooltip(evt, dayTitle.format(utc(p.date)), [{ label: "Heating", value: hrs(p.h) }, { label: "Outside", value: deg(p.t) }]);
      });
      t.addEventListener("pointerleave", () => { c.classList.remove("hover"); hideTooltip(); });
    }
    el("div", { class: "hint" }, host, note ||
      `${pts.length} whole days so far. With a week or more of heating across a few degrees, a line shows how heating follows the cold.`);
  }

  function renderTable(d, bars, on) {
    const body = $("heatdays-table");
    body.replaceChildren();
    let all = 0;
    const rows = d.rooms.map((r, i) => {
      const hours = bars.reduce((a, b) => a + b.on[i], 0), energy = bars.reduce((a, b) => a + b.kWh[i], 0);
      const days = r.days.filter((x) => x[0] >= bars[0].first && x[0] !== d.today && x[3] >= FULL_DAY);
      const daily = days.length ? days.reduce((a, x) => a + x[1], 0) / days.length : null;
      if (on[i]) all += hours;
      return { r, i, hours, energy, daily };
    });
    for (const x of rows) {
      const tr = el("tr", { class: on[x.i] ? "" : "muted" }, body);
      const td = el("td", {}, tr);
      const sw = el("i", { class: "swatch" }, td);
      sw.style.background = color(x.i);
      sw.style.marginRight = "6px";
      td.appendChild(document.createTextNode(x.r.name));
      el("td", { class: "num" }, tr, hrs(x.hours));
      el("td", { class: "num" }, tr, x.daily == null ? "–" : hrs(x.daily));
      el("td", { class: "num" }, tr, x.energy > 0 ? kwh(x.energy) : "–");
      el("td", { class: "num" }, tr, on[x.i] && all > 0 ? `${Math.round((x.hours / all) * 100)} %` : "–");
      el("td", { class: "muted" }, tr, x.r.measured ? "measured by the thermostat" : x.r.thermostat ? "thermostat" : "plug on-time");
    }
  }

  document.querySelectorAll("#heatdays-range button").forEach((b) =>
    b.addEventListener("click", () => {
      document.querySelectorAll("#heatdays-range button").forEach((x) => x.classList.toggle("active", x === b));
      H.days = Number(b.dataset.days);
      load(true);
    }));
  document.querySelectorAll("#heatdays-grain button").forEach((b) =>
    b.addEventListener("click", () => {
      H.grain = b.dataset.grain;
      try { localStorage.setItem(GRAIN_KEY, H.grain); } catch { /* fine */ }
      render();  // same data, other buckets
    }));

  window.HeatDays = { load, render };
})();
