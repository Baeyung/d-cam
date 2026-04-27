### 1. High-Level System Overview

The system creates a low-latency, USB-only video bridge between an Android device and OBS Studio. The end-to-end data flow is:

**Camera → Encoder → TCP Socket → ADB Reverse Tunnel → Desktop Decoder → IPC → OBS Source → Render Pipeline**

**Component Responsibilities:**

*   **Android Application:** Captures camera frames using the Android Camera2 API, encodes them to H.264 Annex B via hardware MediaCodec, and transmits the elementary stream over a local TCP socket.
*   **USB Communication Layer (ADB):** An ADB reverse tunnel (`adb reverse tcp:PHONE_PORT tcp:DESKTOP_PORT`) bridges the Android local socket to the desktop without requiring WiFi or root access. The Android app acts as the TCP client; the desktop service acts as the server.
*   **Desktop Service:** A standalone daemon that manages the ADB lifecycle, accepts the tunneled TCP connection, demuxes the custom application protocol, decodes H.264 to raw NV12/YUV420P frames using FFmpeg/libavcodec, and exposes those frames to the OBS plugin via a high-speed local IPC mechanism (local TCP loopback socket).
*   **OBS Plugin:** A native `libobs` source plugin that connects to the desktop service’s IPC output, maintains a thread-safe frame queue, and pushes video frames into the OBS rendering pipeline via `obs_source_output_video()`.

**Data Flow:**
1.  Android Camera2 outputs YUV frames to a Surface connected to MediaCodec.
2.  MediaCodec outputs H.264 Annex B NAL units.
3.  A custom lightweight binary protocol wraps each frame with a timestamp and type header.
4.  The packet stream flows through the ADB reverse tunnel over USB.
5.  The desktop service receives, parses, and decodes the stream.
6.  Decoded raw frames are forwarded over a local IPC socket to the OBS plugin.
7.  The plugin converts the frame to the OBS pixel format and outputs it.

---

### 2. Architecture Breakdown

**Component Boundaries & Responsibilities:**

| Component | Responsibility | Boundary |
|-----------|---------------|----------|
| **Android App** | Camera acquisition, hardware encoding, protocol framing, TCP client connection to ADB tunnel. | Does not know about OBS, decoding, or desktop OS. Assumes a reliable byte pipe to localhost. |
| **ADB Bridge** | External tool managed by the Desktop Service. Creates the reverse port tunnel. Not part of the custom codebase, but a hard runtime dependency. | Boundary is the shell/process invocation interface. |
| **Desktop Service** | ADB lifecycle management (device detection, tunnel setup/teardown), TCP server for tunneled connection, video decoding, error recovery, IPC server for OBS. | Does not link against libobs. Exposes a language-agnostic binary stream interface. |
| **OBS Plugin** | IPC client to Desktop Service, frame queue management, libobs source registration, rendering, user-facing properties (resolution, bitrate). | Does not perform video decoding or ADB operations. |

**Interaction Points:**

*   **Android ↔ Desktop (over USB):** A single TCP connection tunneled via ADB reverse. This is the only cross-device link.
*   **Desktop Service ↔ OBS Plugin:** A local IPC connection on the same machine (localhost TCP socket for portability, upgradable to shared memory in optimization phases).
*   **Desktop Service ↔ ADB Daemon:** Local shell/process interaction to execute `adb reverse`, monitor device state, and handle USB attach/detach events.

---

### 3. Communication Design (CRITICAL)

**USB Communication Approach:**
Use **ADB Reverse Port Forwarding**. The desktop service executes `adb reverse tcp:<PHONE_PORT> tcp:<DESKTOP_PORT>`. The Android app connects to `localhost:<PHONE_PORT>`. ADB tunnels this over USB to the desktop service listening on `localhost:<DESKTOP_PORT>`.

*   **Assumption:** USB Debugging is enabled on the Android device, and the ADB binary is present on the desktop.
*   **Alternative (fallback):** USB Tethering (RNDIS) could establish an IP network, but it requires user network stack changes and is less reliable than ADB; it is documented only as a contingency.

