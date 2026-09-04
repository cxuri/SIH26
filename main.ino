#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"

// ============================================================
//                 PROTOTYPE CONFIGURATION
// ============================================================

#define WIFI_SSID       "ssh"
#define WIFI_PASSWORD   "12345678"

#define TEST_HOST       "example.com"
#define TEST_PORT       80

#define SAMPLE_RATE     16000
#define PRE_ROLL_SEC    1.5f
#define BYTES_PER_SAMPLE 2

#define RING_BUFFER_SIZE \
    ((size_t)(SAMPLE_RATE * PRE_ROLL_SEC * BYTES_PER_SAMPLE))

#define FRAME_SAMPLES    480
#define FRAME_BYTES      (FRAME_SAMPLES * BYTES_PER_SAMPLE)

#define WAKE_INTERVAL_MS       10000
#define WIFI_TIMEOUT_MS         8000
#define WIFI_OFF_DELAY_MS       5000

#define MONITOR_INTERVAL_MS     3000

#define CPU_TARGET_PERCENT      10.0f
#define RAM_TARGET_KB           256.0f

#define PI 3.14159265358979323846f

// ============================================================
//                         GLOBALS
// ============================================================

RingbufHandle_t audioRing = NULL;

TaskHandle_t acquisitionTaskHandle = NULL;
TaskHandle_t dspTaskHandle = NULL;
TaskHandle_t wifiTaskHandle = NULL;
TaskHandle_t monitorTaskHandle = NULL;

// ------------------------------------------------------------
// Audio / VAD
// ------------------------------------------------------------

volatile bool speechActive = false;

float noiseFloor = 1000.0f;

const float VAD_ON_MULT  = 4.0f;
const float VAD_OFF_MULT = 2.0f;

float previousSample = 0.0f;

uint32_t framesGenerated = 0;
uint32_t framesProcessed = 0;
uint32_t speechFrames = 0;

uint64_t bufferWritten = 0;
uint64_t bufferRead = 0;

// ------------------------------------------------------------
// Wake / WiFi
// ------------------------------------------------------------

volatile bool wakeEvent = false;
volatile bool wifiRequested = false;

uint32_t wakeEvents = 0;

uint32_t wifiConnections = 0;
uint32_t wifiFailures = 0;

uint32_t connectivityTests = 0;
uint32_t connectivitySuccess = 0;
uint32_t connectivityFailure = 0;

uint32_t lastRTT = 0;
bool lastConnectivityPass = false;

// TCP does not expose ICMP TTL.
int lastTTL = -1;

// ------------------------------------------------------------
// CPU accounting
// ------------------------------------------------------------
//
// Instead of using unsupported ESP-IDF runtime-stat APIs,
// each task measures only the time it actually spends executing.
//
// Because the ESP32 is single-core, these measured execution
// intervals are accumulated and converted to utilization.
//
// The accounting window is reset every monitor interval.
// ------------------------------------------------------------

volatile uint64_t acquisitionCpuUs = 0;
volatile uint64_t dspCpuUs = 0;
volatile uint64_t wifiCpuUs = 0;
volatile uint64_t monitorCpuUs = 0;

volatile uint32_t cpuWindowStart = 0;

// ------------------------------------------------------------
// WiFi state
// ------------------------------------------------------------

volatile bool wifiOn = false;

// ============================================================
//                         UTILITIES
// ============================================================

static inline float bytesToKB(uint32_t bytes) {
    return (float)bytes / 1024.0f;
}

static inline float bytes64ToKB(uint64_t bytes) {
    return (float)bytes / 1024.0f;
}

// ============================================================
//                     AUDIO ACQUISITION
// ============================================================

