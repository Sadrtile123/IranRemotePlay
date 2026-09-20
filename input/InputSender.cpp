// Phases 8/11 — client input sender implementation. See InputSender.h.

#include "InputSender.h"

#include "../common/Log.h"

#include <cstring>

namespace rp::input {
namespace {
uint64_t steadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool stateEqual(const GameControllerState& a, const GameControllerState& b) {
    return a.connected == b.connected && a.buttons == b.buttons &&
           a.leftTrigger == b.leftTrigger && a.rightTrigger == b.rightTrigger &&
           a.thumbLX == b.thumbLX && a.thumbLY == b.thumbLY &&
           a.thumbRX == b.thumbRX && a.thumbRY == b.thumbRY;
}
} // namespace



InputSender::InputSender() = default;

InputSender::~InputSender() { stop(); }

bool InputSender::start(net::UdpTransport* transport, uint8_t controllerSlot, std::string* err) {
    if (running_.exchange(true)) { running_.store(false); return false; }
    transport_ = transport;
    slot_ = controllerSlot;

    if (!reader_.discover(err)) {
        running_.store(false);
        return false;
    }
    lastKeepaliveNs_ = steadyNowNs();

    thread_ = std::thread([this] { senderThread(); });
    return true;
}

void InputSender::stop() {
    if (!running_.exchange(false)) return;
    queueCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    transport_ = nullptr;
}

void InputSender::setEnabled(bool controller, bool keyboard, bool mouse) {
    controllerEnabled_.store(controller);
    keyboardEnabled_.store(keyboard);
    mouseEnabled_.store(mouse);
}

void InputSender::queueEvent(const InputEvent& e) {
    if (!running_.load()) return;
    const bool keyboardEvent = e.type == static_cast<uint8_t>(InputEventType::KeyDown) ||
                               e.type == static_cast<uint8_t>(InputEventType::KeyUp);
    if (keyboardEvent && !keyboardEnabled_.load()) return;
    if (!keyboardEvent && !mouseEnabled_.load()) return;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        if (eventQueue_.size() > 256) eventQueue_.clear();   // backpressure: drop stale events
        eventQueue_.push_back(e);
        ++eventsQueued_;
    }
    queueCv_.notify_one();
}

void InputSender::onControlMessage(const std::vector<uint8_t>& payload) {
    if (payload.size() >= sizeof(RumbleCommand)) {
        RumbleCommand rc;
        std::memcpy(&rc, payload.data(), sizeof(rc));
        if (rc.tag == 'R') {
            reader_.rumble(rc.playerIndex, rc.leftMotor, rc.rightMotor);
        }
    }
}

void InputSender::senderThread() {
    std::vector<GameControllerState> states;
    while (running_.load()) {
        const uint64_t now = steadyNowNs();

        // ---- controllers: send on change (or keepalive every 500 ms) ----
        if (controllerEnabled_.load() && transport_) {
            reader_.poll(states);
            for (size_t i = 0; i < states.size() && i < 4; ++i) {
                GameControllerState s = states[i];
                if (i == 0) {
                    // Our pad mirrors the assigned host slot.
                }
                const bool changed = !stateEqual(lastSent_[i], s);
                const bool keepalive = (now - lastKeepaliveNs_) > 500'000'000ull;
                if (changed || keepalive) {
                    auto pkt = packControllerState(s);
                    transport_->sendSmall(net::UdpType::Input, pkt.data(), pkt.size());
                    ++packetsSent_;
                    lastSent_[i] = s;
                }
            }
            if (!states.empty() && (now - lastKeepaliveNs_) > 500'000'000ull) {
                lastKeepaliveNs_ = now;
            }
        }

        // ---- queued keyboard/mouse events ----
        std::vector<InputEvent> toSend;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            if (eventQueue_.empty()) {
                queueCv_.wait_for(lk, std::chrono::milliseconds(8));
            } else {
                toSend.swap(eventQueue_);
            }
        }
        if (transport_) {
            for (const InputEvent& e : toSend) {
                auto pkt = packInputEvent(e);
                transport_->sendSmall(net::UdpType::Input, pkt.data(), pkt.size());
                ++packetsSent_;
            }
        }

        if (toSend.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }
}

std::string InputSender::controllerSummary() const {
    const auto devs = reader_.devices();
    if (devs.empty()) return "no local controllers";
    std::string s;
    for (const auto& d : devs) {
        if (!s.empty()) s += ", ";
        s += d.name;
    }
    return s;
}

} // namespace rp::input
