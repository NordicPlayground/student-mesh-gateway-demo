// Copyright (c) 2026 Nordic Semiconductor ASA
//
// SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
const API = "http://localhost:5000"; //Flask backend URL

// Everything known about the mesh itself lives in the Mesh Configuration
// Database (mesh_cdb.js): nodes with their elements, models and group, subnets
// and AppKeys. What is left here is live readings and session state — neither
// belongs in the CDB, which describes how the network is configured, not what
// it is doing right now.
let nonProvCache = [];                 // UUIDs currently beaconing unprovisioned
let provisionedUuids = [];             // UUIDs provisioned this session (suppresses replayed sightings)
let prov_beacon = false;
let selectedDeviceIdx = null;
const deviceStates = new Map();        // element addr -> "on" / "off"
const deviceBrightness = new Map();    // node addr -> 0-100 (%)
const deviceScenes = new Map();        // node addr -> current scene number
const deviceEnergy = new Map();        // node addr -> { kwh: number|null, ts }
let lastProvNetIdx = 0;                // net idx used for the most recent provisioning
let lastProvAppIdx = 0;                // app idx used for the most recent provisioning
let lastProvGroup = "0xC001";          // group used for the most recent provisioning
let autoProvisionActive = false;       // gateway is auto-provisioning every device it sees

// SIG model IDs the gateway can drive (servers) or that mark a dimmer
// (clients). The website renders controls purely from the models a node reports,
// so it is not tied to any fixed set of device types.
const MODEL = {
    RPR_SRV: 0x0004,
    ONOFF_SRV: 0x1000, ONOFF_CLI: 0x1001,
    LEVEL_SRV: 0x1002, LEVEL_CLI: 0x1003,
    SCENE_SRV: 0x1203, SCENE_SETUP_SRV: 0x1204, SCENE_CLI: 0x1205,
    LIGHTNESS_SRV: 0x1300,
    SENSOR_SRV: 0x1100,
};

function getDeviceStatus(addr) {
    return deviceStates.get(Mesh.addrText(addr)) || "off";
}

function setDeviceStatus(addr, status) {
    deviceStates.set(Mesh.addrText(addr), status.toLowerCase() === "on" ? "on" : "off");
}

// Coarse label + CSS class for the node badge, derived from its model set:
// any drivable server -> "fixture", only client models -> "dimmer",
// otherwise unknown.
function deviceLabel(models) {
    const has = (id) => models.includes(id);
    if (has(MODEL.ONOFF_SRV) || has(MODEL.LIGHTNESS_SRV) || has(MODEL.SCENE_SRV)) {
        return { cls: "ctrl", text: "Light fixture" };
    }
    if (has(MODEL.ONOFF_CLI) || has(MODEL.LEVEL_CLI) || has(MODEL.SCENE_CLI)) {
        return { cls: "dimmer", text: "Dimmer" };
    }
    return { cls: "unknown", text: "Unknown" };
}

// Normalize a user-entered group into a "0x...." string (defaults to 0xC001).
function normalizeGroup(text) {
    let t = String(text || "").trim();
    if (!t) return "0xC001";
    if (!t.toLowerCase().startsWith("0x")) {
        if (/^[0-9a-fA-F]{1,4}$/.test(t)) {
            t = "0x" + t;
        } else {
            const dec = Number.parseInt(t, 10);
            if (Number.isNaN(dec)) return "0xC001";
            t = "0x" + dec.toString(16);
        }
    }
    const n = Number.parseInt(t, 16);
    if (Number.isNaN(n)) return "0xC001";
    return "0x" + n.toString(16).toUpperCase();
}

// Group addresses use the Bluetooth Mesh group range 0xC000–0xFEFF; since the
// field is hex text (not <input type="number">) it gets no native spinner,
// so +/- buttons step it manually, clamped to that range. Both group fields
// (provisioning and the node dialog) share this, each naming its own input.
const GROUP_ADDR_MIN = 0xC000;
const GROUP_ADDR_MAX = 0xFEFF;
// The group fixtures publish their state to, and the gateway's own client models
// subscribe to (STATUS_GROUP_ADDR in the firmware). It is the first address of
// the range, so a group picked for control is never this one.
const STATUS_GROUP_ADDR = 0xC000;

function stepGroup(delta, id) {
    const el = document.getElementById(id);
    if (!el || el.disabled) return;
    const n = Number.parseInt(normalizeGroup(el.value), 16);
    const next = Math.min(GROUP_ADDR_MAX, Math.max(GROUP_ADDR_MIN, n + delta));
    el.value = "0x" + next.toString(16).toUpperCase();
}

const ackWaiters = new Map(); // appId -> { resolve, reject }

function waitForAck(appId, timeoutMs = 10000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            ackWaiters.delete(appId);
            reject(new Error(`Timeout waiting for ack: ${appId}`));
        }, timeoutMs);
        ackWaiters.set(appId, { resolve: (data) => { clearTimeout(timer); resolve(data); }, reject });
    });
}

function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

// Update a scan button's visible label without clobbering its inner markup.
function scanLabel(btn, text) {
    const span = btn && btn.querySelector(".scan-label");
    if (span) span.textContent = text;
    else if (btn) btn.textContent = text;
}

// Reflect the real-time (SSE) link state in the top-bar indicator.
function setLive(connected) {
    const dot = document.getElementById("liveDot");
    const txt = document.getElementById("liveStatus");
    if (dot) dot.classList.toggle("active", connected);
    if (txt) txt.textContent = connected ? "Live" : "Offline";
}

function setStatus(type, text) {
    const badge = document.getElementById("statusBadge");
    badge.style.display = "inline-block";
    badge.className = "status-badge " + type;
    badge.textContent = text;

    // The Subnets and AppKeys dialog covers the top bar while it is open, so it
    // carries its own copy of the line (hidden until one is written).
    const mirror = document.getElementById("keysStatus");
    if (mirror) {
        mirror.className = "keys-status status-badge " + type;
        mirror.textContent = text;
    }
}

function setOutput(content) {
    console.log(typeof content === "string" ? content : JSON.stringify(content, null, 2));
}
// In-flight action feedback
function setOutputSpinner(text) {
    console.log(text);
}

function parseOnOffStatusMessage(message) {
    const match = String(message || "").match(/src=(0x[0-9a-fA-F]+)\s+status=(ON|OFF)\b/i);
    if (!match) return null;

    return {
        address: Mesh.addrText(match[1]),
        status: match[2].toLowerCase(),
    };
}

// Flip just this device's power button in place, so an on/off change never
// rebuilds the rest of the card (which was resetting the scene input etc.
// and made every toggle feel laggy).
function updatePowerButtonUI(addr, status) {
    const btn = document.getElementById(`power-${Mesh.addrText(addr)}`);
    if (!btn) return false;
    const isOn = status === "on";
    btn.classList.toggle("is-on", isOn);
    btn.classList.toggle("is-off", !isOn);
    btn.setAttribute("aria-pressed", isOn ? "true" : "false");
    btn.querySelector(".element-button-state").textContent = status.toUpperCase();
    return true;
}

// Update just this device's brightness slider in place, for the same reason:
// a light going on/off emits a Light_status (lvl=) which must not rebuild the
// whole card and wipe the scene input the user typed into.
function updateBrightnessUI(addr, pct) {
    const slider = document.getElementById(`bri-${Mesh.addrText(addr)}`);
    if (!slider) return false;
    slider.value = pct;
    const val = slider.nextElementSibling;
    if (val) val.textContent = pct + "%";
    return true;
}

async function sendMessage(message) {
    try {
        const res = await fetch(API + "/api/send", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ message }),
        });

        return res;
    } catch (err) {
        setStatus("error", "Network Error");
        setOutput(err.message);
        return null;
    }
}

async function sendMessagebtn() {
    const msg = document.getElementById("message").value.trim();
    if (!msg) {
        setOutput("Please enter a message.");
        return;
    }

    const btn = document.getElementById("sendBtn");
    btn.disabled = true;
    setStatus("pending", "Sending…");
    setOutputSpinner("Sending message to board (c2d)…");

    // Prefix with "-r msg" so the gateway echoes the command's output back to the
    // cloud tagged with the "msg" appId
    const res = await sendMessage(`-r msg ${msg}`);
    if (!res) {
        btn.disabled = false;
        return;
    }

    const data = await res.json();

    if (res.ok) {
        setStatus("success", "Message sent");
    } else {
        setStatus("error", `${res.status} Error`);
    }

    setOutput(data);
    btn.disabled = false;
}

let messageCache = "";

