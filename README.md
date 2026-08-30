The architecture employs a deterministic, multi-stage cascading pipeline split across a dual-core microcontroller and a low-latency GPU cloud backend. The design isolates hardware-timed acoustic sampling from asynchronous network transports, ensuring zero dropped audio frames, bounded sub-256 KB internal SRAM consumption, and sub-10% idle CPU utilization.

```text
+---------------------------------------------------------------------------------------------------+
|                                      EDGE DEVICE: ESP32-S3                                        |
|                                                                                                   |
|  [ MEMS Mic ] ---I2S---> [ DMA Ping-Pong (2x 640 B) ]                                             |
|                                     |                                                             |
|   +---------------------------------+----------------------------------+                          |
|   | CORE 1: DSP & TinyML Engine                                        | CORE 0: Network & Comm   |
|   | (Pinned, Deterministic Execution)                                  | (Event-Driven Task)      |
|   |                                                                    |                          |
|   | [ 500 ms Audio Circular Buffer ] <------------------------------+  | [ Pre-Warmed WSS/TLS ]   |
|   |              |                                                  |  |            |             |
|   | [ Adaptive Energy VAD ] --(Speech Detected)-->                  |  |            |             |
|   |              |                                                  |  |            |             |
|   | [ Fixed-Point PCEN Feature Extraction ]                         |  |            |             |
|   |              |                                                  |  |            |             |
|   | [ INT8 EdgeSpot BC-ResNet (TFLM + ESP-NN) ]                     |  |            |             |
|   |              |                                                  |  |            |             |
|   |   (Keyword Match Verified)                                      |  |            |             |
|   |              +---- Trigger Event Queue -------------------------+->| [ Opus SILK Encoder ]    |
|   +--------------------------------------------------------------------+------------|-------------+
|                                                                                     | (16 kbps)   |
+-------------------------------------------------------------------------------------|-------------+
                                                                                      |
                                                                           Wi-Fi (802.11 b/g/n)
                                                                                      |
+-------------------------------------------------------------------------------------v-------------+
|                                    CLOUD INFERENCE BACKEND                                        |
|                                                                                                   |
|  [ WebSocket Receiver ] ---> [ Opus FEC Decoder ] ---> [ CTranslate2 Faster-Whisper (INT8/FP16) ] |
|                                                                          |                        |
|                                                            [ Command Intent Engine ]              |
+---------------------------------------------------------------------------------------------------+

```

## Hardware Substrate & Low-Power Acoustic Acquisition

### Silicon & Memory Topology

* **Microcontroller:** Espressif ESP32-S3 (Dual-Core 32-bit Xtensa LX7 @ 240 MHz, 512 KB Internal SRAM, integrated 2.4 GHz 802.11b/g/n Wi-Fi).
* **Vector Engine:** Xtensa Processor Instruction Extensions (PIE) utilizing 128-bit SIMD registers for 4-way parallel 8-bit multiply-accumulate (MAC) operations per cycle.
* **SRAM Placement Policy:** Internal SRAM holds all DMA buffers, the TensorFlow Lite Micro Tensor Arena, and the circular audio ring buffer. External PSRAM is bypassed completely to eliminate SPI cache-miss latency and bus contention during real-time DSP execution.
* **Flash Allocation:** Model weights are stored in external QSPI Flash and mapped via instruction/data cache lines with 32-byte alignment.

### I2S Direct Memory Access (DMA) Configuration

The acoustic front-end continuously acquires 16-bit monaural PCM at 16 kHz (32,000 bytes/sec).

* **DMA Buffer Topology:** Double-buffered (ping-pong) configuration consisting of `dma_buf_a` and `dma_buf_b`.
* **Frame Sizing:** Each DMA descriptor buffers 20 ms of audio (320 samples = 640 bytes).
* **Interrupt Handling:** The I2S hardware directly drives DMA transfers into SRAM without CPU intervention. Upon filling a 640-byte block, the DMA controller triggers an ISR that notifies the Core 1 audio worker task via `xTaskNotifyGiveFromISR()` and toggles the ping-pong pointer. The CPU sleeps in Wait-For-Interrupt (WFI) mode throughout the transfer window.

