(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  root.JS1984Gamepad = api;
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const STICK_DEADZONE = 0.5;
  const PRESSURE_DEADZONE = 0.1;

  function buttonPressed(gamepad, index) {
    const button = gamepad.buttons && gamepad.buttons[index];
    return !!button && (button.pressed || button.value > 0.5);
  }

  function axisValue(gamepad, index) {
    const value = gamepad.axes && gamepad.axes[index];
    return Number.isFinite(value) ? value : 0;
  }

  function pressureAxisPressed(gamepad, index) {
    return axisValue(gamepad, index) > PRESSURE_DEADZONE;
  }

  function addLeftStick(gamepad, state) {
    const x = axisValue(gamepad, 0);
    const y = axisValue(gamepad, 1);
    if (y < -STICK_DEADZONE) state[0] = 1;
    if (y >  STICK_DEADZONE) state[1] = 1;
    if (x < -STICK_DEADZONE) state[2] = 1;
    if (x >  STICK_DEADZONE) state[3] = 1;
  }

  function isPs3(gamepad) {
    const id = String(gamepad.id || '').toLowerCase();
    return (id.includes('054c') && id.includes('0268')) ||
      /playstation\s*\(r\)\s*3|playstation.*3|dualshock.*3|sixaxis/.test(id);
  }

  function selectProfile(gamepad) {
    if (gamepad.mapping === 'standard') return 'standard';
    if (isPs3(gamepad)) {
      /* Linux kernels before 4.12 expose PS3 pressure buttons as axes 8-13.
       * Newer hid-sony/hid-playstation layouts expose the D-pad as buttons
       * 13-16. Chromium uses the same axis-count distinction internally. */
      return gamepad.axes && gamepad.axes.length >= 14
        ? 'ps3-legacy-raw'
        : 'ps3-raw';
    }
    return 'generic-raw';
  }

  function mapStandard(gamepad, state) {
    state[0] |= buttonPressed(gamepad, 12);
    state[1] |= buttonPressed(gamepad, 13);
    state[2] |= buttonPressed(gamepad, 14);
    state[3] |= buttonPressed(gamepad, 15);
    state[4] |= buttonPressed(gamepad, 0);
    state[5] |= buttonPressed(gamepad, 1) ||
      buttonPressed(gamepad, 2) || buttonPressed(gamepad, 3);
  }

  function mapPs3Raw(gamepad, state) {
    state[0] |= buttonPressed(gamepad, 13);
    state[1] |= buttonPressed(gamepad, 14);
    state[2] |= buttonPressed(gamepad, 15);
    state[3] |= buttonPressed(gamepad, 16);
    state[4] |= buttonPressed(gamepad, 0);  // Cross
    state[5] |= buttonPressed(gamepad, 1) || buttonPressed(gamepad, 2) ||
      buttonPressed(gamepad, 3);           // Circle/Triangle/Square
  }

  function mapPs3LegacyRaw(gamepad, state) {
    state[0] |= pressureAxisPressed(gamepad, 8);
    state[1] |= pressureAxisPressed(gamepad, 10);
    state[2] |= buttonPressed(gamepad, 7);
    state[3] |= pressureAxisPressed(gamepad, 9);
    state[4] |= buttonPressed(gamepad, 14); // Cross
    state[5] |= buttonPressed(gamepad, 12) || buttonPressed(gamepad, 13) ||
      buttonPressed(gamepad, 15);          // Triangle/Circle/Square
  }

  function mapGenericRaw(gamepad, state) {
    /* Common raw HID layouts either retain standard D-pad button indices or
     * expose a two-axis hat at axes 6/7. The left stick is handled for every
     * profile below. */
    mapStandard(gamepad, state);
    const hatX = axisValue(gamepad, 6);
    const hatY = axisValue(gamepad, 7);
    if (hatY < -STICK_DEADZONE) state[0] = 1;
    if (hatY >  STICK_DEADZONE) state[1] = 1;
    if (hatX < -STICK_DEADZONE) state[2] = 1;
    if (hatX >  STICK_DEADZONE) state[3] = 1;
  }

  function mapGamepad(gamepad) {
    const state = [0, 0, 0, 0, 0, 0];
    const profile = selectProfile(gamepad);
    addLeftStick(gamepad, state);
    if (profile === 'standard') mapStandard(gamepad, state);
    else if (profile === 'ps3-raw') mapPs3Raw(gamepad, state);
    else if (profile === 'ps3-legacy-raw') mapPs3LegacyRaw(gamepad, state);
    else mapGenericRaw(gamepad, state);
    return { profile, state };
  }

  return { mapGamepad, selectProfile };
}));