// Handle one device (d2c) message.
function processItem(item) {
    if (!item || !item.message) return;
    const appId = item.message.appId;
    const data = item.message.data;
    console.log("Processing message with appId:", appId);

    if (ackWaiters.has(appId)) {
        const waiter = ackWaiters.get(appId);
        ackWaiters.delete(appId);
        waiter.resolve(data);
    }
    if (appId === "UUID") {
        getUnprovisionedDevice(data);
    }
    if (appId === "auto_provision") {
        setStatus("pending", `Auto-provisioning ${data}…`);
    }
    if (appId === "init_complete") {
        document.getElementById("initBtn").style.display = "none";
        setStatus("success", "Mesh Initialized");
    }
    if (appId === "Provisioning") {
        onProvisioningMessage(data);
    }
    if (appId === "Classification") {
        onClassificationMessage(data);
    }
    if (appId === "OnOff_status") {
        const statusUpdate = parseOnOffStatusMessage(data);
        if (statusUpdate) {
            setDeviceStatus(statusUpdate.address, statusUpdate.status);
            if (!updatePowerButtonUI(statusUpdate.address, getDeviceStatus(statusUpdate.address))) {
                renderProvisionedList();
            }
        }
    }
    if (appId === "Light_status") {
        const u = parseSrcValueMessage(data, "lvl");
        if (u) {
            const pct = Math.round((u.value * 100) / 65535);
            deviceBrightness.set(u.address, pct);
            if (!updateBrightnessUI(u.address, pct)) {
                renderProvisionedList();
            }
        }
    }
    if (appId === "Scene_status") {
        const u = parseSrcValueMessage(data, "scene");
        // scene=0 (BT_MESH_SCENE_NONE) means the mesh invalidated the current
        // scene because some bound state (e.g. onoff) changed outside of a
        // scene recall — not a scene being activated, so leave the UI as-is.
        if (u && u.value !== 0) {
            deviceScenes.set(u.address, u.value);
            renderProvisionedList();
        }
    }
    if (appId === "Energy_status") {
        const u = parseEnergyMessage(data);
        if (u) {
            deviceEnergy.set(u.address, { kwh: u.kwh, ts: Date.now() });
            renderProvisionedList();
        }
    }
    if (appId === "cdb") {
        saveCdb(data);
    }
    if (appId === "SubnetAdded") {
        onSubnetAddedMessage(data);
    }
    if (appId === "SubnetRemoved") {
        onSubnetRemovedMessage(data);
    }
    if (appId === "AppKeyRemoved") {
        onAppKeyRemovedMessage(data);
    }
    if (appId === "GroupRemoved") {
        onGroupRemovedMessage(data);
    }
    if (appId === "KeyRefresh") {
        onKeyRefreshMessage(data);
    }
    if (appId === "autoProvisioningToggled") {
        onAutoProvisioningToggled(data);
    }
    if (appId === "response") {
        onScanResponse(data);
    }
    if (appId === "Error") {
        onErrorMessage(data);
    }

    // Append newest at the bottom and scroll down, so the log reads like a shell.
    messageCache += "AppId: " + appId + ", message: " + data + "\n";
    const outEl = document.getElementById("output");
    outEl.textContent = messageCache;
    outEl.scrollTop = outEl.scrollHeight;
}

// Real-time device messages via Server-Sent Events from the Flask backend,
// which subscribes to nRF Cloud over MQTT.
function startMessageStream() {
    const es = new EventSource(API + "/api/stream");
    es.onopen = () => setLive(true);
    es.onmessage = (ev) => {
        setLive(true);
        let item;
        try { item = JSON.parse(ev.data); } catch (e) { return; }
        processItem(item);
    };
    es.onerror = () => {
        // EventSource reconnects automatically; just surface the gap.
        setLive(false);
        setStatus("error", "Live stream disconnected — reconnecting…");
    };
}
startMessageStream();

// Return the website to its uninitialized state
function resetWebsite() {
    const ok = window.confirm(
        "Reset the website?\n\n" +
        "This forgets the saved network (nodes, keys and device keys) held in this " +
        "browser and returns to its uninitialized state. The gateway is not " +
        "touched, so reflash it too if you want a clean start.");
    if (!ok) return;

    Mesh.reset();
    window.location.reload();
}

// script.js loads in <head> (no defer), so defer any DOM setup until the body
// exists. The CDB was restored from localStorage by mesh_cdb.js, so the network,
// key picker and database panels can render straight away.
document.addEventListener("DOMContentLoaded", () => {
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
    initRailResizer();
    initKeysModal();
    initNodeModal();
});

// Drag-to-resize the operations rail (Console/Maintenance) against the main
// column. Width lives in the --rail-w custom property read by the workspace
// grid (style.css), and persists across reloads via localStorage.
function initRailResizer() {
    const workspace = document.querySelector(".workspace");
    const resizer = document.getElementById("railResizer");
    if (!workspace || !resizer) return;

    const MIN_RAIL = 360;
    const MIN_MAIN = 360;
    const EDGE_GAP = 32; // workspace padding + resizer track, approx
    const STORAGE_KEY = "railWidthPx";
    const STEP = 24; // px per keyboard press

    function clampRail(px) {
        const maxRail = Math.max(MIN_RAIL, window.innerWidth - MIN_MAIN - EDGE_GAP);
        return Math.min(Math.max(px, MIN_RAIL), maxRail);
    }

    function currentRailPx() {
        const raw = getComputedStyle(workspace).getPropertyValue("--rail-w").trim();
        const px = Number.parseFloat(raw);
        return Number.isNaN(px) ? 520 : px;
    }

    function setRailWidth(px, persist) {
        const clamped = clampRail(px);
        workspace.style.setProperty("--rail-w", clamped + "px");
        if (persist) {
            try { localStorage.setItem(STORAGE_KEY, String(clamped)); } catch (e) { /* ignore */ }
        }
    }

    // Restore a saved width; otherwise leave the CSS default (520px) in place.
    try {
        const saved = Number.parseInt(localStorage.getItem(STORAGE_KEY), 10);
        if (!Number.isNaN(saved)) setRailWidth(saved, false);
    } catch (e) { /* ignore */ }

    let dragging = false;

    resizer.addEventListener("pointerdown", (e) => {
        dragging = true;
        resizer.setPointerCapture(e.pointerId);
        document.body.classList.add("resizing-rail");
    });

    resizer.addEventListener("pointermove", (e) => {
        if (!dragging) return;
        setRailWidth(window.innerWidth - e.clientX - 16, false);
    });

    function endDrag() {
        if (!dragging) return;
        dragging = false;
        document.body.classList.remove("resizing-rail");
        setRailWidth(currentRailPx(), true);
    }
    resizer.addEventListener("pointerup", endDrag);
    resizer.addEventListener("pointercancel", endDrag);

    // Keyboard support for the ARIA separator role.
    // Matches the drag convention: moving the divider left grows the rail.
    resizer.addEventListener("keydown", (e) => {
        if (e.key === "ArrowLeft") { setRailWidth(currentRailPx() + STEP, true); e.preventDefault(); }
        else if (e.key === "ArrowRight") { setRailWidth(currentRailPx() - STEP, true); e.preventDefault(); }
    });


    // Re-clamp (without changing the user's chosen width where possible) when
    // the viewport shrinks enough that the current width no longer fits.
    window.addEventListener("resize", () => setRailWidth(currentRailPx(), false));
}

async function getUnprovisionedDevice(uuid) {
    const plain = Mesh.uuidPlain(uuid) || String(uuid || "");
    if (nonProvCache.includes(plain) || provisionedUuids.includes(plain)) return;
    // A device already in the CDB is provisioned, however it got there (an
    // earlier session, an imported database) — a stray beacon must not offer it.
    if (Mesh.nodeByUuid(plain)) return;
    nonProvCache.push(plain);
    renderDeviceList();
}

// Drop a UUID from the "Unprovisioned devices" list once it's actually been
// provisioned — by any path (manual selection, a raw console command, or the
// gateway's own auto-provision) — and remember it so a straggling/replayed
// "UUID" sighting for it doesn't add it back.
function removeUnprovisionedDevice(uuid) {
    if (!uuid) return;
    if (!provisionedUuids.includes(uuid)) provisionedUuids.push(uuid);

    const idx = nonProvCache.indexOf(uuid);
    if (idx === -1) return;

    nonProvCache.splice(idx, 1);
    if (selectedDeviceIdx === idx) {
        selectedDeviceIdx = null;
    } else if (selectedDeviceIdx !== null && selectedDeviceIdx > idx) {
        selectedDeviceIdx -= 1; // keep pointing at the same device after the splice
    }
    renderDeviceList();
}

// Undo removeUnprovisionedDevice() when the provisioning it was anticipating
// failed: the device is still unprovisioned, so it has to be selectable again.
function restoreUnprovisionedDevice(uuid) {
    if (!uuid) return;

    const suppressed = provisionedUuids.indexOf(uuid);
    if (suppressed !== -1) provisionedUuids.splice(suppressed, 1);

    if (nonProvCache.includes(uuid)) return;
    nonProvCache.push(uuid);
    renderDeviceList();
}


