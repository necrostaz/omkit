"use strict";
// ISP fitter page: a .cube look and a picture; the fit runs on the server (server.py) and the page polls its progress;
// four frames or a split; the cassette and the profile's name; the .isp download. Plain script, no build step.
const $ = (id) => document.getElementById(id);
const st = {
  look: null, image: null, imageInfo: null, imageFile: null, fit: null, ev: 0, tab: "grid", pair: "look-isp", pos: 50,
  imgs: null, limits: { image: 200 * 1024 * 1024, cube: 64 * 1024 * 1024 }, gens: [], casOk: false,
};
const seq = { look: 0, image: 0, fit: 0, cas: 0 };     // the latest request of a kind wins

// ---------------------------------------------------------------- helpers
function status(text, cls = "") { $("status").className = "status " + cls; $("status-text").textContent = text; }

async function post(path, body, name, extra = {}) {
  const opt = { method: "POST" };
  if (name !== undefined) { opt.body = body; opt.headers = { "X-Name": encodeURIComponent(name), ...extra }; }
  else { opt.body = JSON.stringify(body); opt.headers = { "Content-Type": "application/json" }; }
  const r = await fetch(path, opt);
  let j;
  try { j = await r.json(); } catch (e) { throw new Error(`${path}: ${r.status} ${r.statusText}`); }
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}
async function get(path) {
  const r = await fetch(path);
  const j = await r.json();
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}
function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v; else if (k === "text") e.textContent = v; else e.setAttribute(k, v);
  }
  for (const c of kids) if (c != null) e.append(c);
  return e;
}
function fill(sel, options, value) {
  sel.replaceChildren(...Object.entries(options).map(([k, v]) => el("option", { value: k, text: v })));
  sel.value = value;
}
const utf8 = new TextEncoder();
const nbytes = (s) => utf8.encode(s).length;
function cut31(s) { let out = ""; for (const ch of s) { if (nbytes(out + ch) > 31) break; out += ch; } return out; }
const isCube = (name) => /\.cube$/i.test(name);
function tooBig(file, limit) {
  if (file.size <= limit) return false;
  status(`${file.name}: ${(file.size / 1048576).toFixed(1)} MB, larger than the ${Math.round(limit / 1048576)} MB the fitter takes`, "err");
  return true;
}
function cells(prefix, s) { for (const k of ["mean", "p95", "max"]) $(`s-${prefix}-${k}`).textContent = s ? s[k].toFixed(2) : ""; }

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
setupDrop("drop-look", "file-look", onLook);
setupDrop("drop-image", "file-image", onImage);

let veilDepth = 0;
function veil(on) { $("dropveil").classList.toggle("on", on); if (!on) veilDepth = 0; }
document.addEventListener("dragenter", (e) => { if (e.dataTransfer.types.includes("Files")) { veilDepth++; veil(true); } });
document.addEventListener("dragleave", () => { if (--veilDepth <= 0) veil(false); });
document.addEventListener("dragover", (e) => e.preventDefault());
document.addEventListener("drop", async (e) => {
  e.preventDefault(); veil(false);
  const files = [...e.dataTransfer.files];
  const look = files.find((f) => isCube(f.name)), img = files.find((f) => !isCube(f.name));
  if (img) await onImage(img);
  if (look) await onLook(look);
});

// ---------------------------------------------------------------- the look
async function onLook(file) {
  if (!isCube(file.name)) { status(`${file.name}: a look is a .cube file (a 3D LUT)`, "err"); return; }
  if (tooBig(file, st.limits.cube)) return;
  const my = ++seq.look;
  try {
    status(`Reading ${file.name}…`, "busy");
    const r = await post("/api/look", await file.arrayBuffer(), file.name);
    if (my !== seq.look) return;
    st.look = r; st.fit = null;
    const n = $("look-name"); n.hidden = false;
    n.textContent = `${r.file} · ${r.size}³` + (r.title && r.title !== r.name ? ` · "${r.title}"` : "");
    $("drop-look").classList.add("loaded"); $("step-look").classList.add("done");
    fill($("enc-in"), r.options.enc_in, r.enc_in);
    fill($("enc-out"), r.options.enc_out, r.enc_out);
    $("enc-box").hidden = false;
    $("enc-note").hidden = false;
    $("enc-note").textContent = "Guessed from the file's text (its title and comments). A .cube does not say how it is " +
      "encoded: if the LUT's description says otherwise, choose it here.";
    $("isp-name").value = cut31(r.name);
    await cassetteDefaults(r.name);
    await doFit();
  } catch (e) { if (my === seq.look) status(e.message, "err"); }
}
for (const id of ["enc-in", "enc-out"]) $(id).addEventListener("change", doFit);