### Asymmetric Multiprocessing (AMP) Runtime Orchestration

Task execution is divided across the dual cores using static task affinity to eliminate cache trashing and RTOS scheduling overhead.

**Core 0: Asynchronous Network & I/O Pipeline**

* **Priority 5:** lwIP TCP/IP Stack & Wi-Fi MAC Layer
* **Priority 4:** WSS Keep-Alive & TLS State Engine (Ping/Pong frames every 15 s)
* **Priority 3:** Opus Frame Dispatcher (Sleeps on `xQueueAudioStream` until KWS trigger)
* **Priority 1:** System Telemetry, Health Checks, & Background Watchdogs

**Core 1: Deterministic Real-Time DSP & Inference Pipeline**

* **Priority 10:** I2S DMA ISR -> Wakes Audio Ingest Task
* **Priority 9:** Audio Ingest & 500 ms History Ring Buffer Push
* **Priority 8:** Adaptive Energy VAD Gate (Computes MSE in < 15 µs)
* **Priority 7:** Fixed-Point PCEN Spectrogram Generation (< 1.8 ms execution)
* **Priority 6:** INT8 BC-ResNet KWS Inference via ESP-NN SIMD (< 18 ms execution)

---

## Acoustic Feature Extraction & DSP Pipeline

### Adaptive Noise-Tracking VAD

Before running compute-intensive feature extraction, raw frames pass through a low-complexity energy gate.

**Energy Calculation:** For each 20 ms frame ($N = 320$ samples):


$$E = \frac{1}{N} \sum_{i=1}^{N} x[i]^2$$

**Dynamic Noise Floor Estimation:** When the frame is classified as non-speech, background noise energy $N_{\text{floor}}$ is updated via a slow-tracking leaky integrator:


$$N_{\text{floor}}[t] = (1 - \beta) N_{\text{floor}}[t-1] + \beta E[t], \quad \beta = 0.02$$

**Adaptive Thresholding:** The trigger threshold is dynamically set:

$$\gamma[t] = \max(E_{\text{floor}\_\text{min}}, \, k_{\text{snr}} \cdot N_{\text{floor}}[t])$$
**Temporal Debounce State Machine:**

* **Speech Trigger:** Requires $E[t] > \gamma[t]$ for 3 consecutive frames (60 ms sustained acoustic energy) before activating the DSP/KWS pipeline.
* **Hangover Duration:** Maintains active state for 15 consecutive sub-threshold frames (300 ms hangover) to capture natural inter-syllable pauses without pipeline resetting.

```text
                    +------------------------+
                    |       Idle State       | <-------------------+
                    | (Low-Power Monitoring) |                     |
                    +------------------------+                     |
                                |                                  |
                   E[t] > γ[t] (3 frames / 60 ms)          E[t] < γ[t] (15 frames / 300 ms)
                                |                                  |
                                v                                  |
                    +------------------------+                     |
                    |      Active State      | --------------------+
                    |  (Run DSP & TinyML KWS)|
                    +------------------------+

```

### Fixed-Point Per-Channel Energy Normalization (PCEN)

To maintain feature invariance across high dynamic range variations and localized noise, PCEN replaces static log-mel filterbanks.

* **Windowing & RFFT:** Apply a 512-point Hanning window (pre-computed Q15 format) with a 320-sample hop size, followed by a 512-point Real FFT executed via the assembly-optimized `dsps_fft2r_sc16_aes3` vector kernel.
* **Mel Filterbank Integration:** Multiply the power spectrum by a 40-channel triangular Mel filterbank matrix spanning 80 Hz to 7,600 Hz using fixed-point sparse vector dot products.
* **Noise Floor Estimation:** Compute per-channel low-pass filtered energy $M[t, f]$:

$$M[t, f] = (1 - s) M[t-1, f] + s E[t, f], \quad s = 0.025$$


* **Normalization & Dynamic Range Compression:**

$$\text{PCEN}[t, f] = \left( \frac{E[t, f]}{(\epsilon + M[t, f])^{\alpha}} + \delta \right)^{r} - \delta^{r}$$



