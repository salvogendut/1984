# Building and distributing the Flatpak

The Flatpak manifest, `io.github.salvogendut.Emulator1984.yml`, lives in the
repository root on `main`. There is **no separate `flatpak` branch** — the
manifest builds the emulator straight from `main`, so everything needed to
produce a Flatpak ships with the code it packages.

## Manifest at a glance

- **Runtime:** `org.freedesktop.Platform // 24.08` (SDK `org.freedesktop.Sdk//24.08`).
- **Modules:**
  1. `sdl3` — SDL `release-3.4.10` built from source (the freedesktop runtime
     has no SDL3), shared library only, tests/examples stripped.
  2. `emulator-1984` — `autoreconf -fi && ./configure --prefix=/app && make &&
     make install`, sourced via `type: git` from this repository's `main`
     branch.
- **`command: '1984'`** — quoted on purpose. Unquoted, YAML parses `1984` as an
  integer and flatpak-builder rejects it.
- **Permissions** (`finish-args`): Wayland + X11 fallback, PulseAudio, DRI,
  network, `--device=all` (gamepads), and `--filesystem=home` (disk/ROM/SD
  images the user points at).

Because the `emulator-1984` module pulls `branch: main` over git, a Flatpak
build always packages the latest pushed `main`. Push your changes first, then
build.

## Build a local bundle

Requires `flatpak` and `flatpak-builder`, plus the freedesktop 24.08 runtime/SDK:

```bash
flatpak install -y flathub org.freedesktop.Platform//24.08 org.freedesktop.Sdk//24.08

# Build + install into the user installation in one step:
flatpak-builder --user --install --force-clean build-dir \
    io.github.salvogendut.Emulator1984.yml
flatpak run io.github.salvogendut.Emulator1984
```

To produce a single-file, shareable bundle instead:

```bash
flatpak-builder --force-clean --repo=flatpak-repo build-dir \
    io.github.salvogendut.Emulator1984.yml
flatpak build-bundle flatpak-repo 1984.flatpak io.github.salvogendut.Emulator1984
# Install it anywhere with:
flatpak install --user 1984.flatpak
```

All of `build-dir/`, `flatpak-repo/`, `.flatpak-builder/`, and `*.flatpak` are
git-ignored.

## CI

`.github/workflows/build.yml` has a `flatpak` job that builds the bundle in the
`bilelmoussaoui/flatpak-github-actions:freedesktop-24.08` container via the
`flatpak/flatpak-github-actions/flatpak-builder` action. It runs on:

- pushes to `main`,
- version tags (`v*`),
- manual **workflow_dispatch** (Actions tab → *build* → *Run workflow*).

It is **skipped on pull requests** — the manifest builds `branch: main`, so a PR
build wouldn't reflect the PR's own changes, and the `Linux (Fedora)` job
already catches compile breakage. To validate a manifest change before merging,
trigger the workflow manually from your branch via workflow_dispatch.

On a `v*` tag the `release` job attaches the bundle to the GitHub Release as
`1984-<tag>-x86_64.flatpak`, alongside the Linux and Windows assets.