// ---------------------------------------------------------------- the picture
async function onImage(file, space = "auto") {
  if (tooBig(file, st.limits.image)) return;
  const my = ++seq.image;
  try {
    status(`Loading ${file.name}…`, "busy");
    const r = await post("/api/image", await file.arrayBuffer(), file.name, { "X-Space": space });
    if (my !== seq.image) return;
    st.image = r.id; st.imageInfo = r; st.imageFile = file;
    const n = $("image-name"); n.hidden = false; n.textContent = `${r.name} · ${r.w}×${r.h}`;
    $("drop-image").classList.add("loaded"); $("step-image").classList.add("done");
    const sel = $("space");
    $("space-box").hidden = r.space === "raw";
    if (sel.options.length < 2) for (const [k, v] of Object.entries(r.spaces)) sel.append(el("option", { value: k, text: v }));
    if (space === "auto") sel.value = "auto";
    const how = $("image-how"); how.hidden = false;
    how.textContent = `${r.space_label}: ${r.how}; median luminance ${(r.median_y * 100).toFixed(1)} %`;
    $("frame-split").style.setProperty("--ar", r.w / r.h);
    updateButtons();
    if (st.fit) await render();
    else status(st.look ? "Fitting…" : "Now drop a .cube look.", st.look ? "busy" : "");
  } catch (e) { if (my === seq.image) status(e.message, "err"); }
}
$("space").addEventListener("change", () => st.imageFile && onImage(st.imageFile, $("space").value));

// ---------------------------------------------------------------- the fit
const recipeDesc = {};
const recipesReady = get("/api/recipes").then((r) => {
  for (const x of r.recipes) { $("variant").append(el("option", { value: x.id, text: x.label })); recipeDesc[x.id] = x.desc; }
  $("variant").value = r.default; $("matrix").checked = r.matrix;
  $("variant-desc").textContent = recipeDesc[r.default];
}).catch((e) => status("Recipes: " + e.message, "err"));
$("variant").addEventListener("change", () => { $("variant-desc").textContent = recipeDesc[$("variant").value] || ""; doFit(); });
for (const id of ["matrix", "quick"]) $(id).addEventListener("change", doFit);

function progress(p) {
  const box = $("progress");
  if (!p) { box.hidden = true; return; }
  box.hidden = false;
  const stage = Math.max(1, p.stage || 1), frac = Math.min(1, ((stage - 1) + (p.iter || 0) / (p.iters || 40)) / (p.stages || 1));
  $("progress-bar").style.width = `${Math.round(frac * 100)}%`;
  const text = `Fitting: stage ${Math.min(stage, p.stages)} of ${p.stages}, iteration ${p.iter || 0} · ${Math.round(p.seconds || 0)} s`;
  $("progress-text").textContent = text;
  status(text + (p.mono ? " (black and white)" : ""), "busy");
}

async function doFit() {
  if (!st.look) return;
  await recipesReady;
  const my = ++seq.fit;
  const req = { look: st.look.id, variant: $("variant").value, matrix: $("matrix").checked, quick: $("quick").checked,
                enc_in: $("enc-in").value, enc_out: $("enc-out").value };
  st.fit = null; $("download-isp").disabled = true; $("step-fit").classList.remove("done");
  try {
    status("Fitting the camera's tables to the look…", "busy");
    let r = await post("/api/fit", req);
    while (r.state === "running") {
      if (my !== seq.fit) return;                 // another look or option was asked for meanwhile
      if (r.job) st.job = r.job;
      progress(r.stage !== undefined ? r : { stage: 0, stages: 1, iter: 0, iters: 40, seconds: 0 });
      await new Promise((ok) => setTimeout(ok, 400));
      r = await get("/api/job?id=" + st.job);
    }
    if (my !== seq.fit) return;
    progress(null);
    if (r.state === "error") throw new Error(r.error);
    applyFit(r.result);
  } catch (e) { if (my === seq.fit) { progress(null); status(e.message, "err"); } }
}

function applyFit(r) {
  st.fit = r;
  $("step-fit").classList.add("done");
  cells("c", r.colorchecker); cells("g", r.cube);
  const m = $("mono-note");
  m.hidden = !r.mono;
  if (r.mono === "neutral") m.textContent = "Black-and-white look: every recipe fits one stage - one weighted sum of " +
    "the channels (with the matrix, like a film's colour sensitivity), one gamma curve for all three, saturation 0.";
  else if (r.mono === "toned") m.textContent = "Toned black-and-white look: every recipe fits one stage - one weighted " +
    "sum of the channels (with the matrix), the tint from the R, G, B gamma curves, no colour sectors.";
  $("isp-card").hidden = false;
  updateCassette();
  updateButtons();
  if (st.image) render();
  else status(`Fitted${r.cached ? " (kept from before)" : ""}. Drop a picture to see the result.`, "ok");
}