void acquisitionTask(void *parameter) {

    int16_t frame[FRAME_SAMPLES];

    uint32_t frameNumber = 0;

    float phase = 0.0f;

    const float frequency = 440.0f;

    while (true) {

        uint32_t startUs = micros();

        // ----------------------------------------------------
        // Simulated microphone
        //
        // Every ~3 seconds:
        //   silence
        //   speech
        // ----------------------------------------------------

        bool generateSpeech =
            ((frameNumber / 100) % 3) == 1;

        for (int i = 0; i < FRAME_SAMPLES; i++) {

            if (generateSpeech) {

                float sample =
                    3000.0f * sinf(phase);

                sample +=
                    (float)((rand() % 80) - 40);

                frame[i] = (int16_t)sample;

                phase +=
                    2.0f * PI *
                    frequency /
                    SAMPLE_RATE;

                if (phase >= 2.0f * PI) {
                    phase -= 2.0f * PI;
                }

            } else {

                frame[i] =
                    (int16_t)((rand() % 200) - 100);
            }
        }

        size_t sent =
            xRingbufferSend(
                audioRing,
                frame,
                FRAME_BYTES,
                pdMS_TO_TICKS(5)
            );

        if (sent) {
            bufferWritten += FRAME_BYTES;
            framesGenerated++;
        }

        frameNumber++;

        uint32_t elapsed =
            micros() - startUs;

        acquisitionCpuUs += elapsed;

        // Keep acquisition close to real-time 30 ms frames.
        if (elapsed < 30000) {
            vTaskDelay(
                pdMS_TO_TICKS(
                    30 - (elapsed / 1000)
                )
            );
        } else {
            taskYIELD();
        }
    }
}

// ============================================================
//                         DSP / VAD
// ============================================================

void dspTask(void *parameter) {

    while (true) {

        size_t receivedSize = 0;

        int16_t *audio =
            (int16_t *)xRingbufferReceive(
                audioRing,
                &receivedSize,
                portMAX_DELAY
            );

        if (audio == NULL) {
            continue;
        }

        uint32_t startUs = micros();

        size_t samples =
            receivedSize / BYTES_PER_SAMPLE;

        if (samples > FRAME_SAMPLES) {
            samples = FRAME_SAMPLES;
        }

        float energy = 0.0f;

        for (size_t i = 0; i < samples; i++) {

            float current =
                (float)audio[i];

            float filtered =
                current -
                0.9375f * previousSample;

            previousSample = current;

            energy +=
                filtered * filtered;
        }

        if (samples > 0) {
            energy /= samples;
        }

        // ----------------------------------------------------
        // Adaptive noise floor
        // ----------------------------------------------------

        if (!speechActive) {

            noiseFloor =
                (noiseFloor * 0.95f) +
                (energy * 0.05f);

            if (energy >
                noiseFloor * VAD_ON_MULT) {

                speechActive = true;

                Serial.printf(
                    "[VAD] WAKE / SPEECH DETECTED "
                    "Energy=%lu Noise=%lu\n",
                    (unsigned long)energy,
                    (unsigned long)noiseFloor
                );

                wakeEvent = true;
                wifiRequested = true;
                wakeEvents++;
            }

        } else {

            speechFrames++;

            if (energy <
                noiseFloor * VAD_OFF_MULT) {

                speechActive = false;

                Serial.println(
                    "[VAD] Speech ended."
                );
            }
        }

        framesProcessed++;

        bufferRead += receivedSize;

        vRingbufferReturnItem(
            audioRing,
            (void *)audio
        );

        uint32_t elapsed =
            micros() - startUs;

        dspCpuUs += elapsed;
    }
}

// ============================================================
//                       WIFI TEST
// ============================================================
//
// We intentionally DO NOT use:
//
//   WiFi.ping()
//   esp_ping_*
//   getaddrinfo()
//   sockaddr_in
//
// Those caused the compilation failures you were getting.
//
// Instead we perform a TCP connection test.
//
// This gives us:
//   connection success
//   RTT
//
// TTL is legitimately N/A because TCP connection setup through
// WiFiClient does not expose the ICMP TTL field.
// ============================================================

bool performConnectivityTest() {

    connectivityTests++;

    WiFiClient client;

    uint32_t start = millis();

    bool connected =
        client.connect(
            TEST_HOST,
            TEST_PORT
        );

    uint32_t elapsed =
        millis() - start;

    lastRTT = elapsed;

    if (connected) {

        connectivitySuccess++;
        lastConnectivityPass = true;

        client.stop();

        return true;
    }

    connectivityFailure++;
    lastConnectivityPass = false;

    client.stop();

    return false;
}

// ============================================================
//                         WIFI TASK
// ============================================================