**Data Transmission Model:**
Streaming. The Android app pushes encoded video frames as a continuous byte stream. TCP provides reliable, ordered delivery. Because the transport is local (loopback over USB), TCP congestion control is not a concern, but the protocol must handle its buffering behavior.

**Application Protocol (Android → Desktop):**
A custom binary framing protocol over TCP:

*   **Header (per message):**
    *   4 bytes: Magic identifier (`DROI`)
    *   1 byte: Message Type
    *   4 bytes: Payload length (big-endian unsigned integer)
*   **Message Types:**
    *   `CONFIG` (0x01): Contains H.264 SPS/PPS NAL units. Sent initially and on encoder reconfiguration.
    *   `VIDEO_FRAME` (0x02): Contains a single encoded video frame (IDR or P-frame). Payload prefixed with an 8-byte monotonic timestamp (microseconds).
    *   `HEARTBEAT` (0x03): Sent every 2 seconds if no video data is transmitted (keep-alive).
*   **Frame Format:** H.264 Baseline/Main Profile, **Annex B** format (start code prefix `0x00000001`). This avoids length-prefix complexity and is directly compatible with most decoders.

**Error Handling Strategy:**
*   **TCP Disconnect:** The Android app detects a socket close and enters a reconnection loop with exponential backoff (starting at 500ms, capped at 5s). The desktop service continuously listens for new connections.
*   **USB Unplug:** ADB drops the tunnel. The desktop service detects the socket close, kills the stale `adb reverse` mapping, and polls `adb devices` every 2 seconds to re-establish the tunnel upon reconnection. The Android app simultaneously retries its localhost connection.
*   **Decoder Error:** If the desktop service encounters a corrupt frame, it discards data until the next `CONFIG` message or IDR frame. A future extension may send an IDR request message to Android.
*   **Encoder Failure:** The Android app catches MediaCodec exceptions, tears down the camera and encoder, and restarts the capture pipeline.

**Synchronization and Buffering Strategy:**
*   **Android Side:** Camera2 → Surface → MediaCodec. The encoder output buffer queue is limited to **2 frames** to minimize latency. TCP `SO_SNDBUF` should be set to a low value (e.g., 64KB) to prevent excessive kernel buffering.
*   **Desktop Side:** A **2-frame decode jitter buffer** absorbs encoder output variance. The decoder runs on a dedicated thread. Timestamps from the Android monotonic clock are preserved and passed to OBS; no frame is rendered later than 100ms behind wall-clock to prevent stall.
*   **OBS Side:** The plugin maintains a **double-buffer** (2 frames). If a new frame arrives before OBS has rendered the previous one, the oldest frame is dropped. This enforces a "display the latest" policy critical for low-latency preview.

---

### 4. Android Application Plan

**Core Modules:**

1.  **Camera Module:** Wraps Camera2 API. Handles `CameraManager` enumeration, `CameraCharacteristics` capability checks (stream configuration map, supported sizes), and `CameraDevice` session management. Uses a `Surface` from the encoder as the capture target to enable zero-copy where possible.
2.  **Encoder Module:** Wraps `MediaCodec`. Queries `MediaCodecList` for an H.264 encoder. Configures it for Surface input, variable or constant bitrate, and the target resolution/FPS. Handles `onOutputBufferAvailable` to extract encoded data and emit `CONFIG` messages when format changes occur.
3.  **Protocol & Network Module:** Manages a TCP client socket to `localhost:<PHONE_PORT>`. Implements the custom binary protocol serializer. Runs on a dedicated thread to avoid blocking the encoder callback thread.
4.  **Lifecycle & State Manager:** Orchestrates start/stop of camera, encoder, and network modules. Handles Android lifecycle events (`onResume`/`onPause`) and foreground service requirements.