// ---------------------------------------------------------------- render (coalesced: the last request wins)
const R = { busy: false, again: false };
async function render() {
  if (!st.fit || !st.image) return;
  if (R.busy) { R.again = true; return; }
  R.busy = true;
  try {
    do {
      R.again = false;
      status("Rendering…", "busy");
      const r = await post("/api/render", { fit: st.fit.fit, image: st.image, ev: st.ev });
      st.imgs = r;
      cells("i", r.image_de); cells("l", r.look_strength);
      show();
      const f = st.fit;
      status(`${f.look} · ${f.recipe}${f.matrix ? " + matrix" : ""}${f.quick ? " · quick" : ""}${f.cached ? " · kept from before" : ""}`, "ok");
    } while (R.again);
  } catch (e) {
    status(e.message, "err");
    if (/fit again/.test(e.message)) doFit();
  } finally { R.busy = false; }
}

const PAIRS = { "look-isp": ["look", "isp", "look", "ISP copy"], "plain-isp": ["plain", "isp", "original", "ISP copy"],
                "plain-look": ["plain", "look", "original", "look"] };
function show() {
  const r = st.imgs;
  if (!r) return;
  $("empty").hidden = true;
  $("img-plain").src = r.plain; $("img-look").src = r.look; $("img-isp").src = r.isp; $("img-heat").src = r.heat;
  const [a, b, la, lb] = PAIRS[st.pair];
  $("sw-left").src = r[a]; $("sw-right").src = r[b]; $("sw-la").textContent = la; $("sw-lb").textContent = lb;
  setTab(st.tab);
}

// ---------------------------------------------------------------- tabs and the split
function setTab(tab, focus = false) {
  st.tab = tab;
  for (const b of document.querySelectorAll(".tabs [role=tab]")) {
    const on = b.dataset.tab === tab;
    b.setAttribute("aria-selected", on); b.tabIndex = on ? 0 : -1;
    if (on && focus) b.focus();
  }
  const has = !!st.imgs;
  $("grid").hidden = !has || tab !== "grid";
  $("split").hidden = !has || tab !== "split";
  $("split-hint").hidden = !has || tab !== "split";
  $("pairs").hidden = tab !== "split";
  setPos(st.pos);
}
for (const b of document.querySelectorAll(".tabs [role=tab]")) {
  b.addEventListener("click", () => setTab(b.dataset.tab));
  b.addEventListener("keydown", (e) => {
    if (e.key === "ArrowRight" || e.key === "ArrowLeft") { e.preventDefault(); setTab(st.tab === "grid" ? "split" : "grid", true); }
  });
}
function setPair(pair) {
  st.pair = pair;
  for (const b of $("pairs").children) b.setAttribute("aria-checked", b.dataset.pair === pair);
  show();
}
$("pairs").addEventListener("click", (e) => { const b = e.target.closest("[data-pair]"); if (b) setPair(b.dataset.pair); });

