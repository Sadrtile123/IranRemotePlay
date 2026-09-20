# Dependencies

## Vendored in-tree (no user action needed)

| Library          | Version | License              | Purpose                          |
|------------------|---------|----------------------|----------------------------------|
| Asio (standalone)| 1.30.2  | Boost Software License 1.0 | TCP/UDP networking (header-only) |
| nlohmann/json    | 3.11.3  | MIT                  | JSON configuration file          |

Both live under `third_party/` and are compiled into the project; users do
not install anything. Their license files are included next to the headers
(`third_party/asio/LICENSE_1_0.txt`; the nlohmann single header embeds its
MIT license text).

## Required on the build machine

| Component        | Version    | License        | Notes                             |
|------------------|------------|----------------|-----------------------------------|
| Visual Studio 2022 (MSVC v143) | 17.x | Proprietary (free Community edition) | "Desktop development with C++" workload |
| Qt 6             | 6.5+ (6.8 recommended) | LGPLv3 (dynamic linking) | Qt Widgets only; installed via the Qt online installer, `vcpkg`, or `aqtinstall` |
| CMake            | 3.22+      | BSD            | Bundled with VS 2022              |

The application dynamically links Qt (`Qt6Widgets.dll` and friends). For
developer builds, run `windeployqt` once to collect the DLLs next to the
executable (see `docs/BUILDING.md`); the Phase 17 installer bundles them
properly with the LGPL attribution.

## Planned additions by phase

| Phase | Dependency | License | Purpose |
|-------|-----------|---------|---------|
| 2 | Windows Graphics Capture, D3D11 | Windows SDK | GPU capture without CPU copies |
| 3 | FFmpeg (avcodec, avutil) | LGPL 2.1+ (shared build) | NVENC/AMF/QSV hardware encode via avcodec; H.264 default |
| 5 | FFmpeg | LGPL 2.1+ | Hardware decode (D3D11VA/DXVA2/NVDEC) |
| 6-7 | WASAPI (Windows SDK), Opus | BSD-3 | Low-latency 48 kHz stereo audio |
| 10 | Virtual gamepad (e.g. ViGEm-compatible driver or equivalent) | per-driver; isolated install step, documented separately | Remote controller appears as local gamepad |
| 12 | libsodium | ISC | Authenticated key exchange, per-session AES-256-GCM / ChaCha20-Poly1305 keys |
| 13 | OpenSSL (signaling server TLS) | Apache 2.0 | TLS for the signaling channel |
| 14 | relay server (own code) | MIT (project) | Encrypted-packet relay fallback |

Policy notes:

* FFmpeg will be consumed as prebuilt **shared** libraries (LGPL compliance:
  dynamic linking, no static GPL components, no `--enable-gpl` features). All
  required DLLs ship inside the installer; users never install FFmpeg
  manually.
* The virtual gamepad driver, if required, will be a separate, clearly
  explained install step (see the spec's installer requirements) and never a
  hidden kernel modification.
* The `third_party/` directory is the only place vendored code lives; each
  entry carries its upstream license.
