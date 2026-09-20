#pragma once
// Minimal RAII COM pointer (avoids <wrl/client.h> differences between MSVC and MinGW).
// Same semantics as Microsoft::WRL::ComPtr for the subset we use.

// Exclude winsock.h from windows.h (pulled in by unknwn.h below): standalone
// Asio must include winsock2.h itself, and a pre-included winsock.h breaks it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <guiddef.h>
#include <unknwn.h>

namespace rp {

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    explicit ComPtr(T* p) noexcept : ptr_(p) { addRef(); }
    ComPtr(const ComPtr& o) noexcept : ptr_(o.ptr_) { addRef(); }
    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ~ComPtr() { release(); }

    ComPtr& operator=(const ComPtr& o) noexcept {
        if (this != &o) { T* old = ptr_; ptr_ = o.ptr_; addRef(); if (old) old->Release(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { release(); ptr_ = o.ptr_; o.ptr_ = nullptr; }
        return *this;
    }

    T* operator->() const noexcept { return ptr_; }
    T* get() const noexcept { return ptr_; }
    operator T*() const noexcept { return ptr_; }   // for COM interface upcasts (e.g. ID3D11Texture2D* -> ID3D11Resource*)
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T** operator&() noexcept { release(); return &ptr_; }
    T* operator=(T* p) noexcept { T* old = ptr_; ptr_ = p; addRef(); if (old) old->Release(); return ptr_; }

    // Releases ownership of the raw pointer (caller manages refcount).
    T* detach() noexcept { T* p = ptr_; ptr_ = nullptr; return p; }

    void reset() noexcept { release(); }

    template <typename U>
    ComPtr<U> as() const noexcept {  // QueryInterface helper
        ComPtr<U> out;
        if (ptr_) { void* raw = nullptr; if (SUCCEEDED(ptr_->QueryInterface(__uuidof(U), &raw))) out.attach(static_cast<U*>(raw)); }
        return out;
    }

    // Attaches without AddRef (takes ownership of an already-addref'd pointer).
    void attach(T* p) noexcept { release(); ptr_ = p; }

private:
    void addRef() noexcept { if (ptr_) ptr_->AddRef(); }
    void release() noexcept { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }
    T* ptr_ = nullptr;
};

} // namespace rp
