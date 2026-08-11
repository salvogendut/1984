# Javascript 1984

Javascript 1984 is the standalone, client-side WebAssembly edition of 1984.
It compiles the same C emulation core used by the SDL3 application and wraps
it in a static HTML, CSS, and JavaScript interface. It is separate from the
native application's streaming Web GUI and multi-session Web Service: no 1984
process runs on the server after the files have been published.

## Current capabilities

- CPC 6128 and CPC 6128 Plus machines using the bundled firmware and system
  cartridge.
- Local DSK loading in drive A and CPR cartridge loading on the Plus model.
- Server-hosted DSK and CPR media selected through URL parameters.
- CPC keyboard input from the physical keyboard or the collapsible on-screen
  keyboard, including latched Shift, Ctrl, and Copy modifiers.
- Browser Gamepad API joystick input and optional AMX mouse pointer capture.
- Stereo Web Audio, fullscreen, display scaling, sharp or smooth pixels, and
  colour or green-monochrome output.
- CPC464, Retro CRT, Sapporo, and Sapporo Dark themes.

The browser frontend does not currently expose the native F9 overlay,
expansion devices, tape emulation, snapshots, or additional CPC models. The
CPC464 tape deck in the interface is a placeholder for future tape support.
Disk writes and fetched media changes are held in browser memory and are lost
when the page is reloaded; they are not uploaded to the server.

## Build and publish

Install Emscripten so `emcc` is available, then build from the repository root:

```bash
make -C web
```

The complete publishable application is written to `web/dist/`. Serve that
directory over HTTP or HTTPS; loading `index.html` directly through a `file:`
URL is not supported.

```bash
python3 -m http.server 8080 --directory web/dist
```

Open <http://localhost:8080/>. To deploy it elsewhere, publish the contents of
`web/dist/`, including `6128.js`, `6128.wasm`, the CSS themes, JavaScript
helpers, and image assets. The server should return `.wasm` files as
`application/wasm`.

## Controls

Click the CPC display before using the physical keyboard. Browser audio also
starts after a user gesture, as required by modern autoplay policies. The
**Show keyboard** button expands the on-screen CPC keyboard and tape-deck
assembly; **Hide keyboard** collapses both. On-screen Shift, Ctrl, and Copy
latch until the next key, which makes combinations practical with a mouse.

Enable **Joystick** and press **Detect controller** after connecting a
gamepad. Whether a device is available depends on the browser and its sandbox
permissions. Enable **Mouse** and click the display to capture the pointer;
press Escape to release browser pointer lock.

The media deck loads local DSK and CPR files using the browser file chooser.
Reset, fullscreen, fit-to-window, pixel filtering, display size, and colour
mode are available from the front panel.

## Themes

The default CPC464 theme presents a dark GT65 monitor above a CPC 464 chassis.
Themes can be selected from the front panel or with a case-insensitive URL
parameter:

```text
http://localhost:8080/?theme=CPC464
http://localhost:8080/?theme=Retro%20CRT
http://localhost:8080/?theme=Sapporo%20Dark
```

An unknown theme name falls back to CPC464. A front-panel choice is retained
in browser local storage; a URL parameter overrides it for that page load.

## Server-hosted media

Media URLs are resolved relative to the page URL. A disk can be mounted in
drive A at startup:

```text
http://localhost:8080/?disk=media/thisdisk.dsk
```

Add `autorun` to reset the CPC after mounting and inject `RUN"filename"` after
the same 42-frame boot delay used by native 1984:

```text
http://localhost:8080/?disk=media/thisdisk.dsk&autorun=disc.bas
```

A CPR cartridge URL starts the CPC 6128 Plus model:

```text
http://localhost:8080/?cartridge=media/sonic.cpr
```

Theme and media parameters can be combined in the same URL. Parameter values
containing spaces or other reserved characters must be URL encoded. Media
must be available over HTTP or HTTPS. Cross-origin servers must permit the
request with CORS headers.

The fetched image lives in the browser's in-memory filesystem. Guest writes
are not uploaded to the server.
