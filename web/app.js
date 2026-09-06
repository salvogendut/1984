"use strict";

// KeyboardEvent.code to SDL_Scancode (SDL_scancode.h).
const CODE2SCAN = {
  KeyA:4, KeyB:5, KeyC:6, KeyD:7, KeyE:8, KeyF:9, KeyG:10, KeyH:11, KeyI:12,
  KeyJ:13, KeyK:14, KeyL:15, KeyM:16, KeyN:17, KeyO:18, KeyP:19, KeyQ:20,
  KeyR:21, KeyS:22, KeyT:23, KeyU:24, KeyV:25, KeyW:26, KeyX:27, KeyY:28, KeyZ:29,
  Digit1:30, Digit2:31, Digit3:32, Digit4:33, Digit5:34, Digit6:35, Digit7:36,
  Digit8:37, Digit9:38, Digit0:39,
  Enter:40, Escape:41, Backspace:42, Tab:43, Space:44,
  Minus:45, Equal:46, BracketLeft:47, BracketRight:48, Backslash:49,
  Semicolon:51, Quote:52, Backquote:53, Comma:54, Period:55, Slash:56,
  Delete:76, ArrowRight:79, ArrowLeft:80, ArrowDown:81, ArrowUp:82,
  NumpadEnter:88,
  ControlLeft:224, ShiftLeft:225, AltLeft:226,
  ControlRight:228, ShiftRight:229, AltRight:230,
};

const $ = id => document.getElementById(id);
const canvas = $("screen");
const screenFrame = $("screenFrame");
const statusEl = $("status");
const toastEl = $("toast");
const ledPowerEl = $("ledPower");
const ledAEl = $("ledA");
const ledBEl = $("ledB");
const ledInputEl = $("ledInput");
const ledAudioEl = $("ledAudio");
const ctx = canvas.getContext("2d");
const W = 768;
const H = 272;
const VW = 768;
const VH = 576;

const offscreen = document.createElement("canvas");
offscreen.width = W;
offscreen.height = H;
const offctx = offscreen.getContext("2d");
const image = offctx.createImageData(W, H);
let pixelSharp = true;
let monochromeGreen = false;
let scanlineStrength = 15;
let scanlineCustomized = false;
const crtTone = { brightness: 100, contrast: 100, red: 100, green: 100, blue: 100 };
const crtRedLut = new Uint8Array(256);
const crtGreenLut = new Uint8Array(256);
const crtBlueLut = new Uint8Array(256);
let toastTimer = 0;
let inputLedTimer = 0;

// ---- expansion bay (M4 board + internet relay) ----
const expansionButtonEl = $("expansion");
const expansionPanelEl = $("expansionPanel");
const expansionBackdropEl = $("expansionBackdrop");
const expansionCloseEl = $("expansionClose");
const m4ToggleEl = $("m4Toggle");
const m4StateEl = $("m4State");
const m4SdSlotEl = $("m4SdSlot");
const m4SdLedEl = $("m4SdLed");
const m4SdNameEl = $("m4SdName");
const m4SdFileEl = $("m4SdFile");
const m4SdEjectEl = $("m4SdEject");
const m4NetToggleEl = $("m4NetToggle");
const m4NetStateEl = $("m4NetState");
const m4EndpointEl = $("m4Endpoint");
const m4RelayStateEl = $("m4RelayState");
const m4RelayStateWrapEl = m4RelayStateEl.closest(".relay-state");
const m4RelayLampEl = $("m4RelayLamp");
const m4CertificateEl = $("m4Certificate");
const powergraphToggleEl = $("powergraphToggle");
const powergraphStateEl = $("powergraphState");
const powergraphOutputEl = $("powergraphOutput");
const m4Api = globalThis.JS1984M4 || null;
const m4Bridge = globalThis.JS1984M4Bridge || null;

const M4_STORAGE_KEY = "javascript1984.expansion.m4";
const M4_NET_STORAGE_KEY = "javascript1984.expansion.m4Net";
const M4_ENDPOINT_STORAGE_KEY = "javascript1984.expansion.m4Endpoint";
const POWERGRAPH_STORAGE_KEY = "javascript1984.expansion.powergraph";
const POWERGRAPH_OUTPUT_STORAGE_KEY = "javascript1984.expansion.powergraphOutput";
let m4Enabled = false;
let m4NetEnabled = false;
let m4Endpoint = m4Bridge ? m4Bridge.endpoint : "";
let m4CertificateUrl = "";
let m4SdImage = null;
let applyM4Hardware = () => {};
let powergraphEnabled = false;
let powergraphOutput = "auto";
let applyPowergraphHardware = () => {};
let applyPowergraphOutputHardware = () => {};

// ---- expansion bay: ROM slots ----
const romBoardToggleEl = $("romBoardToggle");
const romBoardStateEl = $("romBoardState");
const romLowerNameEl = $("romLowerName");
const romSlotListEl = $("romSlotList");
const romfileEl = $("romfile");
const ROM_BOARD_STORAGE_KEY = "javascript1984.expansion.romBoard";
let romBoardEnabled = false;
let romSlots = new Map();   // slot -> { name, path }
let pendingRomSlot = -1;
let applyRomBoard = () => {};
try {
  romBoardEnabled = localStorage.getItem(ROM_BOARD_STORAGE_KEY) === "true";
} catch (_) {
  // Keep the ROM board disabled when storage is unavailable.
}

function updateRomBoardUi() {
  romBoardToggleEl.checked = romBoardEnabled;
  romBoardStateEl.textContent = romBoardEnabled ? "Enabled" : "Disabled";
  updateExpansionIndicator();
}

function setRomBoardEnabled(enabled, persist = true, announce = false) {
  romBoardEnabled = Boolean(enabled);
  applyRomBoard(romBoardEnabled);
  updateRomBoardUi();
  if (persist) {
    try { localStorage.setItem(ROM_BOARD_STORAGE_KEY, String(romBoardEnabled)); } catch (_) {}
  }
  if (announce) showToast("ROM board " + (romBoardEnabled ? "fitted" : "removed"));
}

try {
  m4Enabled = localStorage.getItem(M4_STORAGE_KEY) === "true";
  m4NetEnabled = localStorage.getItem(M4_NET_STORAGE_KEY) === "true";
  const storedEndpoint = localStorage.getItem(M4_ENDPOINT_STORAGE_KEY);
  if (storedEndpoint !== null) m4Endpoint = storedEndpoint;
  powergraphEnabled = localStorage.getItem(POWERGRAPH_STORAGE_KEY) === "true";
  const storedPowergraphOutput = localStorage.getItem(POWERGRAPH_OUTPUT_STORAGE_KEY);
  if (["auto", "cpc", "v9990"].includes(storedPowergraphOutput))
    powergraphOutput = storedPowergraphOutput;
} catch (_) {
  // Keep the default M4 settings when storage is unavailable.
}

function updateExpansionIndicator() {
  expansionButtonEl.classList.toggle(
    "has-expansion", m4Enabled || m4NetEnabled || powergraphEnabled || romBoardEnabled);
}

function updatePowergraphUi() {
  powergraphToggleEl.checked = powergraphEnabled;
  powergraphStateEl.textContent = powergraphEnabled ? "Enabled" : "Disabled";
  powergraphOutputEl.disabled = !powergraphEnabled;
  powergraphOutputEl.value = powergraphOutput;
  updateExpansionIndicator();
}

function setPowergraphEnabled(enabled, persist = true, announce = false) {
  powergraphEnabled = Boolean(enabled);
  applyPowergraphHardware(powergraphEnabled);
  updatePowergraphUi();
  if (persist) {
    try { localStorage.setItem(POWERGRAPH_STORAGE_KEY, String(powergraphEnabled)); } catch (_) {}
  }
  if (announce)
    showToast("PowerGraph V9990 " + (powergraphEnabled ? "enabled" : "disabled"));
}

function setPowergraphOutput(output, persist = true, announce = false) {
  if (!["auto", "cpc", "v9990"].includes(output)) output = "auto";
  powergraphOutput = output;
  applyPowergraphOutputHardware(powergraphOutput);
  updatePowergraphUi();
  if (persist) {
    try { localStorage.setItem(POWERGRAPH_OUTPUT_STORAGE_KEY, powergraphOutput); } catch (_) {}
  }
  if (announce) showToast("PowerGraph output: " + powergraphOutput.toUpperCase());
}

function updateM4Ui() {
  m4ToggleEl.checked = m4Enabled;
  m4StateEl.textContent = m4Enabled ? "Enabled" : "Disabled";
  m4SdSlotEl.setAttribute("aria-disabled", String(!m4Enabled));
  m4SdFileEl.disabled = !m4Enabled;
  m4SdEjectEl.disabled = !m4Enabled || !m4SdImage;
  updateExpansionIndicator();
}

function setM4Enabled(enabled, persist = true, announce = false) {
  m4Enabled = Boolean(enabled);
  applyM4Hardware(m4Enabled);
  updateM4Ui();
  if (persist) {
    try { localStorage.setItem(M4_STORAGE_KEY, String(m4Enabled)); } catch (_) {}
  }
  if (announce) showToast("M4 expansion " + (m4Enabled ? "enabled" : "disabled"));
}

function updateM4NetUi() {
  m4NetToggleEl.checked = m4NetEnabled;
  m4NetStateEl.textContent = m4NetEnabled ? "Enabled" : "Disabled";
  updateExpansionIndicator();
}

function setM4NetEnabled(enabled, persist = true, announce = false) {
  m4NetEnabled = Boolean(enabled);
  if (m4Bridge) m4Bridge.setDevice(m4NetEnabled);
  updateM4NetUi();
  if (persist) {
    try { localStorage.setItem(M4_NET_STORAGE_KEY, String(m4NetEnabled)); } catch (_) {}
  }
  if (announce) showToast("M4 internet " + (m4NetEnabled ? "enabled" : "disabled"));
}

function updateM4RelayStatus(status, detail = "") {
  const labels = {
    disabled: "Relay disabled",
    offline: "Relay offline",
    connecting: "Connecting to relay",
    online: "Relay online",
    error: "Relay error",
  };
  m4RelayStateWrapEl.dataset.state = status;
  let text = labels[status] || "Relay offline";
  if (status === "offline" && detail)
    text += " \u2014 " + detail;
  m4RelayStateEl.textContent = text;
  m4RelayStateEl.title = detail;
}

function applyM4Endpoint(value, persist = true) {
  if (!m4Bridge || !m4Api) return false;
  m4Endpoint = value.trim();
  const accepted = m4Bridge.setEndpoint(m4Endpoint);
  m4EndpointEl.setAttribute("aria-invalid", String(!accepted));
  m4CertificateUrl = "";
  if (accepted) {
    try {
      const healthUrl = m4Api.relayHealthEndpoint(m4Endpoint);
      if (healthUrl.startsWith("https:")) m4CertificateUrl = healthUrl;
    } catch (_) {
      // Non-secure relays have no certificate step.
    }
  }
  m4CertificateEl.disabled = !m4CertificateUrl;
  if (persist) {
    try { localStorage.setItem(M4_ENDPOINT_STORAGE_KEY, m4Endpoint); } catch (_) {}
  }
  return accepted;
}

function expansionFocusableElements() {
  return [...expansionPanelEl.querySelectorAll(
    'button:not([disabled]), input:not([disabled]), select:not([disabled]), [tabindex]:not([tabindex="-1"])'
  )];
}

