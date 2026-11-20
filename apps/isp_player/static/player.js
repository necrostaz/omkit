"use strict";
// ISP player page: a picture and an .isp profile; the profile's tables rendered by the server (server.py), the file's
// contents in the side panel, two views: the ISP view and a before/after split.
const $ = (id) => document.getElementById(id);
const st = {
  image: null, imageInfo: null, isp: null, ispInfo: null, ev: 0, tab: "view", pos: 50,
  busy: false, again: false, imgs: null, limits: { image: 200 * 1024 * 1024, isp: 1024 * 1024 },
};

// ---------------------------------------------------------------- helpers
function status(text, cls = "") {
  $("status").className = "status " + cls;
  $("status-text").textContent = text;
}

async function post(path, body, name, extra = {}) {
  const opt = { method: "POST" };
  if (name !== undefined) { opt.body = body; opt.headers = { "X-Name": encodeURIComponent(name), ...extra }; }
  else { opt.body = JSON.stringify(body); opt.headers = { "Content-Type": "application/json" }; }
  const r = await fetch(path, opt);
  const j = await r.json();
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v; else if (k === "text") e.textContent = v; else if (k === "style") e.style.cssText = v;
    else e.setAttribute(k, v);
  }
  for (const c of kids) if (c != null) e.append(c);
  return e;
}

const fmt = (v, d = 2) => (v >= 0 ? "" : "−") + Math.abs(v).toFixed(d);
const bytes = (n) => n < 1024 ? `${n} B` : `${(n / 1024).toFixed(1)} KB (${n.toLocaleString("en")} B)`;
const isIsp = (name) => /\.isp$/i.test(name);
function tooBig(file, limit) {
  if (file.size <= limit) return false;
  status(`${file.name}: ${(file.size / 1048576).toFixed(1)} MB, larger than the ${Math.round(limit / 1048576)} MB the player takes`, "err");
  return true;
}

// ---------------------------------------------------------------- drop zones
function setupDrop(zoneId, inputId, handler) {
  const z = $(zoneId), inp = $(inputId);
  z.addEventListener("click", () => inp.click());
  z.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); inp.click(); } });
  inp.addEventListener("change", () => { if (inp.files[0]) handler(inp.files[0]); inp.value = ""; });
  z.addEventListener("dragover", (e) => { e.preventDefault(); z.classList.add("over"); });
  z.addEventListener("dragleave", () => z.classList.remove("over"));
  z.addEventListener("drop", (e) => {
    e.preventDefault(); e.stopPropagation(); z.classList.remove("over"); veil(false);
    if (e.dataTransfer.files[0]) handler(e.dataTransfer.files[0]);
  });
}
setupDrop("drop-image", "file-image", onImage);
setupDrop("drop-isp", "file-isp", onIsp);

// anywhere on the page: by the file's type; several files at once (a picture and a profile) work too
let veilDepth = 0;
function veil(on) { $("dropveil").classList.toggle("on", on); if (!on) veilDepth = 0; }
document.addEventListener("dragenter", (e) => { if (e.dataTransfer.types.includes("Files")) { veilDepth++; veil(true); } });
document.addEventListener("dragleave", () => { if (--veilDepth <= 0) veil(false); });
document.addEventListener("dragover", (e) => e.preventDefault());
document.addEventListener("drop", async (e) => {
  e.preventDefault(); veil(false);
  const files = [...e.dataTransfer.files];
  const isp = files.find((f) => isIsp(f.name)), img = files.find((f) => !isIsp(f.name));
  if (img) await onImage(img);
  if (isp) await onIsp(isp);
});

// ---------------------------------------------------------------- picture
async function onImage(file, space = "auto") {
  if (tooBig(file, st.limits.image)) return;
  try {
    status(`Loading ${file.name}…`, "busy");
    const r = await post("/api/image", await file.arrayBuffer(), file.name, { "X-Space": space });
    st.image = r.id; st.imageInfo = r; st.imageFile = file;
    const n = $("image-name"); n.hidden = false; n.textContent = `${r.name} · ${r.w}×${r.h}`;
    $("drop-image").classList.add("loaded"); $("step-image").classList.add("done");
    const sel = $("space"), box = $("space-box");
    box.hidden = r.space === "raw";
    if (sel.options.length < 2) for (const [k, v] of Object.entries(r.spaces)) sel.append(el("option", { value: k, text: v }));
    if (space === "auto") sel.value = "auto";
    const how = $("image-how"); how.hidden = false;
    how.textContent = `${r.space_label}: ${r.how}; median luminance ${(r.median_y * 100).toFixed(1)} %`;
    $("ev-note").hidden = false;
    $("frame-view").style.setProperty("--ar", r.w / r.h); $("frame-split").style.setProperty("--ar", r.w / r.h);
    updateButtons();
    await render();
  } catch (e) { status(e.message, "err"); }
}
$("space").addEventListener("change", () => st.imageFile && onImage(st.imageFile, $("space").value));