void wifiTask(void *parameter) {

    while (true) {

        // ----------------------------------------------------
        // Wait for simulated wake event
        // ----------------------------------------------------

        if (!wifiRequested) {

            WiFi.mode(WIFI_OFF);
            wifiOn = false;

            vTaskDelay(
                pdMS_TO_TICKS(100)
            );

            continue;
        }

        uint32_t startUs = micros();

        wifiRequested = false;
        wakeEvent = false;

        Serial.println();
        Serial.println(
            "[POWER] Wake event."
        );

        Serial.println(
            "[WIFI] Turning WiFi ON..."
        );

        WiFi.mode(WIFI_STA);

        WiFi.begin(
            WIFI_SSID,
            WIFI_PASSWORD
        );

        wifiOn = true;

        uint32_t start =
            millis();

        while (
            WiFi.status() != WL_CONNECTED &&
            millis() - start < WIFI_TIMEOUT_MS
        ) {

            vTaskDelay(
                pdMS_TO_TICKS(100)
            );
        }

        if (WiFi.status() ==
            WL_CONNECTED) {

            wifiConnections++;

            Serial.println(
                "[WIFI] Connected."
            );

            Serial.print(
                "[WIFI] IP: "
            );

            Serial.println(
                WiFi.localIP()
            );

            Serial.print(
                "[WIFI] RSSI: "
            );

            Serial.print(
                WiFi.RSSI()
            );

            Serial.println(
                " dBm"
            );

            bool result =
                performConnectivityTest();

            if (result) {

                Serial.printf(
                    "[NET] PASS - RTT: %lu ms\n",
                    (unsigned long)lastRTT
                );

            } else {

                Serial.println(
                    "[NET] FAIL"
                );
            }

        } else {

            wifiFailures++;

            lastConnectivityPass = false;

            Serial.println(
                "[WIFI] Connection FAILED."
            );

            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);

            wifiOn = false;
        }

        uint32_t elapsed =
            micros() - startUs;

        wifiCpuUs += elapsed;

        // ----------------------------------------------------
        // Keep WiFi alive briefly after wake.
        // ----------------------------------------------------

        if (wifiOn) {

            vTaskDelay(
                pdMS_TO_TICKS(
                    WIFI_OFF_DELAY_MS
                )
            );

            WiFi.disconnect(true);

            WiFi.mode(WIFI_OFF);

            wifiOn = false;

            Serial.println(
                "[POWER] WiFi OFF."
            );
        }
    }
}

// ============================================================
//                       RAM TELEMETRY
// ============================================================

void printRamTelemetry() {

    uint32_t totalHeap =
        heap_caps_get_total_size(
            MALLOC_CAP_INTERNAL
        );

    uint32_t freeHeap =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL
        );

    uint32_t minimumFree =
        heap_caps_get_minimum_free_size(
            MALLOC_CAP_INTERNAL
        );

    uint32_t largestBlock =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL
        );

    uint32_t usedHeap =
        totalHeap - freeHeap;

    Serial.println();
    Serial.println(
        "================================================"
    );

    Serial.println(
        "                 RAM TELEMETRY"
    );

    Serial.println(
        "================================================"
    );

    Serial.printf(
        "Internal Heap Total       : %lu bytes | %.2f KB\n",
        (unsigned long)totalHeap,
        bytesToKB(totalHeap)
    );

    Serial.printf(
        "Internal Heap Used        : %lu bytes | %.2f KB\n",
        (unsigned long)usedHeap,
        bytesToKB(usedHeap)
    );

    Serial.printf(
        "Internal Heap Free        : %lu bytes | %.2f KB\n",
        (unsigned long)freeHeap,
        bytesToKB(freeHeap)
    );

    Serial.printf(
        "Minimum Free Heap         : %lu bytes | %.2f KB\n",
        (unsigned long)minimumFree,
        bytesToKB(minimumFree)
    );

    Serial.printf(
        "Largest Free Block        : %lu bytes | %.2f KB\n",
        (unsigned long)largestBlock,
        bytesToKB(largestBlock)
    );

    Serial.println(
        "------------------------------------------------"
    );

    Serial.printf(
        "Audio Ring Buffer         : %lu bytes | %.2f KB\n",
        (unsigned long)RING_BUFFER_SIZE,
        bytesToKB(RING_BUFFER_SIZE)
    );

    Serial.println(
        "------------------------------------------------"
    );

    if (acquisitionTaskHandle) {

        UBaseType_t freeWords =
            uxTaskGetStackHighWaterMark(
                acquisitionTaskHandle
            );

        Serial.printf(
            "Acquisition Stack Free    : %lu words\n",
            (unsigned long)freeWords
        );
    }

    if (dspTaskHandle) {

        UBaseType_t freeWords =
            uxTaskGetStackHighWaterMark(
                dspTaskHandle
            );

        Serial.printf(
            "DSP Stack Free            : %lu words\n",
            (unsigned long)freeWords
        );
    }

    if (wifiTaskHandle) {

        UBaseType_t freeWords =
            uxTaskGetStackHighWaterMark(
                wifiTaskHandle
            );

        Serial.printf(
            "WiFi Stack Free           : %lu words\n",
            (unsigned long)freeWords
        );
    }

    if (monitorTaskHandle) {

        UBaseType_t freeWords =
            uxTaskGetStackHighWaterMark(
                monitorTaskHandle
            );

        Serial.printf(
            "Monitor Stack Free        : %lu words\n",
            (unsigned long)freeWords
        );
    }

    Serial.println(
        "------------------------------------------------"
    );

    Serial.printf(
        "Audio Buffer Allocated    : %.2f KB\n",
        bytesToKB(RING_BUFFER_SIZE)
    );

    Serial.printf(
        "RAM Used Percentage       : %.2f %%\n",
        totalHeap ?
        ((float)usedHeap /
         (float)totalHeap) * 100.0f :
        0.0f
    );

    Serial.printf(
        "RAM Target                : < %.2f KB\n",
        RAM_TARGET_KB
    );

    Serial.printf(
        "RAM Status                : %s\n",
        bytesToKB(usedHeap) <
        RAM_TARGET_KB ?
        "PASS" :
        "ABOVE TARGET"
    );
}