**Lifecycle Considerations:**
*   The app must run a **Foreground Service** (with a persistent notification) to prevent the OS from killing the camera and network threads when the user switches apps or the screen turns off.
*   On `onPause`, the pipeline should pause (release camera and encoder) to free resources for other apps. On `onResume`, it reinitializes and reconnects.
*   Handle device orientation changes by locking the encoder to a fixed orientation (e.g., landscape) and applying metadata for rotation, or by dynamically restarting the encoder with new dimensions. **Decision:** Lock the stream to a single orientation (sensor landscape) and send rotation metadata in the protocol to avoid encoder reconfiguration storms.

**Permissions and Constraints:**
*   `CAMERA`: Runtime permission required.
*   `INTERNET`: Required for localhost socket operation.
*   `FOREGROUND_SERVICE`: Required for Android 9+.
*   `POST_NOTIFICATIONS`: Required for Android 13+ foreground service notification.
*   **Constraint:** The app targets API 29+ (Android 10) to ensure `adb reverse` compatibility and Camera2 stability, while avoiding scoped storage complexities.

**Performance Considerations:**
*   **Resolution/FPS Tradeoffs:** Default to 1280x720 @ 30fps. Support 1920x1080 @ 30fps and 1280x720 @ 60fps as user-selectable profiles. Higher resolutions increase encoding latency and USB bandwidth usage.
*   **Bitrate:** Default to 8 Mbps for 720p30, 12 Mbps for 1080p30. Use CBR if the encoder supports it to smooth USB throughput.
*   **Threading:** Camera callbacks run on a HandlerThread with `THREAD_PRIORITY_URGENT_DISPLAY`. Network I/O runs on its own thread. Encoder callbacks run on the internal MediaCodec thread.

---

### 5. Desktop / OBS Plugin Plan

**Intermediate Desktop Service: Required**
A standalone desktop service is mandatory to isolate ADB management and video decoding from the OBS process. Crashing or blocking in ADB or decode logic must not crash OBS. The service also allows independent testing of the video pipeline without running OBS.

**Desktop Service Responsibilities:**
*   Locate or bundle the ADB binary. Execute `adb reverse` and monitor the ADB daemon state.
*   Listen on `localhost:<DESKTOP_PORT>` for the tunneled Android connection.
*   Parse the custom application protocol, buffer NAL units, and feed them to the decoder.
*   Decode H.264 to NV12 (or YUV420P) using libavcodec. Prefer hardware acceleration (DXVA2/D3D11VA on Windows, VideoToolbox on macOS, VAAPI on Linux) with a software fallback.
*   Expose decoded frames via a **local IPC server** (localhost TCP socket on a secondary port). Each frame is prefixed with a small header: `width`, `height`, `pixel_format` (fourcc), `timestamp`, `data_length`.
*   Handle reconnection loops, ADB device enumeration (supporting multiple devices via serial number selection), and graceful teardown.

**OBS Plugin Responsibilities:**
*   Register a new source type (`droidcam_usb_source`) with `libobs`.
*   In the source `create`/`update` callbacks, read user properties (target resolution, bitrate, ADB device serial).
*   Spawn a background thread that connects to the Desktop Service’s IPC output socket.
*   Maintain a thread-safe frame queue (lock-protected or lock-free ring buffer).
*   In the source `video_tick`/`video_render` callbacks, dequeue the latest available frame, convert if necessary to the OBS standard format (usually `VIDEO_FORMAT_NV12` or `VIDEO_FORMAT_I420`), and call `obs_source_output_video()`.
*   Handle source visibility changes: when hidden, the plugin may signal the desktop service to pause the stream to save USB bandwidth and CPU, or simply drop frames locally.

**Memory and Performance Considerations:**
*   The plugin must **not** allocate memory on the render thread. Frame buffers should be pre-allocated in a pool.
*   IPC via localhost TCP introduces one memory copy. For Phase 5 optimization, migrate to platform-specific shared memory (POSIX shm or Windows shared memory section) with a circular buffer to achieve near-zero-copy.
*   OBS runs its own GPU upload path. The plugin should provide CPU-accessible frame data in a planar format that libobs can efficiently upload.

