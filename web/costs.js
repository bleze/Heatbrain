"use strict";
// Heating cost card: estimated from on-time x radiator wattage x the real price per slot.
(() => {
  const C = { days: 7, data: null, loadedAt: 0 };
  const money = (v, cur) => `${v.toFixed(v >= 100 ? 0 : 2)} ${cur === "DKK" ? "kr" : cur}`;
  const kwh = (v) => `${v.toFixed(v >= 10 ? 0 : 1)} kWh`;

  async function load(force = false) {
    if (!force && Date.now() - C.loadedAt < 120_000 && C.data) return render();
    try {
      C.data = await api(`/api/costs?days=${C.days}`);
      C.loadedAt = Date.now();
      render();
    } catch (e) { /* the dashboard shows connectivity problems already */ }
  }

  function tile(host, iconName, label, value, sub, cls = "") {
    const t = el("div", { class: `tile ${cls}` }, host);
    const k = el("div", { class: "k" }, t);
    k.appendChild(icon(iconName));
    k.appendChild(document.createTextNode(label));
    el("div", { class: "v" }, t, value);
    if (sub) el("div", { class: "s" }, t, sub);
  }

  function render() {
    const d = C.data;
    const card = $("costs");
    if (!d || !d.rooms.length) { card.hidden = true; return; }
    card.hidden = false;
    const cur = d.currency;
    const rooms = d.rooms.filter((r) => r.total);
    const missing = d.rooms.filter((r) => !r.total);

    // Tiles
    const tiles = $("cost-tiles");
    tiles.replaceChildren();
    const priced = rooms.reduce((a, r) => a + r.days.reduce((b, x) => b + x.kWh - x.kWhWithoutPrice, 0), 0);
    const anyLive = rooms.some((r) => r.total.liveKWh > 0);
    tile(tiles, "coin", anyLive ? "Spent" : "Would have cost", money(d.total.cost, cur), `last ${d.days} days`);
    tile(tiles, "bolt", "Energy", kwh(d.total.kWh), `${rooms.reduce((a, r) => a + r.total.onHours, 0).toFixed(0)} heating hours`);
    if (priced > 0) {
      const paid = d.total.cost / priced;
      const market = (d.total.cost + d.total.saved) / priced;
      tile(tiles, "gauge", "Average price paid", `${paid.toFixed(2)}`, `${cur}/kWh · day average ${market.toFixed(2)}`);
      const pct = market > 0 ? (d.total.saved / (d.total.cost + d.total.saved)) * 100 : 0;
      tile(tiles, "check", d.total.saved >= 0 ? "Saved by timing" : "Extra from timing",
        money(Math.abs(d.total.saved), cur), `${Math.abs(pct).toFixed(0)}% vs paying the day's average price`,
        d.total.saved >= 0 ? "good" : "");
    }

    renderChart(d, rooms);

    // Table (also the accessible view of the chart)
    const body = $("cost-table");
    body.replaceChildren();
    for (const r of rooms) {
      const tr = el("tr", {}, body);
      el("td", {}, tr, r.name);
      el("td", { class: "num" }, tr, `${r.total.onHours.toFixed(1)} h`);
      el("td", { class: "num" }, tr, kwh(r.total.kWh));
      el("td", { class: "num" }, tr, money(r.total.cost, cur));
      const p = r.days.reduce((b, x) => b + x.kWh - x.kWhWithoutPrice, 0);
      el("td", { class: "num" }, tr, p > 0 ? (r.total.cost / p).toFixed(2) : "–");
      el("td", { class: "num" }, tr, money(r.total.saved, cur));
      el("td", { class: "muted" }, tr, r.total.liveKWh > 0 ? `${r.watts} W` : `${r.watts} W · dry-run (simulated)`);
    }
    for (const r of missing) {
      const tr = el("tr", {}, body);
      el("td", {}, tr, r.name);
      const td = el("td", { colspan: 6, class: "muted" }, tr, "Set the radiator power for ");
      td.appendChild(document.createTextNode(r.missingWatts.join(", ") || "its plugs"));
      td.appendChild(document.createTextNode(" in "));
      el("a", { href: "#settings" }, td, "Settings → Plugs");
    }
    $("cost-note").textContent =
      "Estimated: radiator power × time the plug was on × the actual price in each 15-minute slot. " +
      "The Sonoff Basic can't measure power, and a radiator's own thermostat may switch it off now and then.";
  }

  function renderChart(d, rooms) {
    const host = $("cost-chart");
    host.replaceChildren();
    // Daily totals across rooms; one series, the breakdown lives in the tooltip and table.
    const byDate = new Map();
    for (const r of rooms) {
      for (const x of r.days) {
        const e = byDate.get(x.date) || { cost: 0, kWh: 0, rooms: [] };
        e.cost += x.cost; e.kWh += x.kWh; e.rooms.push([r.name, x.cost, x.kWh]);
        byDate.set(x.date, e);
      }
    }
    // Every day of the period, including days without heating.
    const dates = [];
    for (let i = d.days - 1; i >= 0; i--) {
      const t = d.to - i * 86400;
      dates.push(new Intl.DateTimeFormat("en-CA", { timeZone: state.tz, year: "numeric", month: "2-digit", day: "2-digit" }).format(t * 1000));
    }
    const W = host.clientWidth, H = 150, m = { l: 40, r: 8, t: 10, b: 22 };
    const max = Math.max(1, ...dates.map((x) => byDate.get(x)?.cost || 0));
    const yTicks = niceTicks(0, max, 3);
    const yMax = yTicks[yTicks.length - 1];
    const bw = (W - m.l - m.r) / dates.length;
    const y = (v) => m.t + (1 - v / yMax) * (H - m.t - m.b);
    const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, height: H, role: "img", "aria-label": "Estimated heating cost per day" }, host);
    const defs = el("defs", {}, svg);
    const g = el("linearGradient", { id: "cost-fill", x1: 0, y1: 0, x2: 0, y2: 1 }, defs);
    el("stop", { offset: "0", "stop-color": "#f59f00" }, g);
    el("stop", { offset: "1", "stop-color": "#e8590c" }, g);
    for (const v of yTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
      el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, svg, v.toFixed(v < 10 ? 1 : 0));
    }
    const every = Math.ceil(dates.length / Math.max(1, Math.floor((W - m.l) / 56)));
    dates.forEach((date, i) => {
      const e = byDate.get(date);
      const x0 = m.l + i * bw;
      const day = new Date(`${date}T12:00:00Z`);
      if (i % every === 0) {
        const label = new Intl.DateTimeFormat("en-GB", { weekday: "short", day: "numeric", timeZone: "UTC" }).format(day);
        el("text", { x: x0 + bw / 2, y: H - 6, "text-anchor": "middle" }, svg, label);
      }
      const h = e ? Math.max(e.cost > 0 ? 2 : 0, y(0) - y(e.cost)) : 0;
      const barW = Math.max(2, Math.min(36, bw - 4));  // 2px+ surface gap between bars
      const bx = x0 + (bw - barW) / 2;
      if (h > 0) {
        // Rounded data end at the top, square at the baseline.
        const r = Math.min(4, barW / 2, h);
        const top = y(0) - h;
        el("path", { class: "bar", d: `M${bx},${y(0)}V${top + r}Q${bx},${top} ${bx + r},${top}H${bx + barW - r}Q${bx + barW},${top} ${bx + barW},${top + r}V${y(0)}Z` }, svg);
      }
      // Hit target: the whole day column, not just the painted bar.
      const hit = el("rect", { x: x0, y: m.t, width: bw, height: H - m.t - m.b, fill: "transparent" }, svg);
      hit.addEventListener("pointermove", (evt) => {
        const rows = [{ label: "Total", value: e ? money(e.cost, d.currency) : "–" }];
        for (const [name, cost, k] of e?.rooms || []) rows.push({ label: name, value: `${money(cost, d.currency)} · ${kwh(k)}` });
        if (d.market[date] != null) rows.push({ label: "Day average price", value: `${d.market[date].toFixed(2)}` });
        showTooltip(evt, new Intl.DateTimeFormat("en-GB", { weekday: "long", day: "numeric", month: "short", timeZone: "UTC" }).format(day), rows);
      });
      hit.addEventListener("pointerleave", hideTooltip);
    });
    el("line", { class: "axis", x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }, svg);
  }

  document.querySelectorAll("#cost-range button").forEach((b) =>
    b.addEventListener("click", () => {
      document.querySelectorAll("#cost-range button").forEach((x) => x.classList.toggle("active", x === b));
      C.days = Number(b.dataset.days);
      load(true);
    }));

  window.Costs = { load, render };
})();
