// Phase 10 — ViGEm virtual gamepad implementation. See VirtualGamepad.h.
//
// ViGEmBus driver + vigemClient.dll (MIT, Nefarius) are optional prerequisites.
// We speak only its documented public C API, resolved dynamically.

#include "VirtualGamepad.h"

#include "../common/Log.h"

#include <windows.h>

#include <cstring>

namespace rp {
namespace input {
namespace {

void __stdcall x360NotificationThunk(void* /*client*/, void* target, unsigned char large,
                                      unsigned char small, unsigned char /*led*/, void* userData) {
    auto* self = static_cast<VirtualGamepad*>(userData);
    if (!self) return;
    self->handleVibration(target, large, small);
}

} // namespace

// The thunk calls this; public on the class so the free thunk can reach it.
void VirtualGamepad::handleVibration(void* target, uint8_t large, uint8_t small) {
    for (const Pad& p : pads_) {
        if (p.target == target) {
            if (onVibration_) onVibration_(p.playerIndex, large, small);
            return;
        }
    }
}

VirtualGamepad::~VirtualGamepad() { shutdown(); }

bool VirtualGamepad::init(std::string* err) {
    HMODULE dll = LoadLibraryA("vigemClient.dll");
    if (!dll) {
        status_ = "vigemClient.dll not found (install ViGEmBus driver)";
        if (err) *err = status_;
        RP_INFO() << "[vigem] " << status_;
        return false;
    }
    dll_ = dll;
    vigem_alloc_ = reinterpret_cast<void* (*)()>(GetProcAddress(dll, "vigem_alloc"));
    vigem_connect_ = reinterpret_cast<long (*)(void*)>(GetProcAddress(dll, "vigem_connect"));
    vigem_disconnect_ = reinterpret_cast<long (*)(void*)>(GetProcAddress(dll, "vigem_disconnect"));
    vigem_free_ = reinterpret_cast<void (*)(void*)>(GetProcAddress(dll, "vigem_free"));
    vigem_target_x360_alloc_ = reinterpret_cast<void* (*)()>(GetProcAddress(dll, "vigem_target_x360_alloc"));
    vigem_target_free_ = reinterpret_cast<void (*)(void*)>(GetProcAddress(dll, "vigem_target_free"));
    vigem_target_add_ = reinterpret_cast<long (*)(void*, void*)>(GetProcAddress(dll, "vigem_target_add"));
    vigem_target_remove_ = reinterpret_cast<long (*)(void*, void*)>(GetProcAddress(dll, "vigem_target_remove"));
    vigem_target_x360_update_ = reinterpret_cast<long (*)(void*, void*, XUsbReport)>(GetProcAddress(dll, "vigem_target_x360_update"));
    vigem_target_x360_register_notification_ =
        reinterpret_cast<void (*)(void*, void*, void*, void*)>(GetProcAddress(dll, "vigem_target_x360_register_notification"));
    vigem_target_x360_unregister_notification_ =
        reinterpret_cast<void (*)(void*)>(GetProcAddress(dll, "vigem_target_x360_unregister_notification"));

    if (!vigem_alloc_ || !vigem_connect_ || !vigem_target_x360_alloc_ || !vigem_target_add_ ||
        !vigem_target_x360_update_ || !vigem_target_remove_ || !vigem_free_) {
        status_ = "vigemClient.dll found but exports missing (wrong version?)";
        if (err) *err = status_;
        return false;
    }

    client_ = vigem_alloc_();
    if (!client_) { status_ = "vigem_alloc failed"; if (err) *err = status_; return false; }
    const long rc = vigem_connect_(client_);
    if (rc != 0) {   // VIGEM_ERROR_NONE = 0
        status_ = "vigem_connect failed (ViGEmBus driver not installed?) error=" + std::to_string(rc);
        if (err) *err = status_;
        vigem_free_ ? vigem_free_(client_) : (void)0;
        client_ = nullptr;
        return false;
    }
    status_ = "ViGEm ready";
    RP_INFO() << "[vigem] connected";
    return true;
}

void VirtualGamepad::shutdown() {
    for (Pad& p : pads_) {
        if (p.target) {
            if (vigem_target_x360_unregister_notification_) vigem_target_x360_unregister_notification_(p.target);
            if (vigem_target_remove_ && client_) vigem_target_remove_(client_, p.target);
            if (vigem_target_free_) vigem_target_free_(p.target);
        }
    }
    pads_.clear();
    if (client_) {
        if (vigem_disconnect_) vigem_disconnect_(client_);
        if (vigem_free_) vigem_free_(client_);
        client_ = nullptr;
    }
    if (dll_) { FreeLibrary(static_cast<HMODULE>(dll_)); dll_ = nullptr; }
}

bool VirtualGamepad::addPlayer(uint8_t playerIndex, std::string* err) {
    if (!client_) { if (err) *err = status_; return false; }
    if (hasPlayer(playerIndex)) return true;      // idempotent
    if (pads_.size() >= 4) { if (err) *err = "max virtual pads reached"; return false; }

    void* target = vigem_target_x360_alloc_();
    if (!target) { if (err) *err = "vigem_target_x360_alloc failed"; return false; }
    if (const long rc = vigem_target_add_(client_, target); rc != 0) {
        if (err) *err = "vigem_target_add failed code " + std::to_string(rc);
        vigem_target_free_ ? vigem_target_free_(target) : (void)0;
        return false;
    }
    if (vigem_target_x360_register_notification_) {
        vigem_target_x360_register_notification_(client_, target,
                                                 reinterpret_cast<void*>(&x360NotificationThunk), this);
    }
    pads_.push_back(Pad{ playerIndex, target });
    RP_INFO() << "[vigem] virtual pad added for player " << (playerIndex + 1);
    return true;
}

void VirtualGamepad::removePlayer(uint8_t playerIndex) {
    for (auto it = pads_.begin(); it != pads_.end(); ++it) {
        if (it->playerIndex == playerIndex) {
            if (it->target) {
                if (vigem_target_x360_unregister_notification_) vigem_target_x360_unregister_notification_(it->target);
                if (client_ && vigem_target_remove_) vigem_target_remove_(client_, it->target);
                if (vigem_target_free_) vigem_target_free_(it->target);
            }
            pads_.erase(it);
            return;
        }
    }
}

bool VirtualGamepad::hasPlayer(uint8_t playerIndex) const {
    for (const Pad& p : pads_) if (p.playerIndex == playerIndex) return true;
    return false;
}

bool VirtualGamepad::update(uint8_t playerIndex, const GameControllerState& s) {
    void* target = nullptr;
    for (const Pad& p : pads_) if (p.playerIndex == playerIndex) { target = p.target; break; }
    if (!target || !client_) return false;

    XUsbReport r;
    r.wButtons = s.buttons;
    r.bLeftTrigger = s.leftTrigger;
    r.bRightTrigger = s.rightTrigger;
    r.sThumbLX = s.thumbLX;
    r.sThumbLY = s.thumbLY;
    r.sThumbRX = s.thumbRX;
    r.sThumbRY = s.thumbRY;
    return vigem_target_x360_update_(client_, target, r) == 0;
}

std::string VirtualGamepad::statusText() const { return status_; }

} // namespace input
} // namespace rp
