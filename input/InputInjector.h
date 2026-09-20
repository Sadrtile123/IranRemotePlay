#pragma once
// Phases 9/11 — host-side input application with per-client permission
// enforcement on the LIVE path (checked for every packet, not only at
// handshake time), plus key release safety when clients vanish.

#include <utility>
#include "InputProtocol.h"
#include "VirtualGamepad.h"

#include "../networking/Protocol.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rp::input {

class InputInjector {
public:
    // Called when the game rumbles a virtual pad; echo to the client.
    using RumbleSender = std::function<void(uint8_t playerIndex, uint8_t left, uint8_t right)>;

    InputInjector() = default;
    ~InputInjector();

    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // Initializes ViGEm (optional: gamepad streaming works only with it).
    bool init(std::string* err = nullptr);
    void shutdown();

    void setPermissions(uint8_t playerIndex, const proto::msg::InputPermission& perms);
    void setRumbleSender(RumbleSender send) { rumbleSender_ = std::move(send); }

    // Applies a raw input datagram payload (controller state or kb/m event)
    // from `playerIndex` AFTER checking live permissions. Returns false when
    // the payload was rejected (malformed or not permitted).
    bool applyInput(uint8_t playerIndex, const uint8_t* data, size_t size);

    // Client leaving: remove its virtual pad and release its held keys.
    void clientDisconnected(uint8_t playerIndex);

    [[nodiscard]] std::string gamepadStatus() const { return pads_.statusText(); }
    [[nodiscard]] uint64_t injectedEvents() const { return injectedEvents_; }
    [[nodiscard]] uint64_t rejectedEvents() const { return rejectedEvents_; }

private:
    bool applyController(uint8_t playerIndex, const GameControllerState& s);
    bool applyEvent(uint8_t playerIndex, const InputEvent& e);
    void releaseAllKeys(uint8_t playerIndex);

    VirtualGamepad pads_;
    RumbleSender rumbleSender_;

    std::mutex mutex_;
    struct PerClient {
        uint8_t playerIndex = 0;
        bool controller = true;
        bool keyboard = false;
        bool mouse = false;
        bool vibration = true;
        bool keysDown[256]{};        // vk -> held (for release safety)
        bool mouseButtons[5]{};      // 0 left, 1 right, 2 middle, 3 x1, 4 x2
    };
    std::vector<PerClient> clients_;
    PerClient& ensureClient(uint8_t playerIndex);

    uint64_t injectedEvents_ = 0;
    uint64_t rejectedEvents_ = 0;
};

} // namespace rp::input
