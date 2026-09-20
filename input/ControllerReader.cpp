// Phase 8 — client controller reading implementation. See ControllerReader.h.

#include "ControllerReader.h"
#include "../common/WinHeaders.h"

#include "../common/Log.h"

#include <dinput.h>
#include <windows.h>
#include <xinput.h>

#include <chrono>
#include <cstring>

namespace rp::input {
namespace {
uint64_t steadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
constexpr int16_t clampI16(LONG v) { return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }
} // namespace

// ---------------------------------------------------------------- DirectInput

struct ControllerReader::DInputImpl {
    IDirectInput8W* di8 = nullptr;
    struct Device {
        IDirectInputDevice8W* dev = nullptr;
        int slot = 0;
        std::wstring name;
        LONG axisRangeMin[6]{};      // lX lY lZ lRx lRy lRz
        LONG axisRangeMax[6]{};
    };
    std::vector<Device> devices;
};

struct EnumCtx {
    ControllerReader::DInputImpl* impl;
    int nextSlot = 0;
    static constexpr int kMaxDevices = 8;
};

struct EnumObjectsCtx {
    IDirectInputDevice8W* dev;
};

static BOOL FAR PASCAL enumObjectsCallback(LPCDIDEVICEOBJECTINSTANCEW obj, LPVOID raw) {
    auto* c = static_cast<EnumObjectsCtx*>(raw);
    if (obj->dwFlags & DIDFT_AXIS) {
        DIPROPRANGE range;
        std::memset(&range, 0, sizeof(range));
        range.diph.dwSize = sizeof(DIPROPRANGE);
        range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        range.diph.dwObj = obj->dwType;
        range.diph.dwHow = DIPH_BYOFFSET;
        range.lMin = -32768;
        range.lMax = 32767;
        c->dev->SetProperty(DIPROP_RANGE, &range.diph);
    }
    return DIENUM_CONTINUE;
}

static BOOL FAR PASCAL enumDevicesCallback(LPCDIDEVICEINSTANCEW inst, LPVOID ctxRaw) {
    auto* ctx = static_cast<EnumCtx*>(ctxRaw);
    if (ctx->nextSlot >= EnumCtx::kMaxDevices) return DIENUM_STOP;
    if (GET_DIDEVICE_TYPE(inst->dwDevType) != DI8DEVTYPE_GAMEPAD &&
        GET_DIDEVICE_TYPE(inst->dwDevType) != DI8DEVTYPE_JOYSTICK) {
        return DIENUM_CONTINUE;
    }

    ControllerReader::DInputImpl::Device d;
    d.slot = ctx->nextSlot++;
    d.name = inst->tszProductName;

    IDirectInput8W* di8 = ctx->impl->di8;
    IDirectInputDevice8W* dev = nullptr;
    if (SUCCEEDED(di8->CreateDevice(inst->guidInstance, &dev, nullptr))) {
        DIPROPDWORD prop;
        std::memset(&prop, 0, sizeof(prop));
        prop.diph.dwSize = sizeof(DIPROPDWORD);
        prop.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        prop.diph.dwHow = DIPH_DEVICE;
        prop.dwData = DIPROPAXISMODE_ABS;
        dev->SetProperty(DIPROP_AXISMODE, &prop.diph);
        dev->SetCooperativeLevel(nullptr, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);

        // Normalize every axis to [-32768, 32767] so gamepad mapping is uniform.
        EnumObjectsCtx objCtx{ dev };
        dev->EnumObjects(enumObjectsCallback, &objCtx, DIDFT_AXIS);

        d.dev = dev;
        d.name = inst->tszProductName;
        ctx->impl->devices.push_back(std::move(d));
    }
    return DIENUM_CONTINUE;
}

ControllerReader::ControllerReader() = default;

ControllerReader::~ControllerReader() {
    closeDirectInput();
    if (xinputDll_) { FreeLibrary(static_cast<HMODULE>(xinputDll_)); xinputDll_ = nullptr; }
}

void ControllerReader::closeDirectInput() {
    if (!dinput_) return;
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& d : dinput_->devices) {
        if (d.dev) { d.dev->Unacquire(); d.dev->Release(); d.dev = nullptr; }
    }
    if (dinput_->di8) { dinput_->di8->Release(); dinput_->di8 = nullptr; }
    dinput_.reset();
}