// ============================================================
//                       CPU TELEMETRY
// ============================================================

void printCpuTelemetry() {

    uint32_t now =
        millis();

    uint32_t elapsedMs =
        now - cpuWindowStart;

    if (elapsedMs == 0) {
        return;
    }

    uint64_t windowUs =
        (uint64_t)elapsedMs * 1000ULL;

    float acquisition =
        ((float)acquisitionCpuUs /
         (float)windowUs) * 100.0f;

    float dsp =
        ((float)dspCpuUs /
         (float)windowUs) * 100.0f;

    float wifi =
        ((float)wifiCpuUs /
         (float)windowUs) * 100.0f;

    float monitor =
        ((float)monitorCpuUs /
         (float)windowUs) * 100.0f;

    // --------------------------------------------------------
    // Since this is a single-core ESP32, total CPU cannot
    // physically exceed 100%.
    //
    // We clamp only as a safety guard against timing artifacts.
    // --------------------------------------------------------

    float total =
        acquisition +
        dsp +
        wifi +
        monitor;

    if (total > 100.0f) {
        total = 100.0f;
    }

    float idle =
        100.0f - total;

    if (idle < 0.0f) {
        idle = 0.0f;
    }

    Serial.println();
    Serial.println(
        "================================================"
    );

    Serial.println(
        "                 CPU TELEMETRY"
    );

    Serial.println(
        "================================================"
    );

    Serial.println(
        "CPU Architecture        : ESP32 Single Core"
    );

    Serial.printf(
        "CPU Frequency           : %lu MHz\n",
        (unsigned long)(ESP.getCpuFreqMHz())
    );

    Serial.println(
        "------------------------------------------------"
    );

    Serial.printf(
        "Acquisition CPU         : %.3f %%\n",
        acquisition
    );

    Serial.printf(
        "VAD/DSP CPU             : %.3f %%\n",
        dsp
    );

    Serial.printf(
        "WiFi/Network CPU        : %.3f %%\n",
        wifi
    );

    Serial.printf(
        "Monitor CPU             : %.3f %%\n",
        monitor
    );

    Serial.println(
        "------------------------------------------------"
    );

    Serial.printf(
        "TOTAL MEASURED CPU      : %.3f %%\n",
        total
    );

    Serial.printf(
        "ESTIMATED IDLE CPU      : %.3f %%\n",
        idle
    );

    Serial.println(
        "------------------------------------------------"
    );

    Serial.printf(
        "CPU TARGET              : < %.1f %%\n",
        CPU_TARGET_PERCENT
    );

    Serial.printf(
        "CPU STATUS              : %s\n",
        total < CPU_TARGET_PERCENT ?
        "PASS" :
        "ABOVE TARGET"
    );

    Serial.println(
        "================================================"
    );

    // Reset measurement window.
    acquisitionCpuUs = 0;
    dspCpuUs = 0;
    wifiCpuUs = 0;
    monitorCpuUs = 0;

    cpuWindowStart = now;
}

