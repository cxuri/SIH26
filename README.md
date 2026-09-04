# ESP32 Low-Power Audio Prototype: Experimental Results

> **Headline Result:** The prototype demonstrated sub-10% measured application CPU utilization (7.33%) and approximately 81.0 KB heap usage against a 256 KB project budget, while successfully maintaining a 1.5-second audio pre-roll buffer and wake-triggered Wi-Fi operation.

## 1. Prototype Target vs. Measured Performance

| Parameter | Measured Result | Target Constraint | Status |
| --- | --- | --- | --- |
| **CPU Utilization** | 7.33% | < 10.0% | **PASS** |
| **Heap Used** | 81.01 KB | < 256.0 KB | **PASS** |
| **Heap Used vs Budget** | 31.65% | < 100% | **PASS** |
| **Audio Pre-Roll Buffer** | 46.88 KB | 1.5 seconds | **PASS** |

## 2. System Telemetry Breakdown

**CPU Utilization (240 MHz, Single-Core FreeRTOS)**

* **Acquisition CPU:** 1.180%
* **VAD/DSP CPU:** 0.157%
* **Monitor CPU:** 5.989%
* **Wi-Fi/Ping CPU:** 0.001%
* **Total Measured CPU:** 7.326% *(Estimated Idle: 92.674%)*

**Internal RAM & Heap**

* **Total Internal Heap:** 319.91 KB
* **Used Heap:** 81.01 KB
* **Minimum Free Heap:** 204.91 KB
* **Largest Free Block:** 107.99 KB

**Network & Stability**

* **Continuous Processing:** 3,584 consecutive frames processed (zero drops).
* **Wi-Fi RTT:** 53–209 ms observed.
* **Wi-Fi RSSI:** -47 dBm observed.

## 3. Core Architectural Behaviors Validated

1. **Persistent Audio Buffering:** Successfully maintained a 46.88 KB (1.5s, 16kHz, 16-bit mono) ring buffer with no memory leaks over extended runs.
2. **Event-Driven Networking:** Wi-Fi radio successfully activated *only* post-wake, executed a TCP connection/RTT test, and smoothly disconnected to return to a low-power state.
3. **Monitored Stack Headroom:** Verified no task starvation or stack overflow during maximum load spikes (Acquisition: >1,500 words free, DSP: >1,050 words free).

## 4. Current Limitations & Next Steps

These measurements validate the baseline resource-gated architecture. To transition to a production build, the following simulated components must be replaced and benchmarked on final hardware:

* **Hardware Integration:** Replace synthetic input with a physical I2S MEMS microphone.
* **AI/ML Deployment:** Integrate the quantized INT8 KWS neural-network model and measure False Activations/Hour (FAH) and False Rejection Rate (FRR).
* **Audio Encoding:** Implement and benchmark the Opus encoder memory/CPU footprint.
* **Power & Latency Profiling:** Measure exact energy-per-inference (mJ) and end-to-end cloud ASR latency.