function recordProvKeys() {
    if (Mesh.appKeyBoundToOtherNet(lastProvAppIdx, lastProvNetIdx) !== null) return;

    Mesh.upsertNetKey(lastProvNetIdx, "");
    Mesh.upsertAppKey(lastProvAppIdx, lastProvNetIdx, "");
}

// "Provisioning" message, emitted by the gateway right after a node is added
//   "Provisioned node 0xADDR (N elements) uuid=<uuid>, classifying via composition data"
function onProvisioningMessage(msg) {
    const s = String(msg || "");

    const m = s.match(/Provisioned node\s+0x([0-9a-fA-F]+)\s*\((\d+)\s*elements?\)\s*uuid=([0-9a-fA-F]+)/i);
    if (!m) return;

    const addr = Number.parseInt(m[1], 16);
    const numElements = Number.parseInt(m[2], 10) || 1;
    const uuid = m[3].toLowerCase();

    // The node joins the CDB here. Its device key only arrives with a
    // "Save Config DB" dump, and its models with the Classification message.
    recordProvKeys();
    Mesh.upsertNode({
        uuid,
        address: addr,
        elementCount: numElements,
        netIdx: lastProvNetIdx,
        appIdx: lastProvAppIdx,
    });
    Mesh.save();

    removeUnprovisionedDevice(uuid);
    setStatus("success",
        `Provisioned ${Mesh.addrText(addr)} (${numElements} el) — classifying…`);
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
}

// "Error" message. A provisioning failure names the device it was for
//   "provisioning failed: <reason> uuid=<uuid>"
// so put that device back in the unprovisioned list
function onErrorMessage(msg) {
    const s = String(msg || "");
    const m = s.match(/^provisioning failed:\s*(.*?)\s*(?:uuid=([0-9a-fA-F]{32}))?$/i);
    if (!m) {
        setStatus("error", s);
        return;
    }

    const reason = m[1] || "unknown reason";
    const uuid = (m[2] || "").toLowerCase();
    if (uuid) restoreUnprovisionedDevice(uuid);
    setStatus("error", `Provisioning failed — ${reason}`);
}

function onSubnetAddedMessage(msg) {
    const m = String(msg || "").match(/0x([0-9a-fA-F]+)\s+subnet=0x([0-9a-fA-F]+)/i);
    if (!m) return;

    const addr = Mesh.addrText(Number.parseInt(m[1], 16));
    const netIdx = Number.parseInt(m[2], 16);

    Mesh.upsertNetKey(netIdx, "");
    Mesh.addNodeNetKey(addr, netIdx, false);
    Mesh.save();

    setStatus("success", `${addr} added to net ${netIdx}`);
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
}

// "SubnetRemoved" message: "0xADDR subnet=0xNNN netkey=removed". The gateway
// only sends it once the node has acknowledged the NetKey Delete, so the key is
// really gone by the time this lands.
function onSubnetRemovedMessage(msg) {
    const m = String(msg || "").match(/0x([0-9a-fA-F]+)\s+subnet=0x([0-9a-fA-F]+)/i);
    if (!m) return;

    const addr = Mesh.addrText(Number.parseInt(m[1], 16));
    const netIdx = Number.parseInt(m[2], 16);

    Mesh.removeNodeNetKey(addr, netIdx);
    Mesh.save();

    setStatus("success", `${addr} removed from net ${netIdx}`);
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
}

// "AppKeyRemoved" message: "0xADDR appkey=0xNNN net=0xNNN appkey=removed".
// The subnet it names is the one the AppKey is bound to; the node keeps it.
function onAppKeyRemovedMessage(msg) {
    const m = String(msg || "").match(/0x([0-9a-fA-F]+)\s+appkey=0x([0-9a-fA-F]+)/i);
    if (!m) return;

    const addr = Mesh.addrText(Number.parseInt(m[1], 16));
    const appIdx = Number.parseInt(m[2], 16);

    Mesh.removeNodeAppKey(addr, appIdx);
    Mesh.save();

    setStatus("success", `${addr} unbound from app ${appIdx}`);
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
}

// "GroupRemoved" message:
// "0xADDR group=0xGGGG unsub=N unpub=N failed=N". The counts are per model: how
// many were unsubscribed from the group and how many had a publication to it
// cleared. A partial result still arrives here — "failed" says how many models
// the node refused — so the mirror drops the group either way and the badge
// carries the failure count.
function onGroupRemovedMessage(msg) {
    const s = String(msg || "");
    const m = s.match(/0x([0-9a-fA-F]+)\s+group=0x([0-9a-fA-F]+)/i);
    if (!m) return;

    const addr = Mesh.addrText(Number.parseInt(m[1], 16));
    const group = Mesh.addrText(Number.parseInt(m[2], 16));
    const failed = Number.parseInt((s.match(/failed=(\d+)/i) || [])[1], 10) || 0;

    Mesh.removeNodeGroup(addr, group);
    Mesh.save();

    if (failed) {
        setStatus("error", `${addr} removed from group ${group} — ${failed} model(s) failed`);
    } else {
        setStatus("success", `${addr} removed from group ${group}`);
    }
    if (nodeModalAddr === addr) fillNodeModalHead();
    renderProvisionedList();
    renderCdbPanel();
}

// "Classification" message: "0xADDR models=ID,ID,... group=0xGGGG cfg=ok|failed|skipped"
// The node reports the SIG models it instantiates; the UI decides what to
// render, and the CDB records them along with the bindings, subscriptions and
// publications the gateway configured for them.
function onClassificationMessage(msg) {
    const s = String(msg || "");
    const m = s.match(/0x([0-9a-fA-F]+)\s+models=([0-9a-fA-F,]*)(?:\s+group=0x([0-9a-fA-F]+))?(?:\s+cfg=(\w+))?/i);
    if (!m) return;

    const addr = Mesh.addrText(Number.parseInt(m[1], 16));
    const models = (m[2] || "")
        .split(",")
        .filter(Boolean)
        .map(x => Number.parseInt(x, 16))
        .filter(n => !Number.isNaN(n));
    const group = m[3] ? Mesh.addrText(Number.parseInt(m[3], 16)) : "";

    // Normally the Provisioning message put the node in the CDB first; a page
    // reloaded mid-provisioning may not have seen it, so make sure it exists
    // (its UUID and device key then arrive with the next Config DB save).
    if (!Mesh.node(addr)) {
        recordProvKeys();
        Mesh.upsertNode({ address: addr, netIdx: lastProvNetIdx, appIdx: lastProvAppIdx });
    }
    Mesh.setNodeComposition(addr, models, Mesh.nodeAppIdx(addr), group || lastProvGroup);
    Mesh.save();

    const cfg = m[4] ? m[4].toLowerCase() : "";
    const label = deviceLabel(models).text;
    if (cfg === "failed") {
        setStatus("error", `${label} ${addr}: configuration failed`);
    } else if (cfg === "skipped") {
        // The gateway could not read the node's composition data, so it applied
        // no configuration at all.
        setStatus("error", `${addr}: could not read its models, not configured`);
    } else if (cfg === "queued") {
        // The gateway has read the models and queued the configuration; the
        // commands run after this message, and a failure only reaches its log.
        const shown = Mesh.nodeGroup(addr);
        setStatus("success", `${label} ${addr} configuration sent${shown ? " (group " + shown + ")" : ""}`);
    } else if (cfg === "ok") {
        const shown = Mesh.nodeGroup(addr);
        setStatus("success", `${label} ${addr} configured${shown ? " (group " + shown + ")" : ""}`);
    }
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
}

// Parses "src=0xADDR <key>=<number>" status lines (e.g. lvl=, scene=).
function parseSrcValueMessage(message, key) {
    const re = new RegExp("src=(0x[0-9a-fA-F]+)\\s+" + key + "=(\\d+)", "i");
    const m = String(message || "").match(re);
    if (!m) return null;
    return { address: Mesh.addrText(m[1]), value: Number.parseInt(m[2], 10) };
}

// Parses the energy status line "src=0xADDR ukwh=<int|unknown>". The firmware
// sends integer micro-kWh (1 kWh = 1e6); the cloud does the scaling. Returns
// kwh as a number, or null when the fixture reported a non-numeric value.
function parseEnergyMessage(message) {
    const m = String(message || "").match(/src=(0x[0-9a-fA-F]+)\s+ukwh=(\d+|unknown)/i);
    if (!m) return null;
    const address = Mesh.addrText(m[1]);
    if (m[2].toLowerCase() === "unknown") {
        return { address, kwh: null };
    }
    return { address, kwh: Number.parseInt(m[2], 10) / 1e6 };
}


