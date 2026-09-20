#pragma once
// Phases 8/11 — client-side input transmission.
//
// Polls local controllers (ControllerReader) on a 8 ms thread and sends full
// states ONLY when they change (plus a 500 ms keepalive so the host can drop
// stale pads). Keyboard/mouse events are queued by the UI and drained by the
// same thread. Rumble commands coming back over UDP are applied to local pads.

#include "ControllerReader.h"
#include "InputProtocol.h"

#include "../networking/UdpTransport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rp::input {

class InputSender {
public:
    InputSender();
    ~InputSender();

    InputSender(const InputSender&) = delete;
    InputSender& operator=(const InputSender&) = delete;

    // `controllerSlot` = this client's player slot (0..3) on the host.
    bool start(net::UdpTransport* transport, uint8_t controllerSlot, std::string* err = nullptr);
    void stop();

    // UI thread -> queue one keyboard/mouse event (mouse coords normalized 0..65535).
    void queueEvent(const InputEvent& e);

    // Called by the transport's control-callback thread for rumble commands.
    void onControlMessage(const std::vector<uint8_t>& payload);

    [[nodiscard]] uint64_t packetsSent() const { return packetsSent_.load(); }
    [[nodiscard]] uint64_t eventsQueued() const { return eventsQueued_.load(); }
    [[nodiscard]] std::string controllerSummary() const;    // for UI
    [[nodiscard]] bool running() const { return running_.load(); }

    void setEnabled(bool controller, bool keyboard, bool mouse);

private:
    void senderThread();

    net::UdpTransport* transport_ = nullptr;    // owned by ClientStreamer; not null while running
    uint8_t slot_ = 0;

    ControllerReader reader_;
    std::thread thread_;
    std::atomic<bool> running_{ false };

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::vector<InputEvent> eventQueue_;

    std::atomic<bool> controllerEnabled_{ true };
    std::atomic<bool> keyboardEnabled_{ false };
    std::atomic<bool> mouseEnabled_{ false };

    std::atomic<uint64_t> packetsSent_{ 0 };
    std::atomic<uint64_t> eventsQueued_{ 0 };

    // last sent state per local device slot for change detection
    GameControllerState lastSent_[4]{};
    uint64_t lastKeepaliveNs_ = 0;
};

} // namespace rp::input