function setExpansionPanelOpen(open, restoreFocus = true) {
  const isOpen = Boolean(open);
  document.body.classList.toggle("expansion-open", isOpen);
  expansionButtonEl.setAttribute("aria-expanded", String(isOpen));
  expansionPanelEl.setAttribute("aria-hidden", String(!isOpen));
  expansionPanelEl.inert = !isOpen;
  if (isOpen) {
    setThemeMenu(false);
    requestAnimationFrame(() => expansionCloseEl.focus());
  } else if (restoreFocus) {
    expansionButtonEl.focus();
  }
}

function setStatus(message) {
  statusEl.textContent = message;
}

function showToast(message) {
  toastEl.textContent = message;
  toastEl.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toastEl.classList.remove("show"), 3200);
}

const THEMES = {
  "retro-crt": "Retro CRT",
  "sapporo": "Sapporo",
  "sapporo-dark": "Sapporo Dark",
  "cpc464": "CPC464",
};
const THEME_STORAGE_KEY = "javascript1984.theme";
const themePickerEl = document.querySelector(".theme-picker");
const themeButtonEl = $("themeButton");
const themeMenuEl = $("themeMenu");
const themeNameEl = $("themeName");

function resolveTheme(theme) {
  if (typeof theme !== "string") return null;
  const normalized = theme.trim().toLowerCase();
  for (const [id, label] of Object.entries(THEMES)) {
    if (normalized === id.toLowerCase() || normalized === label.toLowerCase())
      return id;
  }
  return null;
}

function setThemeMenu(open) {
  themeMenuEl.hidden = !open;
  themeButtonEl.setAttribute("aria-expanded", String(open));
}

function applyTheme(theme, persist = true) {
  const selected = resolveTheme(theme) || "cpc464";
  document.documentElement.dataset.theme = selected;
  themeNameEl.textContent = THEMES[selected];
  for (const option of themeMenuEl.querySelectorAll("[data-theme]"))
    option.setAttribute("aria-checked", String(option.dataset.theme === selected));
  if (persist) {
    try {
      localStorage.setItem(THEME_STORAGE_KEY, selected);
    } catch (_) {
      // Storage can be unavailable in privacy-restricted browser contexts.
    }
  }
}

let savedTheme = "cpc464";
try {
  savedTheme = localStorage.getItem(THEME_STORAGE_KEY) || "cpc464";
} catch (_) {
  // Keep the CPC464 theme when storage access is unavailable.
}
const requestedTheme = new URLSearchParams(window.location.search).get("theme");
applyTheme(requestedTheme || savedTheme, false);

themeButtonEl.addEventListener("click", event => {
  event.stopPropagation();
  setThemeMenu(themeMenuEl.hidden);
});
themeMenuEl.addEventListener("click", event => {
  const option = event.target.closest("[data-theme]");
  if (!option) return;
  applyTheme(option.dataset.theme);
  if (!scanlineCustomized)
    setScanlineStrength(themeScanlineDefault(), false);
  setThemeMenu(false);
  themeButtonEl.focus();
  showToast(THEMES[option.dataset.theme] + " theme selected");
});
themePickerEl.addEventListener("keydown", event => {
  if (event.key === "Escape") {
    setThemeMenu(false);
    themeButtonEl.focus();
  }
});
document.addEventListener("click", event => {
  if (!themePickerEl.contains(event.target)) setThemeMenu(false);
});

const cpcKeyboardEl = document.querySelector(".cpc464-keyboard");
const cpcKeyboardKeysEl = $("cpcKeyboardKeys");
const cpcKeyboardToggleEl = $("cpcKeyboardToggle");
const cpcTapeDeckEl = $("tapeDeckMock");

function setCpcKeyboardOpen(open) {
  cpcKeyboardEl.dataset.keyboardOpen = String(open);
  cpcKeyboardKeysEl.hidden = !open;
  cpcTapeDeckEl.hidden = !open;
  cpcKeyboardToggleEl.setAttribute("aria-expanded", String(open));
  cpcKeyboardToggleEl.textContent = open ? "Hide keyboard" : "Show keyboard";
}

cpcKeyboardToggleEl.addEventListener("click", () => {
  setCpcKeyboardOpen(cpcKeyboardKeysEl.hidden);
});
setCpcKeyboardOpen(false);

const mlMonitorEl = $("mlMonitor");
const mlMonitorPanelEl = $("mlMonitorPanel");
const mlMonitorToggleEl = $("mlMonitorToggle");

// Anchor the debugger to the monitor chassis instead of the browser edge.
$("screenStage").append(mlMonitorEl);

function setMlMonitorOpen(open) {
  mlMonitorEl.dataset.open = String(open);
  mlMonitorPanelEl.hidden = !open;
  mlMonitorToggleEl.setAttribute("aria-expanded", String(open));
}

mlMonitorToggleEl.addEventListener("click", () => {
  setMlMonitorOpen(mlMonitorPanelEl.hidden);
});
mlMonitorPanelEl.addEventListener("keydown", event => {
  if (event.key === "Escape") {
    setMlMonitorOpen(false);
    mlMonitorToggleEl.focus();
  }
});
setMlMonitorOpen(false);

function pulseInputLed() {
  ledInputEl.classList.add("on");
  clearTimeout(inputLedTimer);
  inputLedTimer = setTimeout(() => ledInputEl.classList.remove("on"), 120);
}

function setScreenScale(value) {
  const scale = Number(value);
  document.documentElement.style.setProperty("--screen-scale", String(scale / 100));
  $("screenScale").value = String(scale);
  $("scaleValue").textContent = scale + "%";
  const rotation = -115 + ((scale - 70) / 30) * 230;
  $("sizeNeedle").style.transform = "rotate(" + rotation + "deg)";
}

function updatePixelMode() {
  pixelSharp = $("pixelToggle").checked;
  canvas.style.imageRendering = pixelSharp ? "pixelated" : "auto";
  updateScreenModeReadout();
}

function updateScreenModeReadout() {
  $("screenMode").textContent = "768 x 576 / " +
    (pixelSharp ? "Sharp" : "Smooth") + " / " +
    (monochromeGreen ? "Green" : "Color") + " / Scan " +
    scanlineStrength + "%";
}

const SCANLINE_STORAGE_KEY = "javascript1984.scanlines";
const scanlineOverlayEl = document.querySelector(".scanline-overlay");
function themeScanlineDefault() {
  scanlineOverlayEl.style.removeProperty("opacity");
  const opacity = Number.parseFloat(getComputedStyle(scanlineOverlayEl).opacity);
  return Number.isFinite(opacity) ? Math.round(opacity * 20) * 5 : 15;
}

function setScanlineStrength(value, persist = true) {
  const numeric = Number(value);
  scanlineStrength = Number.isFinite(numeric)
    ? Math.max(0, Math.min(95, Math.round(numeric / 5) * 5))
    : 15;
  $("scanlineStrength").value = String(scanlineStrength);
  $("scanlineValue").textContent = scanlineStrength + "%";
  scanlineOverlayEl.style.opacity = String(scanlineStrength / 100);
  const rotation = -115 + (scanlineStrength / 95) * 230;
  $("scanlineNeedle").style.transform = "rotate(" + rotation + "deg)";
  updateScreenModeReadout();
  if (persist) {
    scanlineCustomized = true;
    try {
      localStorage.setItem(SCANLINE_STORAGE_KEY, String(scanlineStrength));
    } catch (_) {
      // Keep the in-memory selection when storage is unavailable.
    }
  }
}

const CRT_TONE_STORAGE_PREFIX = "javascript1984.crt.";
const CRT_TONE_CONTROLS = {
  brightness: { input: "crtBrightness", value: "brightnessValue", needle: "brightnessNeedle", min: 50, max: 100 },
  contrast: { input: "crtContrast", value: "contrastValue", needle: "contrastNeedle", min: 50, max: 150 },
  red: { input: "crtRed", value: "redValue", needle: "redNeedle", min: 50, max: 150 },
  green: { input: "crtGreen", value: "greenValue", needle: "greenNeedle", min: 50, max: 150 },
  blue: { input: "crtBlue", value: "blueValue", needle: "blueNeedle", min: 50, max: 150 },
};

function adjustedComponent(component, gain) {
  let value = 128 + Math.trunc(((component - 128) * crtTone.contrast + 50) / 100);
  value = Math.trunc((value * crtTone.brightness + 50) / 100);
  value = Math.trunc((value * gain + 50) / 100);
  return Math.max(0, Math.min(255, value));
}

function rebuildCrtToneLuts() {
  for (let component = 0; component < 256; component++) {
    crtRedLut[component] = adjustedComponent(component, crtTone.red);
    crtGreenLut[component] = adjustedComponent(component, crtTone.green);
    crtBlueLut[component] = adjustedComponent(component, crtTone.blue);
  }
}

function setCrtTone(name, value, persist = true) {
  const control = CRT_TONE_CONTROLS[name];
  const numeric = Number(value);
  const setting = Number.isFinite(numeric)
    ? Math.max(control.min, Math.min(control.max, Math.round(numeric / 5) * 5))
    : 100;
  crtTone[name] = setting;
  $(control.input).value = String(setting);
  $(control.value).textContent = setting + "%";
  const rotation = -115 + ((setting - control.min) / (control.max - control.min)) * 230;
  $(control.needle).style.transform = "rotate(" + rotation + "deg)";
  rebuildCrtToneLuts();
  if (persist) {
    try {
      localStorage.setItem(CRT_TONE_STORAGE_PREFIX + name, String(setting));
    } catch (_) {
      // Keep the in-memory selection when storage is unavailable.
    }
  }
}

const DISPLAY_MODE_STORAGE_KEY = "javascript1984.displayMode";
const colorModeEl = $("colorMode");
function setDisplayColorMode(green, persist = true) {
  monochromeGreen = green;
  colorModeEl.setAttribute("aria-pressed", String(green));
  colorModeEl.classList.toggle("active", green);
  $("colorModeName").textContent = green ? "Green monochrome" : "Color display";
  colorModeEl.querySelector("small").textContent = green
    ? "Switch to full color"
    : "Switch to green monochrome";
  updateScreenModeReadout();
  if (persist) {
    try {
      localStorage.setItem(DISPLAY_MODE_STORAGE_KEY, green ? "green" : "color");
    } catch (_) {
      // Keep the in-memory selection when storage is unavailable.
    }
  }
}