// ---------------------------------------------------------------- profile
async function onIsp(file) {
  if (tooBig(file, st.limits.isp)) return;
  try {
    status(`Reading ${file.name}…`, "busy");
    showIsp(await post("/api/isp", await file.arrayBuffer(), file.name));
    $("isp-card").scrollIntoView({ behavior: "smooth", block: "start" });
    await render();
  } catch (e) { status(e.message, "err"); }
}

function showIsp(r) {
  st.isp = r.id; st.ispInfo = r;
  const n = $("isp-name"); n.hidden = false; n.textContent = r.file;
  $("drop-isp").classList.add("loaded"); $("step-isp").classList.toggle("done", r.renderable);
  $("isp-card").hidden = false;
  // cassette
  $("cassette").src = `/api/cassette?isp=${r.id}`;
  const c = r.cani || {};
  $("cassette-cap").textContent = r.art === "file" ? `cassette from the file (CANI, ${c.records} records, ${c.size} B)`
    : r.art.startsWith("generic") ? `no valid CANI in the file - a generic cassette: "${c.generic}"` : "no cassette";
  $("i-title").textContent = r.name || "(no name)";
  $("i-file").textContent = r.file;
  // chips
  const chips = $("i-chips"); chips.replaceChildren();
  const add = (t, cls = "", title = "") => chips.append(el("span", { class: "chip " + cls, text: t, ...(title ? { title } : {}) }));
  if (c.brand) add(c.brand, "on", "Brand on the cassette");
  if (c.iso) add(`ISO ${c.iso}`, "on", "ISO on the cassette");
  if (r.platform) add(r.platform, "dim", "Platform");
  if (r.version) add(`format ${r.version}`, "dim");
  if (r.isp && r.isp.mono) add("B&W", "on", "Saturation factor k = 0: the camera gives black and white");
  // verdict
  const v = $("i-verdict"); v.replaceChildren();
  const list = (items) => el("ul", {}, ...items.map((t) => el("li", { text: t })));
  if (r.errors.length) {
    v.className = "verdict bad";
    v.append(el("b", { text: "The file cannot be shown" }), list(r.errors));
    if (r.warnings.length) v.append(list(r.warnings));
  } else if (r.warnings.length) {
    v.className = "verdict warn"; v.append(el("b", { text: "The file reads, with remarks" }), list(r.warnings));
  } else {
    v.className = "verdict ok"; v.append(el("b", { text: "The file is fine: the header and every section match their CRC" }));
  }
  // key facts
  const kv = $("i-kv"); kv.replaceChildren();
  const row = (k, val, title) => kv.append(el("dt", { text: k, ...(title ? { title } : {}) }), el("dd", { text: val }));
  const names = { matrix: "matrix", gamma: "gamma", tone: "tone", sat: "saturation", saturation: "saturation", sectors: "sectors" };
  row("Size", bytes(r.size));
  row("File CRC-32", r.crc, "CRC-32 of the whole file: FILMLOOK writes the preset into a shot's EXIF by it");
  if (r.header_crc) row("Header CRC", r.header_crc);
  if (r.isp) {
    const p = r.isp.present;
    row("Base mode", r.isp.base_mode);
    row("Gamma domain", r.domain === "lin" ? "linear" : r.domain);
    row("Blocks rendered", r.use.length ? r.use.map((u) => names[u]).join(", ") : "curves only");
    row("Marked in the file", Object.entries(p).filter(([, on]) => on).map(([k]) => names[k]).join(", ") || "-");
  }
  // the ISP block
  const has = !!r.isp;
  $("i-curves-box").hidden = !has; $("i-matrix").parentElement.parentElement.hidden = !has; $("i-sectors-box").hidden = !has;
  if (has) { drawCurves(r.isp); showMatrix(r.isp); showSectors(r.isp); }
  // META and sections
  const meta = Object.entries(r.meta || {});
  $("i-meta-box").hidden = !meta.length;
  $("i-meta-note").textContent = meta.length ? `${meta.length} fields` : "";
  $("i-meta").replaceChildren(...meta.map(([k, val]) => el("tr", {}, el("th", { text: k }), el("td", { class: "mono", text: val }))));
  $("i-sec-note").textContent = `${r.sections.length}`;
  $("i-sections").replaceChildren(el("tr", {}, ...["section", "offset", "size", "CRC-32", ""].map((t) => el("th", { text: t }))),
    ...r.sections.map((s) => el("tr", {}, el("td", { class: "mono", text: s.tag }), el("td", { class: "num", text: s.offset }),
      el("td", { class: "num", text: s.size }), el("td", { class: "mono", text: s.crc }),
      el("td", { class: s.ok ? "" : "bad", text: s.ok ? "✓" : "✗ CRC" }))));
  updateButtons();
}

