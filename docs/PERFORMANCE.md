# Performance & latency notes (v0.1 — honest accounting)

## Pipeline (host)

```
DXGI Output Duplication (GPU texture, <1 ms)
  -> CopyResource to staging + Map      (1 CPU copy, ~0.5-1 ms @1080p)
  -> swscale BGRA->YUV420P + letterbox  (~1-2 ms, SIMD)
  -> FFmpeg encode                       (NVENC ~2-4 ms / x264 ultrafast ~6-12 ms @1080p60)
  -> fragmentation + UDP send            (<0.5 ms)
```

Achievable end-to-end (same-LAN, hardware encoder): **~15-30 ms** glass to
glass. With software encoding add ~5-10 ms.

## Known CPU copies in v1 (deliberate trade-offs)

1. Desktop texture -> staging Map (one full-frame copy). A GPU-direct path
   (D3D11 interop into NVENC) avoids this; planned as an optimization, not
   a correctness item.
2. swscale BGRA->YUV420P conversion + letterbox copy.
3. Decoder output -> BGRA (swscale) -> QImage copy in VideoWidget.

On a modern 6-core CPU this sustains 1080p60 comfortably; 4K60 software
paths may drop frames — use a hardware encoder for 4K.

## Audio

WASAPI loopback capture (10-20 ms OS buffering) -> Opus 20 ms frames ->
jitter buffer (default 30 ms, configurable) -> WASAPI shared-mode render.
Typical audible latency: **60-90 ms**. Lowering
`network.jitterBufferMs` trades robustness for latency.

## Input

Client polls pads every 8 ms; states sent only on change. Input path:
pad -> queue -> UDP (unfragmented, encrypted) -> host injector. Added
latence is one RTT + <2 ms; typical LAN total **< 20 ms**.

## Bitrate guidance

| Link                  | Suggested    |
|-----------------------|--------------|
| Wired LAN             | 30-50 Mbps   |
| Good 5 GHz Wi-Fi      | 15-25 Mbps   |
| Internet (VPS relay)  | 8-15 Mbps    |
| Weak internet         | 2-6 Mbps     |

Adaptive bitrate (hysteresis + proportional steps + cooldown) adjusts
automatically; the manual slider disables it.

## Hardware encoder notes

- NVENC (NVIDIA): preset p1 + tune ull, CBR. Best latency.
- AMF (AMD): usage ultralowlatency.
- QSV (Intel): preset veryfast + low_power.
- Software fallback: libx264 ultrafast/zerolatency (or libx265 for HEVC).

The runtime probe tries hardware first and falls back automatically; the
stats panel shows which encoder is live. Crash recovery offers
"Restart encoder" and "Switch to software".
