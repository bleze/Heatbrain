"use strict";
// Settings editor. Edits a copy of the config in memory; "Save & apply" sends it to the
// server, which validates, backs up, saves and reloads the controller. Device actions
// (provision, restore, test message) always use the saved config.
(() => {
  const MASK = "********";
  const TABS = [
    ["rooms", "Rooms", "home"],
    ["plugs", "Plugs", "plug"],
    ["sensors", "Sensors", "thermo"],
    ["prices", "Prices", "bolt"],
    ["notifications", "Notifications", "bell"],
    ["system", "System", "cpu"],
  ];
  const S = { cfg: null, saved: "", errors: [], tab: "rooms", plugStatus: null, discovered: null, newIds: new Set() };

  // Editors create empty helper objects/arrays while rendering; they don't count as changes.
  const prune = (x) => {
    if (Array.isArray(x)) return x.map(prune);
    if (x && typeof x === "object") {
      const out = {};
      for (const [k, v] of Object.entries(x)) {
        const pv = prune(v);
        const empty = pv && typeof pv === "object" && Object.keys(pv).length === 0;
        if (!empty) out[k] = pv;
      }
      return out;
    }
    return x;
  };
  const dirty = () => S.cfg && JSON.stringify(prune(S.cfg)) !== JSON.stringify(prune(JSON.parse(S.saved)));
  const changed = () => { $("savebar").hidden = !dirty(); };

  async function load() {
    const r = await api("/api/config");
    S.cfg = r.config;
    S.saved = JSON.stringify(r.config);
    S.errors = r.errors || [];
    S.newIds.clear();
  }

  async function open() {
    try {
      if (!S.cfg) await load();
      render();
    } catch (e) { toast(`Could not load settings: ${e.message}`, true); }
  }

  // --- small form helpers ------------------------------------------------------------
  function field(parent, label, input, opts = {}) {
    const f = el("label", { class: `field${opts.wide ? " wide" : ""}` }, parent);
    el("span", {}, f, label);
    f.appendChild(input);
    if (opts.hint) el("div", { class: "hint" }, f, opts.hint);
    return f;
  }
  function num(obj, key, opts = {}) {
    const i = el("input", { type: "number", step: opts.step ?? "any", min: opts.min, max: opts.max, placeholder: opts.placeholder ?? "" });
    i.value = obj[key] ?? "";
    i.addEventListener("input", () => {
      if (i.value === "") delete obj[key]; else obj[key] = Number(i.value);
      opts.onChange?.(); changed();
    });
    return i;
  }
  function txt(obj, key, opts = {}) {
    const i = el("input", { type: opts.type || "text", placeholder: opts.placeholder ?? "", autocomplete: "off", spellcheck: "false" });
    i.value = obj[key] ?? "";
    i.addEventListener("input", () => { obj[key] = i.value; opts.onChange?.(); changed(); });
    return i;
  }
  function secret(obj, key, placeholder) {
    // Masked values stay masked unless the user types a new one.
    const i = txt(obj, key, { type: "password", placeholder });
    i.addEventListener("focus", () => { if (i.value === MASK) i.select(); });
    return i;
  }
  function sel(obj, key, options, onChange) {
    const s = el("select");
    const current = obj[key] ?? "";
    if (current && !options.some(([v]) => v === current)) options = [[current, `${current} (not found)`], ...options];
    for (const [v, label] of options) {
      const o = el("option", { value: v }, s, label);
      if (v === current) o.selected = true;
    }
    s.addEventListener("change", () => { obj[key] = s.value; onChange?.(); changed(); });
    return s;
  }
  function chk(obj, key, label, def = false, onChange) {
    const l = el("label", { class: "check" });
    const i = el("input", { type: "checkbox" }, l);
    i.checked = obj[key] ?? def;
    i.addEventListener("change", () => { obj[key] = i.checked; onChange?.(); changed(); });
    l.appendChild(document.createTextNode(label));
    return l;
  }
  function time(obj, key) {
    const i = el("input", { type: "time", step: 900 });
    i.value = obj[key] === "24:00" ? "00:00" : (obj[key] ?? "");
    i.addEventListener("input", () => { obj[key] = i.value || "00:00"; changed(); renderTimeline?.(); });
    return i;
  }
  function btn(parent, label, iconName, onClick, cls = "") {
    const b = el("button", { type: "button", class: cls }, parent);
    if (iconName) b.appendChild(icon(iconName));
    b.appendChild(document.createTextNode(label));
    b.addEventListener("click", (e) => { e.preventDefault(); onClick(b); });
    return b;
  }
  // Two-step confirmation without browser dialogs.
  function confirmBtn(parent, label, iconName, question, onYes, cls = "") {
    const wrap = el("span", { class: "confirm" }, parent);
    const show = () => {
      wrap.replaceChildren();
      btn(wrap, label, iconName, () => {
        wrap.replaceChildren();
        el("span", {}, wrap, question);
        btn(wrap, "Yes", "check", async (b) => { b.disabled = true; await onYes(); show(); }, "primary");
        btn(wrap, "Cancel", null, show);
      }, cls);
    };
    show();
    return wrap;
  }
  function slug(name, taken) {
    const base = (name || "item").toLowerCase().normalize("NFKD").replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "item";
    let id = base, n = 2;
    while (taken.has(id)) id = `${base}-${n++}`;
    return id;
  }
  function sectionHead(parent, title, iconName, tone, desc) {
    const head = el("div", { class: "card-head" }, parent);
    const h = el("h2", {}, head);
    h.appendChild(badge(iconName, tone));
    h.appendChild(document.createTextNode(title));
    if (desc) el("div", { class: "hint", style: "margin:6px 0 0" }, parent, desc);
    return head;
  }
  const arr = (obj, key) => (obj[key] ||= []);

  // --- layout -------------------------------------------------------------------------
  function render() {
    const tabs = $("settings-tabs");
    tabs.replaceChildren();
    for (const [id, label, iconName] of TABS) {
      const b = btn(tabs, label, iconName, () => { S.tab = id; render(); });
      b.classList.toggle("active", S.tab === id);
    }
    const body = $("settings-body");
    body.replaceChildren();
    if (S.errors.length) {
      const box = el("div", { class: "problem critical" }, body);
      const label = el("span", { class: "label" }, box);
      label.appendChild(icon("alert"));
      label.appendChild(document.createTextNode("Fix before saving"));
      const ul = el("ul", { style: "margin:0;padding-left:18px" }, box);
      for (const e of S.errors) el("li", {}, ul, e);
    }
    ({ rooms: renderRooms, plugs: renderPlugs, sensors: renderSensors, prices: renderPrices,
       notifications: renderNotifications, system: renderSystem })[S.tab](body);
    changed();
  }

  // --- Rooms --------------------------------------------------------------------------
  let renderTimeline = null;

  function sensorOptions() {
    const out = [["", "— choose a sensor —"]];
    for (const s of state.status?.sensors || []) {
      if (s.kind !== "indoor") continue;
      const source = s.key.split(":")[0];
      const t = s.temperature != null ? ` · ${s.temperature.toFixed(1)}°` : "";
      out.push([`${source}:${s.name}`, `${s.name}${t}`]);
    }
    return out;
  }

  function renderRooms(body) {
    const card = el("section", { class: "card" }, body);
    const head = sectionHead(card, "Rooms", "home", "violet",
      "Each room has plugs, a temperature sensor and target temperatures through the day. Rooms without a sensor can run on price alone.");
    btn(head, "Add room", "plus", () => {
      const rooms = arr(S.cfg, "rooms");
      const id = slug("room", new Set(rooms.map((r) => r.id)));
      S.newIds.add(id);
      rooms.push({ id, name: "New room", plugs: [], sensor: "", minTemp: 16, maxTemp: 22,
        schedule: [{ from: "07:00", to: "22:00", target: 20 }, { from: "22:00", to: "07:00", target: 17 }] });
      render();
    }, "primary");

    const rooms = S.cfg.rooms || [];
    if (!rooms.length) el("div", { class: "empty-state" }, card, "No rooms yet. Add plugs first, then a room.");
    const list = el("div", { class: "list" }, card);
    rooms.forEach((room, idx) => renderRoom(list, room, idx));
  }

  function renderRoom(list, room, idx) {
    const c = el("div", { class: "card", style: "margin:0;background:var(--surface-2)" }, list);
    const head = el("div", { class: "item-head" }, c);
    const left = el("div", { class: "row" }, head);
    left.appendChild(badge(room.priceOnly ? "bolt" : "thermo", room.priceOnly ? "amber" : "warm"));
    const name = txt(room, "name", { onChange: () => {
      if (S.newIds.has(room.id)) {  // new rooms follow their name; existing ids are kept (history)
        const taken = new Set(S.cfg.rooms.filter((r) => r !== room).map((r) => r.id));
        S.newIds.delete(room.id);
        room.id = slug(room.name, taken);
        S.newIds.add(room.id);
        idLabel.textContent = `id: ${room.id}`;
      }
    } });
    name.style.fontWeight = "600";
    name.style.fontSize = "15px";
    left.appendChild(name);
    const idLabel = el("span", { class: "muted" }, left, `id: ${room.id}`);

    const right = el("div", { class: "row" }, head);
    const seg = el("div", { class: "segmented" }, right);
    const setMode = (priceOnly) => {
      if (priceOnly && !room.priceOnly) room.priceOnly = { hoursPerDay: 12, avoid: [{ from: "17:00", to: "21:00" }] };
      if (!priceOnly) {
        delete room.priceOnly;
        room.schedule ||= [{ from: "00:00", to: "24:00", target: 20 }];
        room.sensor ??= "";
      }
      changed(); render();
    };
    btn(seg, "Temperature", "thermo", () => setMode(false)).classList.toggle("active", !room.priceOnly);
    btn(seg, "Price only", "bolt", () => setMode(true)).classList.toggle("active", !!room.priceOnly);
    confirmBtn(right, "", "trash", "Delete room?", () => { S.cfg.rooms.splice(idx, 1); changed(); render(); }, "ghost danger");

    // Plugs
    const plugsRow = el("div", { class: "row", style: "margin-top:12px" }, c);
    el("span", { class: "muted" }, plugsRow, "Plugs:");
    const plugs = S.cfg.plugs || [];
    if (!plugs.length) el("span", { class: "hint" }, plugsRow, "add plugs on the Plugs tab first");
    for (const p of plugs) {
      const l = el("label", { class: "check chip" }, plugsRow);
      const i = el("input", { type: "checkbox" }, l);
      i.checked = (room.plugs || []).includes(p.id);
      i.addEventListener("change", () => {
        room.plugs = (room.plugs || []).filter((x) => x !== p.id);
        if (i.checked) room.plugs.push(p.id);
        changed();
      });
      l.appendChild(document.createTextNode(p.name || p.id));
    }

    if (room.priceOnly) {
      const g = el("div", { class: "form-grid" }, c);
      field(g, "Heat during the cheapest … hours per day", num(room.priceOnly, "hoursPerDay", { min: 0, max: 24, step: 1 }));
      el("h3", {}, c, "Never heat between");
      const avoid = arr(room.priceOnly, "avoid");
      avoid.forEach((w, i) => {
        const r = el("div", { class: "sched-row" }, c);
        r.appendChild(time(w, "from")); el("span", {}, r, "–"); r.appendChild(time(w, "to"));
        btn(r, "", "trash", () => { avoid.splice(i, 1); changed(); render(); }, "ghost danger");
      });
      btn(el("div", { style: "margin-top:8px" }, c), "Add window", "plus", () => { avoid.push({ from: "17:00", to: "21:00" }); changed(); render(); });
      return;
    }

    const g = el("div", { class: "form-grid" }, c);
    field(g, "Temperature sensor", sel(room, "sensor", sensorOptions()),
      { hint: (state.status?.sensors || []).length ? "" : "No sensors yet: set up Netatmo on the Sensors tab" });
    field(g, "Never below (°C)", num(room, "minTemp", { step: 0.5 }), { hint: "heats whatever the price" });
    field(g, "Never above (°C)", num(room, "maxTemp", { step: 0.5 }), { hint: "hard stop, even when manual" });
    field(g, "Frost protection (°C)", num(room, "frostTemp", { step: 0.5, placeholder: "5" }));

    el("h3", {}, c, "Target temperature through the day");
    const sched = arr(room, "schedule");
    const tl = el("div", {}, c);
    renderTimeline = () => drawTimeline(tl, room);
    sched.forEach((b, i) => {
      const r = el("div", { class: "sched-row" }, c);
      r.appendChild(time(b, "from")); el("span", {}, r, "–"); r.appendChild(time(b, "to"));
      const t = num(b, "target", { step: 0.5, onChange: () => drawTimeline(tl, room) });
      r.appendChild(t); el("span", { class: "muted" }, r, "°C");
      btn(r, "", "trash", () => { sched.splice(i, 1); changed(); render(); }, "ghost danger");
    });
    btn(el("div", { style: "margin-top:8px" }, c), "Add period", "plus", () => {
      sched.push({ from: "06:00", to: "08:00", target: 20 }); changed(); render();
    });
    drawTimeline(tl, room);

    const adv = el("details", { class: "advanced" }, c);
    el("summary", {}, adv, "Advanced: how eagerly to chase cheap prices");
    const st = (room.strategy ||= {});
    const ag = el("div", { class: "form-grid" }, adv);
    field(ag, "Coast during peak down to target − (°C)", num(st, "peakAllowedDrop", { step: 0.1, placeholder: "1.0" }));
    field(ag, "Pre-heat before peak up to target + (°C)", num(st, "preheatBoost", { step: 0.1, placeholder: "1.0" }));
    field(ag, "Look ahead for peaks (minutes)", num(st, "preheatMinutes", { step: 15, placeholder: "180" }));
    field(ag, "Cheap-price boost (°C)", num(st, "cheapBoost", { step: 0.1, placeholder: "0.5" }));
    field(ag, "Hysteresis ± (°C)", num(st, "hysteresis", { step: 0.1, placeholder: "0.3" }));
    field(ag, "Distrust readings older than (min)", num(st, "staleMinutes", { step: 5, placeholder: "30" }));
    field(ag, "Limp mode ON (min/hour)", num(st, "limpOnMinutes", { step: 5, placeholder: "10" }));
    field(ag, "Limp mode OFF (min)", num(st, "limpOffMinutes", { step: 5, placeholder: "50" }));
  }

  // 24 h preview of the schedule: blue (cool) to orange (warm), uncovered time in grey.
  function drawTimeline(host, room) {
    host.replaceChildren();
    const toMin = (s) => { const [h, m] = (s || "00:00").split(":").map(Number); return h * 60 + m; };
    const bands = (room.schedule || []).map((b) => ({ from: toMin(b.from), to: b.to === "24:00" ? 1440 : toMin(b.to), t: b.target }));
    const at = (m) => bands.find((b) => (b.from <= b.to ? m >= b.from && m < b.to : m >= b.from || m < b.to));
    const segs = [];
    for (let m = 0; m < 1440; m += 15) {
      const b = at(m), t = b ? b.t : null, last = segs[segs.length - 1];
      if (last && last.t === t) last.len += 15; else segs.push({ t, len: 15 });
    }
    const lo = room.minTemp ?? 15, hi = room.maxTemp ?? 23;
    const bar = el("div", { class: "timeline" }, host);
    for (const s of segs) {
      const d = el("div", { style: `flex:${s.len}` }, bar, s.t == null ? "–" : s.len >= 90 ? `${s.t}°` : "");
      if (s.t == null) { d.style.background = "var(--grid)"; d.style.color = "var(--muted)"; continue; }
      // Cool blue -> amber -> deep orange (two segments, so the middle never turns muddy).
      const f = Math.max(0, Math.min(1, (s.t - lo) / Math.max(1, hi - lo)));
      d.style.background = f < 0.5
        ? `color-mix(in oklch, #2a78d6 ${Math.round((1 - f * 2) * 100)}%, #f59f00)`
        : `color-mix(in oklch, #f59f00 ${Math.round((2 - f * 2) * 100)}%, #e8590c)`;
      d.style.color = f > 0.3 && f < 0.7 ? "#0b0b0b" : "#fff";
      d.title = `${s.t}°C`;
    }
    const axis = el("div", { class: "timeline-axis" }, host);
    for (const h of ["00", "06", "12", "18", "24"]) el("span", {}, axis, h);
  }

  // --- Plugs --------------------------------------------------------------------------
  async function refreshPlugStatus() {
    try { S.plugStatus = await api("/api/plugs"); }
    catch (e) { toast(`Plug status: ${e.message}`, true); S.plugStatus = []; }
    if (S.tab === "plugs") render();
  }

  function guessSubnet() {
    const h = location.hostname;
    return /^\d+\.\d+\.\d+\.\d+$/.test(h) && !h.startsWith("127.") ? `${h.split(".").slice(0, 3).join(".")}.0/24` : "192.168.1.0/24";
  }

  function renderPlugs(body) {
    // Discovery
    const disc = el("section", { class: "card" }, body);
    sectionHead(disc, "Find plugs", "search", "violet", "Scans your network for Tasmota devices (about 10 seconds).");
    const row = el("div", { class: "row", style: "margin-top:12px" }, disc);
    const subnet = el("input", { type: "text", value: S.subnet || guessSubnet(), style: "width:180px" }, row);
    btn(row, "Scan", "search", async (b) => {
      b.disabled = true; b.lastChild.textContent = "Scanning…";
      try { S.subnet = subnet.value; S.discovered = await api("/api/discover", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ subnet: subnet.value }) }); }
      catch (e) { toast(e.message, true); }
      render();
    }, "primary");
    if (S.discovered) {
      const list = el("div", { class: "list" }, disc);
      if (!S.discovered.length) el("div", { class: "muted" }, list, "No Tasmota devices found.");
      for (const d of S.discovered) {
        const r = el("div", { class: "item-head" }, list);
        const info = el("div", { class: "row" }, r);
        info.appendChild(badge("plug", "gray"));
        el("b", {}, info, d.deviceName || d.host);
        el("span", { class: "muted" }, info, `${d.host} · ${d.hardware} · Tasmota ${d.firmware.split("(")[0]}`);
        const plugs = arr(S.cfg, "plugs");
        if (plugs.some((p) => p.host === d.host)) {
          const ok = el("span", { class: "chip ok" }, r); ok.appendChild(icon("check")); ok.appendChild(document.createTextNode("added"));
        } else {
          btn(r, "Add", "plus", () => {
            const id = slug((d.deviceName || d.host).replace(/^sonoff\s*/i, ""), new Set(plugs.map((p) => p.id)));
            plugs.push({ id, name: d.deviceName || id, type: "tasmota", host: d.host, mode: "dryrun" });
            changed(); render();
          });
        }
      }
    }

    // Configured plugs
    const card = el("section", { class: "card" }, body);
    const head = sectionHead(card, "Your plugs", "plug", "warm",
      "Dry-run: heatbrain decides and logs, but never switches the plug. Live: heatbrain switches it. A plug only goes live with its fail-safe installed.");
    btn(head, "Refresh status", "wifi", () => { S.plugStatus = null; refreshPlugStatus(); });
    const plugs = S.cfg.plugs || [];
    if (!plugs.length) el("div", { class: "empty-state" }, card, "No plugs yet: scan your network above.");
    if (plugs.length && S.plugStatus === null) refreshPlugStatus();
    const list = el("div", { class: "list" }, card);
    plugs.forEach((p, idx) => renderPlug(list, p, idx));
  }

  function renderPlug(list, p, idx) {
    const st = (S.plugStatus || []).find((x) => x.id === p.id);
    const c = el("div", { class: "card", style: "margin:0;background:var(--surface-2)" }, list);
    const head = el("div", { class: "item-head" }, c);
    const left = el("div", { class: "row" }, head);
    left.appendChild(badge("plug", st?.on ? "warm" : "gray"));
    const name = txt(p, "name"); name.style.fontWeight = "600"; left.appendChild(name);
    el("span", { class: "muted" }, left, `id: ${p.id}`);

    const right = el("div", { class: "row" }, head);
    const sw = el("label", { class: "switch", title: "Live" }, right);
    const swi = el("input", { type: "checkbox" }, sw);
    swi.checked = p.mode === "live";
    el("span", {}, sw);
    const modeText = el("b", {}, right, p.mode === "live" ? "Live" : "Dry-run");
    swi.addEventListener("change", () => { p.mode = swi.checked ? "live" : "dryrun"; modeText.textContent = swi.checked ? "Live" : "Dry-run"; changed(); });
    confirmBtn(right, "", "trash", "Remove plug?", () => {
      S.cfg.plugs.splice(idx, 1);
      for (const r of S.cfg.rooms || []) r.plugs = (r.plugs || []).filter((x) => x !== p.id);
      changed(); render();
    }, "ghost danger");

    const g = el("div", { class: "form-grid" }, c);
    if ((p.type || "tasmota") === "tasmota") field(g, "Address", txt(p, "host", { placeholder: "192.168.1.38" }));

    // Live status
    const chips = el("div", { class: "plugs", style: "margin-top:12px" }, c);
    const chip = (cls, iconName, text) => { const ch = el("span", { class: `chip ${cls}` }, chips); ch.appendChild(icon(iconName)); ch.appendChild(document.createTextNode(text)); };
    if (JSON.stringify(prune(p)) !== JSON.stringify(prune((JSON.parse(S.saved).plugs || []).find((x) => x.id === p.id) || {}))) {
      chip("warn", "save", "unsaved changes");
    } else if (!st) {
      chip("", "clock", "checking…");
    } else if (!st.reachable) {
      chip("error", "alert", `unreachable: ${st.error}`);
    } else {
      chip(st.on ? "warn" : "", "power", st.on ? "relay on" : "relay off");
      if (st.rssi != null) chip("", "wifi", `wifi ${st.rssi}%`);
      chip("", "cpu", `Tasmota ${st.firmware.split("(")[0]}`);
      if (st.failsafe?.installed) chip("ok", "shield", "fail-safe installed");
      else chip("error", "shield", "no fail-safe");
      if (p.mode === "live" && !st.failsafe?.installed) chip("error", "alert", "live, but not switched until the fail-safe is installed");
    }

    // Device actions (on the saved config)
    const actions = el("div", { class: "actions" }, c);
    const savedHasPlug = (JSON.parse(S.saved).plugs || []).some((x) => x.id === p.id);
    if (dirty() || !savedHasPlug) {
      el("span", { class: "hint" }, actions, "Save first to provision or restore this plug.");
    } else if (st?.reachable) {
      if (!st.failsafe?.installed) {
        confirmBtn(actions, "Install fail-safe", "shield",
          "Back up the plug's settings, turn off its own timers and install the fail-safe?",
          async () => {
            try { await api(`/api/plugs/${encodeURIComponent(p.id)}/provision`, { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" }); toast(`${p.name || p.id}: fail-safe installed`); }
            catch (e) { toast(e.message, true); }
            S.plugStatus = null; refreshPlugStatus();
          }, "primary");
      }
      confirmBtn(actions, "Restore original settings", "undo", "Put back the settings from the last backup?",
        async () => {
          try { await api(`/api/plugs/${encodeURIComponent(p.id)}/restore`, { method: "POST" }); toast(`${p.name || p.id}: original settings restored`); }
          catch (e) { toast(e.message, true); }
          S.plugStatus = null; refreshPlugStatus();
        });
    }

    const adv = el("details", { class: "advanced" }, c);
    el("summary", {}, adv, "Advanced: fail-safe timing and password");
    const fs = (p.failsafe ||= {});
    const ag = el("div", { class: "form-grid" }, adv);
    field(ag, "Auto-off after (min) without refresh", num(fs, "pulseMinutes", { step: 1, placeholder: "20" }));
    field(ag, "Limp mode after (min) of silence", num(fs, "watchdogMinutes", { step: 1, placeholder: "30" }));
    field(ag, "Limp mode ON (min)", num(fs, "limpOnMinutes", { step: 1, placeholder: "10" }), { hint: "0 = stay off" });
    field(ag, "Limp mode OFF (min)", num(fs, "limpOffMinutes", { step: 1, placeholder: "50" }));
    field(ag, "Tasmota web password", secret(p, "password", "none"));
  }

  // --- Sensors ------------------------------------------------------------------------
  function renderSensors(body) {
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Netatmo weather station", "thermo", "warm", "Indoor modules give each room its temperature.");
    const sources = arr(S.cfg, "temperatureSources");
    let n = sources.find((s) => s.type === "netatmo");
    if (!n) {
      btn(el("div", { style: "margin-top:12px" }, card), "Connect Netatmo", "plus", () => {
        sources.push({ id: "netatmo", type: "netatmo", clientId: "", clientSecret: "", refreshToken: "" }); changed(); render();
      }, "primary");
    } else {
      const g = el("div", { class: "form-grid" }, card);
      field(g, "Client ID", txt(n, "clientId"));
      field(g, "Client secret", secret(n, "clientSecret"));
      field(g, "Refresh token", secret(n, "refreshToken"), { hint: "only used once; heatbrain keeps the rotating token itself" });
      const steps = el("details", { class: "advanced" }, card);
      el("summary", {}, steps, "Where do I get these?");
      const ol = el("ol", {}, steps);
      el("li", {}, ol, "Log in at dev.netatmo.com/apps with your Netatmo account and create an app.");
      el("li", {}, ol, "Copy its client ID and client secret here.");
      el("li", {}, ol, "On the same page, use the Token generator with scope read_station and copy the refresh token.");
      el("li", {}, ol, "Save. Your modules appear below within a minute.");
    }

    const mods = el("section", { class: "card" }, body);
    sectionHead(mods, "Modules", "wifi", "violet");
    const sensors = state.status?.sensors || [];
    if (!sensors.length) el("div", { class: "muted", style: "margin-top:12px" }, mods, "No readings yet.");
    const list = el("div", { class: "list" }, mods);
    for (const s of sensors) {
      const r = el("div", { class: "item-head" }, list);
      const left = el("div", { class: "row" }, r);
      left.appendChild(badge(s.kind === "outdoor" ? "snow" : "thermo", s.kind === "outdoor" ? "cool" : "warm"));
      el("b", {}, left, s.name);
      el("span", { class: "muted" }, left, s.kind);
      const chips = el("div", { class: "plugs" }, r);
      const chip = (cls, iconName, text) => { const ch = el("span", { class: `chip ${cls}` }, chips); ch.appendChild(icon(iconName)); ch.appendChild(document.createTextNode(text)); };
      if (s.temperature != null) chip("", "thermo", `${s.temperature.toFixed(1)}°C`);
      if (s.humidity != null) chip("", "info", `${Math.round(s.humidity)}%`);
      if (s.battery != null) chip(s.battery <= 20 ? "error" : s.battery <= 40 ? "warn" : "ok", "battery", `${s.battery}%`);
      if (!s.reachable) chip("error", "alert", "offline");
      else if (s.ageMinutes != null) chip("", "clock", `${s.ageMinutes} min ago`);
    }
  }

  // --- Prices -------------------------------------------------------------------------
  const COMPONENT_TYPES = [
    ["energidataservice.spot", "Spot price (Energi Data Service)"],
    ["energidataservice.tariff", "Grid tariff (Datahub price list)"],
    ["fixed", "Fixed amount per kWh"],
    ["schedule", "Time-of-day schedule"],
  ];

  function renderPrices(body) {
    const price = (S.cfg.price ||= { components: [] });
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "What a kWh costs", "bolt", "amber",
      "The price is the sum of these parts, then the surcharges (VAT) on top. Defaults fit EWII/Radius in DK2.");
    const g = el("div", { class: "form-grid" }, card);
    field(g, "Currency", txt(S.cfg, "currency", { placeholder: "DKK" }));

    const comps = arr(price, "components");
    const list = el("div", { class: "list" }, card);
    comps.forEach((comp, i) => {
      const c = el("div", { class: "card", style: "margin:0;background:var(--surface-2)" }, list);
      const head = el("div", { class: "item-head" }, c);
      const left = el("div", { class: "row" }, head);
      left.appendChild(badge(comp.type === "fixed" ? "plus" : comp.type === "schedule" ? "clock" : "bolt", "amber"));
      left.appendChild(txt(comp, "name"));
      el("span", { class: "muted" }, left, (COMPONENT_TYPES.find(([t]) => t === comp.type) || [, comp.type])[1]);
      confirmBtn(head, "", "trash", "Remove?", () => { comps.splice(i, 1); changed(); render(); }, "ghost danger");
      const cg = el("div", { class: "form-grid" }, c);
      if (comp.type === "energidataservice.spot") {
        field(cg, "Price area", sel(comp, "area", [["DK1", "DK1 (Jutland, Funen)"], ["DK2", "DK2 (Zealand, Copenhagen)"]]));
      } else if (comp.type === "energidataservice.tariff") {
        field(cg, "Grid company GLN", txt(comp, "gln", { placeholder: "5790000705689" }));
        const codes = el("input", { type: "text", value: [].concat(comp.codes || []).join(", ") });
        codes.addEventListener("input", () => { comp.codes = codes.value.split(",").map((x) => x.trim()).filter(Boolean); changed(); });
        field(cg, "Charge codes (comma separated)", codes, { hint: "Radius C-customers: DT_C_01" });
      } else if (comp.type === "fixed") {
        field(cg, `Amount (${S.cfg.currency || "DKK"}/kWh, excl. VAT)`, num(comp, "amount", { step: 0.001 }));
      } else if (comp.type === "schedule") {
        const ta = el("textarea", { rows: 8 });
        ta.value = JSON.stringify(comp.seasons || [], null, 2);
        ta.addEventListener("input", () => {
          try { comp.seasons = JSON.parse(ta.value); ta.style.borderColor = ""; changed(); }
          catch { ta.style.borderColor = "var(--critical)"; }
        });
        field(cg, "Seasons (JSON)", ta, { wide: true, hint: '[{"months":[10,11,12,1,2,3],"bands":[{"from":"17:00","to":"21:00","amount":0.95}]}]' });
      }
    });
    const add = el("div", { class: "row", style: "margin-top:12px" }, card);
    const typeSel = el("select", {}, add);
    for (const [t, label] of COMPONENT_TYPES) el("option", { value: t }, typeSel, label);
    btn(add, "Add part", "plus", () => {
      const t = typeSel.value;
      const comp = { type: t, name: (COMPONENT_TYPES.find(([x]) => x === t) || [, t])[1].split(" (")[0] };
      if (t === "fixed") comp.amount = 0;
      if (t === "energidataservice.spot") comp.area = "DK2";
      if (t === "energidataservice.tariff") { comp.gln = ""; comp.codes = []; }
      if (t === "schedule") comp.seasons = [{ bands: [{ from: "00:00", to: "24:00", amount: 0 }] }];
      comps.push(comp); changed(); render();
    });

    const sc = el("section", { class: "card" }, body);
    sectionHead(sc, "Surcharges", "plus", "amber", "Percentages applied on top, in order (VAT).");
    const surcharges = arr(price, "surcharges");
    surcharges.forEach((s, i) => {
      const r = el("div", { class: "sched-row" }, sc);
      r.appendChild(txt(s, "name", { placeholder: "Moms" }));
      r.appendChild(num(s, "percent", { step: 0.1 }));
      el("span", { class: "muted" }, r, "%");
      btn(r, "", "trash", () => { surcharges.splice(i, 1); changed(); render(); }, "ghost danger");
    });
    btn(el("div", { style: "margin-top:8px" }, sc), "Add surcharge", "plus", () => { surcharges.push({ name: "Moms", percent: 25 }); changed(); render(); });
  }

  // --- Notifications ------------------------------------------------------------------
  function renderNotifications(body) {
    const n = (S.cfg.notifications ||= { channels: [] });
    const channels = arr(n, "channels");
    let discord = channels.find((c) => c.type === "discord");
    const isNew = !discord;
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Discord", "send", "violet",
      "In Discord: Edit Channel → Integrations → Webhooks → New Webhook → Copy Webhook URL.");
    if (isNew) discord = { type: "discord", webhookUrl: "" };  // joins the config once something is typed
    const g = el("div", { class: "form-grid" }, card);
    const url = secret(discord, "webhookUrl", "https://discord.com/api/webhooks/…");
    url.addEventListener("input", () => { if (!channels.includes(discord)) { channels.push(discord); changed(); } });
    field(g, "Webhook URL", url, { wide: true });
    const actions = el("div", { class: "actions" }, card);
    if (dirty()) el("span", { class: "hint" }, actions, "Save first, then send a test message.");
    else btn(actions, "Send test message", "send", async (b) => {
      b.disabled = true;
      try {
        const r = await api("/api/notify/test", { method: "POST" });
        const bad = r.filter((x) => !x.ok);
        bad.length ? toast(bad.map((x) => `${x.channel}: ${x.error}`).join("; "), true) : toast("Test message sent");
      } catch (e) { toast(e.message, true); }
      b.disabled = false;
    }, "primary");

    const rules = el("section", { class: "card" }, body);
    sectionHead(rules, "When to notify", "bell", "amber",
      "Problems must last a while before you hear about them; you also get a message when they are resolved.");
    const rg = el("div", { class: "form-grid" }, rules);
    field(rg, "Remind about ongoing problems every (hours)", num(n, "repeatHours", { step: 1, min: 1, placeholder: "6" }));
    const quietOn = !!n.quietHours;
    const qwrap = el("div", { class: "field" }, rg);
    el("span", {}, qwrap, "Quiet hours (critical alerts still come through)");
    const qrow = el("div", { class: "row" }, qwrap);
    const qchk = el("input", { type: "checkbox" }, el("label", { class: "check" }, qrow));
    qchk.checked = quietOn;
    qchk.addEventListener("change", () => { if (qchk.checked) n.quietHours = { from: "23:00", to: "07:00" }; else delete n.quietHours; changed(); render(); });
    if (quietOn) { qrow.appendChild(time(n.quietHours, "from")); el("span", {}, qrow, "–"); qrow.appendChild(time(n.quietHours, "to")); }
    const sg = el("div", { class: "field" }, rg);
    el("span", {}, sg, "Startup");
    sg.appendChild(chk(n, "notifyOnStart", "Send a message when heatbrain starts", true));
  }

  // --- System -------------------------------------------------------------------------
  function renderSystem(body) {
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "System", "cpu", "violet");
    const g = el("div", { class: "form-grid" }, card);
    field(g, "Time zone", txt(S.cfg, "timezone", { placeholder: "Europe/Copenhagen" }));
    const control = (S.cfg.control ||= {});
    field(g, "Control step every (seconds)", num(control, "tickSeconds", { step: 60, min: 60, placeholder: "300" }),
      { hint: "at most half of each live plug's auto-off time" });
    const web = (S.cfg.web ||= {});
    field(g, "Web UI port", num(web, "port", { step: 1, placeholder: "8080" }), { hint: "takes effect after a restart" });

    const raw = el("section", { class: "card" }, body);
    sectionHead(raw, "Raw configuration", "gear", "gray", "For power users. Secrets are shown as ******** and kept unless you replace them.");
    const ta = el("textarea", { rows: 18 }, raw);
    ta.value = JSON.stringify(S.cfg, null, 2);
    const row = el("div", { class: "actions" }, raw);
    btn(row, "Apply JSON to the form", "check", () => {
      try { S.cfg = JSON.parse(ta.value); changed(); render(); toast("Applied - review and save"); }
      catch (e) { toast(`Invalid JSON: ${e.message}`, true); }
    });
  }

  // --- save / discard -----------------------------------------------------------------
  $("btn-save").addEventListener("click", async () => {
    const b = $("btn-save");
    b.disabled = true;
    try {
      const res = await fetch("/api/config", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(S.cfg) });
      const r = await res.json().catch(() => ({}));
      if (!res.ok) {
        S.errors = r.errors || [r.error || `HTTP ${res.status}`];
        toast("Not saved: please fix the problems listed at the top", true);
        render();
        return;
      }
      await load();
      S.plugStatus = null;
      toast("Saved - heatbrain is applying the new settings");
      render();
      setTimeout(refreshStatus, 2500);
    } catch (e) {
      toast(e.message, true);
    } finally {
      b.disabled = false;
    }
  });

  $("btn-discard").addEventListener("click", () => {
    S.cfg = JSON.parse(S.saved);
    S.errors = [];
    render();
  });

  window.addEventListener("beforeunload", (e) => { if (dirty()) e.preventDefault(); });

  window.Settings = { open };
})();
