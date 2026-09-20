# Building RemotePlay

## Prerequisites (Windows 10/11 x64)

1. **Visual Studio 2022** with the *Desktop development with C++* workload
   (MSVC v143 toolset, Windows 11 SDK). The free Community edition works.
2. **Qt 6.5 or newer** (6.8.x recommended), the *MSVC 64-bit* variant.
   Install options, pick one:
   * Qt online installer from https://www.qt.io/download-open-source ->
     select "Qt 6.8.x -> MSVC 2022 64-bit". Default install path:
     `C:\Qt\6.8.1\msvc2022_64`.
   * Or vcpkg: `vcpkg install qtbase` (then pass the vcpkg toolchain file).
3. **CMake 3.22+** (bundled with VS 2022; ensure it is on PATH, or use the
   one inside Visual Studio).

Asio and nlohmann/json are vendored under `third_party/` - nothing to install.

## Build (command line, recommended)

Open *"x64 Native Tools Command Prompt for VS 2022"* and:

```bat
cd RemotePlay
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH="C:/Qt/6.8.1/msvc2022_64"
cmake --build build --config Release
```

The binaries land in `build\bin\Release\`:

| Binary | Description |
|--------|-------------|
| `RemotePlay.exe` | The application (host + client modes) |
| `test_packet.exe`, `test_protocol.exe`, `test_session_codes.exe`, `test_handshake.exe` | Test suites |

### Build inside Visual Studio (alternative)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.1/msvc2022_64"
```
Then open `build\RemotePlay.sln`, select the `Release|x64` configuration, and
build the `RemotePlay` target.

## Deploying Qt DLLs next to the executable (developer machines)

```bat
C:\Qt\6.8.1\msvc2022_64\bin\windeployqt.exe build\bin\Release\RemotePlay.exe
```

This copies `Qt6Widgets.dll`, `Qt6Gui.dll`, `Qt6Core.dll`, platform plugins,
etc. next to the executable so it runs outside the IDE. The proper bundling
(with license attribution) is part of the Phase 17 installer.

## Running the tests

```bat
ctest --test-dir build -C Release --output-on-failure
```

Expected: 4/4 suites pass (packet, protocol, session codes, handshake
integration). The handshake suite binds loopback sockets only.

## Linux/CI notes

The core library and tests are platform-independent C++20 and build with GCC
or Clang; useful for quick iteration without a Windows box:

```sh
cmake -S . -B build-verify -DCMAKE_BUILD_TYPE=Release -DREMOTEPLAY_BUILD_GUI=OFF
cmake --build build-verify -j
ctest --test-dir build-verify --output-on-failure
```

GitHub Actions (`.github/workflows/windows-build.yml`) builds the full
application with MSVC + Qt on every push.

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `Qt6 was not found - building the core library and tests only` | Pass `-DCMAKE_PREFIX_PATH` pointing at the Qt MSVC 64-bit directory (the one containing `lib\cmake\Qt6`). |
| `cmake` not recognized | Use the *x64 Native Tools Command Prompt*, or install CMake from cmake.org and re-open the prompt. |
| Link errors about `ws2_32`/`mswsock` | These are linked automatically; ensure the C++ workload's Windows SDK is installed. |
| Tests fail with bind errors | Something occupies the ephemeral port range; rerun, or check firewall policies. |