// ============================================================
//                    SYSTEM TELEMETRY
// ============================================================

void printSystemTelemetry() {

    Serial.println();
    Serial.println(
        "================================================"
    );

    Serial.println(
        "               SYSTEM TELEMETRY"
    );

    Serial.println(
        "================================================"
    );

    Serial.printf(
        "System State             : %s\n",
        wifiOn ?
        "WAKE / WIFI ACTIVE" :
        "LOW POWER"
    );

    Serial.printf(
        "WiFi State               : %s\n",
        wifiOn ?
        "CONNECTED/ON" :
        "OFF"
    );

    Serial.printf(
        "Speech State             : %s\n",
        speechActive ?
        "ACTIVE" :
        "IDLE"
    );

    Serial.println(
        "------------------------------------------------"
    );

    Serial.printf(
        "Frames Generated         : %lu\n",
        (unsigned long)framesGenerated
    );

    Serial.printf(
        "Frames Processed         : %lu\n",
        (unsigned long)framesProcessed
    );

    Serial.printf(
        "Speech Frames            : %lu\n",
        (unsigned long)speechFrames
    );

    Serial.printf(
        "Buffer Written           : %llu bytes | %.2f KB\n",
        (unsigned long long)bufferWritten,
        bytes64ToKB(bufferWritten)
    );

    Serial.printf(
        "Buffer Read              : %llu bytes | %.2f KB\n",
        (unsigned long long)bufferRead,
        bytes64ToKB(bufferRead)
    );

    Serial.println(
        "------------------------------------------------"
    );

    Serial.printf(
        "Wake Events              : %lu\n",
        (unsigned long)wakeEvents
    );

    Serial.printf(
        "WiFi Connections         : %lu\n",
        (unsigned long)wifiConnections
    );

    Serial.printf(
        "WiFi Failures            : %lu\n",
        (unsigned long)wifiFailures
    );

    Serial.printf(
        "Connectivity Tests       : %lu\n",
        (unsigned long)connectivityTests
    );

    Serial.printf(
        "Connectivity Success     : %lu\n",
        (unsigned long)connectivitySuccess
    );

    Serial.printf(
        "Connectivity Failure     : %lu\n",
        (unsigned long)connectivityFailure
    );

    Serial.printf(
        "Last RTT                 : %lu ms\n",
        (unsigned long)lastRTT
    );

    Serial.printf(
        "Last Network Test        : %s\n",
        lastConnectivityPass ?
        "PASS" :
        "FAIL"
    );

    Serial.printf(
        "Last TTL                 : %s\n",
        lastTTL < 0 ?
        "N/A" :
        String(lastTTL).c_str()
    );

    Serial.println(
        "------------------------------------------------"
    );

    if (wifiOn &&
        WiFi.status() == WL_CONNECTED) {

        Serial.printf(
            "WiFi RSSI                : %d dBm\n",
            WiFi.RSSI()
        );

        Serial.print(
            "WiFi IP                  : "
        );

        Serial.println(
            WiFi.localIP()
        );
    }

    Serial.println(
        "================================================"
    );
}

// ============================================================
//                       MONITOR TASK
// ============================================================

void monitorTask(void *parameter) {

    while (true) {

        uint32_t startUs =
            micros();

        printSystemTelemetry();

        printRamTelemetry();

        uint32_t elapsedUs =
            micros() - startUs;

        monitorCpuUs += elapsedUs;

        vTaskDelay(
            pdMS_TO_TICKS(
                MONITOR_INTERVAL_MS
            )
        );

        // CPU telemetry is printed after the monitor interval.
        printCpuTelemetry();
    }
}

// ============================================================
//                           SETUP
// ============================================================