// A node's subnets, each with the AppKeys bound to it. The first subnet is the
// one this node's commands are sent on. An AppKey whose subnet is not known yet
// (the Config DB has not been saved since it was created) cannot be placed under
// a subnet, so it is returned apart in `loose`.
function nodeKeyGroups(n) {
    const groups = n.netKeys.map(k => ({ net: k.index, apps: [] }));
    const loose = [];

    n.appKeys.forEach(k => {
        const bound = Mesh.appKey(k.index);
        const group = bound && groups.find(g => g.net === bound.boundNetKey);
        if (group) group.apps.push(k.index);
        else loose.push(k.index);
    });
    return { groups, loose };
}

// "net 1 → app 1, 2 · net 2" — the same grouping as plain text.
function nodeKeysText(n) {
    const { groups, loose } = nodeKeyGroups(n);
    const parts = groups.map(g => `net ${g.net}` + (g.apps.length ? ` → app ${g.apps.join(", ")}` : ""));
    if (loose.length) parts.push(`app ${loose.join(", ")}`);
    return parts.join(" · ") || "—";
}

function renderProvisionedList() {
    const list = document.getElementById("provisionedList");
    if (!list) return;

    const nodes = Mesh.provisionedNodes();

    if (nodes.length === 0) {
        list.innerHTML = '<div class="no-devices">No provisioned devices yet.</div>';
        return;
    }

    list.innerHTML = "";
    list.className = "node-list";

    nodes.forEach((n) => {
        const address = Mesh.addrText(n.unicastAddress);
        const netIdx = Mesh.nodeNetIdx(address);
        const appIdx = Mesh.nodeAppIdx(address);
        const models = Mesh.nodeModels(address);
        const group = Mesh.nodeGroup(address);


        const { groups, loose } = nodeKeyGroups(n);
        const keyChips = groups.map((g, i) => {
            const title = i === 0 ? "Commands for this node are sent on this subnet"
                                  : "Held by the node";
            const apps = g.apps.length ? " → app " + g.apps.join(", ") : "";
            return `<span class="key-group${i === 0 ? " is-active" : ""}" title="${title}">net ${g.net}${apps}</span>`;
        });
        if (loose.length) {
            keyChips.push(`<span class="key-group is-loose" title="Its subnet is not known yet — save the Config DB">app ${loose.join(", ")}</span>`);
        }
        // Every group the node is still subscribed to, the one it publishes to
        // marked. The status group is on every node by default and says nothing
        // about how it was configured, so it is left out.
        const groupChips = Mesh.nodeGroups(address)
            .filter(g => Number.parseInt(g, 16) !== STATUS_GROUP_ADDR)
            .map(g => {
                const isPub = g === group;
                const title = isPub
                    ? "Publishes to this group — what the latest configuration pointed it at"
                    : "Still subscribed from an earlier configuration";
                return `<span class="key-group${isPub ? " is-active" : ""}" title="${title}">grp ${g}</span>`;
            });

        const metaParts = [];
        if (n.UUID) metaParts.push("uuid " + n.UUID);

        const card = document.createElement("div");
        card.className = "node-card";

        const has = (id) => models.includes(id);
        const { cls, text: typeLabel } = deviceLabel(models);
        const modelTitle = models.length
            ? "Models: " + models.map(m => "0x" + m.toString(16)).join(", ")
            : "No composition data yet";
        const header = `
            <div class="element-header">
                <div class="el-top">
                    <span class="el-addr">${address} <span class="type-badge type-${cls}" title="${modelTitle}">${typeLabel}</span></span>
                    <button class="el-cfg" aria-label="Configure ${address}"
                        title="Add a subnet to this node, or configure it with an AppKey"
                        onclick="openNodeModal('${address}')">⚙</button>
                </div>
                <div class="el-keys">${keyChips.join("")}</div>
                ${groupChips.length ? `<div class="el-keys">${groupChips.join("")}</div>` : ""}
                ${metaParts.length ? `<span class="el-meta">${metaParts.join(" · ")}</span>` : ""}
            </div>`;

        // Build the server-control widgets independently, one per server model
        // present, so a node exposing only some of them renders only those.
        const controls = [];
        if (has(MODEL.ONOFF_SRV)) {
            const status = getDeviceStatus(address);
            controls.push(`
                    <button
                        id="power-${address}"
                        class="element-button power-button ${status === "on" ? "is-on" : "is-off"}"
                        aria-pressed="${status === "on" ? "true" : "false"}"
                        onclick="toggleProvisionedLight(${netIdx}, ${appIdx}, '${address}')">
                        <span class="element-button-address">Light</span>
                        <span class="element-button-state">${status.toUpperCase()}</span>
                    </button>`);
        }
        if (has(MODEL.LIGHTNESS_SRV)) {
            const bri = deviceBrightness.has(address) ? deviceBrightness.get(address) : 100;
            controls.push(`
                    <div class="bri-row">
                        <label>Brightness</label>
                        <input id="bri-${address}" type="range" min="0" max="100" value="${bri}"
                            oninput="this.nextElementSibling.textContent = this.value + '%'"
                            onchange="setBrightness(${netIdx}, ${appIdx}, '${address}', this.value)">
                        <span class="bri-val">${bri}%</span>
                    </div>`);
        }
        if (has(MODEL.SCENE_SRV)) {
            const scene = deviceScenes.get(address) || 1;
            controls.push(`
                    <div class="scene-row">
                        <label>Scene</label>
                        <input id="scene-${address}" class="scene-input" type="number" min="1" max="16" value="${scene}">
                        <button class="btn-mini" onclick="recallScene(${netIdx}, ${appIdx}, '${address}')">Recall</button>
                        <button class="btn-mini" onclick="storeScene(${netIdx}, ${appIdx}, '${address}')">Store</button>
                    </div>`);
        }
        if (has(MODEL.SENSOR_SRV)) {
            const e = deviceEnergy.get(address);
            const disp = e ? (e.kwh === null ? "unknown" : e.kwh.toFixed(3) + " kWh") : "— kWh";
            controls.push(`
                    <div class="energy-row">
                        <label>Total device energy usage:</label>
                        <span class="energy-val" id="energy-${address}">${disp}</span>
                        <button class="btn-mini" title="Fetch total energy use from this fixture"
                            onclick="readEnergy(${netIdx}, ${appIdx}, '${address}')">Fetch</button>
                    </div>`);
        }

        let body;
        if (controls.length) {
            // Fixture: render whichever server controls the node actually exposes.
            body = `<div class="fixture-controls">${controls.join("")}</div>`;
        } else if (has(MODEL.ONOFF_CLI) || has(MODEL.LEVEL_CLI) || has(MODEL.SCENE_CLI)) {
            // Dimmer: only client models, nothing for the cloud to drive directly.
            body = `
                <div class="dimmer-note">
                    Dimmer — drives group <strong>${group || "?"}</strong> from its buttons.
                    <div class="dimmer-hint">Button 1: tap = on/off, hold = dim · Button 2: scenes</div>
                </div>`;
        } else {
            // No models the gateway can drive or observe.
            body = `
                <div class="dimmer-note">
                    No controllable models reported.
                    <div class="dimmer-hint">Waiting for composition data…</div>
                </div>`;
        }

        card.innerHTML = header + body;

        // Pressing the card opens its configure dialog. The card's own controls
        // (light, brightness, scene, energy) keep their clicks to themselves;
        // the gear in the header has its own handler and is a button too.
        card.addEventListener("click", (e) => {
            if (e.target.closest("button, input, select, label")) return;
            openNodeModal(address);
        });
        list.appendChild(card);
    });

    // Status messages redraw this list constantly; keep an open dialog's header
    // in step with what it is showing.
    if (nodeModalAddr) fillNodeModalHead();
}

// "Provision selected" only ever makes sense with a device picked, and never
// while the gateway is auto-provisioning everything it sees on its own. The
// net/app/group fields are locked for the same reason: the gateway is already
// using whatever values were sent when auto-provisioning started, so letting
// them change here would desync the displayed config from what's actually
// being applied to incoming devices.
function updateProvisionBtnState() {
    const provBtn = document.getElementById("provisionBtn");
    if (provBtn) provBtn.disabled = autoProvisionActive || selectedDeviceIdx === null;

    for (const id of ["provNetIdx", "provAppIdx", "provGroup", "provGroupUp", "provGroupDown"]) {
        const el = document.getElementById(id);
        if (el) el.disabled = autoProvisionActive;
    }
}

