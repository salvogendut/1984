# Javascript 1984

Javascript 1984 is the standalone, client-side WebAssembly edition of 1984.
It compiles the same C emulation core used by the SDL3 application and wraps
it in a static HTML, CSS, and JavaScript interface. It is separate from the
native application's streaming Web GUI and multi-session Web Service: no 1984
process runs on the server after the files have been published.

## Use the hosted edition

Open **[Javascript 1984](https://salvogendut.github.io/chimeric/js1984/)** to
run it directly. No download or installation is required. Click the CPC
display to give it keyboard focus, then use the Media Deck to select local DSK
images independently for drives A and B, or load a CPR or SNA file. Local files
and guest changes stay in browser memory and are not uploaded by the
application.

## Current capabilities

- CPC 6128 and CPC 6128 Plus machines using the bundled firmware and system
  cartridge.
- Independent local DSK loading in drives A and B, CPR cartridge loading on
  the Plus model, and SNA v1-v3 snapshot loading with v3 snapshot downloads,
  including chunked RASM supersnapshots, embedded ROMs, and REMU labels,
  comments, and breakpoints.
- Server-hosted DSK and CPR media selected through URL parameters.
- CPC keyboard input from the physical keyboard or the collapsible on-screen
  keyboard, including latched Shift, Ctrl, and Copy modifiers.
- Browser Gamepad API joystick input and optional AMX mouse pointer capture.
- Stereo Web Audio, fullscreen, display scaling, sharp or smooth pixels,
  colour or green-monochrome output, and persistent CRT scanline, brightness,
  contrast, and RGB-gain adjustments.
- CDT cassette loading through the CPC464 data recorder, with working Play,
  Stop, Pause, Rewind, and block-forward controls, a tape counter, cassette
  audio, and motor-driven reel animation.
- CPC464, Retro CRT, Sapporo, and Sapporo Dark themes.
- A collapsible machine-language monitor in the CPC464 theme with Z80
  registers and disassembly, breakpoints, continue, Step In, Step Out,
  one-instruction Step Back, labelled memory-write notifications, and mapped
  memory-slice reading and writing.

The browser frontend does not currently expose the native F9 overlay,
expansion devices, cassette recording, or additional CPC models. Disk and
tape changes are held in browser memory and are lost when the page is
reloaded; they are not uploaded to the server.

## Build and self-host

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
Click the cassette door to load a CDT image. Press **PLAY**, then start the
CPC tape loader; the transport waits for the CPC motor before advancing.
**REW** returns to the beginning and **F.FWD** skips the current CDT block.

Enable **Joystick** and press **Detect controller** after connecting a
gamepad. Whether a device is available depends on the browser and its sandbox
permissions. Enable **Mouse** and click the display to capture the pointer;
press Escape to release browser pointer lock.

The media deck has separate local DSK file choosers and eject controls for
drives A and B, plus CPR and SNA file controls. Dropping a DSK on the display
loads it into drive A. **Save SNA** downloads the current machine state as an
Amstrad SNA v3 file. Reset, fullscreen, fit-to-window, pixel filtering,
display size, and colour mode are available from the front panel. The six
controls under the display adjust scanline visibility, brightness, contrast,
and individual red, green, and blue gain. Their values are retained in browser
local storage.

## ML Monitor

The **ML Monitor** handle on the right edge of the CPC464 theme opens a
hardware-styled diagnostic panel. It opens without interrupting the CPC and
reserves no page width while collapsed. Its in-process adapter follows Debug
Adapter Protocol 1.71.0 request, response, event, lifetime, and `Content-Length`
framing rules. Use **BREAK** to pause between Z80 instructions before
inspecting registers, disassembling, or accessing memory; **CONT** resumes
execution.

Breakpoints accept hexadecimal CPU addresses and remain armed across a warm
reset. Step In executes one instruction and Step Over uses the DAP `next`
request; both create a single rollback point for Step Back. Step Out runs to the
return address currently found at the top of the Z80 stack; this is intended
for use immediately after entering a CALL or where SP points at the routine
return address. BREAK cancels an in-progress Step Over or Step Out.

The label write detector associates a descriptive name with a CPU address and
reports each write with its old value, new value, and writer PC. Its event ring
retains the latest 64 writes between browser polls. Memory Slice reads up to
256 mapped bytes and accepts whitespace- or comma-separated hexadecimal bytes
for writing. Reads and writes are available only while the CPU is paused.

The adapter advertises only implemented capabilities: configuration completion,
instruction breakpoints, instruction-granularity stepping, one-level reverse
stepping, disassembly, and base64 memory reads/writes. A CPC exposes one
`Z80 CPU` thread, one synthetic stack frame, and a register scope. Frame and
variable references expire whenever execution resumes. Label watches do not
stop execution, so they are deliberately reported as DAP output and memory
telemetry rather than falsely advertising data-breakpoint support.

Loading a RASM supersnapshot maps its `LOWR` and `RMxx` ROM chunks and imports
`REMU` labels and source comments into the disassembly. Embedded execution
breakpoints remain dormant until explicitly added in the ML Monitor, so
assembler debug state cannot stop ordinary snapshot playback. Snapshot
downloads preserve the ROM context, supported metadata, and debug records or
chunks that 1984 does not yet execute.
See [SNA REMU Debug Metadata](../docs/SNA-REMU.md).

`dap.js` includes an incremental parser and serializer for standard
`Content-Length` framed UTF-8 JSON messages. The browser UI currently uses the
same protocol engine in process; it does not yet expose a WebSocket, TCP, or
stdio endpoint to an external IDE. See the official
[Debug Adapter Protocol overview](https://microsoft.github.io/debug-adapter-protocol/overview)
and [protocol schema](https://github.com/microsoft/debug-adapter-protocol/blob/main/debugAdapterProtocol.json).

Protocol unit tests and a compiled-core integration test are available with:

```bash
make -C web test
make -C web test-wasm
```

## Themes

The default CPC464 theme presents a dark GT65 monitor above a CPC 464 chassis.
Themes can be selected from the front panel or with a case-insensitive URL
parameter:

```text
https://salvogendut.github.io/chimeric/js1984/?theme=CPC464
https://salvogendut.github.io/chimeric/js1984/?theme=Retro%20CRT
https://salvogendut.github.io/chimeric/js1984/?theme=Sapporo%20Dark
```

An unknown theme name falls back to CPC464. A front-panel choice is retained
in browser local storage; a URL parameter overrides it for that page load.

## Memory size

Use the `memory` parameter to select one of the RAM sizes supported by the
front-panel slider at startup:

```text
http://localhost:8080/?memory=128
http://localhost:8080/?memory=256
http://localhost:8080/?memory=512
http://localhost:8080/?memory=1024
```

The value is in kilobytes. An unsupported value is rejected instead of being
silently rounded. Memory, theme, and media parameters can be combined.

## Server-hosted media

Media URLs are resolved relative to the page URL. Use `diska` or `diskb` to
mount a disk in either drive at startup:

```text
http://localhost:8080/?diska=media/system.dsk
http://localhost:8080/?diskb=media/data.dsk
```

Both drives can be populated by one URL:

```text
http://localhost:8080/?diska=media/system.dsk&diskb=media/data.dsk
```

Add `autorun` alongside `diska` to reset the CPC after mounting both images and
inject `RUN"filename"` after the same 42-frame boot delay used by native 1984:

```text
http://localhost:8080/?memory=512&diska=media/system.dsk&diskb=media/data.dsk&autorun=disc.bas
```

The former `disk` parameter remains accepted as a compatibility alias for
`diska`, but new links should use the drive-specific spelling.

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
