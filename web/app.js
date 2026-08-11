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
let toastTimer = 0;
let inputLedTimer = 0;

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

function setCpcKeyboardOpen(open) {
  cpcKeyboardEl.dataset.keyboardOpen = String(open);
  cpcKeyboardKeysEl.hidden = !open;
  cpcKeyboardToggleEl.setAttribute("aria-expanded", String(open));
  cpcKeyboardToggleEl.textContent = open ? "Hide keyboard" : "Show keyboard";
}

cpcKeyboardToggleEl.addEventListener("click", () => {
  setCpcKeyboardOpen(cpcKeyboardKeysEl.hidden);
});
setCpcKeyboardOpen(false);

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
    (monochromeGreen ? "Green" : "Color");
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
$("expansion").addEventListener("click", () => {
  showToast("Expansion bay reserved for future browser devices");
});
setScreenScale(100);
updatePixelMode();
let savedDisplayMode = "color";
try {
  savedDisplayMode = localStorage.getItem(DISPLAY_MODE_STORAGE_KEY) || "color";
} catch (_) {
  // Keep the color display when storage access is unavailable.
}
setDisplayColorMode(savedDisplayMode === "green", false);

create6128().then(m => {
  if (m._poc_init() !== 0) {
    setStatus("Emulator initialization failed");
    return;
  }

  ledPowerEl.classList.add("on");
  setStatus("CPC 6128 booting - click the display for keyboard focus");

  const framebuffer = m._poc_pixels();
  const modelEl = $("model");
  const resetEl = $("reset");
  const diskfileEl = $("diskfile");
  const disknameEl = $("diskname");
  const diskEjectEl = $("diskEject");
  const cartfileEl = $("cartfile");
  const cartnameEl = $("cartname");
  const cartSlotEl = $("cartSlot");
  const cartLoadEl = $("cartLoad");
  const cartDefaultEl = $("cartDefault");
  const joytoggleEl = $("joytoggle");
  const mousetoggleEl = $("mousetoggle");
  const joystatusEl = $("joystatus");
  const joymatrixEl = $("joymatrix");
  const mousestatusEl = $("mousestatus");

  let currentModel = 0;
  let audioCtx = null;
  let audioState = null;
  let nextAudioStart = 0;
  let prevGamepad = null;
  let joyEnabled = true;
  let mouseEnabled = false;
  let ledState = 0;
  const heldKeys = new Set();
  const virtualKeys = new Set();
  const latchedVirtualModifiers = new Set();

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

  function clearDiskUi() {
    disknameEl.textContent = "No disk loaded";
    diskEjectEl.disabled = true;
    diskfileEl.value = "";
  }

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
    m._poc_audio_reset();
    if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
    releaseAllJoy();
    m._poc_set_mouse(mouseEnabled ? 1 : 0);
    clearDiskUi();
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

  resetEl.addEventListener("click", () => {
    m._poc_reset();
    m._poc_audio_reset();
    if (audioCtx) nextAudioStart = audioCtx.currentTime + 0.3;
    releaseAllJoy();
    m._poc_set_mouse(mouseEnabled ? 1 : 0);
    setStatus("Warm reset complete");
    showToast("CPC reset");
    canvas.focus();
  });

  function mountDisk(data, name, path) {
    m.FS.writeFile(path, data);
    const rc = m.ccall("poc_load_disk", "number", ["string"], [path]);
    if (rc !== 0) throw new Error("unsupported or damaged disk image");
    disknameEl.textContent = name;
    diskEjectEl.disabled = false;
    setStatus("Drive A: " + name);
    showToast("Disk loaded into Drive A");
  }

  function mountCartridge(data, name, path) {
    m.FS.writeFile(path, data);
    if (!reinit(1, path)) throw new Error("unsupported or damaged cartridge");
    cartnameEl.textContent = name;
    updateCartUi();
    setStatus("Cartridge: " + name);
    showToast("Cartridge loaded and CPC 6128 Plus started");
  }

  async function loadDiskFile(file) {
    if (!file) return;
    try {
      const data = new Uint8Array(await file.arrayBuffer());
      mountDisk(data, file.name, "/disk.dsk");
    } catch (error) {
      setStatus("Disk load failed: " + error.message);
      showToast("Could not load " + file.name);
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

  diskfileEl.addEventListener("change", () => loadDiskFile(diskfileEl.files[0]));
  diskEjectEl.addEventListener("click", () => {
    m._poc_eject_disk();
    clearDiskUi();
    setStatus("Drive A ejected");
  });
  cartfileEl.addEventListener("change", () => loadCartridgeFile(cartfileEl.files[0]));
  cartDefaultEl.addEventListener("click", () => {
    if (reinit(1)) {
      cartnameEl.textContent = "system.cpr";
      setStatus("Default system cartridge restored");
    }
  });
  updateCartUi();

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
      setStatus("Media URL error: " + error.message);
      showToast("Invalid server media URL");
      return;
    }
    if (!media.disk && !media.cartridge) return;

    try {
      if (media.cartridge) {
        const cartridge = await fetchServerMedia(media.cartridge, "cartridge");
        mountCartridge(cartridge.data, cartridge.name, "/server-cartridge.cpr");
      }
      if (media.disk) {
        const disk = await fetchServerMedia(media.disk, "disk");
        mountDisk(disk.data, disk.name, "/server-disk.dsk");
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
            "Drive A: " + disk.name + " - autorun " + media.autorun + " armed"
          );
          showToast("Autorun " + media.autorun + " armed");
        }
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
    if (lowerName.endsWith(".dsk")) loadDiskFile(file);
    else if (lowerName.endsWith(".cpr")) loadCartridgeFile(file);
    else showToast("Use a DSK disk or CPR cartridge image");
  });

  function updateLed() {
    const on = m._poc_disk_motor();
    if (on !== ledState) {
      ledState = on;
      ledAEl.classList.toggle("on", Boolean(on));
    }
  }

  let lastFrame = 0;
  function frame(time) {
    while (time - lastFrame >= 20) {
      m._poc_step();
      lastFrame += 20;
      scheduleAudio();
      pollGamepad();
      updateLed();
    }

    const pixels = m.HEAPU32.subarray(framebuffer >> 2, (framebuffer >> 2) + W * H);
    for (let i = 0, destination = 0; i < W * H; i++, destination += 4) {
      const color = pixels[i];
      const red = (color >> 16) & 0xff;
      const green = (color >> 8) & 0xff;
      const blue = color & 0xff;
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
  bootstrapServerMedia();
}).catch(error => {
  setStatus("Failed to start: " + error);
});
