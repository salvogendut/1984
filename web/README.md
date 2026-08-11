# Javascript 1984

Build the browser edition with Emscripten and serve the publish directory:

    make -C web
    python3 -m http.server 8080 --directory web/dist

The default CPC464 theme presents the emulator as a dark GT65 monitor above a
CPC 464 chassis. Its optional on-screen keyboard and visual tape-deck
placeholder expand and collapse together, and are collapsed by default. Retro
CRT, Sapporo, and Sapporo Dark remain available from the theme selector.

A theme can be selected in a link using its case-insensitive display name:

    http://localhost:8080/?theme=CPC464
    http://localhost:8080/?theme=Sapporo%20Dark

## Server-hosted media

Media URLs are resolved relative to the page URL. A disk can be mounted in
drive A at startup:

    http://localhost:8080/?disk=media/thisdisk.dsk

Add `autorun` to reset the CPC after mounting and inject `RUN"filename"` after
the same 42-frame boot delay used by native 1984:

    http://localhost:8080/?disk=media/thisdisk.dsk&autorun=disc.bas

A CPR cartridge URL starts the CPC 6128 Plus model:

    http://localhost:8080/?cartridge=media/sonic.cpr

Parameter values containing spaces or other reserved characters must be URL
encoded. Media must be available over HTTP or HTTPS. Cross-origin servers must
permit the request with CORS headers.

The fetched image lives in the browser's in-memory filesystem. Guest writes
are not uploaded to the server.