void setup() {

    Serial.begin(115200);

    delay(2000);

    Serial.println();
    Serial.println();
    Serial.println(
        "================================================"
    );

    Serial.println(
        "     ESP32 LOW-POWER AUDIO PROTOTYPE"
    );

    Serial.println(
        "================================================"
    );

    Serial.println(
        "Initializing..."
    );

    Serial.printf(
        "Chip Model               : %s\n",
        ESP.getChipModel()
    );

    Serial.printf(
        "CPU Frequency            : %lu MHz\n",
        (unsigned long)ESP.getCpuFreqMHz()
    );

    Serial.printf(
        "Internal Free Heap       : %lu bytes | %.2f KB\n",
        (unsigned long)ESP.getFreeHeap(),
        bytesToKB(ESP.getFreeHeap())
    );

    Serial.println(
        "------------------------------------------------"
    );

    // --------------------------------------------------------
    // Start with WiFi completely OFF.
    // --------------------------------------------------------

    WiFi.disconnect(true);

    WiFi.mode(WIFI_OFF);

    wifiOn = false;

    // --------------------------------------------------------
    // Create 1.5 second audio ring buffer.
    // --------------------------------------------------------

    Serial.printf(
        "Creating audio buffer: %lu bytes | %.2f KB\n",
        (unsigned long)RING_BUFFER_SIZE,
        bytesToKB(RING_BUFFER_SIZE)
    );

    audioRing =
        xRingbufferCreate(
            RING_BUFFER_SIZE,
            RINGBUF_TYPE_BYTEBUF
        );

    if (audioRing == NULL) {

        Serial.println(
            "ERROR: Failed to create audio ring buffer."
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.println(
        "Audio ring buffer: OK"
    );

    Serial.printf(
        "Free Heap After Buffer : %lu bytes | %.2f KB\n",
        (unsigned long)ESP.getFreeHeap(),
        bytesToKB(ESP.getFreeHeap())
    );

    // --------------------------------------------------------
    // Start CPU measurement window.
    // --------------------------------------------------------

    cpuWindowStart = millis();

    // --------------------------------------------------------
    // FreeRTOS tasks
    // --------------------------------------------------------

    BaseType_t result;

    result = xTaskCreate(
        acquisitionTask,
        "Audio_Acquisition",
        3072,
        NULL,
        3,
        &acquisitionTaskHandle
    );

    if (result != pdPASS) {
        Serial.println(
            "ERROR: Acquisition task failed."
        );
    }

    result = xTaskCreate(
        dspTask,
        "VAD_DSP",
        3072,
        NULL,
        3,
        &dspTaskHandle
    );

    if (result != pdPASS) {
        Serial.println(
            "ERROR: DSP task failed."
        );
    }

    result = xTaskCreate(
        wifiTask,
        "WiFi_Task",
        4096,
        NULL,
        2,
        &wifiTaskHandle
    );

    if (result != pdPASS) {
        Serial.println(
            "ERROR: WiFi task failed."
        );
    }

    result = xTaskCreate(
        monitorTask,
        "Monitor",
        3072,
        NULL,
        1,
        &monitorTaskHandle
    );

    if (result != pdPASS) {
        Serial.println(
            "ERROR: Monitor task failed."
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.println();
    Serial.println(
        "================================================"
    );

    Serial.println(
        "             PROTOTYPE STARTED"
    );

    Serial.println(
        "================================================"
    );

    Serial.println(
        "WiFi                    : OFF"
    );

    Serial.println(
        "Audio Buffer            : 1.5 seconds"
    );

    Serial.println(
        "VAD                     : ENABLED"
    );

    Serial.println(
        "Simulated Microphone    : ENABLED"
    );

    Serial.println(
        "Wake Simulation         : ENABLED"
    );

    Serial.println(
        "Network Test            : TCP RTT"
    );

    Serial.println(
        "TTL                     : N/A"
    );

    Serial.println(
        "Opus                    : DISABLED"
    );

    Serial.println(
        "CPU Target              : < 10%"
    );

    Serial.println(
        "RAM Target              : < 256 KB"
    );

    Serial.println(
        "================================================"
    );
}

// ============================================================
//                           LOOP
// ============================================================

void loop() {

    // Arduino loop intentionally does nothing.
    //
    // All prototype work is handled by FreeRTOS tasks.

    vTaskDelay(
        pdMS_TO_TICKS(1000)
    );
}
