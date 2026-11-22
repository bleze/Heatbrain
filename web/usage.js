"use strict";
// Electricity use: the household's measured use (Eloverblik, a day behind) with the part heatbrain's
// radiators drew in the same intervals. One axis (kW), stacked: radiators at the bottom, the rest on top.
(() => {
  const U = { days: 7, data: null, loadedAt: 0 };
  const kwh = (v) => `${v.toFixed(v >= 10 ? 0 : 1)} kWh`;
  const kw = (v) => `${v.toFixed(v >= 10 ? 1 : 2)} kW`;

  async function load(force = false) {
    if (!force && Date.now() - U.loadedAt < 300_000 && U.data) return render();
    try {
      U.data = await api(`/api/usage?days=${U.days}`);
      U.loadedAt = Date.now();
      render();
    } catch { /* optional feature */ }
  }

  function tile(host, iconName, label, value, sub) {
    const t = el("div", { class: "tile" }, host);
    const k = el("div", { class: "k" }, t);
    k.appendChild(icon(iconName));
    k.appendChild(document.createTextNode(label));
    el("div", { class: "v" }, t, value);
    if (sub) el("div", { class: "s" }, t, sub);
  }

  // Hourly buckets beyond two days: 15-minute bars get too thin to read or hover.
  function buckets(d) {
    const step = U.days > 2 ? 3600 : 0;
    const out = [];
    d.meter.forEach(([start, secs, total], i) => {
      const heat = d.heating[i]?.[2] || 0;
      const key = step ? Math.floor(start / step) * step : start;
      const last = out[out.length - 1];
      if (last && last.start === key) { last.total += total; last.heat += heat; last.secs += secs; }
      else out.push({ start: key, secs, total, heat });
    });
    return out;
  }

  function render() {
    const d = U.data;
    const card = $("usage");
    if (!d || !d.meter?.length) { card.hidden = true; return; }
    card.hidden = false;
    const uh = $("usage-help");
    if (uh && !uh.childElementCount && typeof helpButton === "function") helpButton(uh, "meter", $("usage-tiles"));

    const total = d.meter.reduce((a, m) => a + m[2], 0);
    const heat = d.heating.reduce((a, h) => a + h[2], 0);
    const tiles = $("usage-tiles");
    tiles.replaceChildren();
    tile(tiles, "home", "Household", kwh(total), `measured, last ${U.days} days`);
    tile(tiles, "flame", "Radiators", kwh(Math.min(heat, total)), total > 0 ? `${Math.round((Math.min(heat, total) / total) * 100)} % of the household` : "");
    tile(tiles, "bolt", "Everything else", kwh(Math.max(0, total - heat)), "");
    tile(tiles, "clock", "Meter data until", dayHm(d.to), "Eloverblik is about a day behind");
    renderChart(d, buckets(d));
  }

  function renderChart(d, bars) {
    const host = $("usage-chart");
    host.replaceChildren();
    const W = host.clientWidth, H = 170, m = { l: 40, r: 8, t: 10, b: 22 };
    const kwOf = (b, v) => v / (b.secs / 3600);  // average power over the bar
    const max = Math.max(0.5, ...bars.map((b) => kwOf(b, b.total)));
    const yTicks = niceTicks(0, max, 3);
    const yMax = yTicks[yTicks.length - 1];
    const t0 = d.from, t1 = d.to;
    const x = (t) => m.l + ((t - t0) / (t1 - t0)) * (W - m.l - m.r);
    const y = (v) => m.t + (1 - v / yMax) * (H - m.t - m.b);
    const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, height: H, role: "img", "aria-label": "Household electricity use and the radiators' part" }, host);
    for (const v of yTicks) {
      el("line", { class: "grid", x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }, svg);
      el("text", { x: m.l - 6, y: y(v) + 4, "text-anchor": "end" }, svg, v.toFixed(v < 10 ? 1 : 0));
    }
    el("text", { class: "panel-label", x: m.l, y: m.t - 1 }, svg, "kW");
    drawTimeAxis(svg, x, timeTicks(t0, t1, W - m.l - m.r), m.t, H - m.b, H - 6);
    for (const b of bars) {
      const x0 = x(b.start), x1 = x(b.start + b.secs);
      const w = Math.max(1, x1 - x0 - (x1 - x0 > 4 ? 2 : 0));  // 2px gap when there's room
      const heatKw = Math.min(kwOf(b, b.heat), kwOf(b, b.total)), restKw = Math.max(0, kwOf(b, b.total) - heatKw);
      if (heatKw > 0) el("rect", { class: "use-heat", x: x0, y: y(heatKw), width: w, height: y(0) - y(heatKw) }, svg);
      if (restKw > 0) el("rect", { class: "use-rest", x: x0, y: y(heatKw + restKw), width: w, height: y(heatKw) - y(heatKw + restKw) }, svg);
      const hit = el("rect", { x: x0, y: m.t, width: Math.max(2, x1 - x0), height: H - m.t - m.b, fill: "transparent" }, svg);
      hit.addEventListener("pointermove", (evt) => {
        const rows = [
          { label: "Household", value: `${kw(kwOf(b, b.total))} · ${kwh(b.total)}`, color: "var(--ink)" },
          { label: "Radiators (estimated)", value: `${kw(kwOf(b, b.heat))} · ${kwh(b.heat)}`, color: "var(--flame-1)", box: true },
          { label: "Everything else", value: kwh(Math.max(0, b.total - b.heat)), color: "var(--ink-2)", box: true },
        ];
        if (b.heat > b.total * 1.05) rows.push({ label: "Note", value: "radiators > measured: is a radiator's power set too high?" });
        showTooltip(evt, `${dayHm(b.start)}–${hm(b.start + b.secs)}`, rows);
      });
      hit.addEventListener("pointerleave", hideTooltip);
    }
    el("line", { class: "axis", x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }, svg);
  }

  document.querySelectorAll("#usage-range button").forEach((b) =>
    b.addEventListener("click", () => {
      document.querySelectorAll("#usage-range button").forEach((x) => x.classList.toggle("active", x === b));
      U.days = Number(b.dataset.days);
      load(true);
    }));

  window.Usage = { load, render };
})();