---

### 6. Milestone-Based Task Breakdown

**Phase 1: USB Communication & Protocol Prototype**
*   **Goal:** Establish a reliable, reconnectable byte stream between Android and Desktop over USB, and validate the custom application protocol.
*   **Deliverables:**
    *   Android module: TCP client to `localhost:<PHONE_PORT>` with protocol serializer/deserializer.
    *   Desktop service: TCP listener on `localhost:<DESKTOP_PORT>`, protocol parser, ADB reverse tunnel management script.
    *   Test harness: Send 20Mbps of dummy framed data from Android to Desktop continuously for 10 minutes without corruption.
*   **Dependencies:** ADB installed on desktop; Android device with USB debugging enabled.

**Phase 2: Android Camera & Hardware Encoding**
*   **Goal:** Produce a valid, continuous H.264 Annex B elementary stream from the device camera.
*   **Deliverables:**
    *   Camera2 implementation with configurable resolution and FPS.
    *   MediaCodec H.264 encoder integration with Surface input.
    *   Extraction and transmission of SPS/PPS `CONFIG` messages.
    *   Foreground service wrapper for background stability.
*   **Dependencies:** Phase 1 (integrate network module to send real encoded data).

**Phase 3: Desktop Video Decode & IPC Output**
*   **Goal:** Receive the encoded stream and output raw decoded frames to a local IPC socket.
*   **Deliverables:**
    *   Desktop service protocol handler integrated with FFmpeg/libavcodec.
    *   H.264 decode pipeline with hardware acceleration detection.
    *   IPC output server exposing NV12 frames with metadata headers.
    *   Standalone viewer tool (optional but recommended) to verify decoded output without OBS.
*   **Dependencies:** Phase 1, Phase 2.

**Phase 4: OBS Plugin Development**
*   **Goal:** Render the video stream as a native OBS source.
*   **Deliverables:**
    *   libobs source plugin skeleton compiled against the OBS Studio plugin SDK.
    *   Properties UI for device serial selection, resolution, and bitrate.
    *   IPC client thread connecting to Desktop Service.
    *   Frame queue and `obs_source_output_video()` integration.
    *   Source lifecycle handling (create, destroy, show, hide, update).
*   **Dependencies:** Phase 3.

**Phase 5: Integration, Error Recovery & Optimization**
*   **Goal:** Achieve an end-to-end, self-healing pipeline with acceptable latency.
*   **Deliverables:**
    *   Reconnection logic on both sides (exponential backoff, ADB tunnel re-establishment).
    *   Buffer size tuning (reduce kernel TCP buffers, optimize decoder jitter buffer).
    *   Multi-device support (ADB serial number selection and validation).
    *   Memory leak auditing and thread safety hardening.
    *   Cross-platform build system (Windows, macOS, Linux).
*   **Dependencies:** Phase 4.

**Phase 6: Validation, Stress Testing & Packaging**
*   **Goal:** Production-ready stability and performance.
*   **Deliverables:**
    *   Pass 4-hour continuous stream test without memory growth or frame drops >0.1%.
    *   Achieve <150ms glass-to-glass latency at 720p30.
    *   Validate 1080p30 and 720p60 profiles on reference devices.
    *   Packaged Android APK, Desktop Service installer, and OBS plugin distribution files.
*   **Dependencies:** Phase 5.

---

### 7. Risks & Challenges

**Technical Challenges:**
*   **Latency Budget:** Each stage (camera shutter, encoder, TCP buffer, decode, OBS render) adds latency. Aggressive buffer reduction increases the risk of frame drops under load.
*   **USB Throughput:** USB 2.0 effective throughput is ~280–350 Mbps shared. H.264 at 12 Mbps is safe, but if the user selects a high bitrate or the encoder produces a bitrate spike, the tunnel may stall. CBR enforcement is critical.
*   **Encoding Cost:** MediaCodec is hardware-accelerated, but some low-end devices use software fallback or have broken OMX implementations that produce non-standard H.264 streams.
*   **Clock Synchronization:** Android and desktop monotonic clocks are not synchronized. Timestamps are useful for measuring one-way latency approximately but cannot be used for absolute sync without a handshake protocol.

