// Phases 9/11 — input injection implementation. See InputInjector.h.

#include "InputInjector.h"

#include "../common/WinHeaders.h"
#include "../common/Log.h"

#include <windows.h>

#include <cstring>

namespace rp::input {
namespace {

INPUT makeKeyInput(uint16_t vk, bool down, bool extended) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX));
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    return in;
}

INPUT makeMouseMoveInput(int32_t absX, int32_t absY) {   // 0..65535 normalized desktop space
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = absX;
    in.mi.dy = absY;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    return in;
}

INPUT makeMouseButtonInput(int button, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (button) {
        case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 3: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break;
        case 4: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break;
        default: break;
    }
    return in;
}

} // namespace

InputInjector::~InputInjector() { shutdown(); }

bool InputInjector::init(std::string* err) {
    std::string vgErr;
    if (!pads_.init(&vgErr)) {
        RP_WARN() << "[input-injector] virtual gamepads unavailable: " << vgErr
                  << " (keyboard/mouse still active; install ViGEmBus for gamepads)";
        // Not fatal: kb/m injection works without ViGEm.
    }
    pads_.setVibrationCallback([this](uint8_t player, uint8_t left, uint8_t right) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const PerClient& c : clients_) {
            if (c.playerIndex == player && c.vibration && rumbleSender_) {
                rumbleSender_(player, left, right);
                return;
            }
        }
    });
    (void)err;
    return true;      // always operational for kb/m; gamepad state visible via statusText()
}

void InputInjector::shutdown() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (PerClient& c : clients_) {
        releaseAllKeys(c.playerIndex);
        pads_.removePlayer(c.playerIndex);
    }
    clients_.clear();
    pads_.shutdown();
}

InputInjector::PerClient& InputInjector::ensureClient(uint8_t playerIndex) {
    for (PerClient& c : clients_) if (c.playerIndex == playerIndex) return c;
    PerClient c;
    c.playerIndex = playerIndex;
    c.controller = true;      // mirrored from handshake; updated on permission pushes
    clients_.push_back(c);
    return clients_.back();
}

void InputInjector::setPermissions(uint8_t playerIndex, const proto::msg::InputPermission& perms) {
    std::lock_guard<std::mutex> lk(mutex_);
    PerClient& c = ensureClient(playerIndex);
    const bool controllerWas = c.controller;
    c.controller = perms.controller;
    c.keyboard = perms.keyboard;
    c.mouse = perms.mouse;
    c.vibration = perms.vibration;

    if (c.controller && !controllerWas) {
        std::string err;
        if (!pads_.addPlayer(playerIndex, &err)) {
            RP_WARN() << "[input-injector] addPlayer failed: " << err;
        }
    } else if (!c.controller && controllerWas) {
        pads_.removePlayer(playerIndex);
    }
    if (!perms.keyboard) releaseAllKeys(playerIndex);
}

bool InputInjector::applyInput(uint8_t playerIndex, const uint8_t* data, size_t size) {
    GameControllerState state;
    InputEvent event;
    std::lock_guard<std::mutex> lk(mutex_);

    if (!unpackInput(data, size, state, event)) {
        ++rejectedEvents_;
        return false;
    }
    const PerClient& c = ensureClient(playerIndex);

    if (data[0] == 0) {           // controller state
        if (!c.controller) { ++rejectedEvents_; return false; }
        return applyController(playerIndex, state);
    }
    // event
    const bool keyboardEvent = (event.type == static_cast<uint8_t>(InputEventType::KeyDown) ||
                                event.type == static_cast<uint8_t>(InputEventType::KeyUp));
    const bool mouseEvent = !keyboardEvent;
    if (keyboardEvent && !c.keyboard) { ++rejectedEvents_; return false; }
    if (mouseEvent && !c.mouse) { ++rejectedEvents_; return false; }
    return applyEvent(playerIndex, event);
}

bool InputInjector::applyController(uint8_t playerIndex, const GameControllerState& s) {
    if (s.version != 1) { ++rejectedEvents_; return false; }
    if (!s.connected) return true;   // a disconnected pad is a valid state to mirror
    const bool ok = pads_.update(playerIndex, s);
    if (ok) ++injectedEvents_;
    else ++rejectedEvents_;
    return ok;
}

bool InputInjector::applyEvent(uint8_t playerIndex, const InputEvent& e) {
    PerClient& c = ensureClient(playerIndex);
    INPUT in{};

    switch (static_cast<InputEventType>(e.type)) {
        case InputEventType::KeyDown:
        case InputEventType::KeyUp: {
            if (e.code == 0 || e.code >= 256) { ++rejectedEvents_; return false; }
            const bool down = e.type == static_cast<uint8_t>(InputEventType::KeyDown);
            in = makeKeyInput(e.code, down, (e.flags & 1) != 0);
            c.keysDown[e.code] = down;
            break;
        }
        case InputEventType::MouseMove:
            in = makeMouseMoveInput(e.x, e.y);
            break;
        case InputEventType::MouseButtonDown:
        case InputEventType::MouseButtonUp: {
            if (e.code >= 5) { ++rejectedEvents_; return false; }
            const bool down = e.type == static_cast<uint8_t>(InputEventType::MouseButtonDown);
            in = makeMouseButtonInput(e.code, down);
            c.mouseButtons[e.code] = down;
            break;
        }
        case InputEventType::MouseWheel: {
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = static_cast<DWORD>(e.delta) * WHEEL_DELTA;
            break;
        }
        default:
            ++rejectedEvents_;
            return false;
    }

    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        ++rejectedEvents_;
        return false;
    }
    ++injectedEvents_;
    return true;
}

void InputInjector::releaseAllKeys(uint8_t playerIndex) {
    PerClient& c = ensureClient(playerIndex);
    // Keys.
    for (int vk = 1; vk < 256; ++vk) {
        if (!c.keysDown[vk]) continue;
        INPUT in = makeKeyInput(static_cast<uint16_t>(vk), false, false);
        SendInput(1, &in, sizeof(INPUT));
        c.keysDown[vk] = false;
    }
    // Mouse buttons.
    for (int b = 0; b < 5; ++b) {
        if (!c.mouseButtons[b]) continue;
        INPUT in = makeMouseButtonInput(b, false);
        SendInput(1, &in, sizeof(INPUT));
        c.mouseButtons[b] = false;
    }
}

void InputInjector::clientDisconnected(uint8_t playerIndex) {
    std::lock_guard<std::mutex> lk(mutex_);
    releaseAllKeys(playerIndex);
    pads_.removePlayer(playerIndex);
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if (it->playerIndex == playerIndex) { clients_.erase(it); break; }
    }
}

} // namespace rp::input
