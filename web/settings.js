"use strict";
// Settings editor. Edits a copy of the config in memory; "Save & apply" sends it to the
// server, which validates, backs up, saves and reloads the controller. Device actions
// (provision, restore, test message) always use the saved config.
(() => {
  const MASK = "********";
  const TABS = [
    ["rooms", "Rooms", "home"],
    ["plugs", "Heating", "flame"],
    ["sensors", "Sensors", "thermo"],
    ["prices", "Prices", "bolt"],
    ["notifications", "Notifications", "bell"],
    ["system", "System", "cpu"],
  ];
  const S = { cfg: null, saved: "", errors: [], tab: "rooms", plugStatus: null, discovered: null, newIds: new Set(), jobs: {}, confirming: new Set(), pending: new Set(), openDetails: new Set() };
  let jobTimer = null;
  const jobRunning = () => Object.values(S.jobs).some((j) => j.state === "running");

  async function refreshJobs() {
    try { S.jobs = await api("/api/jobs"); } catch { return; }
    const running = jobRunning();
    if (S.tab === "plugs") render();
    clearTimeout(jobTimer);
    if (running) jobTimer = setTimeout(refreshJobs, 2000);
    else if (S.wasRunning) { S.plugStatus = null; refreshPlugStatus(); }  // finished: show the new state
    S.wasRunning = running;
  }

  async function startPlugJob(p, action) {
    try {
      await api(`/api/plugs/${encodeURIComponent(p.id)}/${action}`, { method: "POST" });
      if (action !== "restart") {
        S.jobs[p.id] = { kind: action, state: "running", log: [[Math.floor(Date.now() / 1000), "Starting…"]] };
        toast(`${p.name || p.id}: ${action === "upgrade" ? "firmware upgrade" : "fail-safe test"} started - progress is shown under the plug`);
      }
      if (action === "restart") { toast(`${p.name || p.id} is restarting`); setTimeout(() => { S.plugStatus = null; refreshPlugStatus(); }, 8000); return; }
      S.wasRunning = true;
      refreshJobs();
    } catch (e) { toast(e.message, true); }
  }

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
    S.fixedWebPort = r.fixedWebPort ?? null;  // set by Docker: not a setting there
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
    const f = el("label", { class: `field${opts.wide ? " wide" : ""}${opts.span2 ? " span2" : ""}` }, parent);
    el("span", { title: label }, f, label);
    f.appendChild(input);
    if (opts.hint) el("div", { class: "hint" }, f, opts.hint);
    return f;
  }
  function num(obj, key, opts = {}) {
    const step = Number(opts.step) || 1;
    const decimals = (String(step).split(".")[1] || "").length;
    const wrap = el("span", { class: "stepper" });
    const i = el("input", { type: "number", step: opts.step ?? "any", min: opts.min, max: opts.max,
                            placeholder: opts.placeholder ?? "", inputmode: "decimal" });
    i.value = obj[key] ?? "";
    const commit = () => {
      if (i.value === "") delete obj[key]; else obj[key] = Number(i.value);
      opts.onChange?.(); changed();
    };
    const bump = (dir) => {
      // Start from the current value, else the placeholder (the default), else 0.
      let v = i.value !== "" ? Number(i.value) : Number(opts.placeholder) || 0;
      v = Math.round((v + dir * step) / step) * step;
      if (opts.min != null) v = Math.max(Number(opts.min), v);
      if (opts.max != null) v = Math.min(Number(opts.max), v);
      i.value = v.toFixed(decimals);
      commit();
    };
    const minus = el("button", { type: "button", class: "step", "aria-label": "Decrease", tabindex: -1 }, wrap);
    minus.appendChild(icon("minus"));
    wrap.appendChild(i);
    if (opts.unit) {
      wrap.classList.add("has-unit");
      el("span", { class: "unit" }, wrap, opts.unit);
    }
    const plus = el("button", { type: "button", class: "step", "aria-label": "Increase", tabindex: -1 }, wrap);
    plus.appendChild(icon("plus"));
    minus.addEventListener("click", (e) => { e.preventDefault(); bump(-1); });
    plus.addEventListener("click", (e) => { e.preventDefault(); bump(1); });
    i.addEventListener("input", commit);
    return wrap;
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
    // A value that is not (or not yet) on the list goes right after the "choose" placeholder, not above it.
    if (current && !options.some(([v]) => v === current)) {
      const missing = [current, `${current} (not found)`];
      options = options[0]?.[0] === "" ? [options[0], missing, ...options.slice(1)] : [missing, ...options];
    }
    for (const [v, label] of options) {
      const o = el("option", { value: v }, s, label);
      if (v === current) o.selected = true;
    }
    s.addEventListener("change", () => { obj[key] = s.value; onChange?.(); changed(); });
    return s;
  }
  // On/off settings all use the same switch (like the plug's dry-run/live switch).
  function toggle(parent, checked, label, onChange) {
    const l = el("label", { class: "check" }, parent);
    const sw = el("span", { class: "switch" }, l);
    const i = el("input", { type: "checkbox" }, sw);
    i.checked = checked;
    el("span", {}, sw);
    if (label) l.appendChild(document.createTextNode(label));
    i.addEventListener("change", () => onChange(i.checked));
    return l;
  }
  function chk(obj, key, label, def = false, onChange) {
    return toggle(null, obj[key] ?? def, label, (v) => { obj[key] = v; onChange?.(); changed(); });
  }
  function time(obj, key, isEnd = false) {
    const i = el("input", { type: "time", step: 900 });
    i.value = obj[key] === "24:00" ? "00:00" : (obj[key] ?? "");
    i.addEventListener("input", () => {
      const v = i.value || "00:00";
      obj[key] = isEnd && v === "00:00" ? "24:00" : v;  // an end of 00:00 is midnight
      changed(); renderTimeline?.();
    });
    return i;
  }
  const allDayNote = (row, w) => {
    const toMin = (x) => (x === "24:00" ? 1440 : Number(x.slice(0, 2)) * 60 + Number(x.slice(3, 5)));
    if (w.from && w.to && toMin(w.to) - toMin(w.from) === 1440) el("span", { class: "muted" }, row, "all day");
  };
  // A collapsible section that stays open across re-renders (job/status refreshes redraw).
  function section(parent, key) {
    const d = el("details", { class: "advanced" }, parent);
    d.open = S.openDetails.has(key);
    d.addEventListener("toggle", () => { if (d.open) S.openDetails.add(key); else S.openDetails.delete(key); });
    return d;
  }
  function btn(parent, label, iconName, onClick, cls = "") {
    const b = el("button", { type: "button", class: cls }, parent);
    if (iconName) b.appendChild(icon(iconName));
    b.appendChild(document.createTextNode(label));
    b.addEventListener("click", (e) => { e.preventDefault(); onClick(b); });
    return b;
  }
  // Two-step confirmation without browser dialogs. With a key, an open question survives
  // re-renders (the plugs tab redraws when status or job progress arrives).
  // busyLabel: shown while onYes runs ("Installing fail-safe…"); defaults to "<label>…".
  function confirmBtn(parent, label, iconName, question, onYes, cls = "", key = null, busyLabel = null) {
    const busyText = busyLabel || (label ? `${label}…` : "Working…");
    const wrap = el("span", { class: "confirm" }, parent);
    const ask = () => {
      if (key) S.confirming.add(key);
      wrap.replaceChildren();
      wrap.classList.add("asking");
      el("span", {}, wrap, question);
      btn(wrap, "Yes", "check", async (b) => {
        b.disabled = true;
        b.lastChild.textContent = busyText;
        if (key) { S.confirming.delete(key); S.pending.add(key); }
        try { await onYes(); } finally { if (key) S.pending.delete(key); }
        if (wrap.isConnected) show();
      }, "primary");
      btn(wrap, "Cancel", null, () => { if (key) S.confirming.delete(key); show(); });
    };
    const show = () => {
      wrap.replaceChildren();
      wrap.classList.remove("asking");
      if (key && S.pending.has(key)) {
        const b = btn(wrap, busyText, "clock", () => {}, cls);
        b.disabled = true;
        return;
      }
      btn(wrap, label, iconName, ask, cls);
    };
    if (key && S.confirming.has(key)) ask(); else show();
    return wrap;
  }
  function slug(name, taken) {
    const base = (name || "item").toLowerCase().normalize("NFKD").replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "item";
    let id = base, n = 2;
    while (taken.has(id)) id = `${base}-${n++}`;
    return id;
  }
  function sectionHead(parent, title, iconName, tone, desc, helpKey) {
    const head = el("div", { class: "card-head" }, parent);
    const h = el("h2", {}, head);
    h.appendChild(badge(iconName, tone));
    h.appendChild(document.createTextNode(title));
    const d = desc ? el("div", { class: "hint", style: "margin:6px 0 0" }, parent, desc) : null;
    if (helpKey) helpButton(h, helpKey, d || head);
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

  // Netatmo modules first (by name, as configs always had it), then the thermostats' own probes (by
  // id: devi:<id> and devi:<id>-floor) - also before the first reading, as soon as one is set up.
  function sensorOptions() {
    const out = [["", "— choose a sensor —"]];
    const readings = state.status?.sensors || [];
    const temp = (r) => (r?.temperature != null ? ` · ${r.temperature.toFixed(1)}°` : "");
    for (const s of readings) {
      const source = s.key.split(":")[0];
      if (s.kind !== "indoor" || source === "devi") continue;
      out.push([`${source}:${s.name}`, `${s.name}${temp(s)}`]);
    }
    for (const t of S.cfg.thermostats || []) {
      const name = t.name || t.id;
      for (const [key, what] of [[`devi:${t.id}`, "thermostat room sensor"], [`devi:${t.id}-floor`, "thermostat floor probe"]]) {
        out.push([key, `${name} · ${what}${temp(readings.find((r) => r.key === key))}`]);
      }
    }
    return out;
  }

  function renderRooms(body) {
    const card = el("section", { class: "card" }, body);
    const head = sectionHead(card, "Rooms", "home", "violet",
      "Each room has plugs, a temperature sensor and target temperatures through the day. A room without a sensor runs the cheapest hours of each day.",
      "start");
    btn(head, "Add room", "plus", () => {
      const rooms = arr(S.cfg, "rooms");
      const id = slug("room", new Set(rooms.map((r) => r.id)));
      S.newIds.add(id);
      rooms.push({ id, name: "New room", plugs: [], sensor: "", minTemp: 16, maxTemp: 22,
        program: [{ at: "07:00", mode: "heat", target: 20 }, { at: "17:00", mode: "off" }, { at: "21:00", mode: "heat", target: 20 }, { at: "23:00", mode: "keep", target: 16 }] });
      S.scrollToRoom = id;  // the new room is added at the bottom: bring it into view, name ready to type
      render();
    }, "primary");

    const rooms = S.cfg.rooms || [];
    if (!rooms.length) el("div", { class: "empty-state" }, card, "No rooms yet. Add plugs first, then a room.");
    const list = el("div", { class: "list" }, card);
    rooms.forEach((room, idx) => renderRoom(list, room, idx));
  }

  // The room's sensor decides what kind of room it is: with one it follows its daily program (heat
  // to / keep above / off); with "no sensor" it runs the cheapest hours of each day.
  const NO_SENSOR = "__none__";
  function sensorField(g, room) {
    const s = el("select");
    const opts = sensorOptions().filter(([v]) => v !== "");
    const current = room.priceOnly ? NO_SENSOR : room.sensor || "";
    const list = [...(current === "" ? [["", "— choose a sensor —"]] : []), [NO_SENSOR, "No sensor (cheapest hours)"], ...opts];
    if (current && current !== NO_SENSOR && !opts.some(([v]) => v === current)) list.splice(1, 0, [current, `${current} (not found)`]);
    for (const [v, label] of list) {
      const o = el("option", { value: v }, s, label);
      if (v === current) o.selected = true;
    }
    s.addEventListener("change", () => {
      if (s.value === NO_SENSOR) {
        room.priceOnly ||= { hoursPerDay: 12, avoid: [{ from: "17:00", to: "21:00" }] };
        room.sensor = "";
      } else {
        delete room.priceOnly;
        room.program ||= [{ at: "00:00", mode: "heat", target: 20 }];
        room.sensor = s.value;
      }
      changed(); render();
    });
    field(g, "Temperature sensor", s, { hint: room.priceOnly ? "without one, the room runs the cheapest hours of each day"
      : (state.status?.sensors || []).length ? "" : "No sensors yet: set up Netatmo on the Sensors tab" });
  }

  function renderRoom(list, room, idx) {
    const c = el("div", { class: "card", style: "margin:0;background:var(--surface-2)" }, list);
    if (S.scrollToRoom === room.id) {
      S.scrollToRoom = null;
      requestAnimationFrame(() => {
        c.scrollIntoView({ behavior: "smooth", block: "start" });
        const input = c.querySelector("input");
        if (input) { input.focus({ preventScroll: true }); input.select(); }
      });
    }
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
    // A room still called "New room" takes the name of the first device picked for it.
    const nameAfter = (deviceName) => {
      if (!deviceName || (room.name && room.name !== "New room")) return;
      room.name = deviceName;
      if (S.newIds.has(room.id)) {
        const taken = new Set(S.cfg.rooms.filter((r) => r !== room).map((r) => r.id));
        S.newIds.delete(room.id);
        room.id = slug(room.name, taken);
        S.newIds.add(room.id);
      }
    };

    const right = el("div", { class: "row" }, head);
    // Order: the dashboard shows the rooms in this order.
    const move = (dir) => {
      const to = idx + dir;
      if (to < 0 || to >= S.cfg.rooms.length) return;
      [S.cfg.rooms[idx], S.cfg.rooms[to]] = [S.cfg.rooms[to], S.cfg.rooms[idx]];
      changed(); render();
    };
    const up = btn(right, "", "up", () => move(-1), "ghost move-btn");
    up.title = "Move up (earlier on the dashboard)"; up.disabled = idx === 0;
    const down = btn(right, "", "down", () => move(1), "ghost move-btn");
    down.title = "Move down (later on the dashboard)"; down.disabled = idx === S.cfg.rooms.length - 1;
    confirmBtn(right, "", "trash", "Delete room?", () => { S.cfg.rooms.splice(idx, 1); changed(); render(); }, "ghost danger");

    // Plugs, or a thermostat (then heatbrain sets its setpoint instead of switching)
    const plugsRow = el("div", { class: "row", style: "margin-top:12px" }, c);
    el("span", { class: "muted" }, plugsRow, room.thermostat ? "Thermostat:" : "Plugs:");
    const plugs = room.thermostat ? [] : S.cfg.plugs || [];
    if (!room.thermostat && !plugs.length && !(S.cfg.thermostats || []).length) el("span", { class: "hint" }, plugsRow, "add plugs (or a thermostat) on the Heating tab first");
    for (const t of S.cfg.thermostats || []) {
      const on = room.thermostat === t.id;
      if (!on && room.plugs?.length) continue;  // plugs chosen: no thermostat pills
      const pill = el("button", { type: "button", class: `pick${on ? " on" : ""}`, "aria-pressed": on }, plugsRow);
      pill.appendChild(icon(on ? "check" : "thermo"));
      pill.appendChild(document.createTextNode(t.name || t.id));
      pill.addEventListener("click", (e) => {
        e.preventDefault();
        if (on) delete room.thermostat;
        else { room.thermostat = t.id; delete room.plugs; delete room.priceOnly; if (!room.sensor) room.sensor = `devi:${t.id}`; nameAfter(t.name || t.id); }
        changed(); render();
      });
    }
    for (const p of plugs) {
      // Pick several from a list: selectable pills (on/off settings use switches).
      const on = (room.plugs || []).includes(p.id);
      const pill = el("button", { type: "button", class: `pick${on ? " on" : ""}`, "aria-pressed": on }, plugsRow);
      pill.appendChild(icon(on ? "check" : "plug"));
      pill.appendChild(document.createTextNode(p.name || p.id));
      pill.addEventListener("click", (e) => {
        e.preventDefault();
        room.plugs = (room.plugs || []).filter((x) => x !== p.id);
        if (!on) { room.plugs.push(p.id); nameAfter(p.name || p.id); }
        changed(); render();
      });
    }

    if (room.priceOnly) {
      const g = el("div", { class: "form-grid" }, c);
      sensorField(g, room);
      const ch = field(g, "Cheapest hours per day", num(room.priceOnly, "hoursPerDay", { min: 0, max: 24, step: 1, unit: "h" }), { hint: "heats only in those hours" });
      helpButton(ch.querySelector("span"), "priceOnly", g);
      el("h3", {}, c, "Never heat between");
      const avoid = arr(room.priceOnly, "avoid");
      avoid.forEach((w, i) => {
        const r = el("div", { class: "sched-row" }, c);
        r.appendChild(time(w, "from")); el("span", {}, r, "–"); r.appendChild(time(w, "to", true));
        btn(r, "", "trash", () => { avoid.splice(i, 1); changed(); render(); }, "ghost danger row-delete");
      });
      btn(el("div", { style: "margin-top:8px" }, c), "Add window", "plus", () => { avoid.push({ from: "17:00", to: "21:00" }); changed(); render(); });
      return;
    }

    const g = el("div", { class: "form-grid" }, c);
    sensorField(g, room);
    const nb = field(g, "Never below", num(room, "minTemp", { step: 0.5, unit: "°C" }), { hint: "heats whatever the price" });
    helpButton(nb.querySelector("span"), "limits", g);
    field(g, "Never above", num(room, "maxTemp", { step: 0.5, unit: "°C" }), { hint: "hard stop, even when manual" });
    field(g, "Frost protection", num(room, "frostTemp", { step: 0.5, placeholder: "5", unit: "°C" }));
    field(g, "Warm-up for bookings", num(room, "warmupMinutes", { step: 15, min: 0, max: 720, placeholder: "180", unit: "min" }), { hint: "until heatbrain has learned how fast the room warms (concrete floors: 12+ h)" });

    const ph = el("h3", {}, c, "Daily program ");
    helpButton(ph, ["program", "modes"]);
    el("div", { class: "hint", style: "margin-top:-4px" }, c,
      "Each switch point runs until the next one; the last continues past midnight. Off still protects against frost.");
    const program = arr(room, "program");
    if (!program.length) program.push({ at: "00:00", mode: "heat", target: 20 });
    const toMin = (x) => Number(x.slice(0, 2)) * 60 + Number(x.slice(3, 5));
    program.sort((x, y) => toMin(x.at) - toMin(y.at));
    const tl = el("div", {}, c);
    renderTimeline = () => drawTimeline(tl, room);
    program.forEach((pt, i) => {
      const r = el("div", { class: "sched-row" }, c);
      const at = time(pt, "at");
      // Re-sort when leaving the field, and only if the order changed: Chrome fires "change"
      // on every digit typed into a time field, so redrawing then would steal the focus.
      at.addEventListener("blur", () => {
        const sorted = program.every((x, k) => k === 0 || toMin(program[k - 1].at) <= toMin(x.at));
        if (!sorted) render();
      });
      r.appendChild(at);
      r.appendChild(sel(pt, "mode", [["heat", "Heat to"], ["keep", "Keep above"], ["off", "Off"]], () => {
        if (pt.mode === "off") delete pt.target;
        else pt.target ??= pt.mode === "keep" ? 15 : 20;
        render();
      }));
      if (pt.mode !== "off") {
        r.appendChild(num(pt, "target", { step: 0.5, unit: "°C", onChange: () => drawTimeline(tl, room) }));
      }
      const next = program[(i + 1) % program.length];
      el("span", { class: "muted" }, r, program.length === 1 ? "all day" : `until ${next.at}`);
      if (program.length > 1) {
        btn(r, "", "trash", () => { program.splice(i, 1); changed(); render(); }, "ghost danger row-delete");
      }
    });
    const addRow = el("div", { class: "row", style: "margin-top:8px" }, c);
    btn(addRow, "Add switch point", "plus", () => {
      const used = new Set(program.map((x) => x.at));
      let m = (toMin(program[program.length - 1].at) + 120) % 1440;
      while (used.has(`${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`)) m = (m + 60) % 1440;
      program.push({ at: `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`, mode: "heat", target: 20 });
      changed(); render();
    });
    // One click for the peak tariff: off 17-21, then back to whatever was running at 21:00.
    const modeAt = (m) => { let cur = program[program.length - 1]; for (const x of program) if (toMin(x.at) <= m) cur = x; return cur; };
    const peakOff = program.some((x) => x.at === "17:00" && x.mode === "off");
    if (!peakOff) {
      btn(addRow, "Off 17:00-21:00 (peak tariff)", "bolt", () => {
        const resume = { ...modeAt(21 * 60) };
        for (let k = program.length - 1; k >= 0; k--) {
          const m = toMin(program[k].at);
          if (m >= 17 * 60 && m < 21 * 60) program.splice(k, 1);  // the window replaces these
        }
        program.push({ at: "17:00", mode: "off" });
        if (!program.some((x) => x.at === "21:00")) {
          delete resume.at;
          program.push({ at: "21:00", ...resume });
        }
        changed(); render();
      });
    }
    drawTimeline(tl, room);

    const adv = section(c, `room:${room.id}`);
    const sm = el("summary", {}, adv, "Advanced: how eagerly to chase cheap prices ");
    helpButton(sm, "strategy");
    const st = (room.strategy ||= {});
    const ag = el("div", { class: "form-grid" }, adv);
    field(ag, "Peak coasting", num(st, "peakAllowedDrop", { step: 0.1, placeholder: "1.0", unit: "−°C" }), { hint: "below target in expensive hours" });
    field(ag, "Pre-heat", num(st, "preheatBoost", { step: 0.1, placeholder: "1.0", unit: "+°C" }), { hint: "above target before a peak" });
    field(ag, "Peak look-ahead", num(st, "preheatMinutes", { step: 15, placeholder: "180", unit: "min" }), { hint: "how early to start pre-heating" });
    field(ag, "Cheap boost", num(st, "cheapBoost", { step: 0.1, placeholder: "0.5", unit: "+°C" }), { hint: "extra in the cheapest hours" });
    field(ag, "Hysteresis", num(st, "hysteresis", { step: 0.1, placeholder: "0.3", unit: "±°C" }), { hint: "avoids rapid on/off" });
    field(ag, "Stale after", num(st, "staleMinutes", { step: 5, placeholder: "30", unit: "min" }), { hint: "older readings are not trusted" });
    field(ag, "Limp mode ON", num(st, "limpOnMinutes", { step: 5, min: 0, placeholder: "10", unit: "min" }), { hint: "when the temperature is unknown" });
    field(ag, "Limp mode OFF", num(st, "limpOffMinutes", { step: 5, placeholder: "50", unit: "min" }), { hint: "then off for this long" });
  }

  // Cool -> warm ramp through nearby stops only (blending blue straight into amber passes
  // through pink or mud, depending on the colour space).
  const TEMP_STOPS = [[0, "#2a78d6"], [0.35, "#9ec5f4"], [0.5, "#f2d27a"], [0.75, "#f59f00"], [1, "#e8590c"]];
  function tempColor(f) {
    for (let k = 1; k < TEMP_STOPS.length; k++) {
      const [f1, c1] = TEMP_STOPS[k - 1], [f2, c2] = TEMP_STOPS[k];
      if (f <= f2) return `color-mix(in oklab, ${c1} ${Math.round((1 - (f - f1) / (f2 - f1)) * 100)}%, ${c2})`;
    }
    return TEMP_STOPS[TEMP_STOPS.length - 1][1];
  }

  // 24 h preview of the program: heat in blue (cool) to orange (warm), keep-above as a
  // light tint, off striped.
  function drawTimeline(host, room) {
    host.replaceChildren();
    const toMin = (x) => Number((x || "00:00").slice(0, 2)) * 60 + Number((x || "00:00").slice(3, 5));
    const pts = [...(room.program || [])].sort((x, y) => toMin(x.at) - toMin(y.at));
    const stateAt = (m) => { let cur = pts[pts.length - 1]; for (const x of pts) if (toMin(x.at) <= m) cur = x; return cur; };
    const segs = [];
    for (let m = 0; m < 1440; m += 15) {
      const st = pts.length ? stateAt(m) : null;
      const key = st ? `${st.mode}:${st.target ?? ""}` : "none";
      const last = segs[segs.length - 1];
      if (last && last.key === key) last.len += 15; else segs.push({ key, st, len: 15 });
    }
    const lo = room.minTemp ?? 15, hi = room.maxTemp ?? 23;
    const bar = el("div", { class: "timeline" }, host);
    for (const sg of segs) {
      const st = sg.st;
      if (!st) { el("div", { style: `flex:${sg.len};background:var(--grid)` }, bar, "–"); continue; }
      if (st.mode === "off") {
        const d = el("div", { style: `flex:${sg.len}`, class: "tl-off", title: "off (frost protection only)" }, bar, "off");
        continue;
      }
      const label = st.mode === "keep" ? `≥${st.target}°` : `${st.target}°`;
      const d = el("div", { style: `flex:${sg.len}`, title: st.mode === "keep" ? `keep above ${st.target}°C` : `heat to ${st.target}°C` }, bar, label);
      const f = Math.max(0, Math.min(1, ((st.target ?? lo) - lo) / Math.max(1, hi - lo)));
      const color = tempColor(f);
      if (st.mode === "keep") {
        d.className = "tl-keep";
        d.style.background = `color-mix(in oklch, ${color} 35%, var(--surface-2))`;
        d.style.color = "var(--ink)";
      } else {
        d.style.background = color;
        d.style.color = f > 0.25 && f < 0.8 ? "#0b0b0b" : "#fff";
      }
    }
    const axis = el("div", { class: "timeline-axis" }, host);
    for (const h of ["00", "06", "12", "18", "24"]) el("span", {}, axis, h);
    // Label every segment, then hide only the labels that really don't fit (depends on width).
    requestAnimationFrame(() => {
      for (const d of bar.children) if (d.scrollWidth > d.clientWidth) d.style.color = "transparent";
    });
  }

  // --- Plugs --------------------------------------------------------------------------
  let plugStatusLoading = null;
  function refreshPlugStatus() {
    plugStatusLoading ||= (async () => {
      try { S.plugStatus = await api("/api/plugs"); }
      catch (e) { toast(`Plug status: ${e.message}`, true); S.plugStatus = []; }
      plugStatusLoading = null;
      if (S.tab === "plugs") render();
    })();
    return plugStatusLoading;
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
      "Dry-run: heatbrain decides and logs, but never switches the plug. Live: heatbrain switches it. A plug only goes live with its fail-safe installed.",
      ["dryrun", "failsafe", "firmware"]);
    btn(head, "Refresh status", "wifi", () => { S.plugStatus = null; refreshPlugStatus(); });
    const plugs = S.cfg.plugs || [];
    if (!plugs.length) el("div", { class: "empty-state" }, card, "No plugs yet: scan your network above.");
    if (plugs.length && S.plugStatus === null && !plugStatusLoading) { refreshPlugStatus(); refreshJobs(); }
    const list = el("div", { class: "list" }, card);
    plugs.forEach((p, idx) => renderPlug(list, p, idx));
    renderThermostats(body);
  }

  // --- Thermostats (DEVIreg Smart): pairing, and which of them heatbrain controls -------
  async function loadDevi(force = false) {
    if (!force && S.devi && Date.now() - S.devi.at < 20_000) return;
    try {
      const fresh = await api("/api/devi");
      const changedData = JSON.stringify(fresh) !== JSON.stringify(S.devi?.data);
      S.devi = { at: Date.now(), data: fresh, ...fresh };
      // Redraw only when something changed, and never under the cursor of someone typing.
      const typing = document.activeElement?.closest?.("#settings-body") && /INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName);
      if (changedData && !typing) render();
    } catch { /* optional */ }
  }
  // Live thermostat state while the Heating tab is open: every 5 s while one is connecting, else 20 s.
  setInterval(() => {
    if ($("view-settings").hidden || S.tab !== "plugs" || document.visibilityState !== "visible" || !S.devi) return;
    const connecting = (S.devi.rooms || []).some((r) => !r.connected || r.roomTemp == null);
    if (Date.now() - S.devi.at >= (connecting ? 5_000 : 20_000)) loadDevi(true);
  }, 2_500);
  function renderThermostats(body) {
    loadDevi();
    const d = S.devi || {};
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Thermostats", "thermo", "violet",
      "DEVIreg Smart floor heating: heatbrain sets the thermostat's temperature by price and program, and it regulates by itself.", "thermostats");
    const list = el("div", { class: "list" }, card);
    const cfgT = arr(S.cfg, "thermostats");
    for (const room of d.rooms || []) {
      const t = cfgT.find((x) => x.peerId === room.peerId);
      const c = el("div", { class: "card", style: "margin:0;background:var(--surface-2)" }, list);
      const head = el("div", { class: "item-head" }, c);
      const left = el("div", { class: "row" }, head);
      left.appendChild(badge("thermo", room.heating ? "warm" : "gray"));
      el("b", {}, left, t ? t.name || t.id : room.name);
      el("span", { class: "muted" }, left, d.houseName ? `DEVI · ${d.houseName}` : "DEVI");
      const right = el("div", { class: "row" }, head);
      if (t) {
        const seg = el("div", { class: "segmented mode-seg", role: "radiogroup", "aria-label": "Thermostat mode" }, right);
        for (const [value, label, tip] of [["dryrun", "Dry-run", "heatbrain decides and logs, but does not change the thermostat"],
                                           ["live", "Live", "heatbrain sets the thermostat's temperature (manual mode)"]]) {
          const b = btn(seg, label, value === "live" ? "bolt" : null, () => { t.mode = value; changed(); render(); });
          b.title = tip;
          b.classList.toggle("active", (t.mode || "dryrun") === value);
          if (value === "live") b.classList.add("live");
        }
        confirmBtn(right, "", "trash", "Stop using this thermostat?", () => {
          cfgT.splice(cfgT.indexOf(t), 1);
          for (const r of S.cfg.rooms || []) if (r.thermostat === t.id) delete r.thermostat;
          changed(); render();
        }, "ghost danger");
      } else {
        btn(right, "Use in heatbrain", "plus", () => {
          cfgT.push({ id: slug(room.name, new Set(cfgT.map((x) => x.id))), type: "devi", name: room.name, peerId: room.peerId, mode: "dryrun" });
          changed(); render();
        }, "primary");
      }
      const chips = el("div", { class: "plugs", style: "margin-top:10px" }, c);
      const chip = (cls, iconName, text) => { const ch = el("span", { class: `chip ${cls}` }, chips); ch.appendChild(icon(iconName)); ch.appendChild(document.createTextNode(text)); };
      if (room.error) chip("error", "alert", room.error);
      else if (!room.connected) chip("", "clock", "connecting…");
      else {
        chip("ok", "wifi", "connected");
        if (room.roomTemp != null) chip("", "thermo", `room ${room.roomTemp.toFixed(1)}°`);
        if (room.floorTemp != null) chip("", "thermo", `floor ${room.floorTemp.toFixed(1)}°`);
        if (room.mode) chip(room.mode === "manual" ? "ok" : "warn", "gauge", room.mode);
        if (room.manualSetpoint != null) chip("", "flame", `set to ${room.manualSetpoint.toFixed(1)}°`);
        if (room.heating != null) chip(room.heating ? "warn" : "", "power", room.heating ? "heating" : "not heating");
      }
      if (t) {
        const g = el("div", { class: "form-grid" }, c);
        // Name spans two columns like a plug's address, so the power lines up with the plugs' power.
        field(g, "Name", txt(t, "name", { placeholder: room.name }), { span2: true });
        field(g, "Heating power", num(t, "watts", { step: 50, min: 0, placeholder: "1200", unit: "W" }), { hint: "from the DEVI app" });
        el("div", { class: "hint", style: "margin-top:6px" }, c, "Choose it as the room's device on the Rooms tab. In Live, heatbrain switches the thermostat to manual mode, so its own schedule no longer applies.");
      }
    }
    // Pairing
    const pair = d.paired ? section(card, "devi-pair") : el("div", { style: "margin-top:12px" }, card);
    if (d.paired) el("summary", {}, pair, "Pair again (e.g. after adding a thermostat in the DEVI app)");
    const ol = el("ol", { class: "phone-steps" }, pair);
    el("li", {}, ol, "In the DEVI Smart app: Settings → Share house → create a one-time code. (If the iPhone app says it couldn't send the configuration, try the Android app.)");
    el("li", {}, ol, "Type the code here and press Pair, while the app stays open.");
    const row = el("div", { class: "row", style: "margin-top:8px" }, pair);
    const code = el("input", { type: "text", inputmode: "numeric", placeholder: "one-time code", style: "width:180px", autocomplete: "off", value: S.deviCode || "" }, row);
    code.addEventListener("input", () => { S.deviCode = code.value; });
    const pairBtn = btn(row, S.devPairing ? "Pairing… (up to a minute)" : "Pair", "check", async () => {
      S.devPairing = true; S.devPairResult = null; render();
      try {
        const r = await api("/api/devi/pair", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ code: S.deviCode || "" }) });
        S.devPairResult = { ok: true, text: `Paired with ${r.houseName || "your house"}: ${r.rooms.length} thermostat(s). Choose "Use in heatbrain" above.` };
        S.deviCode = "";
        await loadDevi(true);
      } catch (e) {
        S.devPairResult = { ok: false, text: e.message };  // the code stays, for a quick retry with a new one
      }
      S.devPairing = false; render();
    }, "primary");
    pairBtn.disabled = !!S.devPairing;
    // The answer stays here (a toast is easy to miss after a minute of waiting).
    if (S.devPairResult) {
      el("div", { class: "hint", style: `margin-top:8px;color:var(${S.devPairResult.ok ? "--good" : "--critical"})` }, pair, S.devPairResult.text);
    }
    if (d.error) el("div", { class: "hint", style: "color:var(--critical)" }, card, d.error);
  }

  // Job log: full while running; afterwards one summary line with the log behind "Show log".
  // Successful jobs hide themselves after an hour; failed ones stay until dismissed.
  const DISMISSED_KEY = "heatbrain-dismissed-jobs";
  const dismissed = () => { try { return new Set(JSON.parse(localStorage.getItem(DISMISSED_KEY) || "[]")); } catch { return new Set(); } };
  function dismissJob(id) {
    const d = dismissed(); d.add(id);
    try { localStorage.setItem(DISMISSED_KEY, JSON.stringify([...d].slice(-50))); } catch {}
  }

  function renderJob(parent, p) {
    const job = S.jobs[p.id];
    if (!job) return;
    const jobId = `${p.id}:${job.started}`;
    const what = job.kind === "upgrade" ? "Firmware upgrade" : "Fail-safe test";
    const finishedAgo = job.finished ? Date.now() / 1000 - job.finished : 0;
    if (job.state !== "running" && (dismissed().has(jobId) || (job.state === "done" && finishedAgo > 3600))) return;

    const box = el("div", { class: `joblog ${job.state}` }, parent);
    const title = el("div", { class: "joblog-title" }, box);
    title.appendChild(icon(job.state === "running" ? "clock" : job.state === "done" ? "check" : "alert"));
    const list = () => {
      const ol = el("ol", {});
      for (const [t, msg] of job.log) {
        const li = el("li", {}, ol);
        el("span", { class: "muted" }, li, `${hm(t)}  `);
        li.appendChild(document.createTextNode(msg));
      }
      return ol;
    };
    if (job.state === "running") {
      title.appendChild(document.createTextNode(`${what}: in progress…`));
      box.appendChild(list());
      return;
    }
    const last = job.log.length ? job.log[job.log.length - 1][1] : "";
    title.appendChild(document.createTextNode(
      job.state === "done" ? `${what} completed at ${hm(job.finished)}` : `${what} failed at ${hm(job.finished)}`));
    if (job.state === "failed" && last) el("div", { class: "hint" }, box, last);
    const row = el("div", { class: "row", style: "margin-top:4px" }, box);
    const log = section(row, `job:${jobId}`);
    el("summary", {}, log, "Show log");
    log.appendChild(list());
    btn(row, "Dismiss", null, () => { dismissJob(jobId); render(); }, "ghost");
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
    // Two named choices, the active one filled: a lone switch labelled "Dry-run" read as "dry-run is off".
    const seg = el("div", { class: "segmented mode-seg", role: "radiogroup", "aria-label": "Plug mode" }, right);
    for (const [value, label, tip] of [["dryrun", "Dry-run", "heatbrain decides and logs, but never switches this plug"],
                                       ["live", "Live", "heatbrain switches this plug"]]) {
      const b = btn(seg, label, value === "live" ? "bolt" : null, () => { p.mode = value; changed(); render(); });
      b.title = tip;
      b.setAttribute("role", "radio");
      b.setAttribute("aria-checked", String((p.mode || "dryrun") === value));
      b.classList.toggle("active", (p.mode || "dryrun") === value);
      if (value === "live") b.classList.add("live");
    }
    confirmBtn(right, "", "trash", "Remove plug?", () => {
      S.cfg.plugs.splice(idx, 1);
      for (const r of S.cfg.rooms || []) r.plugs = (r.plugs || []).filter((x) => x !== p.id);
      changed(); render();
    }, "ghost danger");

    const g = el("div", { class: "form-grid" }, c);
    // Address with the device's own buttons next to it: its web UI (also when heatbrain can't
    // reach it), restart and restore.
    const savedHasPlug = (JSON.parse(S.saved).plugs || []).some((x) => x.id === p.id);
    const deviceReady = !dirty() && savedHasPlug && st?.reachable;
    const busy = jobRunning();
    if ((p.type || "tasmota") === "tasmota") {
      const addr = el("div", { class: "row addr-row" });
      addr.appendChild(txt(p, "host", { placeholder: "192.168.1.38" }));
      field(g, "Address", addr, { span2: true });
      if (p.host) {
        btn(addr, "Web UI", "external", () => window.open(`http://${p.host}/`, "_blank", "noopener"), "ghost")
          .title = `Open the plug's own page (${p.host}) in a new tab`;
      }
      if (deviceReady && !busy) {
        confirmBtn(addr, "Restart", "power", "Restart the plug now? The relay keeps its state.", () => startPlugJob(p, "restart"), "ghost", `${p.id}:restart`, "Restarting…");
      }
      if (deviceReady && st.failsafe?.installed && st.backup) {
        const when = st.backup.takenAt
          ? new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, weekday: "short", day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" }).format(new Date(st.backup.takenAt))
          : st.backup.file;
        confirmBtn(addr, "Remove fail-safe", "undo",
          `Remove the fail-safe and put back the plug's own timers, power-on state and rule from the backup of ${when}?`,
          async () => {
            try { const r = await api(`/api/plugs/${encodeURIComponent(p.id)}/restore`, { method: "POST" }); toast(`${p.name || p.id}: fail-safe removed, own settings restored (${r.restored})`); }
            catch (e) { toast(e.message, true); }
            S.plugStatus = null; await refreshPlugStatus();
          }, "ghost", `${p.id}:restore`, "Removing fail-safe…");
      }
    }
    field(g, "Heating power", num(p, "watts", { step: 50, min: 0, placeholder: "1500", unit: "W" }),
      { hint: "from the radiator's label" });

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
      if (st.updateAvailable) {
        const a = el("a", { class: "chip warn", href: st.latest.url, target: "_blank", rel: "noopener" }, chips);
        a.appendChild(icon("bolt"));
        a.appendChild(document.createTextNode(`${st.latest.version.replace(/^v/, "")} available`));
      }
      if (st.failsafe?.installed) chip("ok", "shield", "fail-safe installed");
      else if (st.failsafe?.unknown) {
        const ch = el("span", { class: "chip warn", title: st.failsafe.problems.join("; ") }, chips);
        ch.appendChild(icon("shield"));
        ch.appendChild(document.createTextNode("fail-safe: plug too slow to check - refresh status"));
      } else {
        const ch = el("span", { class: "chip error", title: (st.failsafe?.problems || []).join("\n") }, chips);
        ch.appendChild(icon("shield"));
        ch.appendChild(document.createTextNode("no fail-safe"));
      }
      if (st.test && st.failsafe?.installed) {
        if (!st.test.verified) chip("error", "alert", "fail-safe test failed");
        else if (!st.test.matchesFirmware) chip("warn", "alert", "not tested on this firmware");
        else chip("ok", "check", `tested on ${st.test.version.split("(")[0]}`);
      }
      if (p.mode === "live" && !st.failsafe?.installed) chip("error", "alert", "live, but not switched until the fail-safe is installed");
    }

    // Device actions (on the saved config)
    const actions = el("div", { class: "actions" }, c);
    if (dirty() || !savedHasPlug) {
      el("span", { class: "hint" }, actions, "Save or discard your changes first: device actions (fail-safe, test, upgrade) use the saved settings.");
    } else if (st?.reachable) {
      if (!st.failsafe?.installed && !st.failsafe?.unknown) {
        confirmBtn(actions, "Install fail-safe", "shield",
          "Back up the plug's settings, turn off its own timers and install the fail-safe (about 10 seconds)? " +
          "From then on the plug needs heatbrain: switch it to Live afterwards - in dry-run it switches itself off and runs limp mode (10 min on, 50 min off).",
          async () => {
            try {
              const r = await api(`/api/plugs/${encodeURIComponent(p.id)}/provision`, { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
              if (r.failsafe?.installed) toast(`${p.name || p.id}: fail-safe installed - test it, then switch the plug to Live`);
              else toast(`${p.name || p.id}: fail-safe not complete - ${(r.failsafe?.problems || []).join("; ")}`, true);
            } catch (e) { toast(`${p.name || p.id}: fail-safe not installed - ${e.message}`, true); }
            S.plugStatus = null; await refreshPlugStatus();
          }, "primary", `${p.id}:provision`, "Installing fail-safe…");
      }
      if (p.mode !== "live" && !busy) {
        if (st.failsafe?.installed) {
          confirmBtn(actions, "Test fail-safe", "check",
            "The radiator turns on for about 30 seconds to check the auto-off. Go ahead?",
            () => startPlugJob(p, "test"), "", `${p.id}:test`, "Starting test…");
        }
        if (st.updateAvailable) {
          confirmBtn(actions, `Upgrade to ${st.latest.version.replace(/^v/, "")}`, "bolt",
            "Takes 2-5 minutes; the plug restarts and must not lose power meanwhile. The fail-safe is tested afterwards. Start?",
            () => startPlugJob(p, "upgrade"), "warm", `${p.id}:upgrade`);
        }
      } else if (p.mode === "live") {
        el("span", { class: "hint" }, actions, "Switch to dry-run and save to test or upgrade this plug.");
      }
    }

    renderJob(c, p);

    const adv = section(c, `plug:${p.id}`);
    const fsm = el("summary", {}, adv, "Advanced: fail-safe timing and password ");
    helpButton(fsm, "failsafe");
    const fs = (p.failsafe ||= {});
    const ag = el("div", { class: "form-grid" }, adv);
    field(ag, "Auto-off after", num(fs, "pulseMinutes", { step: 1, min: 1, placeholder: "20", unit: "min" }), { hint: "if heatbrain stops refreshing it" });
    field(ag, "Limp mode after", num(fs, "watchdogMinutes", { step: 1, min: 1, placeholder: "30", unit: "min" }), { hint: "of silence from heatbrain" });
    field(ag, "Limp mode ON", num(fs, "limpOnMinutes", { step: 1, min: 0, placeholder: "10", unit: "min" }), { hint: "0 = stay off (silent if heatbrain is gone)" });
    field(ag, "Limp mode OFF", num(fs, "limpOffMinutes", { step: 1, min: 5, placeholder: "50", unit: "min" }));
    field(ag, "Tasmota web password", secret(p, "password", "none"));
  }

  // --- Sensors ------------------------------------------------------------------------
  function renderSensors(body) {
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Netatmo weather station", "thermo", "warm", "Indoor modules give each room its temperature.", "sensors");
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
      field(g, "Refresh token", secret(n, "refreshToken"), { hint: "only needed once" });
      const steps = section(card, "netatmo-help");
      el("summary", {}, steps, "Where do I get these?");
      const ol = el("ol", {}, steps);
      el("li", {}, ol, "Log in at dev.netatmo.com/apps with your Netatmo account and create an app.");
      el("li", {}, ol, "Copy its client ID and client secret here.");
      el("li", {}, ol, "On the same page, use the Token generator with scope read_station and copy the refresh token.");
      el("li", {}, ol, "Save. Your modules appear below within a minute.");
    }

    const out = el("section", { class: "card" }, body);
    sectionHead(out, "Outdoor temperature", "snow", "cool",
      "Used to pre-heat more before expensive or off periods when it is cold outside, and less when it is mild.");
    const outdoorOptions = [["", "Automatic"]];
    for (const sns of state.status?.sensors || []) {
      if (sns.kind === "outdoor") outdoorOptions.push([`${sns.key.split(":")[0]}:${sns.name}`, `${sns.name}${sns.temperature != null ? ` · ${sns.temperature.toFixed(1)}°` : ""}`]);
    }
    if (!S.cfg.outdoorSensor) S.cfg.outdoorSensor = undefined;
    // No status yet (heatbrain just restarted): the chosen sensor is not "not found", just not read yet.
    if (!state.status && S.cfg.outdoorSensor) {
      outdoorOptions.push([S.cfg.outdoorSensor, `${S.cfg.outdoorSensor.split(":").slice(1).join(":")} (waiting for readings)`]);
    }
    const og = el("div", { class: "form-grid" }, out);
    field(og, "Outdoor sensor", sel(S.cfg, "outdoorSensor", outdoorOptions), { span2: true, hint: "automatic = the first outdoor module" });

    // Weather forecast (Open-Meteo): pre-heating is sized for the weather during the expensive hours.
    const fc = el("section", { class: "card" }, body);
    sectionHead(fc, "Weather forecast", "snow", "cool",
      "Pre-heating and booking warm-up use the forecast for the hours ahead, corrected to your outdoor sensor.", "forecast");
    if (!S.cfg.forecast || typeof S.cfg.forecast !== "object") S.cfg.forecast = {};
    const fcfg = S.cfg.forecast;
    const fg = el("div", { class: "form-grid" }, fc);
    const fon = el("div", { class: "field span2" }, fg);
    el("span", {}, fon, "Forecast");
    fon.appendChild(chk(fcfg, "enabled", "Fetch the forecast from Open-Meteo (free, no account)", true, () => render()));
    if (fcfg.enabled !== false) {
      const st = state.status?.forecast;
      // Where: a searched place, else where the Netatmo station is.
      const where = el("div", { class: "field span2" }, fg);
      el("span", {}, where, "Location");
      const cur = el("div", { class: "row" }, where);
      const own = fcfg.latitude != null && fcfg.longitude != null;
      el("span", {}, cur, own ? `${fcfg.place || "Set by hand"} (${fcfg.latitude.toFixed(2)}, ${fcfg.longitude.toFixed(2)})`
        : st?.from === "netatmo" ? `Your Netatmo station (${st.latitude.toFixed(2)}, ${st.longitude.toFixed(2)})`
        : "Your Netatmo station (not known yet)");
      if (own) btn(cur, "Use Netatmo's", "undo", () => { delete fcfg.latitude; delete fcfg.longitude; delete fcfg.place; changed(); render(); }, "ghost");

      // Search a town or address (Open-Meteo's place search, called from this browser).
      const search = el("div", { class: "field span2" }, fg);
      el("span", {}, search, "Search for a place");
      const srow = el("div", { class: "row" }, search);
      const q = el("input", { type: "search", placeholder: "town or postcode, e.g. Roskilde", autocomplete: "off", value: S.placeQuery || "", style: "flex: 1; min-width: 0" }, srow);
      const results = el("div", { class: "place-results" }, search);
      const find = async () => {
        const text = q.value.trim();
        S.placeQuery = text;
        results.replaceChildren();
        if (text.length < 2) return;
        results.textContent = "Searching…";
        try {
          const r = await fetch(`https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(text)}&count=6&language=en&format=json`);
          if (!r.ok) throw new Error(`HTTP ${r.status}`);
          const places = (await r.json()).results || [];
          results.replaceChildren();
          if (!places.length) { results.textContent = "No place found."; return; }
          for (const pl of places) {
            const label = [pl.name, pl.admin1, pl.country].filter(Boolean).join(", ");
            btn(results, label, "pin", () => {
              fcfg.latitude = Math.round(pl.latitude * 100) / 100;
              fcfg.longitude = Math.round(pl.longitude * 100) / 100;
              fcfg.place = [pl.name, pl.admin1].filter(Boolean).join(", ");
              S.placeQuery = "";
              changed(); render();
            }, "ghost");
          }
        } catch (e) { results.textContent = `Search failed: ${e.message}`; }
      };
      q.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); find(); } });
      btn(srow, "Search", "search", find);

      const info = el("div", { class: "hint", style: "grid-column: 1 / -1" }, fg);
      if (!st) info.textContent = "No position known yet: search for your town, or wait for the Netatmo station.";
      else if (st.error && st.min == null) info.textContent = `No forecast: ${st.error}`;
      else if (st.min) info.textContent = `Next 24 h: lowest ${st.min.temperature.toFixed(1)}° at ${hm(st.min.at)}, ` +
        `highest ${st.max.temperature.toFixed(1)}° at ${hm(st.max.at)}.` + (dirty() ? " (Save to use the new location.)" : "");
      else info.textContent = "Fetching the forecast…";
    }

    // Open-window detection: a fast fall in temperature pauses the room's heating.
    const ow = el("section", { class: "card" }, body);
    sectionHead(ow, "Open windows", "thermo", "warm",
      "When a room's temperature falls fast, a window or door is probably open: heating pauses for a while.", "openWindow");
    if (!S.cfg.openWindow || typeof S.cfg.openWindow !== "object") S.cfg.openWindow = {};
    const owc = S.cfg.openWindow;
    const wg = el("div", { class: "form-grid" }, ow);
    const won = el("div", { class: "field span2" }, wg);
    el("span", {}, won, "Detection");
    won.appendChild(chk(owc, "enabled", "Pause heating when the temperature falls fast", true, () => render()));
    if (owc.enabled !== false) {
      field(wg, "Fall within 30 min (°C)", num(owc, "drop", { step: 0.1, min: 0.3, max: 5, placeholder: "1.0" }));
      field(wg, "Pause (min)", num(owc, "pauseMinutes", { step: 5, min: 5, max: 240, placeholder: "30" }));
    }

    const mods = el("section", { class: "card" }, body);
    sectionHead(mods, "Modules", "wifi", "violet");
    // Indoor modules by name (Netatmo first, then the thermostats' own probes), the outdoor ones last.
    const rank = (s) => (s.kind === "outdoor" ? 2 : s.key.startsWith("devi:") ? 1 : 0);
    const sensors = [...(state.status?.sensors || [])].sort((a, b) => rank(a) - rank(b) || a.name.localeCompare(b.name));
    if (!sensors.length) el("div", { class: "muted", style: "margin-top:12px" }, mods, "No readings yet.");
    const list = el("div", { class: "list" }, mods);
    for (const s of sensors) {
      const r = el("div", { class: "item-head" }, list);
      const left = el("div", { class: "row" }, r);
      left.appendChild(badge(s.kind === "outdoor" ? "snow" : "thermo", s.kind === "outdoor" ? "cool" : "warm"));
      el("b", {}, left, s.name);
      // Where the reading comes from: a Netatmo module, or a thermostat's own probe (set up under Heating).
      const from = (s.key || "").startsWith("devi:") ? `thermostat · ${s.key.endsWith("-floor") ? "floor probe" : "room sensor"}`
        : [s.station, s.kind].filter(Boolean).join(" · ");
      el("span", { class: "muted" }, left, from);
      const chips = el("div", { class: "plugs" }, r);
      const chip = (cls, iconName, text) => { const ch = el("span", { class: `chip ${cls}` }, chips); ch.appendChild(icon(iconName)); ch.appendChild(document.createTextNode(text)); };
      // Battery first: the chips are right-aligned, so temperature and age line up on every module.
      if (s.battery != null) chip(s.battery <= 20 ? "error" : s.battery <= 40 ? "warn" : "ok", "battery", `${s.battery}%`);
      if (s.humidity != null) chip("", "drop", `${Math.round(s.humidity)}%`);
      if (s.temperature != null) chip("", "thermo", `${s.temperature.toFixed(1)}°C`);
      if (!s.reachable) chip("error", "alert", "offline");
      else if (s.ageMinutes != null) chip("", "clock", `${s.ageMinutes} min ago`);
    }
  }

  // --- Prices -------------------------------------------------------------------------
  const COMPONENT_TYPES = [
    ["energidataservice.spot", "Spot price"],
    ["energidataservice.tariff", "Grid tariff"],
    ["fixed", "Fixed per kWh"],
    ["schedule", "Time-of-day schedule"],
  ];

  // Household meter (Eloverblik): measured use, a day behind, shown next to the radiators.
  async function loadMeterStatus(force = false) {
    if (!force && S.meterStatus && Date.now() - S.meterStatus.at < 60_000) return;
    try { S.meterStatus = { at: Date.now(), ...(await api("/api/meter/status")) }; render(); } catch { /* optional */ }
  }
  function renderMeter(body) {
    loadMeterStatus();
    const st = S.meterStatus || {};
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Your meter (Eloverblik)", "gauge", "amber",
      "Measured use from your electricity meter, about a day behind - shown on the dashboard next to what the radiators drew.", "meter");
    const g = el("div", { class: "form-row" }, card);
    // The section only exists in the config once a token is entered.
    const m = S.cfg.meter || {};
    // A long token: a full-width textarea, shown as dots except while editing.
    const tf = el("div", { class: "field token-field" }, card);
    card.insertBefore(tf, g);
    el("span", {}, tf, "Token from eloverblik.dk");
    const ti = el("textarea", { rows: 3, class: "token-input", spellcheck: "false", autocomplete: "off",
                                placeholder: "Paste the token here: on eloverblik.dk log in with MitID → Data sharing → Create token" }, tf);
    ti.value = m.refreshToken || "";
    ti.addEventListener("focus", () => { ti.classList.add("editing"); if (ti.value === MASK) ti.select(); });
    ti.addEventListener("blur", () => ti.classList.remove("editing"));
    ti.addEventListener("input", () => {
      if (ti.value) S.cfg.meter = { type: "eloverblik", ...(S.cfg.meter || {}), refreshToken: ti.value };
      else delete S.cfg.meter;
      changed();
    });
    el("div", { class: "hint" }, tf, "stored on this heatbrain only, shown as ********");
    const points = st.points || [];
    if (points.length > 1) {
      field(g, "Meter", sel(S.cfg.meter ||= { type: "eloverblik" }, "meteringPoint",
        [["", "First consumption meter"], ...points.map((p) => [p.id, `${p.address || p.id} (${p.type})`])]), { wide: true });
    }
    const line = el("div", { class: `meter-status${st.error ? " error" : ""}` }, card);
    if (!st.configured) {
      line.textContent = "Not connected. Enter a token and save.";
    } else if (st.error) {
      line.textContent = `Problem: ${st.error}`;
    } else if (st.lastReading) {
      const since = st.firstReading ? ` · history from ${new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, day: "numeric", month: "short", year: "numeric" }).format(st.firstReading * 1000)}` : "";
      line.textContent = `Connected to ${st.address || st.meteringPoint}${st.supplier ? ` (${st.supplier})` : ""} · data until ${dayHm(st.lastReading)}${since} · ${st.resolutionMinutes}-minute values · checked ${dayHm(st.lastSync)}`;
    } else {
      line.textContent = "Connected - fetching the first readings…";
    }
    if (st.configured) {
      const row = el("div", { class: "row", style: "margin-top:8px" }, card);
      btn(row, "Fetch now", "wifi", async (b) => {
        b.disabled = true; b.lastChild.textContent = "Fetching…";
        try { await api("/api/meter/sync", { method: "POST" }); } catch (e) { toast(e.message, true); }
        // The fetch runs in the background; look again shortly.
        setTimeout(() => loadMeterStatus(true), 6000);
        setTimeout(() => loadMeterStatus(true), 20000);
      }, "ghost");
    }
  }

  function renderPrices(body) {
    const price = (S.cfg.price ||= { components: [] });
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "What a kWh costs", "bolt", "amber",
      "The price is the sum of these parts, then the surcharges (VAT) on top. Defaults fit EWII/Radius in DK2.", "prices");
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
        field(cg, "Price area", sel(comp, "area", [["DK1", "DK1 (West)"], ["DK2", "DK2 (East)"]]), { hint: "West: Jutland, Funen · East: Zealand" });
      } else if (comp.type === "energidataservice.tariff") {
        field(cg, "Grid company GLN", txt(comp, "gln", { placeholder: "5790000705689" }), { hint: "13-digit company number" });
        const codes = el("input", { type: "text", value: [].concat(comp.codes || []).join(", ") });
        codes.addEventListener("input", () => { comp.codes = codes.value.split(",").map((x) => x.trim()).filter(Boolean); changed(); });
        field(cg, "Charge codes", codes, { hint: "comma separated" });
      } else if (comp.type === "fixed") {
        field(cg, "Amount", num(comp, "amount", { step: 0.01, unit: `${S.cfg.currency === "DKK" || !S.cfg.currency ? "kr" : S.cfg.currency}/kWh` }), { hint: "excluding VAT" });
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
    // Same nested card as the price parts, so the delete icons line up.
    const slist = el("div", { class: "list" }, sc);
    surcharges.forEach((s, i) => {
      const box = el("div", { class: "card", style: "margin:0;background:var(--surface-2)" }, slist);
      const r = el("div", { class: "sched-row", style: "margin-top:0" }, box);
      r.appendChild(txt(s, "name", { placeholder: "Moms" }));
      r.appendChild(num(s, "percent", { step: 1, unit: "%" }));
      btn(r, "", "trash", () => { surcharges.splice(i, 1); changed(); render(); }, "ghost danger row-delete");
    });
    btn(el("div", { style: "margin-top:8px" }, sc), "Add surcharge", "plus", () => { surcharges.push({ name: "Moms", percent: 25 }); changed(); render(); });
    renderMeter(body);
  }

  // --- Notifications ------------------------------------------------------------------
  function renderNotifications(body) {
    const n = (S.cfg.notifications ||= { channels: [] });
    const channels = arr(n, "channels");
    const testButton = (parent) => {
      if (dirty()) { el("span", { class: "hint" }, parent, "Save first, then send a test message."); return; }
      btn(parent, "Send test message", "send", async (b) => {
        b.disabled = true;
        try {
          const r = await api("/api/notify/test", { method: "POST" });
          const bad = r.filter((x) => !x.ok);
          bad.length ? toast(bad.map((x) => `${x.channel}: ${x.error}`).join("; "), true)
                     : toast(`Test message sent (${r.map((x) => x.channel).join(", ")})`);
        } catch (e) { toast(e.message, true); }
        b.disabled = false;
      }, "primary");
    };

    // --- Phone notifications via ntfy (recommended: no account, no server) ---
    const phone = el("section", { class: "card" }, body);
    sectionHead(phone, "Phone notifications", "bell", "violet",
      "Free push notifications with the ntfy app (iPhone and Android). No account needed.", "notifications");
    const ntfy = channels.find((c) => c.type === "ntfy");
    const randomTopic = () => {
      const bytes = crypto.getRandomValues(new Uint8Array(12));
      return "heatbrain-" + [...bytes].map((x) => "abcdefghijkmnpqrstuvwxyz23456789"[x % 32]).join("");
    };
    if (!ntfy) {
      btn(el("div", { style: "margin-top:12px" }, phone), "Set up phone notifications", "plus", () => {
        channels.unshift({ type: "ntfy", server: "https://ntfy.sh", topic: randomTopic() });
        changed(); render();
      }, "primary");
    } else {
      const ol = el("ol", { class: "steps" }, phone);
      const li1 = el("li", {}, ol, "Install the ");
      el("a", { href: "https://docs.ntfy.sh/subscribe/phone/", target: "_blank", rel: "noopener" }, li1, "ntfy app");
      li1.appendChild(document.createTextNode(" from the App Store or Google Play."));
      el("li", {}, ol, "In the app, tap + and subscribe to this topic (it works like a password - keep it private):");
      const row = el("div", { class: "row", style: "margin:6px 0 0 0" }, phone);
      const topic = txt(ntfy, "topic");
      topic.style.width = "300px";
      topic.style.fontFamily = "ui-monospace, Consolas, monospace";
      row.appendChild(topic);
      btn(row, "Copy", "check", async () => {
        if (await copyText(ntfy.topic)) toast("Topic copied"); else topic.select();
      });
      btn(row, "New topic", "undo", () => { ntfy.topic = randomTopic(); changed(); render(); }, "ghost");
      const server = (ntfy.server || "https://ntfy.sh").replace(/\/+$/, "");
      const hint = el("div", { class: "hint" }, phone, "On your phone you can also just open ");
      el("a", { href: `${server}/${encodeURIComponent(ntfy.topic)}`, target: "_blank", rel: "noopener" }, hint, `${server.replace(/^https?:\/\//, "")}/${ntfy.topic}`);
      hint.appendChild(document.createTextNode(" - it opens the subscription in the app."));
      // Faster still: scan with the phone's camera. The iPhone app has no link it answers to, so
      // its code carries just the topic (the camera offers "Copy"); Android's opens the app directly.
      const qrs = el("div", { class: "qr-row" }, phone);
      const host = server.replace(/^https?:\/\//, "");
      const insecure = server.startsWith("http://") ? "?secure=false" : "";
      for (const [label, text, how] of [
        ["iPhone", ntfy.topic, "Scan → Copy → in ntfy tap + and paste"],
        ["Android", `ntfy://${host}/${ntfy.topic}${insecure}`, "Scan → opens ntfy and subscribes"],
      ]) {
        const box = el("div", { class: "qr-box" }, qrs);
        try {
          el("b", {}, box, label);
          box.appendChild(QR.svg(text, 150));
          el("div", { class: "hint" }, box, how);
        } catch { box.remove(); }
      }
      const ol2 = el("ol", { class: "steps", start: 3 }, phone);
      el("li", {}, ol2, "Save & apply, then send a test message.");
      testButton(el("div", { class: "actions" }, phone));
      const adv = section(phone, "ntfy-advanced");
      el("summary", {}, adv, "Advanced: own ntfy server");
      const ag = el("div", { class: "form-grid" }, adv);
      field(ag, "Server", txt(ntfy, "server", { placeholder: "https://ntfy.sh" }), { hint: "e.g. ntfy running on your NAS" });
      field(ag, "Access token", secret(ntfy, "token", "none"), { hint: "only for protected servers" });
      const rm = el("div", { class: "actions" }, adv);
      confirmBtn(rm, "Turn off phone notifications", "trash", "Remove the ntfy channel?", () => {
        channels.splice(channels.indexOf(ntfy), 1); changed(); render();
      }, "ghost danger");
    }

    // --- Discord (optional: needs your own server/channel) ---
    let discord = channels.find((c) => c.type === "discord");
    const isNew = !discord;
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Discord (optional)", "send", "gray",
      "Posts to a channel on a Discord server you own: Edit Channel → Integrations → Webhooks → New Webhook → Copy Webhook URL.");
    if (isNew) discord = { type: "discord", webhookUrl: "" };  // joins the config once something is typed
    const g = el("div", { class: "form-grid" }, card);
    const url = secret(discord, "webhookUrl", "https://discord.com/api/webhooks/…");
    url.addEventListener("input", () => { if (!channels.includes(discord)) { channels.push(discord); changed(); } });
    field(g, "Webhook URL", url, { wide: true });
    if (!ntfy) testButton(el("div", { class: "actions" }, card));

    const rules = el("section", { class: "card" }, body);
    sectionHead(rules, "When to notify", "bell", "amber",
      "Problems must last a while before you hear about them; you also get a message when they are resolved.");
    const rg = el("div", { class: "form-grid" }, rules);
    field(rg, "Reminder interval", num(n, "repeatHours", { step: 1, min: 1, placeholder: "6", unit: "h" }), { hint: "for problems that are still ongoing" });
    const quietOn = !!n.quietHours;
    const qwrap = el("div", { class: "field span2" }, rg);
    el("span", {}, qwrap, "Quiet hours");
    const qrow = el("div", { class: "row" }, qwrap);
    toggle(qrow, quietOn, quietOn ? "" : "Off", (v) => { if (v) n.quietHours = { from: "23:00", to: "07:00" }; else delete n.quietHours; changed(); render(); });
    el("div", { class: "hint" }, qwrap, "critical alerts still come through");
    if (quietOn) { qrow.appendChild(time(n.quietHours, "from")); el("span", {}, qrow, "–"); qrow.appendChild(time(n.quietHours, "to", true)); }
    const sg = el("div", { class: "field span2" }, rg);
    el("span", {}, sg, "Startup");
    sg.appendChild(chk(n, "notifyOnStart", "Send a message when heatbrain starts", true));
    const wk = el("div", { class: "field span2" }, rg);
    el("span", {}, wk, "Weekly summary");
    wk.appendChild(chk(n, "weeklySummary", "Cost and savings every Monday morning", true));
    const mo = el("div", { class: "field span2" }, rg);
    el("span", {}, mo, "Monthly report");
    mo.appendChild(chk(n, "monthlyReport", "Last month's cost, price paid and the dearest day, on the 1st", true));
    const hu = el("div", { class: "field span2" }, rg);
    el("span", {}, hu, "Tomorrow's prices");
    hu.appendChild(chk(n, "priceHeadsUp", "Tell me when tomorrow is unusually expensive or cheap", true));
    const fw = el("div", { class: "field span2" }, rg);
    el("span", {}, fw, "Firmware");
    fw.appendChild(chk(n, "firmwareUpdates", "Tell me when a new Tasmota release is out for my plugs", true));
  }

  // --- Wi-Fi off period: plugs get a night schedule, expected outages are not reported ---
  function renderWifiOff(body) {
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Wi-Fi off period", "wifi", "gray",
      "If you switch the Wi-Fi off at night: the plugs get a schedule beforehand and switch on their own clock.", "wifiOff");
    const g = el("div", { class: "form-row" }, card);
    const wifiOn = !!S.cfg.wifiOff;
    const wf = el("div", { class: "field span2" }, g);
    el("span", {}, wf, "Every night");
    const wrow = el("div", { class: "row" }, wf);
    toggle(wrow, wifiOn, wifiOn ? "" : "No Wi-Fi off period", (v) => {
      if (v) S.cfg.wifiOff = { from: "23:30", to: "06:30" }; else delete S.cfg.wifiOff;
      changed(); render();
    });
    if (wifiOn) { wrow.appendChild(time(S.cfg.wifiOff, "from")); el("span", {}, wrow, "–"); wrow.appendChild(time(S.cfg.wifiOff, "to", true)); }
    el("div", { class: "hint" }, wf, wifiOn ? "when the router switches the Wi-Fi off and on" : "the Wi-Fi is always on");
    if (wifiOn) {
      const wk = el("div", { class: "field span2" }, g);
      el("span", {}, wk, "At the weekend");
      const krow = el("div", { class: "row" }, wk);
      const weekendOn = !!S.cfg.wifiOff.weekend;
      toggle(krow, weekendOn, weekendOn ? "" : "Same as other nights", (v) => {
        if (v) S.cfg.wifiOff.weekend = { from: S.cfg.wifiOff.from, to: "09:00" }; else delete S.cfg.wifiOff.weekend;
        changed(); render();
      });
      if (weekendOn) { krow.appendChild(time(S.cfg.wifiOff.weekend, "from")); el("span", {}, krow, "–"); krow.appendChild(time(S.cfg.wifiOff.weekend, "to", true)); }
      el("div", { class: "hint" }, wk, "the nights ending Saturday and Sunday morning");
    }
    renderWifiSeen(card);
  }

  // --- On your phone: home-screen app and a widget (Scriptable) ------------------------
  // Copy without the Clipboard API: it needs HTTPS, and heatbrain runs on plain http at home.
  function copyText(text, fallbackEl) {
    if (navigator.clipboard && window.isSecureContext) return navigator.clipboard.writeText(text).then(() => true, () => false);
    const ta = fallbackEl || el("textarea", { style: "position:fixed;opacity:0" }, document.body);
    ta.value = text; ta.focus(); ta.select(); ta.setSelectionRange(0, text.length);
    let ok = false;
    try { ok = document.execCommand("copy"); } catch { ok = false; }
    if (!fallbackEl) ta.remove();
    return Promise.resolve(ok);
  }
  function renderPhone(body) {
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "On your phone", "home", "violet",
      "heatbrain as an app on the home screen, and a widget with your rooms at a glance.", "phone");
    const grid = el("div", { class: "transfer" }, card);

    const app = el("div", {}, grid);
    const ah = el("h3", {}, app); ah.appendChild(icon("home")); ah.appendChild(document.createTextNode("Home-screen app"));
    const ol = el("ol", { class: "phone-steps" }, app);
    el("li", {}, ol, `On the phone, open ${location.origin} in Safari (Chrome on Android).`);
    el("li", {}, ol, "iPhone: Share → Add to Home Screen. Android: ⋮ → Add to Home screen / Install app.");
    el("li", {}, ol, "It opens full screen with the heatbrain icon.");
    const qr = el("div", { class: "qr-box", style: "margin-top:8px" }, app);
    try { qr.appendChild(QR.svg(location.origin + "/", 130)); el("div", { class: "hint" }, qr, "scan to open heatbrain on the phone"); } catch { qr.remove(); }

    const wid = el("div", {}, grid);
    const wh = el("h3", {}, wid); wh.appendChild(icon("gauge")); wh.appendChild(document.createTextNode("Widget (iPhone)"));
    const ol2 = el("ol", { class: "phone-steps" }, wid);
    el("li", {}, ol2, "Install the free app Scriptable from the App Store.");
    el("li", {}, ol2, "Open this page on the phone, tap Copy widget script below, then in Scriptable tap + and paste. Name it heatbrain.");
    el("li", {}, ol2, "Home screen: long-press → Edit → + → Scriptable → pick a size → tap the widget → Script: heatbrain.");
    // Address for away from home (e.g. Tailscale): a real setting, saved with the config, so it
    // is there on every device (browser storage was unreliable on the iPhone).
    const phoneCfg = S.cfg.phone || {};
    const af = el("div", { class: "field", style: "margin-top:8px" }, wid);
    el("span", {}, af, "Address away from home (optional)");
    const ai = el("input", { type: "url", value: phoneCfg.awayUrl || "", placeholder: "e.g. http://100.x.y.z:8090 (the NAS in Tailscale)",
                             autocapitalize: "off", autocorrect: "off", spellcheck: "false" }, af);
    const cleanUrl = (v) => {
      v = v.trim().replace(/\/+$/, "");
      return v && !/^https?:\/\//.test(v) ? `http://${v}` : v;  // "100.84.100.80:8090" is fine too
    };
    const urlsNow = () => [location.origin, cleanUrl(ai.value)].filter((u, i, a) => u && a.indexOf(u) === i);
    const tries = el("div", { class: "hint" }, af);
    const showTries = () => { tries.textContent = `The widget tries: ${urlsNow().join(", then ")}`; };
    showTries();
    ai.addEventListener("input", () => {
      const v = cleanUrl(ai.value);
      if (v) S.cfg.phone = { ...(S.cfg.phone || {}), awayUrl: v }; else delete S.cfg.phone;
      showTries();
      changed();
    });
    el("div", { class: "hint" }, af, "Save, then copy the script. Needs Tailscale on the NAS and the phone; never forward a router port to heatbrain - it has no login.");
    const row = el("div", { class: "row", style: "margin-top:8px" }, wid);
    const ta = el("textarea", { class: "widget-script", rows: 4, readonly: true, spellcheck: "false" });
    btn(row, "Copy widget script", "check", async () => {
      try {
        const src = await (await fetch("widget-scriptable.js", { cache: "no-store" })).text();
        const urls = urlsNow();
        const script = src.replace("__HEATBRAIN_URLS__", JSON.stringify(urls));
        ta.value = script;
        if (!ta.isConnected) wid.appendChild(ta);
        toast((await copyText(script, ta)) ? "Widget script copied - paste it in Scriptable" : "Select the script below and copy it");
      } catch (e) { toast(`Could not load the script: ${e.message}`, true); }
    }, "primary");
    el("div", { class: "hint" }, wid, "It tries the addresses in order; when none answers it shows the last data it got.");
  }

  // --- Wi-Fi off times as the sensors saw them -------------------------------------------
  // Netatmo uses the same Wi-Fi: gaps in its readings show when the network was really off.
  const hmOf = (m) => `${String(Math.floor(m / 60) % 24).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
  const minOf = (s) => { const [h, m] = s.split(":").map(Number); return h * 60 + m; };
  function localParts(ts) {
    const p = Object.fromEntries(new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, weekday: "short", day: "numeric", month: "short",
      hour: "2-digit", minute: "2-digit", hourCycle: "h23" }).formatToParts(ts * 1000).map((x) => [x.type, x.value]));
    return { minute: Number(p.hour) * 60 + Number(p.minute), weekday: p.weekday, label: `${p.weekday} ${p.day} ${p.month}` };
  }
  async function loadWifiSeen() {
    if (S.wifiSeen && Date.now() - S.wifiSeen.at < 300_000) return;
    try { S.wifiSeen = { at: Date.now(), ...(await api("/api/wifi-gaps")) }; render(); } catch { /* optional */ }
  }
  function renderWifiSeen(card) {
    loadWifiSeen();
    const seen = S.wifiSeen;
    // Ignore short hiccups; one still running counts from 30 minutes (the server's minimum).
    const nights = (seen?.gaps || []).filter((g) => g.ongoing || g.to - g.from >= 3600);
    if (!nights.length) return;
    const box = el("div", { class: "wifi-seen" }, card);
    el("h3", {}, box, "Wi-Fi off, as the sensors saw it (last 7 days)");
    const w = S.cfg.wifiOff;
    const groups = { weekday: [], weekend: [] };
    const tbl = el("table", { class: "events" }, box);
    for (const g of nights) {
      const a = localParts(g.from), b = localParts(g.to);
      const weekend = b.weekday === "Sat" || b.weekday === "Sun";
      if (!g.ongoing) (weekend ? groups.weekend : groups.weekday).push([a.minute, b.minute]);
      // Covered: the readings stopped at or after the planned start and came back by the planned end (±10 min).
      let verdict = "", cls = "";
      if (w && !g.ongoing) {
        const win = weekend && w.weekend ? w.weekend : w;
        const rel = (m) => (m - 720 + 1440) % 1440;  // minutes after noon: nights don't wrap
        const okStart = rel(a.minute) >= rel(minOf(win.from)) - 10;
        const okEnd = rel(b.minute) <= rel(minOf(win.to)) + 10;
        verdict = okStart && okEnd ? "covered" : !okStart ? `off before ${win.from}` : `back after ${win.to}`;
        cls = okStart && okEnd ? "ok" : "warn";
      }
      const tr = el("tr", {}, tbl);
      el("td", {}, tr, b.label);
      el("td", {}, tr, `${hmOf(a.minute)} – ${g.ongoing ? "now" : hmOf(b.minute)}`);
      el("td", { class: `seen-${cls}` }, tr, g.ongoing ? "still off" : verdict);
    }
    // Suggest: earliest stop and latest return per group, rounded outwards to 15 minutes.
    const suggest = (list) => {
      if (list.length < 2) return null;
      const rel = (m) => (m - 720 + 1440) % 1440, back = (r) => (r + 720) % 1440;
      const from = back(Math.floor(Math.min(...list.map(([s]) => rel(s))) / 15) * 15);
      const to = back(Math.ceil(Math.max(...list.map(([, e]) => rel(e))) / 15) * 15);
      return { from: hmOf(from), to: hmOf(to) };
    };
    const sw = suggest(groups.weekday), se = suggest(groups.weekend);
    if (sw || se) {
      const row = el("div", { class: "row", style: "margin-top:8px" }, box);
      const text = [sw && `other nights ${sw.from}–${sw.to}`, se && `weekend ${se.from}–${se.to}`].filter(Boolean).join(", ");
      el("span", { class: "muted" }, row, `Suggested: ${text}`);
      btn(row, "Use these times", "check", () => {
        const base = sw || se;
        S.cfg.wifiOff = { from: base.from, to: base.to };
        if (se && sw && (se.from !== sw.from || se.to !== sw.to)) S.cfg.wifiOff.weekend = se;
        changed(); render(); toast("Times filled in - review and save");
      }, "ghost");
    }
    el("div", { class: "hint" }, box, "From gaps in the Netatmo readings (they come every ~10 minutes, so times are approximate). The period should cover the whole time the Wi-Fi is off.");
  }

  // --- Export / import (moving heatbrain, e.g. to the NAS) ------------------------------
  function renderTransfer(body) {
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "Move or back up heatbrain", "save", "green",
      "One file with everything needed to run heatbrain elsewhere - for example when moving from this PC to the NAS.", "transfer");
    const wrap = el("div", { class: "transfer" }, card);

    // Export
    const ex = el("div", {}, wrap);
    const eh = el("h3", {}, ex); eh.appendChild(icon("save")); eh.appendChild(document.createTextNode("Export"));
    el("div", { class: "hint" }, ex, "Settings incl. passwords and the Netatmo login, plug backups and test results, away period and overrides. Keep the file private.");
    const exRow = el("div", { class: "row" }, ex);
    S.exportHistory ??= true;
    exRow.appendChild(toggle(null, S.exportHistory, "Include history (graphs, costs)", (v) => { S.exportHistory = v; }));
    btn(exRow, "Download export", "save", async (b) => {
      if (dirty()) return toast("Save or discard your changes first - the export uses the saved settings.", true);
      b.disabled = true; b.lastChild.textContent = "Exporting…";
      try {
        const res = await fetch(`/api/export?history=${S.exportHistory ? 1 : 0}`, { method: "POST" });
        if (!res.ok) throw new Error((await res.json().catch(() => ({}))).error || `HTTP ${res.status}`);
        const blob = await res.blob();
        const stamp = new Date().toISOString().slice(0, 16).replace(/[-:]/g, "").replace("T", "-");
        const a = el("a", { href: URL.createObjectURL(blob), download: `heatbrain-export-${stamp}.json` }, document.body);
        a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
        toast(`Exported (${(blob.size / 1024).toFixed(0)} KB)`);
      } catch (e) { toast(`Export failed: ${e.message}`, true); }
      b.disabled = false; b.lastChild.textContent = "Download export";
    }, "primary");

    // Import
    const im = el("div", {}, wrap);
    const ih = el("h3", {}, im); ih.appendChild(icon("undo")); ih.appendChild(document.createTextNode("Import"));
    el("div", { class: "hint" }, im, "Replaces this heatbrain's settings and state with an export. The current settings are backed up first.");
    const imRow = el("div", { class: "row" }, im);
    const file = el("input", { type: "file", accept: ".json,application/json", hidden: true }, imRow);
    btn(imRow, S.importFile ? "Choose another file" : "Choose export file", "search", () => file.click(), "tone-btn-amber");
    file.addEventListener("change", async () => {
      const f = file.files[0];
      if (!f) return;
      try {
        const text = await f.text();
        const j = JSON.parse(text);
        if (j.format !== "heatbrain-export") throw new Error("not a heatbrain export file");
        S.importFile = { name: f.name, text, exportedAt: j.exportedAt, rooms: (j.config?.rooms || []).length,
                         plugs: (j.config?.plugs || []).length, files: Object.keys(j.files || {}).length, history: !!j.history };
      } catch (e) { S.importFile = null; toast(`Can't use ${f.name}: ${e.message}`, true); }
      render();
    });
    const f = S.importFile;
    if (f) {
      const when = f.exportedAt ? new Intl.DateTimeFormat("en-GB", { timeZone: state.tz, dateStyle: "medium", timeStyle: "short" }).format(new Date(f.exportedAt)) : "unknown time";
      el("div", { class: "file-summary" }, im,
        `${f.name}: exported ${when} · ${f.rooms} rooms · ${f.plugs} plugs · ${f.files} state files · ${f.history ? "with history" : "no history"}`);
      const go = el("div", { class: "row" }, im);
      confirmBtn(go, "Import", "check",
        `Replace this heatbrain's settings${f.history ? ", history" : ""} and state with the export of ${when}?`,
        async () => {
          try {
            const res = await fetch("/api/import", { method: "POST", headers: { "Content-Type": "application/json" }, body: f.text });
            const r = await res.json().catch(() => ({}));
            if (!res.ok) throw new Error(r.error || `HTTP ${res.status}`);
            S.importFile = null;
            toast(`Imported ${r.files} files${r.history ? " and the history" : ""} - heatbrain is reloading`);
            setTimeout(() => location.reload(), 2500);
          } catch (e) { toast(`Import failed: ${e.message}`, true); }
        }, "primary", "import", "Importing…");
      btn(go, "Cancel", null, () => { S.importFile = null; render(); }, "ghost");
    }
  }

  // --- System -------------------------------------------------------------------------
  function renderSystem(body) {
    const card = el("section", { class: "card" }, body);
    sectionHead(card, "System", "cpu", "violet");
    const g = el("div", { class: "form-row" }, card);
    // All IANA zones the browser knows; the server validates the choice again.
    const zones = (Intl.supportedValuesOf?.("timeZone") || ["Europe/Copenhagen"]).map((z) => [z, z.replaceAll("_", " ")]);
    if (!S.cfg.timezone) S.cfg.timezone = "Europe/Copenhagen";
    field(g, "Time zone", sel(S.cfg, "timezone", zones));
    const control = (S.cfg.control ||= {});
    field(g, "Control interval", num(control, "tickSeconds", { step: 60, min: 60, max: 3600, placeholder: "300", unit: "s" }),
      { hint: "at most half of each live plug's auto-off time" });
    // What the dashboard shows under the room chart (for everyone: part of the config).
    const dash = (S.cfg.dashboard ||= {});
    const sp = el("div", { class: "field" }, g);
    el("span", {}, sp, "Price panel");
    sp.appendChild(chk(dash, "price", "Show the electricity price under the room chart", true));
    const sm = el("div", { class: "field" }, g);
    el("span", {}, sm, "Power mix panel");
    sm.appendChild(chk(dash, "powerMix", "Show wind, solar, power plants and imports per hour", true));
    // Appearance: stored in this browser only, applied immediately (not part of the config).
    const ap = el("div", { class: "field" }, g);
    el("span", {}, ap, "Appearance");
    const seg = el("div", { class: "segmented" }, ap);
    let current = "auto";
    try { current = localStorage.getItem("heatbrain-theme") || "auto"; } catch {}
    for (const [value, label, iconName] of [["auto", "Auto", "gauge"], ["light", "Light", "info"], ["dark", "Dark", "snow"]]) {
      const b = btn(seg, label, null, () => {
        try { value === "auto" ? localStorage.removeItem("heatbrain-theme") : localStorage.setItem("heatbrain-theme", value); } catch {}
        if (value === "auto") delete document.documentElement.dataset.theme;
        else document.documentElement.dataset.theme = value;
        render();
      });
      b.classList.toggle("active", current === value);
    }
    el("div", { class: "hint" }, ap, "this browser only");
    // Animations (the fire on heating rooms): follow the OS's reduce-motion setting, always, or off.
    const an = el("div", { class: "field" }, g);
    el("span", {}, an, "Animations");
    const aseg = el("div", { class: "segmented" }, an);
    let motion = "auto";
    try { motion = localStorage.getItem("heatbrain-motion") || "auto"; } catch {}
    for (const [value, label] of [["auto", "Auto"], ["always", "Always"], ["off", "Off"]]) {
      const b = btn(aseg, label, null, () => {
        try { value === "auto" ? localStorage.removeItem("heatbrain-motion") : localStorage.setItem("heatbrain-motion", value); } catch {}
        if (value === "auto") delete document.documentElement.dataset.motion;
        else document.documentElement.dataset.motion = value;
        render();
      });
      b.classList.toggle("active", motion === value);
    }
    el("div", { class: "hint" }, an, matchMedia("(prefers-reduced-motion: reduce)").matches
      ? "this browser only · the system asks for less motion, so Auto is still" : "this browser only · Auto follows the system");
    if (S.fixedWebPort) {
      // In Docker the address is chosen by the compose file's port mapping, not here.
      const wp = el("div", { class: "field" }, g);
      el("span", {}, wp, "Web UI port");
      el("div", { class: "fixed-value" }, wp, `Set by Docker (${location.port || "80"})`);
      el("div", { class: "hint" }, wp, "change the first number under ports: in docker-compose.yml");
    } else {
      const web = (S.cfg.web ||= {});
      field(g, "Web UI port", num(web, "port", { step: 1, min: 1, max: 65535, placeholder: "8080" }), { hint: "takes effect after a restart" });
    }

    renderWifiOff(body);
    renderPhone(body);
    renderTransfer(body);

    const raw = el("section", { class: "card" }, body);
    sectionHead(raw, "Raw configuration", "gear", "gray", "For power users. Secrets are shown as ******** and kept unless you replace them.");
    const ta = el("textarea", { rows: 18 }, raw);
    ta.value = JSON.stringify(S.cfg, null, 2);
    const row = el("div", { class: "actions" }, raw);
    btn(row, "Apply JSON to the form", "check", () => {
      try { S.cfg = JSON.parse(ta.value); changed(); render(); toast("Applied - review and save"); }
      catch (e) { toast(`Invalid JSON: ${e.message}`, true); }
    }, "apply-btn");
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
      // heatbrain restarts with the new settings; ask until the status of its first step is in, so the
      // dashboard (room order, names, programs) is current when you go back to it.
      if (typeof refreshAfterTick === "function") refreshAfterTick(); else setTimeout(refreshStatus, 2500);
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