function renderDeviceList() {
    const list = document.getElementById("deviceList");
    if (!list) return;

    if (nonProvCache.length === 0) {
        list.innerHTML = '<div class="no-devices">No unprovisioned devices found. Click Scan to start.</div>';
        selectedDeviceIdx = null;
        updateProvisionBtnState();
        return;
    }

    list.innerHTML = "";

    nonProvCache.forEach((uuid, i) => {
        const el = document.createElement("div");
        el.className = "device-item" + (selectedDeviceIdx === i ? " selected" : "");
        el.innerHTML =
            `<span><span class="device-index">#${i}</span>` +
            `<span class="device-uuid">${uuid}</span></span>`;
        el.onclick = () => selectDevice(i);
        list.appendChild(el);
    });

    updateProvisionBtnState();
}

function selectDevice(idx) {
    selectedDeviceIdx = idx;
    renderDeviceList();
}

let dump = newDump();

function newDump() {
    return {
        nodes: new Set(), nets: new Set(), apps: new Set(),
        want: { nodes: null, nets: null, apps: null },
    };
}

// One line of a `mesh cdb show` dump. Each line is folded into the CDB.
function saveCdb(data) {
    const line = String(data || "").trim();
    if (!line) return;

    // "> Total <kind>: N" — how many lines of that kind this dump holds.
    const total = line.match(/^> Total (nodes|subnets|app-keys): (\d+)$/);
    if (total) {
        const key = { nodes: "nodes", subnets: "nets", "app-keys": "apps" }[total[1]];
        dump.want[key] = Number.parseInt(total[2], 10);
        finishDumpIfComplete();
        return;
    }

    // Banner, headers and separators carry nothing; "Mesh Network Information"
    // included, since it may arrive after the lines it used to introduce.
    if (
        line.startsWith("Mesh Network") ||
        line.startsWith("===") ||
        line.startsWith("---") ||
        line.startsWith("Address") ||
        line.startsWith("NetIdx") ||
        line.startsWith(">")
    ) return;

    const fields = line.split(/\s+/);

    if (fields.length === 5 && /^0x[0-9a-fA-F]+$/i.test(fields[0])) {
        // Node: Address  Elements  Flags  UUID  DevKey. Flags is "C" once the
        // gateway has finished configuring the node.
        const n = Mesh.upsertNode({
            address: fields[0],
            elementCount: Number.parseInt(fields[1], 10),
            configComplete: fields[2].toUpperCase() === "C",
            uuid: fields[3],
            deviceKey: fields[4],
        });
        if (n) dump.nodes.add(n.unicastAddress);
        renderProvisionedList();

    } else if (fields.length === 2 && /^0x[0-9a-fA-F]+$/i.test(fields[0]) && /^[0-9a-fA-F]{32}$/i.test(fields[1])) {
        // Subnet: NetIdx  NetKey
        dump.nets.add(Mesh.upsertNetKey(Number.parseInt(fields[0], 16), fields[1]).index);

    } else if (fields.length === 3 && /^0x[0-9a-fA-F]+$/i.test(fields[0]) && /^0x[0-9a-fA-F]+$/i.test(fields[1]) && /^[0-9a-fA-F]{32}$/i.test(fields[2])) {
        // App-key: NetIdx  AppIdx  AppKey
        dump.apps.add(Mesh.upsertAppKey(
            Number.parseInt(fields[1], 16),
            Number.parseInt(fields[0], 16),
            fields[2]).index);
    }

    Mesh.save();

    // Refresh as each line lands so the key picker and database view fill in as
    // the dump arrives rather than all at once at the end.
    renderKeyRefreshList();
    renderCdbPanel();

    finishDumpIfComplete();
}

// Prune once the dump has everything it said it would: whatever the gateway did
// not list in it is gone from the gateway, so drop it here too.
function finishDumpIfComplete() {
    const w = dump.want;
    if (w.nodes === null || w.nets === null || w.apps === null) return;
    if (dump.nodes.size !== w.nodes || dump.nets.size !== w.nets ||
        dump.apps.size !== w.apps) return;

    Mesh.pruneTo([...dump.nodes], [...dump.nets], [...dump.apps]);
    dump = newDump();
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
}

// Opened from the top bar rather than held open in the rail: creating subnets
// and AppKeys is occasional setup, and the rail is crowded enough.
function openKeysModal() {
    const dlg = document.getElementById("keysModal");
    if (dlg) dlg.showModal();
}

function closeKeysModal() {
    const dlg = document.getElementById("keysModal");
    if (dlg) dlg.close();
}

// Esc comes free with <dialog>; this adds click-outside. The listener is on the
// dialog itself, so a click lands on it only when it missed .modal-inner.
function initKeysModal() {
    const dlg = document.getElementById("keysModal");
    if (!dlg) return;
    dlg.addEventListener("click", (e) => {
        if (!e.target.closest(".modal-inner")) dlg.close();
    });
}

// The node whose configure dialog is open, or null while it is shut. The
// dialog is addressed at one node for as long as it stays open, so the commands
// in it do not need a node picker of their own.
let nodeModalAddr = null;

// Opened by pressing a node card (or the gear in its header).
function openNodeModal(address) {
    const dlg = document.getElementById("nodeModal");
    if (!dlg) return;

    nodeModalAddr = address;

    // Start from the keys the node is on now, so pressing Configure without
    // touching a field re-applies its current pair rather than a stale one.
    const set = (id, value) => {
        const el = document.getElementById(id);
        if (el && value !== "") el.value = value;
    };
    set("nodeNetIdx", Mesh.nodeNetIdx(address));
    set("nodeAppIdx", Mesh.nodeAppIdx(address));
    // Through normalizeGroup so the field reads "0xC001" like the provisioning
    // one, not the mirror's lowercase "0xc001". Left empty for a node with no
    // group, which `set` then skips.
    const group = Mesh.nodeGroup(address);

    set("nodeGroup", group && normalizeGroup(group));

    fillNodeModalHead();
    dlg.showModal();
}

function closeNodeModal() {
    const dlg = document.getElementById("nodeModal");
    if (dlg) dlg.close();
}

// Esc comes free with <dialog>; this adds click-outside. The listener is on the
// dialog itself, so a click lands on it only when it missed .modal-inner. The
// close event covers every way out, including Esc and the backdrop.
function initNodeModal() {
    const dlg = document.getElementById("nodeModal");
    if (!dlg) return;
    dlg.addEventListener("click", (e) => {
        if (!e.target.closest(".modal-inner")) dlg.close();
    });
    dlg.addEventListener("close", () => { nodeModalAddr = null; });
}

// The dialog's header: which node it is pointed at, and the keys it is on.
function fillNodeModalHead() {
    const title = document.getElementById("nodeModalTitle");
    const sub = document.getElementById("nodeModalSub");
    if (!nodeModalAddr || !title || !sub) return;

    const n = Mesh.node(nodeModalAddr);
    const keys = n ? nodeKeysText(n) : "—";
    const group = Mesh.nodeGroup(nodeModalAddr);
    // The groups it kept from earlier configurations, which it still takes
    // commands on even though it no longer publishes to them.
    const others = Mesh.nodeGroups(nodeModalAddr)
        .filter(g => g !== group && Number.parseInt(g, 16) !== STATUS_GROUP_ADDR);

    title.textContent = "Configure " + nodeModalAddr;
    sub.textContent = `Currently on ${keys}` +
                      (group ? ` · publishes to ${group}` : "") +
                      (others.length ? ` · also on ${others.join(", ")}` : "");
}

// The node dialog's fields. Returns null (with the reason on the badge) if the
// dialog somehow lost the node it was opened for.
function nodePanelFields() {
    if (!nodeModalAddr) {
        setStatus("error", "No node selected — open a node card");
        return null;
    }
    return {
        addr: nodeModalAddr,
        netIdx: Number.parseInt(document.getElementById("nodeNetIdx").value, 10) || 0,
        appIdx: Number.parseInt(document.getElementById("nodeAppIdx").value, 10) || 0,
        group: normalizeGroup(document.getElementById("nodeGroup").value),
        viaNet: Mesh.nodeNetIdx(nodeModalAddr),
    };
}

// The Subnets and AppKeys dialog's fields.
function keyPanelFields() {
    return {
        netIdx: Number.parseInt(document.getElementById("keyNetIdx").value, 10) || 0,
        appIdx: Number.parseInt(document.getElementById("keyAppIdx").value, 10) || 0,
    };
}

// Send one command and report whether it reached the cloud. What happens after
// that arrives as a d2c message: "SubnetAdded" for a node's subnet,
// "Classification" for a node's configuration. Creating a subnet or an AppKey
// has no report of its own — a Config DB save is what confirms those.
async function sendKeyStep(cmd, pending) {
    setStatus("pending", pending);
    const res = await sendMessage(cmd);
    if (!res || !res.ok) {
        if (res) setStatus("error", `${res.status} Error — "${cmd}" not sent`);
        return false;
    }
    return true;
}