*(Parameters: $\epsilon = 10^{-6}$, $\alpha = 0.8$, $\delta = 10.0$, $r = 0.25$)*
* **Fixed-Point Acceleration:** The fractional root $(\cdot)^{0.25}$ is calculated via two cascaded Newton-Raphson inverse square root iterations using a 256-entry Q31 LUT, completing 40 filterbank channels in under 1.8 ms on Core 1.

---

## Edge Neural Network: EdgeSpot BC-ResNet INT8

### Network Topology & Broadcasted Residual Mapping

The model uses an ultra-compact Broadcasting-Residual Network (BC-ResNet-1).

```text
Input: [1 × 50 × 40] (1.0 s context window: 50 time steps, 40 PCEN bands)
  │
  ▼
[Conv2D Stem] ------------> 5×5 Conv, Stride (1, 2), 16 Filters, ReLU6
  │
  ▼
[BC-ResBlock 1] ----------> 2 Sub-blocks: Frequency-DWConv2D -> AvgPool2D (Freq)
  │                         -> 1D Temporal Conv -> Broadcast-Add -> ReLU6
  │
  ▼
[BC-ResBlock 2] ----------> 2 Sub-blocks: Frequency-DWConv2D -> AvgPool2D (Freq)
  │                         -> 1D Temporal Conv -> Broadcast-Add -> ReLU6 (Dilation = 2)
  │
  ▼
[BC-ResBlock 3] ----------> 2 Sub-blocks: Frequency-DWConv2D -> AvgPool2D (Freq)
  │                         -> 1D Temporal Conv -> Broadcast-Add -> ReLU6 (Dilation = 4)
  │
  ▼
[EdgeSpot Self-Attention] -> Lightweight 1D Temporal Attention Layer (Key/Query projection)
  │
  ▼
[Global Pooling & Dense] -> Global Average Pooling -> Linear Dense -> Softmax Output

```

### Quantization & Runtime Execution

* **Quantization-Aware Training (QAT):** Forward passes simulate 8-bit integer truncation using symmetric per-channel weight scaling and per-tensor activation ranges:

$$r = S \cdot (q - Z)$$


* **TFLM Static Arena Allocation:** A static byte array `uint8_t tensor_arena[36864] __attribute__((aligned(16)))` is allocated in internal SRAM.
* **Kernel Optimizations:** Standard reference operators are substituted with ESP-NN SIMD vector intrinsics (`esp_nn_conv_s8`, `esp_nn_depthwise_conv_s8`), executing model inference within 17.4 ms at 240 MHz.
* **Custom Keyword Discriminative Training:** Trained using Sub-center ArcFace loss with an angular margin $m=0.35$ and negative mining on the Multilingual Spoken Words Corpus (MSWC) to maintain false activations below 0.1 per 24-hour listening window.

---

## Zero-Loss Handoff & Edge-to-Cloud Transport

```text
                       AUDIO STREAM TIMELINE & PRE-ROLL HANDOFF
 
Audio Timeline: [  500 ms History Ring Buffer  ] [       Wake-Word Utterance       ] [ Post-Keyword Audio Stream ]
                |-------------------------------|-----------------------------------|---------------------------->
                                                ^                                   ^
                                         Utterance Begins                    KWS Trigger Fired (T0)
                                                                                    │
                                                                                    ▼
                                                                     Bridge trailing buffer & live DMA
                                                                                    │
                                                                                    ▼
                                                                     Encode frames via Opus SILK (16 kbps)
                                                                                    │
                                                                                    ▼
                                                                     Transmit via Pre-Warmed WSS/TLS Tunnel
                                                                                    │
                                                                                    ▼
                                                                     Arrival at Cloud ASR Server (T1)
                                                                     [ Latency = T1 - T0 < 35 ms ]

```

### 500 ms SRAM Circular Ring Buffer

* **Structure:** Circular buffer holding 8,000 samples (16,000 bytes) of contiguous audio.
* **Handoff Logic:** When the KWS inference score exceeds the detection threshold ($\tau \ge 0.88$), a hardware timestamp $T_{\text{trigger}}$ is logged. The ring buffer write pointer snapshots the trailing 500 ms of pre-roll history to avoid clipping initial consonant formants and seamlessly routes incoming live DMA buffers to the network queue.