const fs = $("frame-split"), handle = $("handle");
function setPos(p) {
  st.pos = Math.max(0, Math.min(100, p));
  fs.style.setProperty("--pos", st.pos + "%");
  handle.setAttribute("aria-valuenow", Math.round(st.pos));
  handle.setAttribute("aria-valuetext", `${Math.round(st.pos)} %: ${$("sw-la").textContent} on the left, ${$("sw-lb").textContent} on the right`);
  $("sw-la").classList.toggle("gone", st.pos < 12); $("sw-lb").classList.toggle("gone", st.pos > 88);
}
const posAt = (x) => { const b = fs.getBoundingClientRect(); return (x - b.left) / b.width * 100; };
let dragging = null;
fs.addEventListener("pointerdown", (e) => {
  if (e.button !== 0) return;
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
  if (dragging.touch && !dragging.on && e.type === "pointerup") setPos(posAt(e.clientX));
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
  evTimer = setTimeout(render, now ? 0 : 150);
}
$("ev").addEventListener("input", () => setEv(parseFloat($("ev").value)));
$("ev0").addEventListener("click", () => setEv(0, true));
$("autoev").addEventListener("click", async () => {
  try {
    status("Choosing the exposure…", "busy");
    const r = await post("/api/autoev", { fit: st.fit.fit, image: st.image });
    setEv(r.ev, true);
  } catch (e) { status(e.message, "err"); }
});
function updateButtons() {
  $("autoev").disabled = !(st.image && st.fit && st.imageInfo.space !== "raw");
}

// ---------------------------------------------------------------- the cassette and the profile's name
const CAS = { nameTouched: false, timer: null, url: null };
async function cassetteDefaults(lookName) {
  const r = await get("/api/cassettes?look=" + encodeURIComponent(lookName));
  st.gens = r.generics;
  const sel = $("cas-gen");
  if (sel.options.length !== r.generics.length) {
    const group = (k) => k.startsWith("p_") ? "Film pictures (no text of their own)" : /^(instant|mini|wetplate)/.test(k) ? "Instant film and old process" : "With a line of their kind";
    sel.replaceChildren();
    let og = null;
    for (const g of r.generics) {
      const name = group(g.key);
      if (!og || og.label !== name) { og = el("optgroup", { label: name }); sel.append(og); }
      og.append(el("option", { value: g.id, text: `${g.id}. ${g.label}` }));
    }
  }
  sel.value = r.default;
  $("cas-iso").value = r.iso;
  $("cas-brand").value = ""; $("cas-proc").value = "";
  CAS.nameTouched = false;
  $("cas-name").value = $("isp-name").value;
  placeholders();
}
function placeholders() {
  const g = st.gens[+$("cas-gen").value] || {};
  $("cas-brand").placeholder = g.brand ? `the picture's: ${g.brand}` : "none";
  $("cas-proc").placeholder = g.proc ? `the picture's: ${g.proc}` : "none";
}
function casReq() {
  return { gen: +$("cas-gen").value, name: $("cas-name").value, iso: $("cas-iso").value, brand: $("cas-brand").value,
           proc: $("cas-proc").value };
}
function updateCassette() { clearTimeout(CAS.timer); CAS.timer = setTimeout(cassettePreview, 150); nameCheck(); }
async function cassettePreview() {
  if ($("cas-gen").value === "") return;
  const my = ++seq.cas;
  try {
    const r = await post("/api/cassette", casReq());
    if (my !== seq.cas) return;
    if (r.png) $("cas-img").src = r.png;
    $("cas-img").classList.toggle("stale", !r.png);
    const m = $("cas-msgs");
    m.replaceChildren(...r.errors.map((t) => el("span", { class: "err", text: t })), ...r.warnings.map((t) => el("span", { class: "warn", text: t })));
    st.casOk = !r.errors.length;
  } catch (e) { st.casOk = false; $("cas-msgs").replaceChildren(el("span", { class: "err", text: e.message })); }
  updateDownload();
}
function nameCheck() {
  const v = $("isp-name").value, n = nbytes(v), note = $("isp-name-note");
  const errs = [], warns = [];
  if (!v.trim()) errs.push("the name is empty");
  if (n > 31) errs.push(`${n} bytes: at most 31 (a letter outside ASCII takes 2 or more)`);
  if ([...v].some((c) => c.charCodeAt(0) < 32 || c.charCodeAt(0) > 126)) warns.push("characters outside ASCII: the camera's font may not show them");
  note.textContent = `${n} / 31 bytes` + (errs.length ? ` - ${errs.join("; ")}` : warns.length ? ` - ${warns.join("; ")}` : "");
  note.classList.toggle("bad", !!errs.length);
  $("isp-name").setAttribute("aria-invalid", String(!!errs.length));
  st.nameOk = !errs.length;
  updateDownload();
}
function updateDownload() { $("download-isp").disabled = !(st.fit && st.casOk && st.nameOk); }

$("isp-name").addEventListener("input", () => { if (!CAS.nameTouched) $("cas-name").value = $("isp-name").value; updateCassette(); });
$("cas-name").addEventListener("input", () => { CAS.nameTouched = $("cas-name").value !== ""; updateCassette(); });
$("cas-gen").addEventListener("change", () => { placeholders(); updateCassette(); });
for (const id of ["cas-iso", "cas-brand", "cas-proc"]) $(id).addEventListener("input", updateCassette);

$("download-isp").addEventListener("click", async () => {
  try {
    const r = await fetch("/api/isp", { method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ fit: st.fit.fit, name: $("isp-name").value, cassette: casReq() }) });
    if (!r.ok) throw new Error((await r.json()).error || r.statusText);
    const m = /filename="([^"]+)"/.exec(r.headers.get("Content-Disposition") || "");
    const url = URL.createObjectURL(await r.blob());
    const a = el("a", { href: url, download: m ? m[1] : "profile.isp" });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
    status(`Saved ${m ? m[1] : "profile.isp"}: copy it to OMKIT\\FILMLOOK\\ on the card.`, "ok");
  } catch (e) { status("Download: " + e.message, "err"); }
});

// ---------------------------------------------------------------- the server: where the fit runs, the prior
(async () => {
  try {
    const r = await get("/api/version");
    st.limits = { image: r.max_image_bytes, cube: r.max_cube_bytes };
    const b = $("backend"); b.textContent = r.backend === "gpu" ? "GPU" : `CPU`; b.classList.toggle("gpu", r.backend === "gpu");
    $("prior").textContent = `prior: ${r.prior}`;
  } catch (e) { /* the first request will say so */ }
})();
nameCheck();