// Create a subnet on the gateway: a fresh NetKey it can hand out, which nodes
// can then be given and configured on.
async function addSubnet() {
    const f = keyPanelFields();

    if (await sendKeyStep(`gtw subnet ${f.netIdx}`, `Creating net ${f.netIdx}…`)) {
        // Nothing comes back for this one, so record it here; the NetKey itself
        // fills in with the next Config DB save.
        Mesh.upsertNetKey(f.netIdx, "");
        Mesh.save();
        setStatus("success", `Net ${f.netIdx} queued — save the Config DB`);
        renderKeyRefreshList();
        renderCdbPanel();
    }
}

// Create an AppKey on a subnet (and bind the gateway's own client models to it,
// so the gateway can drive nodes over it).
async function addAppKeyToSubnet() {
    const f = keyPanelFields();

    // An AppKey belongs to exactly one subnet. Caught here rather than letting
    // the gateway reject the key-add halfway through the command queue.
    const conflictNet = Mesh.appKeyBoundToOtherNet(f.appIdx, f.netIdx);
    if (conflictNet !== null) {
        setStatus("error", `App IDX ${f.appIdx} already on net ${conflictNet}`);
        return;
    }

    if (await sendKeyStep(`gtw appkey ${f.netIdx} ${f.appIdx}`,
                          `Adding app ${f.appIdx} to net ${f.netIdx}…`)) {
        // No d2c report comes back for this one, so record it here; the AppKey
        // itself fills in with the next Config DB save.
        Mesh.upsertNetKey(f.netIdx, "");
        Mesh.upsertAppKey(f.appIdx, f.netIdx, "");
        Mesh.save();
        setStatus("success", `App ${f.appIdx} queued on net ${f.netIdx} — save the Config DB`);
        renderKeyRefreshList();
        renderCdbPanel();
    }
}

// Give this node another subnet, so it can be configured on that subnet's
// AppKeys. The gateway reports back with "SubnetAdded".
async function addSubnetToNode() {
    const f = nodePanelFields();
    if (!f) return;

    await sendKeyStep(`gtw addsubnet ${f.viaNet} ${f.addr} ${f.netIdx}`,
                      `Adding net ${f.netIdx} to ${f.addr}…`);
}

// Pick which subnet this node's commands are sent on.
function setNodeNet() {
    const f = nodePanelFields();
    if (!f) return;

    if (!Mesh.nodeHasNetKey(f.addr, f.netIdx)) {
        setStatus("error",
                  `${f.addr} is not on net ${f.netIdx} — add that subnet to the node first`);
        return;
    }

    // Index 0 of the node's subnets is the one its commands name.
    Mesh.addNodeNetKey(f.addr, f.netIdx, true);
    Mesh.save();

    // A command names a net and an AppKey together, and the mesh stack drops one
    // whose AppKey is not bound to the net it was sent on.
    const appIdx = Mesh.nodeAppIdx(f.addr);
    const bound = Mesh.appKey(appIdx);
    if (bound && bound.boundNetKey !== f.netIdx) {
        setStatus("error",
                  `${f.addr} now addressed on net ${f.netIdx}, but app ${appIdx} is bound to net ${bound.boundNetKey} — configure it with an AppKey on net ${f.netIdx}`);
    } else {
        setStatus("success", `${f.addr} now addressed over net ${f.netIdx}`);
    }
    fillNodeModalHead();
    renderProvisionedList();
    renderKeyRefreshList();
    renderCdbPanel();
}

// Take a subnet off this node, and with it every AppKey bound to that subnet.
// A node rejects a NetKey Delete for the subnet the message arrived on, so the
// subnet the node is currently addressed over cannot be the one removed. The
// gateway reports back with "SubnetRemoved".
async function removeSubnetFromNode() {
    const f = nodePanelFields();
    if (!f) return;

    if (f.netIdx === f.viaNet) {
        setStatus("error",
                  `${f.addr} is addressed over net ${f.netIdx} — change subnet first, then remove it`);
        return;
    }

    await sendKeyStep(`gtw node_delsubnet ${f.viaNet} ${f.addr} ${f.netIdx}`,
                      `Removing net ${f.netIdx} from ${f.addr}…`);
}

// Take one AppKey off this node, leaving its subnets alone. The gateway reports
// back with "AppKeyRemoved".
async function removeAppKeyFromNode() {
    const f = nodePanelFields();
    if (!f) return;

    await sendKeyStep(`gtw node_delappkey ${f.viaNet} ${f.addr} ${f.appIdx}`,
                      `Removing app ${f.appIdx} from ${f.addr}…`);
}

// Take a group off this node: unsubscribe its models from that group address
// and clear any publication aimed at it, leaving its subnets and AppKeys alone.
// The gateway reports back with "GroupRemoved".
async function removeGroupFromNode() {
    const f = nodePanelFields();
    if (!f) return;

    const group = Number.parseInt(f.group, 16);

    if (!(group >= GROUP_ADDR_MIN && group <= GROUP_ADDR_MAX)) {
        setStatus("error",
                  `${f.group} is not a group address — use 0xC000–0xFEFF`);
        return;
    }

    // The status group is how fixture state reaches the cloud at all, so the
    // gateway refuses to remove it. Say so here rather than round-tripping.
    if (group === STATUS_GROUP_ADDR) {
        setStatus("error",
                  `${f.group} is the status group — removing it would stop ${f.addr} reporting state`);
        return;
    }

    await sendKeyStep(`gtw node_delgroup ${f.viaNet} ${f.addr} ${f.group}`,
                      `Removing group ${f.group} from ${f.addr}…`);
}

// Re-run this node's configuration with <app>, which reads its composition data
// again and binds, subscribes and publishes every model it reports against that
// AppKey and group.
async function configureNodeWithAppKey() {
    const f = nodePanelFields();
    if (!f) return;

    // Check if appkey exists and is bound to a net the node is on.
    const appKey = Mesh.appKey(f.appIdx);
    if (!appKey) {
        setStatus("error",
                  `App IDX ${f.appIdx} does not exist — create it under Subnets and AppKeys`);
        return;
    }

    const netIdx = appKey.boundNetKey;
    if (!Mesh.nodeHasNetKey(f.addr, netIdx)) {
        setStatus("error",
                  `${f.addr} is not on net ${netIdx}, which owns app ${f.appIdx} — add that subnet to the node first`);
        return;
    }

    // The Classification message that comes back carries no net/app, and reads
    // the node's current ones out of the CDB, so move the chosen pair to the
    // front first: index 0 is what the node's controls address it on, and what
    // the new bindings get recorded against.
    Mesh.addNodeNetKey(f.addr, netIdx, true);
    Mesh.addNodeAppKey(f.addr, f.appIdx, true);
    Mesh.save();
    lastProvNetIdx = netIdx;
    lastProvAppIdx = f.appIdx;
    lastProvGroup = f.group;
    renderProvisionedList();
    renderCdbPanel();


    await sendKeyStep(`gtw cfg ${f.viaNet} ${f.addr} ${f.appIdx} ${f.group}`,
                      `Configuring ${f.addr} on net ${netIdx} / app ${f.appIdx}…`);
}

// NetKey / AppKey indices (numbers) currently checked for Key Refresh. Kept in
// Sets so the selection survives the checkbox list being rebuilt every time a
// fresh CDB snapshot arrives. A NetKey may be selected on its own (roll the
// NetKey only); an AppKey always drags its own NetKey in, since an AppKey can
// only roll while its subnet's NetKey refreshes.
const netRefreshSelected = new Set();
const appRefreshSelected = new Set();

