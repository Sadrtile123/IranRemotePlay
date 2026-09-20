#pragma once
// Phase 10 — virtual Xbox 360 gamepad on the host via ViGEm.
//
// vigemClient.dll is loaded at runtime; if the ViGEmBus kernel driver or its
// client DLL is not installed, virtual pads are unavailable and the app says
// so clearly (streaming continues; keyboard/mouse still work). ViGEm is a
// separate, optional install step documented in README + installer notes.
//
// Vibration events from the game are forwarded to `onVibration` so the host
// can echo them back to the remote client's physical pad.

#include <utility>
#include "InputProtocol.h"

#include "../common/WinHeaders.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rp::input {

struct XUsbReport {                 // layout identical to XINPUT_GAMEPAD / XUSB_REPORT
    uint16_t wButtons = 0;
    uint8_t  bLeftTrigger = 0;
    uint8_t  bRightTrigger = 0;
    int16_t  sThumbLX = 0, sThumbLY = 0, sThumbRX = 0, sThumbRY = 0;
};
static_assert(sizeof(XUsbReport) == 12);

class VirtualGamepad {
public:
    using VibrationCallback = std::function<void(uint8_t playerIndex, uint8_t leftMotor, uint8_t rightMotor)>;

    VirtualGamepad() = default;
    ~VirtualGamepad();

    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;

    // Loads vigemClient.dll and connects the bus client. Fails cleanly with
    // an actionable message when the driver is missing.
    bool init(std::string* err = nullptr);
    void shutdown();

    // Adds a virtual pad. playerIndex: 0..3 (PLAYER 1..4). Fails when the
    // maximum controller count is reached.
    bool addPlayer(uint8_t playerIndex, std::string* err = nullptr);
    void removePlayer(uint8_t playerIndex);
    [[nodiscard]] bool hasPlayer(uint8_t playerIndex) const;

    // Pushes a controller state to the virtual pad.
    bool update(uint8_t playerIndex, const GameControllerState& s);

    void setVibrationCallback(VibrationCallback cb) { onVibration_ = std::move(cb); }

    // Internal: called by the ViGEm notification thunk (target -> player mapping).
    // NOTE: param names must not be `small`/`large`: rpcndr.h (via the Windows
    // headers pulled in alongside ViGEm/DirectInput) #defines `small` as
    // `char` on MSVC, breaking the declaration with C2628.
    void handleVibration(void* target, uint8_t largeMotor, uint8_t smallMotor);

    [[nodiscard]] bool available() const { return client_ != nullptr; }
    [[nodiscard]] std::string statusText() const;   // for the UI/diagnostics

private:
    void* dll_ = nullptr;            // HMODULE vigemClient.dll
    void* client_ = nullptr;         // PVIGEM_CLIENT (opaque)

    // Function table (resolved via GetProcAddress).
    void* (*vigem_alloc_)() = nullptr;
    long (*vigem_connect_)(void*) = nullptr;
    long (*vigem_disconnect_)(void*) = nullptr;
    void  (*vigem_free_)(void*) = nullptr;
    void* (*vigem_target_x360_alloc_)() = nullptr;
    void  (*vigem_target_free_)(void*) = nullptr;
    long  (*vigem_target_add_)(void*, void*) = nullptr;
    long  (*vigem_target_remove_)(void*, void*) = nullptr;
    long  (*vigem_target_x360_update_)(void*, void*, XUsbReport) = nullptr;
    long  (*vigem_target_set_vid_)(void*, uint16_t) = nullptr;
    long  (*vigem_target_set_pid_)(void*, uint16_t) = nullptr;
    void  (*vigem_target_x360_register_notification_)(void*, void*, void*, void*) = nullptr;
    void  (*vigem_target_x360_unregister_notification_)(void*) = nullptr;

    struct Pad {
        uint8_t playerIndex = 0;
        void* target = nullptr;      // PVIGEM_TARGET (opaque)
    };
    std::vector<Pad> pads_;

    VibrationCallback onVibration_;
    std::string status_;
};

} // namespace rp::input