bool ControllerReader::discover(std::string* err) {
    std::lock_guard<std::mutex> lk(mutex_);
    devices_.clear();
    lastError_.clear();

    // ---- XInput ----
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    if (xi) {
        xinputDll_ = xi;
        xinputGetState_ = reinterpret_cast<DWORD(*)(DWORD, void*)>(GetProcAddress(xi, "XInputGetState"));
        xinputSetState_ = reinterpret_cast<DWORD(*)(DWORD, void*)>(GetProcAddress(xi, "XInputSetState"));
        if (xinputGetState_) {
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
                uint8_t state[kXInputStateSize];
                if (xinputGetState_(i, state) == ERROR_SUCCESS) {
                    DeviceInfo di;
                    di.slot = static_cast<int>(i);
                    di.name = "XInput pad " + std::to_string(i + 1);
                    di.isXInput = true;
                    devices_.push_back(di);
                }
            }
        }
    } else {
        lastError_ = "xinput DLLs not found";
    }

    // ---- DirectInput (legacy pads) ----
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, void*);
    HMODULE di = LoadLibraryA("dinput8.dll");
    if (di) {
        dinputDll_ = di;
        IDirectInput8W* di8 = nullptr;
        auto create = reinterpret_cast<CreateFn>(GetProcAddress(di, "DirectInput8Create"));
        if (create && SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                       reinterpret_cast<void**>(&di8), nullptr))) {
            dinput_ = std::make_unique<DInputImpl>();
            dinput_->di8 = di8;
            EnumCtx ctx{ dinput_.get() };
            di8->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevicesCallback, &ctx, DIEDFL_ATTACHEDONLY);
            for (const auto& d : dinput_->devices) {
                DeviceInfo di2;
                di2.slot = d.slot;
                std::string narrow(d.name.begin(), d.name.end());
                di2.name = narrow.empty() ? "DirectInput pad" : narrow;
                di2.isXInput = false;
                devices_.push_back(di2);
                if (d.dev) d.dev->Acquire();
            }
            RP_INFO() << "[input] DirectInput devices: " << dinput_->devices.size();
        } else if (err) {
            *err = "DirectInput8Create failed";
        }
    }

    RP_INFO() << "[input] discovered " << devices_.size() << " controller(s)";
    return true;
}

void ControllerReader::poll(std::vector<GameControllerState>& states) {
    states.clear();
    std::lock_guard<std::mutex> lk(mutex_);

    if (xinputGetState_) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            // XINPUT_STATE: DWORD packetNumber; XINPUT_GAMEPAD {wButtons; bLeftTrigger; bRightTrigger; sThumbLX; sThumbLY; sThumbRX; sThumbRY}
            uint8_t raw[kXInputStateSize];
            if (xinputGetState_(i, raw) != ERROR_SUCCESS) continue;

            uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry;
            std::memcpy(&buttons, raw + 4, 2);
            lt = raw[6]; rt = raw[7];
            std::memcpy(&lx, raw + 8, 2); std::memcpy(&ly, raw + 10, 2);
            std::memcpy(&rx, raw + 12, 2); std::memcpy(&ry, raw + 14, 2);

            GameControllerState s;
            s.version = 1;
            s.connected = 1;
            s.buttons = buttons;                     // XInput bits match our enum exactly
            s.leftTrigger = lt; s.rightTrigger = rt;
            s.thumbLX = lx; s.thumbLY = ly; s.thumbRX = rx; s.thumbRY = ry;
            s.timestampNs = steadyNowNs();
            states.push_back(s);
        }
    }

    if (dinput_) {
        for (auto& d : dinput_->devices) {
            if (!d.dev) continue;
            DIJOYSTATE2 js;
            std::memset(&js, 0, sizeof(js));
            HRESULT hr = d.dev->Poll();
            if (FAILED(hr)) hr = d.dev->GetDeviceState(sizeof(js), &js);
            if (FAILED(hr)) { d.dev->Acquire(); continue; }

            GameControllerState s;
            s.version = 1;
            s.connected = 1;
            // Axes: lX,lY -> left stick; lZ,lRz -> right stick; lRx -> split triggers.
            s.thumbLX = clampI16(js.lX);
            s.thumbLY = clampI16(js.lY);
            s.thumbRX = clampI16(js.lZ);
            s.thumbRY = clampI16(js.lRz);
            s.leftTrigger  = static_cast<uint8_t>((js.lRx + 32768) >> 8);   // 0..255
            s.rightTrigger = static_cast<uint8_t>((js.rglSlider[0] + 32768) >> 8);
            // D-pad from POV hat (POV is -1 or 0..31500 degrees).
            if (js.rgdwPOV[0] != static_cast<DWORD>(-1)) {
                const int deg = static_cast<int>(js.rgdwPOV[0]) / 100;
                if (deg >= 31500 / 100 || deg <= 4500 / 100) s.buttons |= DUp;
                if (deg >= 4500 / 100 && deg <= 13500 / 100) s.buttons |= DRight;
                if (deg >= 13500 / 100 && deg <= 22500 / 100) s.buttons |= DDown;
                if (deg >= 22500 / 100 && deg <= 31500 / 100) s.buttons |= DLeft;
            }
            for (int b = 0; b < 16 && b < 128; ++b) {
                if (js.rgbButtons[b] & 0x80) s.buttons |= (1u << b);
            }
            s.timestampNs = steadyNowNs();
            // XInput slots may already use buttons 0..14; DirectInput reuses
            // them on a different logical pad (slot disambiguation happens in
            // the sender by device identity).
            states.push_back(s);
        }
    }
}

void ControllerReader::rumble(int playerIndex, uint8_t left, uint8_t right) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (xinputSetState_ && playerIndex >= 0 && playerIndex < XUSER_MAX_COUNT) {
        uint8_t vib[kXInputVibrationSize];
        const uint16_t l = static_cast<uint16_t>(left) * 257;
        const uint16_t r = static_cast<uint16_t>(right) * 257;
        std::memcpy(vib, &l, 2);
        std::memcpy(vib + 2, &r, 2);
        xinputSetState_(static_cast<DWORD>(playerIndex), vib);
    }
    (void)dinput_;   // force feedback on DirectInput devices: future enhancement
}

std::vector<DeviceInfo> ControllerReader::devices() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return devices_;
}

} // namespace rp::input