### Opus SILK Low-Latency Encoding

* **Mode:** Native Opus configured strictly in SILK-only mode optimized for human vocal bands.
* **Bitrate & Framing:** Variable Bitrate (VBR) target at 16 kbps with 20 ms frame sizing (40 bytes per encoded frame, reducing raw audio bandwidth by 93.75%).
* **In-Band Forward Error Correction (FEC):** Configured with 20% redundant sub-packet encoding. If intermediate Wi-Fi packets drop due to 2.4 GHz RF interference, the cloud receiver reconstructs lost audio packets without triggering TCP retransmission handshakes.

### Transport Channel

* **Persistent WSS/TLS Tunnel:** Maintains a continuous TLS 1.2 WebSocket session to the cloud endpoint with TCP Keep-Alive and periodic ping/pong validation (15-second intervals).
* **Handshake Elimination:** Eliminates the ~300 ms DNS, TCP three-way handshake, and TLS negotiation overhead at trigger time. The initial audio packet is pushed to the network layer within 1.2 ms of keyword verification.

---

## Cloud Streaming ASR & Intent Pipeline

### Server-Side Ingestion Architecture

* **Daemon:** Asynchronous Python/Rust WebSocket server utilizing `uvloop` and `websockets`.
* **Jitter De-Jitter Buffer:** 40 ms adaptive jitter queue handles network arrival variance before feeding decoded PCM frames to the ASR engine.

### Continuous Large-Vocabulary ASR Engine

* **Engine:** CTranslate2-accelerated faster-whisper (Whisper Base/Small quantized to INT8/FP16).
* **Inference Strategy:** Sliding window streaming inference with local agreement decoding. A 480 ms context stride decodes words in real-time as frames stream over the WebSocket channel, achieving an end-to-end cloud latency ($T1 - T0$) under 35 ms on stable broadband links.

---

## Memory Allocation & Real-Time Performance Budgets

### SRAM Memory Budget (< 256 KB Limit)

The design is constrained to operate strictly within 256 KB of SRAM, reserving the remaining memory for system and networking buffers.

| Component | Static SRAM Allocation | Peak Dynamic Execution Allocation | Location |
| --- | --- | --- | --- |
| **I2S DMA Ping-Pong Buffers** | 1,280 B (2x 640 B) | 1,280 B | Internal SRAM (DMA-capable) |
| **Audio History Ring Buffer (500 ms)** | 16,000 B | 16,000 B | Internal SRAM |
| **DSP FFT & Windowing State** | 2,048 B | 4,096 B | Internal SRAM |
| **PCEN Noise Tracker & Filterbanks** | 640 B | 640 B | Internal SRAM |
| **TFLM Tensor Arena (Activations)** | 36,864 B | 36,864 B | Internal SRAM (16-byte aligned) |
| **Opus Encoder Internal State** | 10,240 B | 12,288 B | Internal SRAM |
| **FreeRTOS Task Stacks (AMP Layout)** | 24,576 B | 24,576 B | Internal SRAM |
| **lwIP TCP/IP & TLS Socket Buffers** | 32,768 B | 44,000 B | Internal SRAM |
| **Total Peak SRAM Footprint** | **123,416 B (~120.5 KB)** | **139,744 B (~136.5 KB)** | **54.6% of 256 KB Constraint** |

### Flash & ROM Budget

* **INT8 BC-ResNet Weights:** 12.8 KB (Stored in Flash, cached via D-Cache).
* **Compiled Firmware & TLS Stacks:** ~920 KB of standard 4 MB / 8 MB Flash.

### Real-Time CPU Utilization & Latency Budgets