$("screenScale").addEventListener("input", event => setScreenScale(event.target.value));
$("scanlineStrength").addEventListener("input", event => {
  setScanlineStrength(event.target.value);
});
for (const [name, control] of Object.entries(CRT_TONE_CONTROLS)) {
  $(control.input).addEventListener("input", event => {
    setCrtTone(name, event.target.value);
  });
}
for (const input of document.querySelectorAll(".display-dial-knob input[type=range]")) {
  input.addEventListener("wheel", event => {
    event.preventDefault();
    const step = Number(input.step) || 1;
    input.value = String(Number(input.value) + (event.deltaY < 0 ? step : -step));
    input.dispatchEvent(new Event("input", { bubbles: true }));
  }, { passive: false });
}
$("fitScreen").addEventListener("click", () => {
  setScreenScale(100);
  showToast("Display fitted to the receiver");
});
$("pixelToggle").addEventListener("change", updatePixelMode);
colorModeEl.addEventListener("click", () => {
  setDisplayColorMode(!monochromeGreen);
  showToast(monochromeGreen ? "Green monochrome display enabled" : "Color display restored");
});
$("fullscreen").addEventListener("click", async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await screenFrame.requestFullscreen();
  } catch (error) {
    setStatus("Fullscreen unavailable: " + error.message);
  }
});
expansionButtonEl.addEventListener("click", () => {
  setExpansionPanelOpen(
    expansionButtonEl.getAttribute("aria-expanded") !== "true"
  );
});
expansionCloseEl.addEventListener("click", () => setExpansionPanelOpen(false));
expansionBackdropEl.addEventListener("click", () => setExpansionPanelOpen(false));
expansionPanelEl.addEventListener("keydown", event => {
  if (event.key !== "Tab") return;
  const focusable = expansionFocusableElements();
  if (!focusable.length) return;
  const first = focusable[0];
  const last = focusable[focusable.length - 1];
  if (event.shiftKey && document.activeElement === first) {
    event.preventDefault();
    last.focus();
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault();
    first.focus();
  }
});
document.addEventListener("keydown", event => {
  if (event.key === "Escape" &&
      expansionButtonEl.getAttribute("aria-expanded") === "true") {
    event.preventDefault();
    setExpansionPanelOpen(false);
  }
});
m4ToggleEl.addEventListener("change", () => {
  setM4Enabled(m4ToggleEl.checked, true, true);
});
m4NetToggleEl.addEventListener("change", () => {
  setM4NetEnabled(m4NetToggleEl.checked, true, true);
});
powergraphToggleEl.addEventListener("change", () => {
  setPowergraphEnabled(powergraphToggleEl.checked, true, true);
});
powergraphOutputEl.addEventListener("change", () => {
  setPowergraphOutput(powergraphOutputEl.value, true, true);
});
romBoardToggleEl.addEventListener("change", () => {
  setRomBoardEnabled(romBoardToggleEl.checked, true, true);
});
m4EndpointEl.addEventListener("change", () => {
  if (!applyM4Endpoint(m4EndpointEl.value, true))
    showToast("Invalid M4 relay endpoint");
});
m4CertificateEl.addEventListener("click", () => {
  if (!m4CertificateUrl) {
    showToast("Enter a valid secure WSS relay endpoint first");
    return;
  }
  window.open(m4CertificateUrl, "_blank", "noopener,noreferrer");
  showToast("Approve the relay certificate, then return; reconnection is automatic");
});
try {
  const queryEndpoint = new URLSearchParams(window.location.search).get("m4Relay");
  if (queryEndpoint !== null) m4Endpoint = queryEndpoint;
} catch (_) {}
m4EndpointEl.value = m4Endpoint;
applyM4Endpoint(m4Endpoint, false);
if (m4Bridge) m4Bridge.onStatus(updateM4RelayStatus);
setExpansionPanelOpen(false, false);
updateM4Ui();
updateM4NetUi();
updatePowergraphUi();
updateRomBoardUi();
setScreenScale(100);
let savedScanlineStrength = null;
try {
  const stored = localStorage.getItem(SCANLINE_STORAGE_KEY);
  if (stored !== null) {
    savedScanlineStrength = Number(stored);
    scanlineCustomized = true;
  }
} catch (_) {
  // Keep the default scanline visibility when storage is unavailable.
}
setScanlineStrength(
  savedScanlineStrength === null ? themeScanlineDefault() : savedScanlineStrength,
  false
);
for (const name of Object.keys(CRT_TONE_CONTROLS)) {
  let saved = 100;
  try {
    saved = Number(localStorage.getItem(CRT_TONE_STORAGE_PREFIX + name) || 100);
  } catch (_) {
    // Keep the neutral adjustment when storage is unavailable.
  }
  setCrtTone(name, saved, false);
}
updatePixelMode();
let savedDisplayMode = "color";
try {
  savedDisplayMode = localStorage.getItem(DISPLAY_MODE_STORAGE_KEY) || "color";
} catch (_) {
  // Keep the color display when storage access is unavailable.
}
setDisplayColorMode(savedDisplayMode === "green", false);