// Rebuild the Key Refresh list from the CDB, preserving the current selection.
// AppKeys are grouped under the subnet they are bound to; every subnet is listed
// so its NetKey can be rolled on its own.
function renderKeyRefreshList() {
    const list = document.getElementById("keyRefreshList");
    const btn = document.getElementById("keyRefreshBtn");
    if (!list || !btn) return; // DOM not ready yet

    // net index -> AppKey indices bound to it. Seed from the subnets so a
    // subnet with no AppKeys still appears, then attach the AppKeys.
    const nets = new Map();
    for (const s of Mesh.db.netKeys) {
        if (!nets.has(s.index)) nets.set(s.index, []);
    }
    for (const k of Mesh.db.appKeys) {
        if (!nets.has(k.boundNetKey)) nets.set(k.boundNetKey, []);
        nets.get(k.boundNetKey).push(k.index);
    }

    // Drop any selection whose key is no longer in the CDB (e.g. after a node
    // was replaced), so we never key-refresh an index the gateway doesn't have.
    const presentNets = new Set(nets.keys());
    for (const net of [...netRefreshSelected]) {
        if (!presentNets.has(net)) netRefreshSelected.delete(net);
    }
    const presentApps = new Set([...nets.values()].flat());
    for (const app of [...appRefreshSelected]) {
        if (!presentApps.has(app)) appRefreshSelected.delete(app);
    }

    if (nets.size === 0) {
        list.innerHTML = '<div class="no-devices">Save the Config DB to list keys.</div>';
        btn.disabled = true;
        return;
    }

    list.innerHTML = "";
    for (const net of [...nets.keys()].sort((a, b) => a - b)) {
        const apps = nets.get(net).sort((a, b) => a - b);

        const group = document.createElement("div");
        group.className = "keyrefresh-group";

        // References to this subnet's AppKey checkboxes, so unchecking the
        // NetKey can visibly clear them without a full re-render.
        const appCbs = [];

        // NetKey checkbox (the subnet header).
        const netLabel = document.createElement("label");
        netLabel.className = "keyrefresh-item keyrefresh-net";
        const netCb = document.createElement("input");
        netCb.type = "checkbox";
        netCb.checked = netRefreshSelected.has(net);
        netCb.addEventListener("change", () => {
            if (netCb.checked) {
                netRefreshSelected.add(net);
            } else {
                // An AppKey can't roll without its NetKey, so dropping the
                // NetKey drops all of its AppKeys too.
                netRefreshSelected.delete(net);
                for (const a of apps) appRefreshSelected.delete(a);
                for (const cb of appCbs) cb.checked = false;
            }
        });
        const netSpan = document.createElement("span");
        netSpan.textContent = `Net ${net}`;
        netLabel.appendChild(netCb);
        netLabel.appendChild(netSpan);
        group.appendChild(netLabel);

        // AppKeys bound to this subnet, nested under the NetKey.
        for (const app of apps) {
            const appLabel = document.createElement("label");
            appLabel.className = "keyrefresh-item keyrefresh-app";
            const appCb = document.createElement("input");
            appCb.type = "checkbox";
            appCb.checked = appRefreshSelected.has(app);
            appCb.addEventListener("change", () => {
                if (appCb.checked) {
                    appRefreshSelected.add(app);
                    // Selecting an AppKey selects its NetKey too.
                    netRefreshSelected.add(net);
                    netCb.checked = true;
                } else {
                    appRefreshSelected.delete(app);
                }
            });
            const appSpan = document.createElement("span");
            appSpan.textContent = `App ${app}`;
            appLabel.appendChild(appCb);
            appLabel.appendChild(appSpan);
            group.appendChild(appLabel);
            appCbs.push(appCb);
        }

        list.appendChild(group);
    }
    btn.disabled = false;
}

// Bluetooth Mesh Key Refresh: rolls each selected NetKey, plus any selected
// AppKeys alongside their own subnet's NetKey. Selections may span several
// subnets; the gateway rolls each affected NetKey once. The POST only kicks it
// off; live progress arrives asynchronously as "KeyRefresh" d2c messages handled
// by onKeyRefreshMessage() (one started/complete pair per subnet).
async function keyRefresh() {
    const btn = document.getElementById("keyRefreshBtn");
    const net_idxs = [...netRefreshSelected].sort((a, b) => a - b);
    const app_idxs = [...appRefreshSelected].sort((a, b) => a - b);

    if (net_idxs.length === 0) {
        setStatus("error", "Select at least one NetKey to refresh");
        setOutput("Pick one or more NetKeys (and optionally AppKeys) from the list above. " +
                  "If the list is empty, click 'Save' to load the CDB first.");
        return;
    }

    // The gateway doesn't track which subnets each node holds, so send every
    // selected subnet with the nodes the CDB mirror has on it.
    const subnets = net_idxs.map(net => ({
        net_idx: net,
        addrs: Mesh.provisionedNodes()
            .filter(n => Mesh.nodeHasNetKey(n.unicastAddress, net))
            .map(n => n.unicastAddress),
    }));

    btn.disabled = true;
    setStatus("pending", "Key refresh starting…");
    setOutputSpinner("Starting Bluetooth Mesh Key Refresh…");

    try {
        const res = await fetch(API + "/api/keyrefresh", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ subnets, app_idxs }),
        });
        const data = await res.json();
        setOutput(data);
        setStatus(res.ok ? "pending" : "error",
                  res.ok ? "Key refresh running…" : `${res.status} Error`);
    } catch (err) {
        setStatus("error", "Network Error");
        setOutput(err.message);
    } finally {
        // Re-enable once the command is dispatched (never leave it stuck if the
        // firmware later reports an error); progress is shown via the badge.
        btn.disabled = false;
    }
}

// "KeyRefresh" status lines from the gateway:
//   "started subnet 0x000 nodes=N evict=0x0000"
//   "complete subnet 0x000 nodes=N appkeys=M fails p1=A p2=B p3=C ak=D [evicted]"
// The rolled keys themselves arrive with the next Config DB save.
function onKeyRefreshMessage(msg) {
    const s = String(msg || "");
    if (/^complete\b/i.test(s)) {
        const f = s.match(/p1=(\d+)\s+p2=(\d+)\s+p3=(\d+)(?:\s+ak=(\d+))?/i);
        // ak (AppKey transport failures) is optional in the match so older
        // firmware without AppKey refresh still parses correctly.
        const failed = f && (Number(f[1]) + Number(f[2]) + Number(f[3]) + Number(f[4] || 0) > 0);
        setStatus(failed ? "error" : "success",
                  failed ? "Key refresh completed with failures" : "Key refresh complete");
    } else if (/^started\b/i.test(s)) {
        setStatus("pending", "Key refresh running…");
    } else {
        // Any other KeyRefresh line is a gateway-reported failure/abort (e.g.
        // "key refresh: no subnet…", "already refreshing", "no target nodes",
        // "refusing to evict gateway…"). Clear the pending badge with the reason.
        setStatus("error", "Key refresh failed: " + s.replace(/^key refresh:\s*/i, ""));
    }
}

// Rebuild the whole network on a fresh gateway from the CDB: recreate the
// primary subnet with its saved NetKey (so nodes still out there keep talking to
// us), then every other subnet, every AppKey, and finally every node.
async function replaceNode() {
    const nodes = Mesh.provisionedNodes();
    if (Mesh.db.nodes.length === 0) {
        setOutput("No CDB data loaded yet. Click 'Save' to fetch the configuration from the board.");
        return;
    }

    const nextAddress = Mesh.nextFreeAddress();

    // Rebuild the primary subnet (net 0) through replace, using its saved
    // NetKey so nodes still on the network keep talking to the gateway.
    const primary = Mesh.netKey(0) || Mesh.db.netKeys[0];
    await sendMessage("gtw replace " + (primary ? primary.key : "") + " " + nextAddress);
    await waitForAck("reProvisioned");

    // Recreate every additional subnet with its exact saved NetKey, so the
    // AppKeys and nodes below land on subnets that match the old network.
    for (const subnet of Mesh.db.netKeys) {
        if (subnet.index === 0) continue; // primary already recreated by replace
        await sendMessage(`gtw subnet ${subnet.index} ${subnet.key}`);
    }

    for (const appkey of Mesh.db.appKeys) {
        await sendMessage(`gtw appkey ${appkey.boundNetKey} ${appkey.index} ${appkey.key}`);
    }

    for (const node of nodes) {
        const address = Mesh.addrText(node.unicastAddress);
        const uuid = Mesh.uuidPlain(node.UUID);
        if (!uuid || !node.deviceKey) {
            // Without both we cannot re-add it; a Config DB save fills them in.
            setStatus("error", `Skipped ${address} — no UUID/device key in the CDB`);
            continue;
        }
        await sendMessage(`gtw node ${uuid} ${node.deviceKey} ${address} ` +
                          `${node.elements.length} ${Mesh.nodeNetIdx(address)}`);
        await waitForAck("node");
        if (Mesh.nodeModels(address).includes(MODEL.RPR_SRV)) {
            await sendMessage(`gtw rpr-add ${address}`);
        }
    }
}

async function provisionSelected() {
    if (selectedDeviceIdx === null) return;

    const net_idx = Number.parseInt(document.getElementById("provNetIdx").value, 10) || 0;
    const app_idx = Number.parseInt(document.getElementById("provAppIdx").value, 10) || 0;
    const group = normalizeGroup(document.getElementById("provGroup")?.value);

    // A Bluetooth Mesh AppKey belongs to exactly one subnet. If this app_idx is
    // already known on a different subnet, block it here (the gateway rejects it
    // too) instead of silently provisioning onto the wrong subnet.
    const conflictNet = Mesh.appKeyBoundToOtherNet(app_idx, net_idx);
    if (conflictNet !== null) {
        setStatus("error", `App IDX ${app_idx} already on net ${conflictNet}`);
        setOutput(`Cannot provision: app_idx ${app_idx} is already bound to subnet ${conflictNet}. ` +
                  `An AppKey belongs to a single subnet — pick a different App IDX, or use net ${conflictNet}.`);
        return;
    }

    // Remember net/app/group so the Provisioning and Classification messages can
    // be attributed to the right subnet/key when the node appears
    lastProvNetIdx = net_idx;
    lastProvAppIdx = app_idx;
    lastProvGroup = group;

    const uuid = nonProvCache[selectedDeviceIdx];
    document.getElementById("provisionBtn").disabled = true;
    setStatus("pending", `Provisioning into net ${net_idx}, group ${group}…`);
    const res = await sendMessage(`gtw provandconfig ${uuid} ${net_idx} ${app_idx} ${group}`);

    // The command never left for the cloud, so no provisioning was attempted and
    // no failure report is coming — keep the device selectable.
    if (!res || !res.ok) {
        if (res) setStatus("error", `${res.status} Error — provisioning not sent`);
        updateProvisionBtnState();
        return;
    }

    removeUnprovisionedDevice(uuid);
}