function drawCurves(isp) {
  const svg = $("i-curves"), W = 160, H = 100, ns = "http://www.w3.org/2000/svg";
  svg.replaceChildren();
  const path = (pts, color, width, dash) => {
    const p = document.createElementNS(ns, "polyline");
    p.setAttribute("points", pts.map((v, i) => `${(i / (pts.length - 1) * W).toFixed(2)},${(H - v * H).toFixed(2)}`).join(" "));
    p.setAttribute("fill", "none"); p.setAttribute("stroke", color); p.setAttribute("stroke-width", width);
    p.setAttribute("vector-effect", "non-scaling-stroke"); p.setAttribute("stroke-linejoin", "round");
    if (dash) p.setAttribute("stroke-dasharray", dash);
    svg.append(p);
  };
  for (let i = 1; i < 4; i++) {
    path([i / 4, i / 4], "#2c2c30", 1); // horizontal grid
    const l = document.createElementNS(ns, "line");
    l.setAttribute("x1", W * i / 4); l.setAttribute("x2", W * i / 4); l.setAttribute("y1", 0); l.setAttribute("y2", H);
    l.setAttribute("stroke", "#2c2c30"); l.setAttribute("vector-effect", "non-scaling-stroke"); svg.append(l);
  }
  path([0, 1], "#3a3a3f", 1, "3 3");
  ["#e86a5f", "#6cc08b", "#5f9be8"].forEach((c, k) => path(isp.gamma[k], c, 1.2));
  path(isp.tone, "#9a9aa0", 1.2, "4 2");
  path(isp.curve, "#f0c070", 2.2);
  const same = isp.gamma[0].every((v, i) => v === isp.gamma[1][i] && v === isp.gamma[2][i]);
  $("i-curves-note").textContent = same ? "gamma R = G = B" : "gamma per channel";
}

function showMatrix(isp) {
  const t = $("i-matrix"); t.replaceChildren();
  isp.matrix.forEach((r, i) => t.append(el("tr", {}, ...r.map((v, j) => el("td", { class: i === j ? "diag" : "", text: fmt(v, 3), title: `Q7: ${isp.matrix_q7[i][j]}` })))));
  $("i-matrix-note").textContent = isp.matrix_identity ? "identity matrix" : (isp.present.matrix ? "own matrix F" : "not marked - not applied");
  const k = $("i-k"); k.replaceChildren(el("dt", { text: "k", title: "Cb/Cr factor (saturation): 144 = 1.0" }), el("dd", { text: `${isp.k.toFixed(3)}` }),
    el("dt", { text: "sectors" }), el("dd", { text: `${isp.active_sectors} of 12` }));
}

function showSectors(isp) {
  const t = $("i-sectors");
  const head = el("tr", {}, ...["hue", "width", "sat.", "shift°", "luma"].map((x) => el("th", { text: x })));
  const cell = (v, d, suffix = "") => el("td", { class: v === 0 ? "zero" : "", text: v === 0 ? "0" : fmt(v, d) + suffix });
  t.replaceChildren(head, ...isp.sectors.map((s, i) => el("tr", { title: `Sector ${i}: window C ${s.c[0]}…${s.c[0] + s.c[1]}, Y ${s.y[0]}…${s.y[0] + s.y[1]}; editor steps ${s.sat_steps}` },
    el("td", {}, el("span", { class: "sw", style: `background: rgb(${s.rgb.join(",")})` }), `${s.hue}°`),
    el("td", { class: "num", text: `${s.width}°` }), cell(s.sat, 3), cell(s.hue_shift, 1), cell(s.lum, 3))));
  $("i-sectors-note").textContent = isp.active_sectors ? `${isp.active_sectors} active` : "all zero";
}