create6128().then(m => {
  const buildVersion = m.ccall("poc_build_version", "string", [], []);
  const buildCommit = m.ccall("poc_build_commit", "string", [], []);
  const buildIdentityEl = $("buildIdentity");
  buildIdentityEl.textContent = `Version ${buildVersion} / ${buildCommit}`;
  buildIdentityEl.title = `1984 version ${buildVersion}, commit ${buildCommit}`;

  if (m._poc_init() !== 0) {
    setStatus("Emulator initialization failed");
    return;
  }

  ledPowerEl.classList.add("on");
  setStatus("CPC 6128 booting - click the display for keyboard focus");

  const framebuffer = m._poc_pixels();
  const modelEl = $("model");
  const memoryEl = $("memory");
  const memoryValueEl = $("memoryValue");
  const resetEl = $("reset");
  const diskUi = [
    { file: $("diskAfile"), name: $("diskAname"), eject: $("diskAEject") },
    { file: $("diskBfile"), name: $("diskBname"), eject: $("diskBEject") },
  ];
  const cartfileEl = $("cartfile");
  const cartnameEl = $("cartname");
  const cartSlotEl = $("cartSlot");
  const cartLoadEl = $("cartLoad");
  const cartDefaultEl = $("cartDefault");
  const snapshotfileEl = $("snapshotfile");
  const snapshotnameEl = $("snapshotname");
  const snapshotSaveEl = $("snapshotSave");
  const tapefileEl = $("tapefile");
  const tapeDoorEl = $("tapeDoor");
  const tapeLabelEl = $("tapeLabel");
  const tapeStatusEl = $("tapeDeckStatus");
  const tapeCounterEl = $("tapeCounter");
  const tapeEjectEl = $("tapeEject");
  const tapeButtons = [...cpcTapeDeckEl.querySelectorAll("[data-tape-action]")];
  const joytoggleEl = $("joytoggle");
  const mousetoggleEl = $("mousetoggle");
  const joystatusEl = $("joystatus");
  const joymatrixEl = $("joymatrix");
  const mousestatusEl = $("mousestatus");
  const mlSnapshotBreakpointsEl = $("mlSnapshotBreakpoints");
  const CPC_BP_SOURCE_DAP = 1;

  let currentModel = 0;
  let audioCtx = null;
  let audioState = null;
  let nextAudioStart = 0;
  let prevGamepad = null;
  let joyEnabled = true;
  let mouseEnabled = false;
  const ledState = [0, 0];
  let tapeFileName = "";
  let tapeTransportState = "stop";
  const heldKeys = new Set();
  const virtualKeys = new Set();
  const latchedVirtualModifiers = new Set();
  const mlBreakpointSlots = new Map();
  const mlWatchLabels = Array(16).fill(null);
  const mlWriteEvents = [];
  let mlWatchSerial = 0;
  let mlLastRefresh = 0;
  let mlDap = null;
  let mlStopDescription = "Live processor";
  let snapshotBreakpointsEnabled = Boolean(m._poc_snapshot_breakpoints());

  const mlRegisterIds = [
    "mlRegAF", "mlRegBC", "mlRegDE", "mlRegHL",
    "mlRegIX", "mlRegIY", "mlRegSP", "mlRegPC",
  ];
  function setMlMessage(message, error = false) {
    const element = $("mlMonitorMessage");
    element.textContent = message;
    element.classList.toggle("error", error);
  }

  function dapRequest(command, args = {}) {
    const response = mlDap.request(command, args);
    if (!response.success) {
      const detail = response.body && response.body.error && response.body.error.format;
      throw new Error(detail || response.message || `DAP ${command} failed`);
    }
    return response.body || {};
  }

  function createMlDapSession() {
    mlDap = new JS1984DAP.Session({
      isPaused: () => Boolean(m._poc_debug_is_paused()),
      pause: () => m._poc_debug_pause(),
      continue: () => m._poc_debug_continue(),
      stepIn: () => m._poc_debug_step_in(),
      next: () => m._poc_debug_next(),
      stepOut: () => m._poc_debug_step_out(),
      stepBack: () => m._poc_debug_step_back(),
      canStepBack: () => Boolean(m._poc_debug_can_step_back()),
      stopReason: () => m._poc_debug_stop_reason(),
      register: index => m._poc_debug_reg(index),
      setBreakpoint: address => m._poc_debug_breakpoint_set(address),
      clearBreakpoint: slot => m._poc_debug_breakpoint_clear(slot),
      readMemory: address => m._poc_debug_mem_read(address) & 0xff,
      writeMemory: (address, value) => m._poc_debug_mem_write_byte(address, value),
      disassemble: (address, count) => m.ccall(
        "poc_debug_disassemble", "string", ["number", "number"], [address, count]
      ),
    });
    dapRequest("initialize", {
      clientID: "javascript-1984",
      clientName: "Javascript 1984 ML Monitor",
      adapterID: "1984-z80",
      linesStartAt1: true,
      columnsStartAt1: true,
      pathFormat: "uri",
      supportsMemoryReferences: true,
      supportsMemoryEvent: true,
    });
    dapRequest("attach", {});
    const initialized = mlDap.takeEvents();
    if (!initialized.some(event => event.event === "initialized"))
      throw new Error("DAP adapter did not complete initialization");
    dapRequest("configurationDone", {});
    mlStopDescription = mlDap.running ? "Live processor" : "Processor stopped";
  }

  function drainMlDapEvents() {
    let executionChanged = false;
    for (const event of mlDap.takeEvents()) {
      if (event.event === "stopped") {
        mlStopDescription = event.body.description || event.body.reason || "Processor stopped";
        executionChanged = true;
        if (event.body.reason === "instruction breakpoint") {
          const pc = m._poc_debug_reg(7);
          setMlMonitorOpen(true);
          showToast("ML Monitor breakpoint hit at &" + JS1984Monitor.hex(pc, 4));
        }
      } else if (event.event === "continued") {
        mlStopDescription = "Live processor";
        executionChanged = true;
      } else if (event.event === "terminated") {
        mlStopDescription = "Debug session terminated";
        executionChanged = true;
      }
    }
    return executionChanged;
  }

  function resetMlUi() {
    mlBreakpointSlots.clear();
    mlWatchLabels.fill(null);
    mlWriteEvents.length = 0;
    mlWatchSerial = 0;
    renderMlBreakpoints();
    renderMlWatches();
    $("mlWriteLog").textContent = "Write events will appear here.";
    setMlMessage("Monitor channels reset for the selected machine.");
  }

  function updateMlState(forceDisassembly = false) {
    const paused = !mlDap.running && Boolean(m._poc_debug_is_paused());
    mlMonitorEl.classList.toggle("paused", paused);
    $("mlState").textContent = paused ? "PAUSED" : "RUN";
    $("mlStopReason").textContent = paused ? mlStopDescription : "Live processor";
    let pc = -1;

    if (paused) {
      try {
        const stack = dapRequest("stackTrace", { threadId: JS1984DAP.THREAD_ID });
        const frame = stack.stackFrames[0];
        const scopes = dapRequest("scopes", { frameId: frame.id });
        const registers = scopes.scopes.find(scope => scope.presentationHint === "registers");
        const variables = dapRequest("variables", {
          variablesReference: registers.variablesReference,
        });
        const byName = new Map(variables.variables.map(variable => [variable.name, variable]));
        const names = ["AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC"];
        for (let index = 0; index < names.length; index++) {
          const variable = byName.get(names[index]);
          $(mlRegisterIds[index]).textContent = variable
            ? JS1984Monitor.hex(Number.parseInt(variable.value, 16), 4) : "----";
        }
        pc = JS1984DAP.parseReference(frame.instructionPointerReference);
        if (!mlMonitorPanelEl.hidden || forceDisassembly) {
          const result = dapRequest("disassemble", {
            memoryReference: frame.instructionPointerReference,
            instructionCount: 12,
          });
          $("mlDisassembly").textContent = result.instructions.map((instruction, index) => {
            const address = JS1984DAP.parseReference(instruction.address);
            const bytes = (instruction.instructionBytes || "").padEnd(11, " ");
            return `${index === 0 ? ">" : " "}${JS1984Monitor.hex(address, 4)}  ${bytes}  ${instruction.instruction}`;
          }).join("\n");
        }
      } catch (error) {
        setMlMessage(error.message, true);
      }
    } else {
      for (const id of mlRegisterIds) $(id).textContent = "----";
      if (!mlMonitorPanelEl.hidden || forceDisassembly)
        $("mlDisassembly").textContent = "Pause the Z80 to inspect its instruction stream.";
    }

    for (const row of $("mlBreakpointList").querySelectorAll("[data-breakpoint-address]"))
      row.classList.toggle("hit", paused && Number(row.dataset.breakpointAddress) === pc);

    $("mlPause").disabled = paused;
    $("mlContinue").disabled = !paused;
    $("mlStepIn").disabled = !paused;
    $("mlNext").disabled = !paused;
    $("mlStepOut").disabled = !paused;
    $("mlStepBack").disabled = !paused || !m._poc_debug_can_step_back();
    $("mlMemoryRead").disabled = !paused;
    $("mlMemoryWrite").disabled = !paused;
  }

  function renderMlBreakpoints() {
    const list = $("mlBreakpointList");
    list.replaceChildren();
    const entries = [...mlBreakpointSlots.entries()].sort((a, b) => a[1] - b[1]);
    if (!entries.length) {
      const empty = document.createElement("span");
      empty.className = "ml-empty";
      empty.textContent = "No breakpoints armed";
      list.append(empty);
      return;
    }
    for (const [id, address] of entries) {
      const row = document.createElement("div");
      row.className = "ml-channel";
      row.dataset.breakpointAddress = String(address);
      row.innerHTML = `<i></i><b>BP ${String(id).padStart(2, "0")}</b>` +
        `<code>${JS1984Monitor.hex(address, 4)}</code>` +
        `<button type="button" class="ml-channel-remove" data-breakpoint-id="${id}" aria-label="Clear breakpoint ${id}">X</button>`;
      list.append(row);
    }
  }

  function applyMlBreakpoints(addresses) {
    const body = dapRequest("setInstructionBreakpoints", {
      breakpoints: addresses.map(address => ({
        instructionReference: JS1984DAP.addressReference(address),
      })),
    });
    mlBreakpointSlots.clear();
    for (let index = 0; index < body.breakpoints.length; index++) {
      const breakpoint = body.breakpoints[index];
      if (breakpoint.verified && breakpoint.id !== undefined)
        mlBreakpointSlots.set(breakpoint.id, addresses[index]);
    }
    renderMlBreakpoints();
    return body.breakpoints;
  }

  function adoptCoreMlBreakpoints() {
    const dapAddresses = [];
    let active = 0;
    const count = m._poc_debug_breakpoint_count();
    for (let index = 0; index < count; index++) {
      const id = m._poc_debug_breakpoint_id_at(index);
      if (id <= 0 || !m._poc_debug_breakpoint_enabled(id)) continue;
      active++;
      if (m._poc_debug_breakpoint_source(id) !== CPC_BP_SOURCE_DAP) continue;
      const address = m._poc_debug_breakpoint_addr(id);
      if (address >= 0 && !dapAddresses.includes(address))
        dapAddresses.push(address);
    }
    createMlDapSession();
    applyMlBreakpoints(dapAddresses);
    return active;
  }

  function updateSnapshotBreakpointControl() {
    mlSnapshotBreakpointsEl.setAttribute(
      "aria-pressed", snapshotBreakpointsEnabled ? "true" : "false"
    );
    mlSnapshotBreakpointsEl.textContent =
      "SNA Breaks: " + (snapshotBreakpointsEnabled ? "On" : "Off");
  }

  mlSnapshotBreakpointsEl.addEventListener("click", () => {
    snapshotBreakpointsEnabled = !snapshotBreakpointsEnabled;
    m._poc_set_snapshot_breakpoints(snapshotBreakpointsEnabled ? 1 : 0);
    updateSnapshotBreakpointControl();
    const breakpointCount = adoptCoreMlBreakpoints();
    setMlMessage(
      `Snapshot breakpoints ${snapshotBreakpointsEnabled ? "armed" : "disarmed"}; ` +
      `${breakpointCount} breakpoint channel(s) active.`
    );
    updateMlState(true);
  });

  function renderMlWatches() {
    const list = $("mlWatchList");
    list.replaceChildren();
    let count = 0;
    for (let slot = 0; slot < mlWatchLabels.length; slot++) {
      const watch = mlWatchLabels[slot];
      if (!watch) continue;
      count++;
      const row = document.createElement("div");
      row.className = "ml-channel";
      const lamp = document.createElement("i");
      const name = document.createElement("b");
      name.textContent = watch.label;
      name.title = watch.label;
      const address = document.createElement("code");
      address.textContent = JS1984Monitor.hex(watch.address, 4);
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "ml-channel-remove";
      remove.dataset.watchSlot = String(slot);
      remove.setAttribute("aria-label", `Clear watch ${watch.label}`);
      remove.textContent = "X";
      row.append(lamp, name, address, remove);
      list.append(row);
    }
    if (!count) {
      const empty = document.createElement("span");
      empty.className = "ml-empty";
      empty.textContent = "No labels watched";
      list.append(empty);
    }
  }

  function readMlMemory() {
    try {
      const address = JS1984Monitor.parseAddress($("mlMemoryAddress").value);
      const length = JS1984Monitor.parseLength($("mlMemoryLength").value);
      const body = dapRequest("readMemory", {
        memoryReference: JS1984DAP.addressReference(address),
        count: length,
      });
      const bytes = [...JS1984DAP.decodeBase64(body.data || "")];
      $("mlMemoryAddress").value = JS1984Monitor.hex(address, 4);
      $("mlMemoryBytes").value = bytes.map(value => JS1984Monitor.hex(value, 2)).join(" ");
      $("mlMemoryDump").textContent = JS1984Monitor.formatMemory(address, bytes);
      const unreadable = body.unreadableBytes || 0;
      setMlMessage(`Read ${bytes.length} byte${bytes.length === 1 ? "" : "s"} from &${JS1984Monitor.hex(address, 4)}` +
        (unreadable ? `; ${unreadable} beyond Z80 memory.` : "."));
    } catch (error) {
      setMlMessage(error.message, true);
    }
  }

  function pollMlWriteEvents() {
    const newest = m._poc_debug_watch_serial() >>> 0;
    if (newest === mlWatchSerial) return;
    let first = mlWatchSerial + 1;
    if (newest - first >= 64) first = newest - 63;
    for (let serial = first; serial <= newest; serial++) {
      const slot = m._poc_debug_watch_event_slot(serial);
      if (slot < 0) continue;
      const address = m._poc_debug_watch_event_addr(serial);
      const pc = m._poc_debug_watch_event_pc(serial);
      const oldValue = m._poc_debug_watch_event_old(serial);
      const newValue = m._poc_debug_watch_event_new(serial);
      const watch = mlWatchLabels[slot];
      const label = watch ? watch.label : `watch_${slot}`;
      mlDap.notifyWrite({ address, pc, oldValue, newValue, label });
      mlWriteEvents.unshift(
        `${label} @${JS1984Monitor.hex(address, 4)}  ` +
        `${JS1984Monitor.hex(oldValue, 2)}>${JS1984Monitor.hex(newValue, 2)}  ` +
        `PC=${JS1984Monitor.hex(pc, 4)}`
      );
    }
    mlWatchSerial = newest;
    mlWriteEvents.splice(24);
    $("mlWriteLog").textContent = mlWriteEvents.join("\n") ||
      "Write events will appear here.";
  }

  $("mlPause").addEventListener("click", () => {
    try {
      dapRequest("pause", { threadId: JS1984DAP.THREAD_ID });
      drainMlDapEvents();
      m._poc_audio_reset();
      setMlMessage("DAP paused the processor between instructions.");
      updateMlState(true);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });
  $("mlContinue").addEventListener("click", () => {
    try {
      dapRequest("continue", { threadId: JS1984DAP.THREAD_ID });
      setMlMessage("DAP continued execution.");
      updateMlState(true);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });
  $("mlStepIn").addEventListener("click", () => {
    try {
      dapRequest("stepIn", { threadId: JS1984DAP.THREAD_ID, granularity: "instruction" });
      setMlMessage("DAP Step In is executing one Z80 instruction.");
      updateMlState(true);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });
  $("mlNext").addEventListener("click", () => {
    try {
      dapRequest("next", { threadId: JS1984DAP.THREAD_ID, granularity: "instruction" });
      setMlMessage("DAP Step Over is running to the next Z80 instruction.");
      updateMlState(true);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });
  $("mlStepBack").addEventListener("click", () => {
    try {
      dapRequest("stepBack", { threadId: JS1984DAP.THREAD_ID, granularity: "instruction" });
      drainMlDapEvents();
      setMlMessage("DAP restored the previous instruction checkpoint.");
      updateMlState(true);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });
  $("mlStepOut").addEventListener("click", () => {
    try {
      dapRequest("stepOut", { threadId: JS1984DAP.THREAD_ID, granularity: "instruction" });
      setMlMessage("DAP Step Out is running to the stack return address.");
      updateMlState(true);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });

  $("mlBreakpointAdd").addEventListener("click", () => {
    try {
      const address = JS1984Monitor.parseAddress($("mlBreakpointAddress").value);
      if ([...mlBreakpointSlots.values()].includes(address))
        throw new Error("a breakpoint is already armed at this address");
      const addresses = [...mlBreakpointSlots.values(), address];
      const results = applyMlBreakpoints(addresses);
      const breakpoint = results[results.length - 1];
      if (!breakpoint.verified) throw new Error(breakpoint.message || "breakpoint was rejected");
      $("mlBreakpointAddress").value = JS1984Monitor.hex(address, 4);
      setMlMessage(`DAP instruction breakpoint ${breakpoint.id} armed at &${JS1984Monitor.hex(address, 4)}.`);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });
  $("mlBreakpointList").addEventListener("click", event => {
    const button = event.target.closest("[data-breakpoint-id]");
    if (!button) return;
    const id = Number(button.dataset.breakpointId);
    const address = mlBreakpointSlots.get(id);
    try {
      applyMlBreakpoints([...mlBreakpointSlots]
        .filter(([breakpointId]) => breakpointId !== id)
        .map(([, breakpointAddress]) => breakpointAddress));
      setMlMessage(`DAP instruction breakpoint ${id} at &${JS1984Monitor.hex(address, 4)} cleared.`);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });

  $("mlWatchAdd").addEventListener("click", () => {
    try {
      const label = JS1984Monitor.normalizeLabel($("mlWatchLabel").value);
      const address = JS1984Monitor.parseAddress($("mlWatchAddress").value);
      const slot = mlWatchLabels.findIndex(watch => !watch);
      if (slot < 0) throw new Error("all 16 write-watch channels are in use");
      if (m._poc_debug_watch_set(slot, address) !== 0)
        throw new Error("could not arm write watch");
      mlWatchLabels[slot] = { label, address };
      $("mlWatchAddress").value = JS1984Monitor.hex(address, 4);
      $("mlWatchLabel").value = "";
      renderMlWatches();
      setMlMessage(`Watching ${label} at &${JS1984Monitor.hex(address, 4)} for writes.`);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });
  $("mlWatchList").addEventListener("click", event => {
    const button = event.target.closest("[data-watch-slot]");
    if (!button) return;
    const slot = Number(button.dataset.watchSlot);
    const watch = mlWatchLabels[slot];
    m._poc_debug_watch_clear(slot);
    mlWatchLabels[slot] = null;
    renderMlWatches();
    setMlMessage(`${watch ? watch.label : "Write watch"} cleared.`);
  });

  $("mlMemoryRead").addEventListener("click", readMlMemory);
  $("mlMemoryWrite").addEventListener("click", () => {
    try {
      const address = JS1984Monitor.parseAddress($("mlMemoryAddress").value);
      const bytes = JS1984Monitor.parseBytes($("mlMemoryBytes").value);
      dapRequest("writeMemory", {
        memoryReference: JS1984DAP.addressReference(address),
        data: JS1984DAP.encodeBase64(Uint8Array.from(bytes)),
        allowPartial: false,
      });
      drainMlDapEvents();
      pollMlWriteEvents();
      $("mlMemoryLength").value = String(bytes.length);
      readMlMemory();
      setMlMessage(`Wrote ${bytes.length} byte${bytes.length === 1 ? "" : "s"} at &${JS1984Monitor.hex(address, 4)}.`);
      updateMlState(true);
    } catch (error) {
      setMlMessage(error.message, true);
    }
  });

  for (const encoder of document.querySelectorAll(".ml-encoder[data-encoder]")) {
    const adjust = delta => {
      const input = $(encoder.dataset.encoder);
      let address;
      try { address = JS1984Monitor.parseAddress(input.value); }
      catch (_) { address = 0; }
      input.value = JS1984Monitor.hex((address + delta) & 0xffff, 4);
    };
    encoder.addEventListener("click", () => adjust(Number(encoder.dataset.delta)));
    encoder.addEventListener("wheel", event => {
      event.preventDefault();
      const direction = event.deltaY < 0 ? 1 : -1;
      adjust(Math.abs(Number(encoder.dataset.delta)) * direction);
    }, { passive: false });
  }

  renderMlBreakpoints();
  renderMlWatches();
  updateSnapshotBreakpointControl();
  createMlDapSession();
  updateMlState(true);
  mlMonitorToggleEl.addEventListener("click", () => {
    if (!mlMonitorPanelEl.hidden) updateMlState(true);
  });

  function pressVirtualKey(scancode) {
    if (virtualKeys.has(scancode)) return;
    const alreadyPressed = heldKeys.has(scancode);
    virtualKeys.add(scancode);
    if (!alreadyPressed) m._poc_key(scancode, 1);
  }

  function releaseVirtualKey(scancode) {
    if (!virtualKeys.delete(scancode)) return;
    if (!heldKeys.has(scancode)) m._poc_key(scancode, 0);
  }

  function setModifierUi(scancode, active) {
    for (const button of cpcKeyboardKeysEl.querySelectorAll(
      `[data-modifier][data-scancode="${scancode}"]`
    )) {
      button.classList.toggle("latched", active);
      button.setAttribute("aria-pressed", String(active));
    }
  }

  function releaseLatchedModifiers() {
    for (const scancode of latchedVirtualModifiers) {
      releaseVirtualKey(scancode);
      setModifierUi(scancode, false);
    }
    latchedVirtualModifiers.clear();
  }

  function toggleVirtualModifier(scancode) {
    if (latchedVirtualModifiers.delete(scancode)) {
      releaseVirtualKey(scancode);
      setModifierUi(scancode, false);
    } else {
      latchedVirtualModifiers.add(scancode);
      pressVirtualKey(scancode);
      setModifierUi(scancode, true);
    }
  }

  function releaseAllVirtualKeys() {
    for (const scancode of [...virtualKeys]) releaseVirtualKey(scancode);
    latchedVirtualModifiers.clear();
    for (const button of cpcKeyboardKeysEl.querySelectorAll("[data-scancode]")) {
      button.classList.remove("active", "latched");
      if (button.hasAttribute("data-modifier"))
        button.setAttribute("aria-pressed", "false");
    }
  }

  function virtualKeyButton(target) {
    return target.closest("button[data-scancode]");
  }

  cpcKeyboardKeysEl.addEventListener("pointerdown", event => {
    const button = virtualKeyButton(event.target);
    if (!button) return;
    event.preventDefault();
    startAudio();
    const scancode = Number(button.dataset.scancode);
    if (button.hasAttribute("data-modifier")) {
      toggleVirtualModifier(scancode);
    } else {
      pressVirtualKey(scancode);
      button.classList.add("active");
      button.setPointerCapture(event.pointerId);
    }
    pulseInputLed();
  });

  function finishVirtualPointer(event) {
    const button = virtualKeyButton(event.target);
    if (!button || button.hasAttribute("data-modifier")) return;
    releaseVirtualKey(Number(button.dataset.scancode));
    button.classList.remove("active");
    releaseLatchedModifiers();
  }

  cpcKeyboardKeysEl.addEventListener("pointerup", finishVirtualPointer);
  cpcKeyboardKeysEl.addEventListener("pointercancel", finishVirtualPointer);
  cpcKeyboardKeysEl.addEventListener("lostpointercapture", finishVirtualPointer);
  cpcKeyboardKeysEl.addEventListener("click", event => {
    if (event.detail !== 0) return;
    const button = virtualKeyButton(event.target);
    if (!button) return;
    startAudio();
    const scancode = Number(button.dataset.scancode);
    if (button.hasAttribute("data-modifier")) {
      toggleVirtualModifier(scancode);
    } else {
      pressVirtualKey(scancode);
      button.classList.add("active");
      setTimeout(() => {
        releaseVirtualKey(scancode);
        button.classList.remove("active");
        releaseLatchedModifiers();
      }, 90);
    }
    pulseInputLed();
  });
  cpcKeyboardToggleEl.addEventListener("click", () => {
    if (cpcKeyboardKeysEl.hidden) releaseAllVirtualKeys();
  });

  function updateCartUi() {
    const enabled = currentModel === 1;
    cartSlotEl.classList.toggle("media-slot-disabled", !enabled);
    cartfileEl.disabled = !enabled;
    cartDefaultEl.disabled = !enabled;
    cartLoadEl.setAttribute("aria-disabled", enabled ? "false" : "true");
    if (enabled && !cartnameEl.textContent)
      cartnameEl.textContent = "system.cpr";
    if (!enabled)
      cartnameEl.textContent = "Select CPC 6128 Plus";
  }

  function clearTapeUi() {
    tapeFileName = "";
    tapeTransportState = "stop";
    tapefileEl.value = "";
    tapeLabelEl.textContent = "CPC DATA";
    tapeStatusEl.textContent = "NO TAPE - CLICK DECK TO LOAD";
    tapeCounterEl.textContent = "000";
    tapeEjectEl.disabled = true;
    cpcTapeDeckEl.classList.remove("tape-running");
    for (const button of tapeButtons) {
      const record = button.dataset.tapeAction === "record";
      button.disabled = true;
      button.classList.remove("active");
      if (record) button.title = "Recording is not available yet";
    }
  }

  function updateTapeDeck() {
    const loaded = Boolean(m._poc_tape_loaded());
    const motor = Boolean(m._poc_tape_motor());
    const playing = Boolean(m._poc_tape_playing());
    const paused = Boolean(m._poc_tape_paused());
    const ended = Boolean(m._poc_tape_ended());

    cpcTapeDeckEl.classList.toggle("tape-running", playing);
    tapeCounterEl.textContent = String(m._poc_tape_counter()).padStart(3, "0");
    tapeEjectEl.disabled = !loaded;

    for (const button of tapeButtons) {
      const action = button.dataset.tapeAction;
      button.disabled = action === "record" || !loaded;
      button.classList.toggle("active",
        (action === "play" && !paused && !ended) ||
        (action === "pause" && tapeTransportState === "pause") ||
        (action === "stop" && tapeTransportState === "stop" && paused));
    }

    if (!loaded) {
      tapeStatusEl.textContent = "NO TAPE - CLICK DECK TO LOAD";
    } else if (ended) {
      tapeStatusEl.textContent = "END OF TAPE";
    } else if (playing) {
      tapeStatusEl.textContent = "PLAYING - " + tapeFileName;
    } else if (!paused && !motor) {
      tapeStatusEl.textContent = "PLAY ARMED - CPC MOTOR OFF";
    } else if (tapeTransportState === "pause") {
      tapeStatusEl.textContent = "PAUSED - " + tapeFileName;
    } else {
      tapeStatusEl.textContent = "STOPPED - " + tapeFileName;
    }
  }

  async function loadTapeFile(file) {
    if (!file) return;
    const path = "/tape.cdt";
    try {
      const data = new Uint8Array(await file.arrayBuffer());
      m.FS.writeFile(path, data);
      if (m.ccall("poc_tape_load", "number", ["string"], [path]) !== 0)
        throw new Error("unsupported or damaged CDT image");
      tapeFileName = file.name;
      tapeTransportState = "stop";
      tapeLabelEl.textContent = file.name.replace(/\.cdt$/i, "");
      setStatus("Tape: " + file.name + " - press PLAY, then run the CPC loader");
      showToast("CDT loaded into cassette deck");
      updateTapeDeck();
    } catch (error) {
      setStatus("Tape load failed: " + error.message);
      showToast("Could not load " + file.name);
    } finally {
      tapefileEl.value = "";
      try { m.FS.unlink(path); } catch (_) { /* The tape decoder owns its copy. */ }
    }
  }

  function clearDiskUi(drive) {
    const ui = diskUi[drive];
    ui.name.textContent = "No disk loaded";
    ui.eject.disabled = true;
    ui.file.value = "";
  }

  function clearDisksUi() {
    clearDiskUi(0);
    clearDiskUi(1);
  }

  /* ---- M4 expansion board ---- */

  function downloadM4SdImage() {
    if (!m4SdImage) return;
    try {
      const data = m.FS.readFile(m4SdImage.path);
      const blob = new Blob([data], { type: "application/octet-stream" });
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = m4SdImage.name;
      document.body.appendChild(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 0);
    } catch (error) {
      setStatus("Could not save " + m4SdImage.name + ": " + error.message);
    }
  }

  function clearM4SdUi() {
    m4SdImage = null;
    m4SdNameEl.textContent = "No image loaded";
    m4SdEjectEl.disabled = true;
  }

  function ejectM4Sd(options = {}) {
    const clear = options.clear !== false;
    const download = options.download !== false;
    if (m4SdImage) {
      m._poc_eject_m4_sd();
      if (download) downloadM4SdImage();
    }
    if (clear) clearM4SdUi();
  }

  function mountM4SdData(data, name, path) {
    if (!m4Enabled)
      throw new Error("enable the M4 board first");
    if (m4SdImage) ejectM4Sd();
    m.FS.writeFile(path, data);
    if (m.ccall("poc_mount_m4_sd", "number", ["string"], [path]) !== 0)
      throw new Error("unsupported or damaged SD image");
    m4SdImage = { name, path };
    m4SdNameEl.textContent = name;
    m4SdEjectEl.disabled = false;
    setStatus("M4 SD card: " + name);
  }

  function remountM4Sd() {
    if (!m4Enabled || !m4SdImage) return;
    if (m.ccall("poc_mount_m4_sd", "number", ["string"], [m4SdImage.path]) !== 0) {
      setStatus("M4 SD image could not be remounted");
      clearM4SdUi();
    }
  }

  applyM4Hardware = enabled => {
    const requested = Boolean(enabled);
    if (!requested) {
      try {
        ejectM4Sd();
      } catch (error) {
        setStatus("SD eject failed: " + error.message);
        m4Enabled = true;
        updateM4Ui();
        return;
      }
      if (m4Bridge) m4Bridge.resetChannels();
    }
    const wasEnabled = Boolean(m._poc_m4_enabled());
    if (wasEnabled !== requested) {
      if (m._poc_set_m4(requested ? 1 : 0) !== 0) {
        m4Enabled = wasEnabled;
        updateM4Ui();
        setStatus("M4 board firmware could not be installed");
        return;
      }
      // The ROM layout changed; reset the machine (the web equivalent of the
      // native overlay's cold boot on M4 toggle) so the boot sequence
      // re-probes the MX4 bus and M4ROM slot.
      m._poc_reset();
      m._poc_audio_reset();
      if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
      releaseAllJoy();
      m._poc_set_mouse(mouseEnabled ? 1 : 0);
      setStatus(requested
        ? "M4 enabled - machine reset"
        : "M4 disabled - machine reset");
    }
    m4Enabled = Boolean(m._poc_m4_enabled());
    if (m4Enabled) remountM4Sd();
    updateM4Ui();
    renderRomSlots();
    if (requested && !m4Enabled)
      setStatus("M4 board firmware could not be installed");
  };

  applyPowergraphHardware = enabled => {
    const requested = Boolean(enabled);
    const result = m._poc_set_powergraph_v9990(requested ? 1 : 0);
    if (result < 0) {
      powergraphEnabled = Boolean(m._poc_powergraph_v9990_enabled());
      setStatus("PowerGraph V9990 could not be initialized");
    } else {
      powergraphEnabled = Boolean(result);
      setStatus(powergraphEnabled
        ? "PowerGraph V9990 enabled"
        : "PowerGraph V9990 disabled");
    }
    updatePowergraphUi();
  };

  applyPowergraphOutputHardware = output => {
    const sources = { auto: 0, cpc: 1, v9990: 2 };
    const source = sources[output] ?? 0;
    if (m._poc_set_powergraph_video_source(source) < 0) {
      powergraphOutput = "auto";
      m._poc_set_powergraph_video_source(0);
      setStatus("Unsupported PowerGraph output selection");
    }
    updatePowergraphUi();
  };

  m4SdFileEl.addEventListener("change", () => {
    const file = m4SdFileEl.files[0];
    if (!file) return;
    file.arrayBuffer().then(buf => {
      try {
        mountM4SdData(new Uint8Array(buf), file.name, "/m4.img");
        showToast("M4 SD image loaded");
      } catch (error) {
        setStatus("SD image load failed: " + error.message);
        showToast("Could not load " + file.name);
      } finally {
        m4SdFileEl.value = "";
      }
    });
  });

  m4SdEjectEl.addEventListener("click", () => {
    try {
      const wasMounted = Boolean(m4SdImage);
      ejectM4Sd();
      if (wasMounted) {
        setStatus("M4 SD card ejected safely");
        showToast("Updated SD image downloaded");
      } else {
        setStatus("No M4 SD image loaded");
      }
    } catch (error) {
      setStatus("SD eject failed: " + error.message);
      showToast("Could not eject SD image safely");
    }
  });

  /* ---- ROM slots ---- */

  function warmReset(status) {
    m._poc_reset();
    m._poc_audio_reset();
    if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
    releaseAllJoy();
    m._poc_set_mouse(mouseEnabled ? 1 : 0);
    if (status) setStatus(status);
  }

  const romSlotRows = new Map();

  function buildRomSlotRows() {
    if (romSlotRows.size) return;
    for (let slot = 0; slot < 32; slot++) {
      const row = document.createElement("div");
      row.className = "rom-slot-row";
      const num = document.createElement("span");
      num.className = "rom-slot-num";
      num.textContent = "Slot " + String(slot).padStart(2, "0");
      const name = document.createElement("span");
      name.className = "rom-slot-name";
      const load = document.createElement("button");
      load.type = "button";
      load.className = "extension-small-key";
      load.textContent = "Load";
      const eject = document.createElement("button");
      eject.type = "button";
      eject.className = "extension-small-key";
      eject.textContent = "Eject";
      eject.disabled = true;
      row.append(num, name, load, eject);
      romSlotListEl.append(row);
      romSlotRows.set(slot, { name, load, eject });
      load.addEventListener("click", () => {
        pendingRomSlot = slot;
        romfileEl.value = "";
        romfileEl.click();
      });
      eject.addEventListener("click", () => unloadRomSlot(slot));
    }
  }

  function romSlotState(slot) {
    if (slot === 6 && m4Enabled)
      return { name: "M4ROM", empty: false, reserved: true, readonly: true };
    const loaded = romSlots.get(slot);
    if (loaded)
      return { name: loaded.name, empty: false, reserved: false, readonly: false };
    if (slot === 0)
      return { name: "(BASIC)", empty: true, reserved: false, readonly: false };
    if (slot === 7)
      return { name: "(AMSDOS)", empty: true, reserved: false, readonly: false };
    return { name: "[empty]", empty: true, reserved: false, readonly: false };
  }

  function renderRomSlots() {
    if (!romSlotRows.size) return;
    const boardOn = romBoardEnabled;
    for (let slot = 0; slot < 32; slot++) {
      const row = romSlotRows.get(slot);
      if (!row) continue;
      const state = romSlotState(slot);
      row.name.textContent = state.name;
      row.name.classList.toggle("empty", state.empty);
      row.name.classList.toggle("reserved", state.reserved);
      const locked = !boardOn || state.readonly;
      row.name.parentElement.setAttribute("aria-disabled", String(locked));
      row.load.disabled = locked;
      row.eject.disabled = locked || state.empty || state.readonly;
    }
    romLowerNameEl.textContent = currentModel === 1 ? "Plus OS" : "OS_6128.ROM";
  }

  function loadRomIntoSlot(slot, file) {
    if (!romBoardEnabled)
      throw new Error("fit the ROM board first");
    const path = "/roms/user_" + slot + ".rom";
    file.arrayBuffer().then(buf => {
      try {
        m.FS.writeFile(path, new Uint8Array(buf));
        if (m.ccall("poc_rom_slot_load", "number",
                    ["number", "string"], [slot, path]) !== 0)
          throw new Error("unsupported or damaged ROM image");
        romSlots.set(slot, { name: file.name, path });
        warmReset("Slot " + slot + " loaded: " + file.name);
        showToast("ROM loaded into slot " + slot);
      } catch (error) {
        setStatus("ROM slot " + slot + " load failed: " + error.message);
        showToast("Could not load " + file.name);
      } finally {
        renderRomSlots();
      }
    });
  }

  function unloadRomSlot(slot) {
    m._poc_rom_slot_unload(slot);
    romSlots.delete(slot);
    try { m.FS.unlink("/roms/user_" + slot + ".rom"); } catch (_) {}
    warmReset("Slot " + slot + " cleared");
    renderRomSlots();
  }

  function reapplyRomSlots() {
    for (const [slot, entry] of romSlots) {
      if (m.ccall("poc_rom_slot_load", "number",
                  ["number", "string"], [slot, entry.path]) !== 0) {
        romSlots.delete(slot);
        setStatus("ROM slot " + slot + " could not be reloaded");
      }
    }
    renderRomSlots();
  }

  applyRomBoard = (enabled, options = {}) => {
    const requested = Boolean(enabled);
    const doReset = options.reset !== false;
    if (!requested) {
      for (const slot of [...romSlots.keys()]) {
        m._poc_rom_slot_unload(slot);
        try { m.FS.unlink("/roms/user_" + slot + ".rom"); } catch (_) {}
      }
      romSlots.clear();
    }
    if (doReset)
      warmReset(requested
        ? "ROM board fitted - machine reset"
        : "ROM board removed - machine reset");
    renderRomSlots();
  };

  romfileEl.addEventListener("change", () => {
    const file = romfileEl.files[0];
    const slot = pendingRomSlot;
    pendingRomSlot = -1;
    if (!file || slot < 0) return;
    loadRomIntoSlot(slot, file);
  });

  function releaseAllJoy() {
    for (let column = 0; column < 6; column++)
      m._poc_joy(column, 0);
    prevGamepad = null;
  }

  function reinit(model, cartridge) {
    const rc = cartridge !== undefined
      ? m.ccall("poc_load_cartridge", "number", ["string"], [cartridge])
      : m._poc_init_model(model, 0);
    if (rc !== 0) {
      setStatus("Machine initialization failed");
      return false;
    }
    currentModel = model;
    modelEl.value = String(model);
    m._poc_set_snapshot_breakpoints(snapshotBreakpointsEnabled ? 1 : 0);
    m._poc_audio_reset();
    if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
    releaseAllJoy();
    m._poc_set_mouse(mouseEnabled ? 1 : 0);
    applyM4Hardware(m4Enabled);
    applyPowergraphHardware(powergraphEnabled);
    applyPowergraphOutputHardware(powergraphOutput);
    reapplyRomSlots();
    clearDisksUi();
    clearTapeUi();
    snapshotnameEl.textContent = "Machine state";
    createMlDapSession();
    resetMlUi();
    updateMlState(true);
    updateCartUi();
    setStatus("Machine reset");
    return true;
  }

  modelEl.addEventListener("change", () => {
    const model = Number(modelEl.value);
    if (reinit(model)) {
      cartnameEl.textContent = model === 1 ? "system.cpr" : "Select CPC 6128 Plus";
      updateCartUi();
      showToast(model === 1 ? "CPC 6128 Plus selected" : "CPC 6128 selected");
    }
  });

  const memorySizes = [128, 256, 512, 1024];
  function setMemorySlider(memoryKb) {
    let index = memorySizes.indexOf(memoryKb);
    if (index < 0) index = 0;
    const label = memorySizes[index] === 1024 ? "1 MB" : memorySizes[index] + " KB";
    memoryEl.value = String(index);
    memoryEl.setAttribute("aria-valuetext", index === 0 ? label + " (Original)" : label);
    memoryValueEl.textContent = label;
  }

  function applyMemorySize(memoryKb, notify = true, focus = true) {
    if (m._poc_set_memory_kb(memoryKb) !== 0) {
      setMemorySlider(m._poc_memory_kb());
      setStatus("Unsupported memory size");
      return false;
    }
    setMemorySlider(memoryKb);
    if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
    releaseAllJoy();
    m._poc_set_mouse(mouseEnabled ? 1 : 0);
    mlDap.sync();
    drainMlDapEvents();
    setMlMessage("Memory changed; breakpoints and write watches remain armed.");
    updateMlState(true);
    const memoryLabel = memoryKb === 1024 ? "1 MB" : memoryKb + " KB";
    setStatus("Memory set to " + memoryLabel);
    if (notify) showToast(memoryLabel + " RAM selected");
    if (focus) canvas.focus();
    return true;
  }

  memoryEl.addEventListener("input", () => {
    setMemorySlider(memorySizes[Number(memoryEl.value)]);
  });

  memoryEl.addEventListener("change", () => {
    const memoryKb = memorySizes[Number(memoryEl.value)];
    applyMemorySize(memoryKb);
  });

  resetEl.addEventListener("click", () => {
    m._poc_reset();
    m._poc_audio_reset();
    if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
    releaseAllJoy();
    m._poc_set_mouse(mouseEnabled ? 1 : 0);
    mlDap.sync();
    drainMlDapEvents();
    setMlMessage("Warm reset complete; breakpoints and write watches remain armed.");
    updateMlState(true);
    setStatus("Warm reset complete");
    showToast("CPC reset");
    canvas.focus();
  });

  function mountDisk(drive, data, name, path) {
    const label = drive === 0 ? "A" : "B";
    m.FS.writeFile(path, data);
    const rc = m.ccall(
      "poc_load_disk", "number", ["number", "string"], [drive, path]
    );
    if (rc !== 0) throw new Error("unsupported or damaged disk image");
    diskUi[drive].name.textContent = name;
    diskUi[drive].eject.disabled = false;
    setStatus("Drive " + label + ": " + name);
    showToast("Disk loaded into Drive " + label);
  }

  function mountCartridge(data, name, path) {
    m.FS.writeFile(path, data);
    if (!reinit(1, path)) throw new Error("unsupported or damaged cartridge");
    cartnameEl.textContent = name;
    updateCartUi();
    setStatus("Cartridge: " + name);
    showToast("Cartridge loaded and CPC 6128 Plus started");
  }

  async function loadDiskFile(file, drive = 0) {
    if (!file) return;
    const label = drive === 0 ? "A" : "B";
    const path = drive === 0 ? "/drive-a.dsk" : "/drive-b.dsk";
    try {
      const data = new Uint8Array(await file.arrayBuffer());
      mountDisk(drive, data, file.name, path);
    } catch (error) {
      setStatus("Drive " + label + " load failed: " + error.message);
      showToast("Could not load " + file.name);
    } finally {
      diskUi[drive].file.value = "";
    }
  }

  async function loadCartridgeFile(file) {
    if (!file) return;
    try {
      const data = new Uint8Array(await file.arrayBuffer());
      mountCartridge(data, file.name, "/uploaded.cpr");
    } catch (error) {
      setStatus("Cartridge load failed: " + error.message);
      showToast("Could not load " + file.name);
    }
  }

  async function loadSnapshotFile(file) {
    if (!file) return;
    const path = "/uploaded.sna";
    try {
      const data = new Uint8Array(await file.arrayBuffer());
      m.FS.writeFile(path, data);
      const rc = m.ccall("poc_load_snapshot", "number", ["string"], [path]);
      if (rc !== 0) throw new Error("unsupported or damaged snapshot");
      m._poc_audio_reset();
      if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
      releaseAllJoy();
      m._poc_set_mouse(mouseEnabled ? 1 : 0);
      const snapshotMemoryKb = m._poc_memory_kb();
      if ([128, 256, 512, 1024].includes(snapshotMemoryKb))
        setMemorySlider(snapshotMemoryKb);
      snapshotnameEl.textContent = file.name;
      const breakpointCount = adoptCoreMlBreakpoints();
      mlDap.sync();
      drainMlDapEvents();
      setMlMessage(`Snapshot loaded; ${breakpointCount} breakpoint channel(s) armed.`);
      updateMlState(true);
      setStatus("Snapshot: " + file.name);
      showToast("Snapshot loaded");
      canvas.focus();
    } catch (error) {
      setStatus("Snapshot load failed: " + error.message);
      showToast("Could not load " + file.name);
    } finally {
      snapshotfileEl.value = "";
      try { m.FS.unlink(path); } catch (_) { /* File was not staged. */ }
    }
  }

  function saveSnapshotFile() {
    const path = "/download.sna";
    try {
      const rc = m.ccall("poc_save_snapshot", "number", ["string"], [path]);
      if (rc !== 0) throw new Error("snapshot encoder rejected the machine state");
      const data = new Uint8Array(m.FS.readFile(path));
      const blob = new Blob([data], { type: "application/octet-stream" });
      const url = URL.createObjectURL(blob);
      const timestamp = new Date().toISOString().replace(/[:.]/g, "-");
      const filename = "1984-" + timestamp + ".sna";
      const link = document.createElement("a");
      link.href = url;
      link.download = filename;
      link.hidden = true;
      document.body.append(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      snapshotnameEl.textContent = filename;
      setStatus("Snapshot saved: " + filename);
      showToast("Snapshot downloaded");
    } catch (error) {
      setStatus("Snapshot save failed: " + error.message);
      showToast("Could not save snapshot");
    } finally {
      try { m.FS.unlink(path); } catch (_) { /* No snapshot was created. */ }
    }
  }

  for (let drive = 0; drive < diskUi.length; drive++) {
    const ui = diskUi[drive];
    const label = drive === 0 ? "A" : "B";
    ui.file.addEventListener("change", () => loadDiskFile(ui.file.files[0], drive));
    ui.eject.addEventListener("click", () => {
      m._poc_eject_disk(drive);
      clearDiskUi(drive);
      setStatus("Drive " + label + " ejected");
      showToast("Drive " + label + " ejected");
    });
  }
  cartfileEl.addEventListener("change", () => loadCartridgeFile(cartfileEl.files[0]));
  cartDefaultEl.addEventListener("click", () => {
    if (reinit(1)) {
      cartnameEl.textContent = "system.cpr";
      setStatus("Default system cartridge restored");
    }
  });
  snapshotfileEl.addEventListener("change", () => {
    loadSnapshotFile(snapshotfileEl.files[0]);
  });
  snapshotSaveEl.addEventListener("click", saveSnapshotFile);
  tapeDoorEl.addEventListener("click", () => tapefileEl.click());
  tapefileEl.addEventListener("change", () => loadTapeFile(tapefileEl.files[0]));
  tapeEjectEl.addEventListener("click", () => {
    m._poc_tape_eject();
    clearTapeUi();
    setStatus("Cassette ejected");
    showToast("Tape ejected");
  });
  for (const button of tapeButtons) {
    button.addEventListener("click", () => {
      const action = button.dataset.tapeAction;
      if (!m._poc_tape_loaded() || action === "record") return;
      if (action === "play") {
        startAudio();
        if (m._poc_tape_play() !== 0) {
          showToast("Rewind the tape before playing again");
          return;
        }
        tapeTransportState = "play";
      } else if (action === "rewind") {
        m._poc_tape_rewind();
        tapeTransportState = "stop";
      } else if (action === "forward") {
        m._poc_tape_next();
        tapeTransportState = "stop";
      } else if (action === "stop") {
        m._poc_tape_stop();
        tapeTransportState = "stop";
      } else if (action === "pause") {
        if (tapeTransportState === "pause") {
          startAudio();
          m._poc_tape_play();
          tapeTransportState = "play";
        } else {
          m._poc_tape_stop();
          tapeTransportState = "pause";
        }
      }
      updateTapeDeck();
    });
  }
  updateCartUi();
  clearTapeUi();

  async function fetchServerMedia(url, kind) {
    const name = JS1984Media.filenameFromUrl(url, kind);
    setStatus("Fetching " + kind + ": " + name);
    const response = await fetch(url);
    if (!response.ok)
      throw new Error(kind + " request returned HTTP " + response.status);
    const data = new Uint8Array(await response.arrayBuffer());
    if (!data.byteLength) throw new Error(kind + " response was empty");
    return { data, name };
  }

  async function bootstrapServerMedia() {
    let media;
    try {
      media = JS1984Media.parseStartupMedia(
        window.location.search,
        document.baseURI
      );
    } catch (error) {
      setStatus("Startup URL error: " + error.message);
      showToast("Invalid startup URL");
      return;
    }
    if (media.memoryKb !== null &&
        !applyMemorySize(media.memoryKb, false, false)) return;
    if (!media.diskA && !media.diskB && !media.cartridge) return;

    try {
      if (media.cartridge) {
        const cartridge = await fetchServerMedia(media.cartridge, "cartridge");
        mountCartridge(cartridge.data, cartridge.name, "/server-cartridge.cpr");
      }
      let diskA = null;
      if (media.diskA) {
        diskA = await fetchServerMedia(media.diskA, "drive A disk");
        mountDisk(0, diskA.data, diskA.name, "/server-drive-a.dsk");
      }
      if (media.diskB) {
        const diskB = await fetchServerMedia(media.diskB, "drive B disk");
        mountDisk(1, diskB.data, diskB.name, "/server-drive-b.dsk");
      }
      if (media.autorun) {
        m._poc_reset();
        m._poc_audio_reset();
        if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
        releaseAllJoy();
        m._poc_set_mouse(mouseEnabled ? 1 : 0);
        const rc = m.ccall(
          "poc_autorun",
          "number",
          ["string", "number"],
          [media.autorun, 42]
        );
        if (rc !== 0) throw new Error("invalid autorun filename");
        setStatus(
          "Drive A: " + diskA.name + " - autorun " + media.autorun + " armed"
        );
        showToast("Autorun " + media.autorun + " armed");
      }
    } catch (error) {
      setStatus("Server media failed: " + error.message);
      showToast("Could not load server media");
    }
  }

  // The emulator fills a stereo ring at 50 Hz. Schedule short buffers ahead
  // of the Web Audio clock so canvas work cannot starve playback.
  const AUDIO_CHUNK = 2048;
  function startAudio() {
    if (audioCtx) {
      audioCtx.resume();
      return;
    }
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    m._poc_audio_reset();
    audioState = { ringPtr: m._poc_audio_buffer(), ringSize: 44100 * 4 };
    nextAudioStart = audioCtx.currentTime + 0.3;
    audioCtx.resume().then(() => ledAudioEl.classList.add("on"));
  }

  function scheduleAudio() {
    if (!audioCtx || audioCtx.state !== "running") return;
    if (nextAudioStart < audioCtx.currentTime + 0.05)
      nextAudioStart = audioCtx.currentTime + 0.05;
    while (nextAudioStart - audioCtx.currentTime < 0.25) {
      const available = m._poc_audio_avail();
      const frames = Math.min(AUDIO_CHUNK, available >> 1);
      if (frames === 0) break;
      const readPosition = m._poc_audio_read_pos();
      const samples = new Int16Array(
        m.HEAPU8.buffer,
        audioState.ringPtr,
        audioState.ringSize
      );
      const buffer = audioCtx.createBuffer(2, AUDIO_CHUNK, 44100);
      const left = buffer.getChannelData(0);
      const right = buffer.getChannelData(1);
      for (let i = 0; i < frames; i++) {
        left[i] = samples[(readPosition + i * 2) % audioState.ringSize] / 32768;
        right[i] = samples[(readPosition + i * 2 + 1) % audioState.ringSize] / 32768;
      }
      m._poc_audio_advance(frames * 2);
      const source = audioCtx.createBufferSource();
      source.buffer = buffer;
      source.connect(audioCtx.destination);
      source.start(nextAudioStart);
      nextAudioStart += AUDIO_CHUNK / 44100;
    }
  }

  window.addEventListener("pointerdown", startAudio, { once: true });

  function setJoystickEnabled(enabled) {
    joyEnabled = enabled;
    joytoggleEl.checked = enabled;
    if (!enabled) releaseAllJoy();
    joystatusEl.textContent = enabled ? "Joystick: enabled" : "Joystick: disabled";
  }

  function setMouseEnabled(enabled) {
    mouseEnabled = enabled;
    mousetoggleEl.checked = enabled;
    m._poc_set_mouse(enabled ? 1 : 0);
    canvas.classList.toggle("mouse-ready", enabled);
    if (enabled) {
      setJoystickEnabled(false);
      mousestatusEl.textContent = "Mouse: click display to capture";
      $("screenHint").textContent = "Click display to capture AMX mouse";
    } else {
      if (document.pointerLockElement === canvas) document.exitPointerLock();
      mousestatusEl.textContent = "Mouse: disabled";
      $("screenHint").textContent = "Click display for keyboard";
    }
  }

  joytoggleEl.addEventListener("change", () => {
    if (joytoggleEl.checked && mouseEnabled) setMouseEnabled(false);
    setJoystickEnabled(joytoggleEl.checked);
  });
  mousetoggleEl.addEventListener("change", () => setMouseEnabled(mousetoggleEl.checked));

  function gamepadUnavailableReason() {
    if (!window.isSecureContext)
      return "Gamepad API requires HTTPS or localhost";
    if (typeof navigator.getGamepads !== "function")
      return "Gamepad API is unavailable in this browser";
    const policy = document.permissionsPolicy || document.featurePolicy;
    if (policy && typeof policy.allowsFeature === "function" && !policy.allowsFeature("gamepad"))
      return "Gamepad API is blocked by Permissions Policy";
    return "";
  }

  function updateCpcJoyStatus() {
    const row = m._poc_joy_matrix() & 0xff;
    const names = ["UP", "DOWN", "LEFT", "RIGHT", "F1", "F2"];
    const active = names.filter((_, column) => !(row & (1 << column)));
    joymatrixEl.textContent = "CPC joystick: " +
      (active.length ? active.join(" ") : "idle") +
      " (row 9 = 0x" + row.toString(16).padStart(2, "0").toUpperCase() + ")";
  }

  function pollGamepad() {
    const unavailable = gamepadUnavailableReason();
    if (unavailable) {
      joystatusEl.textContent = "Joystick unavailable: " + unavailable;
      return;
    }
    let pads;
    try {
      pads = navigator.getGamepads();
    } catch (error) {
      joystatusEl.textContent = "Joystick unavailable: " + error.message;
      return;
    }
    let gamepad = null;
    for (const pad of pads) {
      if (pad && pad.connected) {
        gamepad = pad;
        break;
      }
    }
    if (!gamepad) {
      if (prevGamepad) releaseAllJoy();
      joystatusEl.textContent = "Joystick: no controller exposed";
      updateCpcJoyStatus();
      return;
    }

    const mapped = JS1984Gamepad.mapGamepad(gamepad);
    const state = mapped.state;
    const names = ["UP", "DOWN", "LEFT", "RIGHT", "F1", "F2"];
    if (!joyEnabled) {
      if (prevGamepad) releaseAllJoy();
      return;
    }

    if (state.some(Boolean)) {
      joystatusEl.textContent = "Joystick [" + mapped.profile + "]: " +
        names.filter((_, column) => state[column]).join(" ");
      pulseInputLed();
    } else {
      const rawButtons = [];
      const rawAxes = [];
      for (let i = 0; i < gamepad.buttons.length; i++) {
        const button = gamepad.buttons[i];
        if (button && (button.pressed || button.value > 0.5)) rawButtons.push(i);
      }
      for (let i = 0; i < gamepad.axes.length; i++) {
        if (Math.abs(gamepad.axes[i]) > 0.5) rawAxes.push(i + "=" + gamepad.axes[i].toFixed(2));
      }
      joystatusEl.textContent = rawButtons.length || rawAxes.length
        ? "Joystick raw: B " + (rawButtons.join(",") || "-") + " / A " + (rawAxes.join(",") || "-")
        : "Joystick: " + gamepad.id;
    }

    if (prevGamepad) {
      for (let column = 0; column < 6; column++) {
        if (prevGamepad[column] !== state[column]) m._poc_joy(column, state[column]);
      }
    } else {
      for (let column = 0; column < 6; column++) {
        if (state[column]) m._poc_joy(column, 1);
      }
    }
    prevGamepad = state;
    updateCpcJoyStatus();
  }

  window.addEventListener("gamepadconnected", event => {
    joystatusEl.textContent = "Joystick: connected " + event.gamepad.id;
    showToast("Game controller connected");
  });
  window.addEventListener("gamepaddisconnected", () => {
    releaseAllJoy();
    joystatusEl.textContent = "Joystick: disconnected";
  });
  window.addEventListener("focus", pollGamepad);
  $("joydetect").addEventListener("click", () => {
    startAudio();
    pollGamepad();
    showToast("Scanning browser game controllers");
  });
  setInterval(pollGamepad, 100);

  canvas.addEventListener("click", () => {
    canvas.focus();
    startAudio();
    if (mouseEnabled && document.pointerLockElement !== canvas)
      canvas.requestPointerLock();
  });
  canvas.addEventListener("contextmenu", event => event.preventDefault());
  document.addEventListener("pointerlockchange", () => {
    const captured = document.pointerLockElement === canvas;
    canvas.classList.toggle("mouse-captured", captured);
    if (mouseEnabled) {
      mousestatusEl.textContent = captured
        ? "Mouse: captured (Esc releases)"
        : "Mouse: click display to capture";
    }
  });
  document.addEventListener("mousemove", event => {
    if (!mouseEnabled || document.pointerLockElement !== canvas) return;
    m._poc_mouse_move(event.movementX, event.movementY);
    if (event.movementX || event.movementY) pulseInputLed();
  });
  document.addEventListener("mousedown", event => {
    if (!mouseEnabled || document.pointerLockElement !== canvas) return;
    const button = event.button === 2 ? 1 : event.button;
    if (button < 2) m._poc_mouse_button(button, 1);
    pulseInputLed();
    event.preventDefault();
  });
  document.addEventListener("mouseup", event => {
    if (!mouseEnabled || document.pointerLockElement !== canvas) return;
    const button = event.button === 2 ? 1 : event.button;
    if (button < 2) m._poc_mouse_button(button, 0);
    event.preventDefault();
  });

  window.addEventListener("keydown", event => {
    const scancode = CODE2SCAN[event.code];
    if (scancode === undefined || document.activeElement !== canvas) return;
    event.preventDefault();
    startAudio();
    if (!heldKeys.has(scancode)) {
      const alreadyPressed = virtualKeys.has(scancode);
      heldKeys.add(scancode);
      if (!alreadyPressed) m._poc_key(scancode, 1);
      pulseInputLed();
    }
  });
  window.addEventListener("keyup", event => {
    const scancode = CODE2SCAN[event.code];
    if (scancode === undefined || !heldKeys.has(scancode)) return;
    event.preventDefault();
    heldKeys.delete(scancode);
    if (!virtualKeys.has(scancode)) m._poc_key(scancode, 0);
  });
  canvas.addEventListener("blur", () => {
    for (const scancode of heldKeys) {
      if (!virtualKeys.has(scancode)) m._poc_key(scancode, 0);
    }
    heldKeys.clear();
  });
  window.addEventListener("blur", releaseAllVirtualKeys);

  for (const eventName of ["dragenter", "dragover"]) {
    screenFrame.addEventListener(eventName, event => {
      event.preventDefault();
      screenFrame.classList.add("dragging");
    });
  }
  for (const eventName of ["dragleave", "drop"]) {
    screenFrame.addEventListener(eventName, event => {
      event.preventDefault();
      screenFrame.classList.remove("dragging");
    });
  }
  screenFrame.addEventListener("drop", event => {
    const file = event.dataTransfer.files[0];
    if (!file) return;
    const lowerName = file.name.toLowerCase();
    if (lowerName.endsWith(".dsk")) loadDiskFile(file, 0);
    else if (lowerName.endsWith(".cpr")) loadCartridgeFile(file);
    else if (lowerName.endsWith(".sna")) loadSnapshotFile(file);
    else if (lowerName.endsWith(".cdt")) loadTapeFile(file);
    else showToast("Use a DSK, CDT, CPR, or SNA file");
  });

  function updateLed() {
    const elements = [ledAEl, ledBEl];
    for (let drive = 0; drive < elements.length; drive++) {
      const on = m._poc_disk_motor(drive);
      if (on !== ledState[drive]) {
        ledState[drive] = on;
        elements[drive].classList.toggle("on", Boolean(on));
      }
    }
  }

  let lastFrame = 0;
  let m4SdLedTimer = 0;
  let m4NetLedTimer = 0;
  function updateM4Activity() {
    if (m._poc_m4_sd_activity()) {
      m4SdLedEl.classList.add("on");
      clearTimeout(m4SdLedTimer);
      m4SdLedTimer = setTimeout(() => m4SdLedEl.classList.remove("on"), 120);
    }
    if (m._poc_m4_net_activity()) {
      m4RelayLampEl.classList.add("activity");
      clearTimeout(m4NetLedTimer);
      m4NetLedTimer = setTimeout(() => m4RelayLampEl.classList.remove("activity"), 120);
    }
  }

  function frame(time) {
    while (time - lastFrame >= 20) {
      m._poc_step();
      lastFrame += 20;
      scheduleAudio();
      pollGamepad();
      updateLed();
      updateTapeDeck();
      updateM4Activity();
    }

    pollMlWriteEvents();
    mlDap.sync();
    if (drainMlDapEvents()) {
      updateMlState(true);
    } else if (!mlMonitorPanelEl.hidden && time - mlLastRefresh >= 150) {
      mlLastRefresh = time;
      updateMlState(true);
    }

    const pixels = m.HEAPU32.subarray(framebuffer >> 2, (framebuffer >> 2) + W * H);
    for (let i = 0, destination = 0; i < W * H; i++, destination += 4) {
      const color = pixels[i];
      const red = crtRedLut[(color >> 16) & 0xff];
      const green = crtGreenLut[(color >> 8) & 0xff];
      const blue = crtBlueLut[color & 0xff];
      if (monochromeGreen) {
        // Rec. 709 integer luminance mapped onto a green phosphor response.
        const luminance = (red * 54 + green * 183 + blue * 19) >> 8;
        image.data[destination] = (luminance * 7) >> 5;
        image.data[destination + 1] = Math.min(255, (luminance * 5) >> 2);
        image.data[destination + 2] = (luminance * 11) >> 5;
      } else {
        image.data[destination] = red;
        image.data[destination + 1] = green;
        image.data[destination + 2] = blue;
      }
      image.data[destination + 3] = 0xff;
    }
    offctx.putImageData(image, 0, 0);
    ctx.imageSmoothingEnabled = !pixelSharp;
    ctx.drawImage(offscreen, 0, 0, W, H, 0, 0, VW, VH);
    requestAnimationFrame(frame);
  }

  requestAnimationFrame(frame);
  // Apply persisted expansion settings now that the core module is ready.
  buildRomSlotRows();
  renderRomSlots();
  applyM4Hardware(m4Enabled);
  applyPowergraphHardware(powergraphEnabled);
  applyPowergraphOutputHardware(powergraphOutput);
  applyRomBoard(romBoardEnabled, { reset: false });
  setM4NetEnabled(m4NetEnabled, false);
  bootstrapServerMedia();
}).catch(error => {
  setStatus("Failed to start: " + error);
});