**Platform Limitations:**
*   **Android USB Restrictions:** ADB reverse requires Android 5.0+ (API 21). The user must manually approve the RSA key dialog on each new desktop. Some OEMs (e.g., certain Xiaomi, Huawei) restrict background services aggressively, potentially killing the foreground service.
*   **OBS Plugin Constraints:** The plugin must be compiled for the exact OBS version and architecture (e.g., OBS 28+ x64). libobs API changes between major versions. Memory leaks or segfaults in the plugin crash the entire OBS process.
*   **Desktop Decoder Fragmentation:** Hardware decoder availability varies by OS and GPU. The desktop service must gracefully fall back to software decoding, which may consume significant CPU at 1080p60.

**Potential Failure Points:**
*   ADB daemon crashes or fails to detect the device due to driver issues (common on Windows without Google USB drivers).
*   MediaCodec encoder enters a bad state after an Android camera resolution switch, requiring a full pipeline restart.
*   OBS source is created before the desktop service is ready, causing the plugin to fail initial connection and requiring robust retry logic.
*   USB port power management on the desktop causes the USB controller to sleep, dropping the ADB connection during idle periods.

---

### 8. Validation & Testing Strategy

**Phase 1 Verification:**
*   **Metric:** Throughput. Verify sustained 20 Mbps transfer with 0% packet loss over 10 minutes.
*   **Method:** Checksum validation on dummy payload blocks.
*   **Edge Case:** Unplug USB mid-transfer; verify both sides detect disconnect and reconnect automatically within 5 seconds of re-plugging.

**Phase 2 Verification:**
*   **Metric:** Encoder output validity. Feed the Android stream into `ffplay` or `ffmpeg` directly (bypassing the custom protocol) to confirm it is a standards-compliant H.264 stream.
*   **Method:** Inspect NAL unit types (IDR frequency, SPS/PPS presence).
*   **Edge Case:** Test on at least three different SoC families (Qualcomm Snapdragon, Samsung Exynos, Google Tensor) to catch OEM encoder quirks.

**Phase 3 Verification:**
*   **Metric:** Decode integrity. Verify output raw frames have no macroblocking artifacts.
*   **Method:** Save 1000 decoded frames to disk and verify checksum consistency for identical input scenes.
*   **Edge Case:** Test with the decoder under CPU stress to verify jitter buffer prevents underrun.

**Phase 4 Verification:**
*   **Metric:** OBS source stability. Source must display video within 2 seconds of creation.
*   **Method:** Automated OBS scene switching (hide/show) 50 times; verify no crash or handle leak.
*   **Edge Case:** Create multiple instances of the source pointing to different devices; verify isolation.

**Phase 5 & 6 Verification (End-to-End):**
*   **Latency Metric:** Glass-to-glass latency. Use a millisecond-precision timer displayed on a phone screen; capture it with the phone’s own camera (pointed at the screen) and compare the OBS-rendered timestamp against the real-time timestamp. Target <150ms at 720p30.
*   **FPS Metric:** Compare OBS stats panel rendered FPS against target. Must maintain >95% of target FPS over 1 hour.
*   **Stability Metric:** Run a 4-hour burn-in test. Monitor for:
    *   Memory growth in Desktop Service and OBS process (must be <10MB growth).
    *   Frame drops (must be <0.1%).
    *   ADB disconnections (must be 0 unless physical USB is disturbed).
*   **Edge Cases:**
    *   Switch Android apps during stream (verify foreground service persistence).
    *   Change OBS canvas resolution mid-stream (verify plugin rescales gracefully).
    *   Use a USB 2.0 port instead of USB 3.0 (verify no bandwidth saturation at default bitrates).
    *   Connect two Android devices simultaneously (verify serial number selection works correctly).