// ---------------------------------------------------------------- render (coalesced: the last request wins)
async function render() {
  if (!st.image) { status(st.isp ? "Now drop a picture (RAW or JPEG)." : "Drop a picture and an .isp profile"); return; }
  if (st.busy) { st.again = true; return; }
  st.busy = true;
  try {
    do {
      st.again = false;
      const isp = st.ispInfo && st.ispInfo.renderable ? st.isp : null;
      status(isp ? "Rendering the frame through the profile…" : "Showing the original…", "busy");
      const r = await post("/api/render", { image: st.image, isp, ev: st.ev });
      st.imgs = r;
      show();
      if (!isp) status(st.ispInfo ? "The profile cannot be used - the original is shown" : "The original. Now drop an .isp profile", st.ispInfo ? "err" : "");
      else status(`Done: ${r.cached ? "from cache" : r.ms + " ms"}, ${r.backend === "gpu" ? "GPU" : "CPU"}`, "ok");
    } while (st.again);
  } catch (e) { status(e.message, "err"); }
  finally { st.busy = false; }
}

function show() {
  const r = st.imgs;
  if (!r) return;
  $("empty").hidden = true; $("fit").hidden = false; $("caption").hidden = false;
  const after = r.isp || r.plain;
  $("img-view").src = after; $("img-after").src = after; $("img-before").src = r.plain;
  const s = $("strength");
  s.hidden = !r.strength;
  if (r.strength) {
    s.replaceChildren("Profile strength ΔE2000: ", el("b", { text: r.strength.mean.toFixed(2) }), ` mean · ${r.strength.p95.toFixed(2)} p95`);
    s.title = `How far the frame through the profile is from the original (the camera without a look, sRGB): mean ${r.strength.mean}, p95 ${r.strength.p95}, max ${r.strength.max}`;
  }
  const d = $("download");
  d.href = after; d.setAttribute("aria-disabled", String(!r.isp));
  const stem = (x) => (x || "").replace(/\.[^.]+$/, "");
  d.download = `${stem(st.imageInfo?.name)}_${stem(st.ispInfo?.file) || "original"}.png`;
  setTab(st.tab, false);
}

function caption() {
  const c = $("caption"), name = st.ispInfo?.name || "profile";
  const on = !!(st.imgs && st.imgs.isp);
  if (st.tab === "view") {
    c.replaceChildren(el("span", {}, on ? "Through the profile " : "The original ", on ? el("b", { text: name }) : null),
      el("span", { class: "hint small", text: on ? "hold the mouse button on the frame to see the original" : "" }));
  } else {
    c.replaceChildren(el("span", {}, "◂ ", el("b", { text: "Original" })), el("span", { class: "hint small", text: "← → move the split, with Shift by 10 %" }),
      el("span", {}, el("b", { text: on ? name : "no profile" }), " ▸"));
  }
  const f = st.tab === "view" ? $("frame-view") : $("frame-split");
  requestAnimationFrame(() => { c.style.setProperty("--cap-w", f.getBoundingClientRect().width + "px"); });
}

// ---------------------------------------------------------------- tabs
function setTab(tab, focus = false) {
  st.tab = tab;
  for (const b of document.querySelectorAll(".tabs [role=tab]")) {
    const on = b.dataset.tab === tab;
    b.setAttribute("aria-selected", on); b.tabIndex = on ? 0 : -1;
    if (on && focus) b.focus();
  }
  $("frame-view").hidden = tab !== "view"; $("frame-split").hidden = tab !== "split";
  if (st.imgs) caption();
}
for (const b of document.querySelectorAll(".tabs [role=tab]")) {
  b.addEventListener("click", () => setTab(b.dataset.tab));
  b.addEventListener("keydown", (e) => {
    if (e.key === "ArrowRight" || e.key === "ArrowLeft") { e.preventDefault(); setTab(st.tab === "view" ? "split" : "view", true); }
  });
}
new ResizeObserver(() => st.imgs && caption()).observe($("viewport"));

// ISP view: hold the pointer on the picture to see the original
const fv = $("frame-view");
fv.addEventListener("pointerdown", (e) => {
  if (!st.imgs || !st.imgs.isp || e.button !== 0) return;
  fv.setPointerCapture(e.pointerId); $("img-view").src = st.imgs.plain; fv.classList.add("peek");
});
const unpeek = () => { if (st.imgs && fv.classList.contains("peek")) { $("img-view").src = st.imgs.isp || st.imgs.plain; fv.classList.remove("peek"); } };
fv.addEventListener("pointerup", unpeek); fv.addEventListener("pointercancel", unpeek); fv.addEventListener("lostpointercapture", unpeek);