// Toggle the gateway's own auto-provisioning: while active, it provisions
// every unprovisioned device it sees (locally or via an RPR relay) onto the
// net/app/group configured here, without any per-device selection.
async function toggleAutoProvision() {
    const enable = !autoProvisionActive;

    const net_idx = Number.parseInt(document.getElementById("provNetIdx").value, 10) || 0;
    const app_idx = Number.parseInt(document.getElementById("provAppIdx").value, 10) || 0;
    const group = normalizeGroup(document.getElementById("provGroup")?.value);

    if (enable) {
        // Same AppKey-belongs-to-one-subnet check as provisionSelected().
        const conflictNet = Mesh.appKeyBoundToOtherNet(app_idx, net_idx);
        if (conflictNet !== null) {
            setStatus("error", `App IDX ${app_idx} already on net ${conflictNet}`);
            setOutput(`Cannot enable auto-provision: app_idx ${app_idx} is already bound to subnet ${conflictNet}. ` +
                      `An AppKey belongs to a single subnet — pick a different App IDX, or use net ${conflictNet}.`);
            return;
        }
        lastProvNetIdx = net_idx;
        lastProvAppIdx = app_idx;
        lastProvGroup = group;
    }

    await sendMessage(`gtw auto-provision ${enable ? 1 : 0} ${net_idx} ${app_idx} ${group}`);
}

// The confirmation that auto-provisioning changed state.
function onAutoProvisioningToggled(msg) {
    autoProvisionActive = /\bon\b/i.test(String(msg || ""));

    const btn = document.getElementById("autoProvisionBtn");
    if (btn) {
        btn.classList.toggle("active", autoProvisionActive);
        scanLabel(btn, autoProvisionActive ? "Stop auto-provision" : "Auto-provision");
    }
    setStatus(autoProvisionActive ? "pending" : "success",
              autoProvisionActive
                  ? `Auto-provisioning onto net ${lastProvNetIdx}, group ${lastProvGroup}…`
                  : "Auto-provisioning stopped");

    if (autoProvisionActive) {
        // Clear the discovered-device list now that the gateway is actually
        // auto-provisioning whatever it sees.
        nonProvCache = [];
    }
    renderDeviceList(); // also updates provNetIdx/provAppIdx/provGroup/provisionBtn lock state
}

async function toggleScan() {
    await sendMessage("gtw scan");
}

// Remote-only scan
let rprScanActive = false;

async function toggleScanRpr() {
    await sendMessage("gtw scan-rpr");
}

// The confirmation that scanning changed state.
function onScanResponse(msg) {
    const s = String(msg || "");
    const scanBtn = document.getElementById("scanBtn");
    const rprBtn = document.getElementById("scanRprBtn");

    if (s === "scan started") {
        prov_beacon = true;
        // A normal scan re-enables local beacons, so any RPR-only scan is no
        // longer the active mode — reset that button to keep the UI honest.
        rprScanActive = false;
        scanLabel(rprBtn, "RPR scan");
        if (rprBtn) rprBtn.classList.remove("scanning");

        scanLabel(scanBtn, "Stop");
        if (scanBtn) scanBtn.classList.add("scanning");
        setStatus("pending", "Scanning…");
    } else if (s === "scan stopped") {
        prov_beacon = false;
        scanLabel(scanBtn, "Scan");
        if (scanBtn) scanBtn.classList.remove("scanning");
        setStatus("success", "Scan stopped");
    } else if (s.startsWith("rpr-only scan started")) {
        rprScanActive = true;
        // Firmware forces local beacon scanning off in RPR-only mode
        prov_beacon = false;
        scanLabel(scanBtn, "Scan");
        if (scanBtn) scanBtn.classList.remove("scanning");

        // Clear the discovered-device list
        nonProvCache = [];
        renderDeviceList();

        scanLabel(rprBtn, "Stop RPR");
        if (rprBtn) rprBtn.classList.add("scanning");
        setStatus(s.includes("NO RPR servers") ? "error" : "pending",
                  s.includes("NO RPR servers") ? s : "RPR-only scan (via relays)…");
    } else if (s === "rpr-only scan stopped") {
        rprScanActive = false;
        scanLabel(rprBtn, "RPR scan");
        if (rprBtn) rprBtn.classList.remove("scanning");
        setStatus("success", "RPR scan stopped");
    }
}

async function toggle_light(net_idx, app_idx, addr, on) {
    if (on) {
        await sendMessage(`gtw light ${net_idx} ${app_idx} ${addr} 1`);
    } else {
        await sendMessage(`gtw light ${net_idx} ${app_idx} ${addr} 0`);
    }
}

async function toggleProvisionedLight(netIdx, appIdx, addr) {
    const nextOn = getDeviceStatus(addr) !== "on";
    await toggle_light(netIdx, appIdx, Mesh.addrNum(addr), nextOn);
}

// Set absolute brightness (0-100%) on a light_ctrl fixture.
async function setBrightness(netIdx, appIdx, addr, pct) {
    const p = Math.max(0, Math.min(100, Number.parseInt(pct, 10) || 0));
    deviceBrightness.set(Mesh.addrText(addr), p);
    await sendMessage(`gtw bri ${netIdx} ${appIdx} ${Mesh.addrNum(addr)} ${p}`);
}

// Recall the scene number entered for this fixture.
async function recallScene(netIdx, appIdx, addr) {
    const input = document.getElementById(`scene-${Mesh.addrText(addr)}`);
    const scene = input ? (Number.parseInt(input.value, 10) || 1) : 1;
    deviceScenes.set(Mesh.addrText(addr), scene);
    await sendMessage(`gtw scene recall ${netIdx} ${appIdx} ${Mesh.addrNum(addr)} ${scene}`);
}

// Store the current fixture state under the scene number entered.
async function storeScene(netIdx, appIdx, addr) {
    const input = document.getElementById(`scene-${Mesh.addrText(addr)}`);
    const scene = input ? (Number.parseInt(input.value, 10) || 1) : 1;
    await sendMessage(`gtw scene store ${netIdx} ${appIdx} ${Mesh.addrNum(addr)} ${scene}`);
}

// Ask a fixture's Sensor Server for its total energy use. The reply arrives
// asynchronously as an "Energy_status" d2c message handled in processItem.
async function readEnergy(netIdx, appIdx, addr) {
    await sendMessage(`gtw energy ${netIdx} ${appIdx} ${Mesh.addrNum(addr)}`);
}

// The Config DB panel shows what `mesh cdb show` reports — the nodes, the
// subnets and the AppKeys — rendered back out of the database in the gateway's
// own layout, so it reads the same as the dump it came from.
function renderCdbPanel() {
    const box = document.getElementById("cdbOutput");
    if (!box) return;

    if (Mesh.isEmpty()) {
        box.textContent = "No Config DB stored yet.";
        return;
    }

    // The gateway prints key indices as 3 hex digits and all hex in lower case.
    const keyIdx = (i) => "0x" + i.toString(16).padStart(3, "0");
    const lines = ["Address  Elements  Flags  UUID                              DevKey"];

    for (const n of Mesh.db.nodes) {
        lines.push([
            Mesh.addrText(n.unicastAddress).padEnd(7),
            String(n.elements.length).padEnd(8),
            (n.configComplete ? "C" : "-").padEnd(5),
            Mesh.uuidPlain(n.UUID).padEnd(32),
            n.deviceKey.toLowerCase(),
        ].join("  "));
    }

    lines.push("---", "NetIdx  NetKey");
    for (const k of Mesh.db.netKeys) {
        lines.push(`${keyIdx(k.index)}   ${k.key.toLowerCase()}`);
    }

    lines.push("---", "NetIdx  AppIdx  AppKey");
    for (const k of Mesh.db.appKeys) {
        lines.push(`${keyIdx(k.boundNetKey)}   ${keyIdx(k.index)}   ${k.key.toLowerCase()}`);
    }

    box.textContent = lines.join("\n");
}
