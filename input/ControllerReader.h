#pragma once
// Phase 8 — client-side controller reading.
//
// XInput (xinput1_4.dll, dynamic) for Xbox-compatible pads, plus DirectInput8
// (dinput8.dll, dynamic) for legacy gamepads that XInput does not expose.
// Produces up to 4 GameControllerState slots at 8 ms poll granularity and
// routes rumble commands back into the local pad.

#include "InputProtocol.h"

#include "../common/WinHeaders.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rp::input {

struct DeviceInfo {
    int slot = 0;                 // 0..3
    std::string name;             // "XInput 0" / DirectInput product name
    bool isXInput = true;
};

class ControllerReader {
public:
    ControllerReader() = default;
    ~ControllerReader();

    ControllerReader(const ControllerReader&) = delete;
    ControllerReader& operator=(const ControllerReader&) = delete;

    // Discovers XInput + DirectInput devices. Safe to re-run.
    bool discover(std::string* err = nullptr);

    // Polls all slots once (cheap; call at 60-125 Hz). Fills `states`
    // with one entry per connected device, current timestamp.
    void poll(std::vector<GameControllerState>& states);

    // Rumble a locally attached pad (0..255). Player index maps to XInput slot
    // or the DirectInput device that occupies that slot.
    void rumble(int playerIndex, uint8_t left, uint8_t right);

    [[nodiscard]] std::vector<DeviceInfo> devices() const;
    [[nodiscard]] std::string lastError() const { return lastError_; }

    struct DInputImpl;                    // opaque, defined in the .cpp

private:
    void closeDirectInput();

    // XInput (function table)
    void* xinputDll_ = nullptr;
    DWORD (*xinputGetState_)(DWORD, void*) = nullptr;        // XINPUT_STATE*
    DWORD (*xinputSetState_)(DWORD, void*) = nullptr;        // XINPUT_VIBRATION*
    static constexpr size_t kXInputStateSize = 16;           // XINPUT_STATE layout
    static constexpr size_t kXInputVibrationSize = 4;

    // DirectInput
    void* dinputDll_ = nullptr;
    std::unique_ptr<DInputImpl> dinput_;

    std::vector<DeviceInfo> devices_;
    mutable std::mutex mutex_;                                // guards devices_ + DI objects
    std::string lastError_;
    std::atomic<uint64_t> lastStateHash_[4]{};                // change detection helper
};

} // namespace rp::input
