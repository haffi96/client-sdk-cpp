# Publish H.264 TCP Source (Passthrough)

This example connects to a TCP server that streams **length-prefixed AVC** H.264, parses access units, and publishes them to LiveKit using `EncodedVideoSource` — **without decoding or re-encoding**.

This mirrors the Go SDK / lk-cli passthrough behaviour:
```bash
lk room join --publish h264://127.0.0.1:5004 --h26x-streaming-format length-prefixed
```

## Stream format (length-prefixed AVC)

The TCP stream must use **AVCC** framing for low-latency parsing:

- Each NAL unit is sent as:
  - **4 bytes** big-endian length (in bytes)
  - **NAL payload** (no start code)

SPS/PPS must appear in-band (e.g. before IDR slices). Keyframes are detected by NAL type (IDR = 5).

See [H264_FRAMING.md](../../docs/H264_FRAMING.md) for more detail.

## Build and run

```bash
# Build from repo root
./build.sh release-examples

# Run
./build-release/bin/PublishH264TcpSource \
  --url wss://your-app.livekit.cloud \
  --token <JWT> \
  --h264-tcp 127.0.0.1:5004 \
  --width 1280 --height 720
```

Options:

- `--h264-tcp <host:port>` – TCP server for length-prefixed AVC (default `127.0.0.1:5004`)
- `--width`, `--height` – Declared resolution (default 1280x720)

Env: `LIVEKIT_URL`, `LIVEKIT_TOKEN`, `LIVEKIT_E2EE_KEY`.

## Test with GStreamer

Start a TCP server that sends length-prefixed H.264:

```bash
gst-launch-1.0 videotestsrc ! video/x-raw,width=1280,height=720 \
  ! x264enc tune=zerolatency byte-stream=true key-int-max=30 \
  ! h264parse ! video/x-h264,stream-format=avc \
  ! tcpserversink host=127.0.0.1 port=5004
```

Then run the example pointing at `127.0.0.1:5004`.

## How it works

1. `H264TcpSource` reads length-prefixed NAL units from TCP
2. NALs are grouped into access units and converted to Annex-B format
3. `EncodedVideoSource::captureEncodedFrame()` pushes each access unit through the FFI
4. A `PassthroughH264Encoder` in WebRTC delivers the data via `OnEncodedImage()`
5. WebRTC's RTP packetizer fragments and sends — no encoding happens