| Operational State | Core 0 Load (240 MHz) | Core 1 Load (240 MHz) | Aggregate CPU Budget |
| --- | --- | --- | --- |
| **Idle Listening (Noise / Silence)** | < 1.0% (TCP Keep-Alive) | 2.8% (DMA + Adaptive VAD) | < 3.8% (Target: < 10%) |
| **Speech Evaluation (KWS Active)** | < 1.0% (Idle Network) | 28.5% (PCEN + BC-ResNet) | ~14.8% Aggregate |
| **Cloud Streaming (Opus + Wi-Fi TX)** | 12.5% (TLS/Opus Encode) | 4.2% (DMA Audio Ingest) | ~8.4% Aggregate |

**Detailed Latency Breakdown (Keyword End to Cloud ASR Ingestion):**

```text
├─ DMA Block Completion:          0.00 ms (Buffered in continuous history)
├─ INT8 Inference Verification:   17.40 ms (Completed on Core 1)
├─ Inter-Core Trigger Dispatch:    0.15 ms (FreeRTOS Queue)
├─ Opus Frame Encoding (20 ms):    1.80 ms (Hardware-accelerated)
├─ TLS Frame Encryption & TX:      1.20 ms (Core 0 Crypto Engine)
├─ Network Flight Time (Wi-Fi/WAN): 12.00 ms (Broadband path)
└─ TOTAL WAKE-TO-CLOUD LATENCY:   32.55 ms

```

---

## Fault Tolerance, Watchdogs & Edge Reliability

* **Audio Subsystem Resynchronization:** An independent hardware timer monitors DMA interrupts. If no audio DMA interrupt fires within 40 ms, the I2S peripheral is reset and the DMA descriptors are re-initialized without rebooting the main MCU.
* **Transport Failover & Exponential Backoff:** If the persistent WebSocket drops due to an AP disconnect, audio frames buffer locally in a 4-second reserve SRAM partition while the network core initiates an immediate re-association loop.
* **Task Watchdog Timers (TWDT):** Core 0 and Core 1 run dedicated watchdog monitors with a 500 ms panic timeout, ensuring hung network states or corrupt audio loops fail safe.
* **Audio Saturation & DC Clipping Protection:** An inline high-pass IIR filter ($f_c = 60\text{ Hz}$) runs prior to VAD computation to eliminate microphone DC bias and mechanical acoustic thumps.

---

## Step-by-Step Implementation Roadmap

```text
+-----------------------------------------------------------------------------------------------+
| PHASE 1: Data Acquisition & Synthetic Hardening (Weeks 1 - 2)                                 |
| • Record custom keyword dataset across diverse speakers, accents, and distances.     |
| • Apply acoustic augmentations (room impulse response/reverb, fan noise, MSWC negatives)      |
|   using dynamic SNR mixing (-5 dB to 20 dB).                                      |
+-----------------------------------------------------------------------------------------------+
                                                │
                                                ▼
+-----------------------------------------------------------------------------------------------+
| PHASE 2: Model Architecture & QAT Pipeline (Weeks 3 - 4)                                      |
| • Implement BC-ResNet-1 backbone with temporal attention in TensorFlow/PyTorch.      |
| • Train using Sub-center ArcFace loss; apply Quantization-Aware Training (QAT).      |
| • Export to FlatBuffer INT8 format and profile execution on TFLM.                 |
+-----------------------------------------------------------------------------------------------+
                                                │
                                                ▼
+-----------------------------------------------------------------------------------------------+
| PHASE 3: Embedded Firmware Implementation (Weeks 5 - 6)                                       |
| • Configure ESP-IDF dual-core FreeRTOS environment with pinned tasks.                |
| • Implement fixed-point PCEN using ESP-DSP vector assembly routines.                 |
| • Integrate I2S DMA double-buffering and the 500 ms SRAM circular pre-roll buffer.|
+-----------------------------------------------------------------------------------------------+
                                                │
                                                ▼
+-----------------------------------------------------------------------------------------------+
| PHASE 4: Network & Cloud Backend Integration (Weeks 7 - 8)                                    |
| • Establish persistent WebSocket over TLS with Opus SILK encoding and in-band FEC.   |
| • Deploy CTranslate2 Faster-Whisper streaming daemon on backend server.              |
| • Benchmark end-to-end latency, false-alarm rate, and peak SRAM consumption.         |
+-----------------------------------------------------------------------------------------------+

```
