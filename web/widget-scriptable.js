// heatbrain home-screen widget for the iPhone app "Scriptable" (free, App Store).
// Install: Scriptable -> + -> paste this script -> name it "heatbrain". Then on the home screen:
// long-press -> Edit Home Screen -> + -> Scriptable -> choose a size -> tap the widget -> Script: heatbrain.
// It reads heatbrain's status from your home network; away from home it shows the last data it got.

// Tried in order: at home the first answers; away from home e.g. a Tailscale address.
const BASES = __HEATBRAIN_URLS__;  // filled in by heatbrain when you copy the script
let BASE = BASES[0];

const fm = FileManager.local();
const cacheFile = fm.joinPath(fm.documentsDirectory(), "heatbrain-status.json");

// Ask every address at once; the first good answer wins (no waiting for a timeout at home or away).
function firstAnswer(timeout) {
  return new Promise((resolve) => {
    let pending = BASES.length;
    if (!pending) resolve(null);
    for (const base of BASES) {
      const req = new Request(`${base}/api/status`);
      req.timeoutInterval = timeout;
      req.loadJSON().then(
        (s) => { if (s && s.rooms) resolve({ base, s }); else if (--pending === 0) resolve(null); },
        () => { if (--pending === 0) resolve(null); });
    }
  });
}

async function loadStatus() {
  const hit = await firstAnswer(8);
  if (hit) {
    BASE = hit.base;
    fm.writeString(cacheFile, JSON.stringify(hit.s));
    return { s: hit.s, live: true };
  }
  if (fm.fileExists(cacheFile)) return { s: JSON.parse(fm.readString(cacheFile)), live: false };
  return { s: null, live: false };
}

const hm = (ts) => {
  const d = new Date(ts * 1000);
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
};
const deg = (v) => (v == null ? "–" : `${v.toFixed(1)}°`);
const C = {
  bg: new Color("#17171b"), ink: new Color("#f2f2f4"), muted: new Color("#9a9aa3"),
  flame: new Color("#ff7a1a"), cool: new Color("#6ea8ff"), warn: new Color("#ffb020"), bad: new Color("#ff5a5f"),
};

function priceInfo(s) {
  const now = Date.now() / 1000;
  const prices = s.prices || [];
  const cur = prices.find(([t]) => now >= t && now < t + 900);
  let best = null;
  for (const [t, p] of prices) if (t >= now && t < now + 24 * 3600 && (!best || p < best[1])) best = [t, p];
  return { now: cur ? cur[1] : null, best };
}

function addText(stack, text, size, color, bold = false) {
  const t = stack.addText(text);
  t.font = bold ? Font.boldSystemFont(size) : Font.systemFont(size);
  t.textColor = color;
  t.lineLimit = 1;
  return t;
}

// A tap runs this script with ?open=1: pick the address that answers now (home or Tailscale)
// and open it in Safari. (The widget's own link would be whatever answered at its last refresh.)
async function openReachable() {
  const hit = await firstAnswer(5);
  if (hit) { Safari.open(hit.base); return; }
  const a = new Alert();
  a.title = "heatbrain is not reachable";
  a.message = BASES.length > 1 ? "Check the Wi-Fi, or that Tailscale is on." : "Connect to your home Wi-Fi.";
  a.addAction("OK");
  await a.present();
}

async function build() {
  const { s, live } = await loadStatus();
  const w = new ListWidget();
  w.backgroundColor = C.bg;
  w.setPadding(12, 14, 12, 14);
  w.url = `${URLScheme.forRunningScript()}?open=1`;
  w.refreshAfterDate = new Date(Date.now() + 10 * 60 * 1000);
  const family = config.widgetFamily || "medium";

  const head = w.addStack();
  head.centerAlignContent();
  addText(head, "🔥 heatbrain", family === "small" ? 12 : 13, C.ink, true);
  head.addSpacer();
  if (s) {
    const p = priceInfo(s);
    if (p.now != null) addText(head, `${p.now.toFixed(2)} kr`, 12, C.muted);
  }
  w.addSpacer(6);

  if (!s) {
    addText(w, "Can't reach heatbrain", 13, C.bad, true);
    addText(w, BASES.length > 1 ? "Check Wi-Fi or Tailscale" : "Connect to your home Wi-Fi", 11, C.muted);
    return w;
  }

  const rooms = s.rooms || [];
  const max = family === "small" ? 3 : family === "large" ? 10 : 4;
  for (const r of rooms.slice(0, max)) {
    const row = w.addStack();
    row.centerAlignContent();
    const icon = addText(row, r.on ? "🔥" : "·", 12, r.on ? C.flame : C.muted);
    icon.font = Font.systemFont(12);
    row.addSpacer(4);
    addText(row, r.name, 12, C.ink);
    row.addSpacer();
    const temp = r.inputs?.temperature;
    const target = r.inputs?.target;
    if (r.priceOnly) addText(row, r.on ? "on" : "off", 12, r.on ? C.flame : C.muted);
    else addText(row, family === "small" ? deg(temp) : `${deg(temp)} → ${deg(target)}`, 12, r.on ? C.flame : C.ink);
    w.addSpacer(3);
  }

  w.addSpacer();
  const foot = w.addStack();
  if (family !== "small") {
    const p = priceInfo(s);
    if (p.best) addText(foot, `cheapest ${hm(p.best[0])} (${p.best[1].toFixed(2)})`, 10, C.muted);
    foot.addSpacer();
  }
  const problems = (s.problems || []).length;
  if (!live) addText(foot, `unreachable · data from ${hm(s.now)}`, 10, C.warn);
  else if (problems) addText(foot, `⚠️ ${problems} problem${problems === 1 ? "" : "s"}`, 10, C.warn);
  else if (s.wifiOff?.active) addText(foot, `Wi-Fi off until ${hm(s.wifiOff.end)}`, 10, C.muted);
  else addText(foot, `updated ${hm(s.now)}`, 10, C.muted);
  return w;
}

if (!config.runsInWidget && args.queryParameters && args.queryParameters.open) {
  await openReachable();  // tapped on the home screen
} else {
  const widget = await build();
  if (config.runsInWidget) Script.setWidget(widget);
  else await widget.presentMedium();
}
Script.complete();