// ---------------------------------------------------------------- the split: the whole frame is the hit area
const fs = $("frame-split"), handle = $("handle");
function setPos(p) {
  st.pos = Math.max(0, Math.min(100, p));
  fs.style.setProperty("--pos", st.pos + "%");
  handle.setAttribute("aria-valuenow", Math.round(st.pos)); handle.setAttribute("aria-valuetext", `${Math.round(st.pos)} %`);
}
const posAt = (x) => { const b = fs.getBoundingClientRect(); return (x - b.left) / b.width * 100; };
let dragging = null;
fs.addEventListener("pointerdown", (e) => {
  if (e.button !== 0) return;
  // a touch starting off the handle keeps the page's vertical scroll (touch-action: pan-y) until it moves sideways
  dragging = { id: e.pointerId, x: e.clientX, y: e.clientY, touch: e.pointerType === "touch" && !e.target.closest(".handle"), on: false };
  if (!dragging.touch) { fs.setPointerCapture(e.pointerId); dragging.on = true; fs.classList.add("drag"); setPos(posAt(e.clientX)); handle.focus({ preventScroll: true }); e.preventDefault(); }
});
fs.addEventListener("pointermove", (e) => {
  if (!dragging || dragging.id !== e.pointerId) return;
  if (!dragging.on && Math.abs(e.clientX - dragging.x) > Math.abs(e.clientY - dragging.y) && Math.abs(e.clientX - dragging.x) > 6) {
    dragging.on = true; fs.setPointerCapture(e.pointerId); fs.classList.add("drag");
  }
  if (dragging.on) setPos(posAt(e.clientX));
});
const endDrag = (e) => {
  if (!dragging || dragging.id !== e.pointerId) return;
  if (dragging.touch && !dragging.on && e.type === "pointerup") setPos(posAt(e.clientX)); // a tap moves the bar there
  dragging = null; fs.classList.remove("drag");
};
fs.addEventListener("pointerup", endDrag); fs.addEventListener("pointercancel", endDrag);
fs.addEventListener("dblclick", () => setPos(50));
handle.addEventListener("keydown", (e) => {
  const step = e.shiftKey ? 10 : 1;
  const k = { ArrowLeft: -step, ArrowDown: -step, ArrowRight: step, ArrowUp: step, PageDown: -10, PageUp: 10 }[e.key];
  if (k !== undefined) { e.preventDefault(); setPos(st.pos + k); }
  else if (e.key === "Home") { e.preventDefault(); setPos(0); }
  else if (e.key === "End") { e.preventDefault(); setPos(100); }
});
setPos(50);

// ---------------------------------------------------------------- exposure
let evTimer = null;
function setEv(v, now = false) {
  st.ev = v; $("ev").value = v;
  $("ev-out").textContent = `${v > 0 ? "+" : v < 0 ? "−" : ""}${Math.abs(v).toFixed(2).replace(/\.?0+$/, "") || "0"} EV`;
  clearTimeout(evTimer);
  evTimer = setTimeout(render, now ? 0 : 120);
}
$("ev").addEventListener("input", () => setEv(parseFloat($("ev").value)));
$("ev0").addEventListener("click", () => setEv(0, true));
$("autoev").addEventListener("click", async () => {
  try {
    status("Choosing the exposure…", "busy");
    const r = await post("/api/autoev", { image: st.image, isp: st.isp });
    setEv(r.ev, true);
  } catch (e) { status(e.message, "err"); }
});
function updateButtons() {
  $("autoev").disabled = !(st.image && st.ispInfo && st.ispInfo.renderable && st.imageInfo.space !== "raw");
}

// ---------------------------------------------------------------- the server: model version and where it renders
async function info() {
  try {
    const r = await (await fetch("/api/version")).json();
    $("version").textContent = `model ${r.version}`;
    if (r.max_image_bytes) st.limits = { image: r.max_image_bytes, isp: r.max_isp_bytes };
    const b = $("backend"); b.textContent = r.backend === "gpu" ? "GPU" : `CPU × ${r.threads}`; b.classList.toggle("gpu", r.backend === "gpu");
  } catch (e) { /* the server is not reachable: the first request will say so */ }
}
info();
