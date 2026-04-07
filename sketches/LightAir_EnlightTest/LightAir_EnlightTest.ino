// ================================================================
// LightAir_EnlightTest.ino
//
// Diagnostic sketch: runs the Enlight optical sensor and streams
// measurements over a WiFi TCP connection.
//
// Setup menu (keypad):
//   Step 1 — Data mode
//               RAW        : individual 12-bit ADC samples from the last
//                            DMA cycle, one SAMPLE line per RGB triple.
//                            Set ENLIGHT_REPS=1; only the last cycle is
//                            retained after poll().
//               ELABORATED : classification result + correlator accumulators
//   Step 2 — Trigger mode
//               MANUAL : primary trigger button fires one measurement
//               AUTO   : measurements fire automatically on a timer
//   Step 3 — Auto interval (AUTO only)
//               1 – 10 seconds, 1 s steps   (</>: change, A: confirm)
//
// After the menu the sketch connects to WiFi (station mode) and
// starts a TCP server.  Connect with e.g.:
//   nc <device-ip> 9000
//
// Stream format (newline-terminated ASCII CSV):
//   On connect :  # mode=<RAW|ELAB> trig=<MANUAL|AUTO[Ns]>
//
//   RAW block :
//     RAW_BEGIN,<ms>,<sample_count>
//     SAMPLE,<ms>,<idx>,<r>,<g>,<b>,<sat>
//       ... (sample_count lines) ...
//     RAW_END,<ms>
//
//     <r>/<g>/<b> are raw 12-bit ADC values (0–4095).
//     <sat> is 1 if any channel >= 4085 or <= 10, else 0.
//     <idx> counts RGB triples (0-based), not raw conversions.
//
//   ELAB line :  ELAB,<ms>,<status>,<id>,<rout>,<gout>,<bout>,
//                     <rnear>,<gnear>,<bnear>
//
//   <status> one of: IDLE LOW_POW NO_HIT PLAYER_HIT NEAR
// ================================================================

#include <WiFi.h>
#include <LightAir.h>

// ---- Edit these before flashing ----------------------------------------
#define WIFI_SSID     "TIM-54267233"
#define WIFI_PASSWORD "3GyCKKKTcRTTAA7YP9cUyEEC"
#define TCP_PORT      9000

// Number of Enlight DMA repetitions per measurement.
// RAW mode: set to 1 — only the last DMA cycle is retained after poll().
// ELABORATED mode: 5 reps ≈ 39 ms per shot at V6R2 defaults.
#define ENLIGHT_REPS  1
// ------------------------------------------------------------------------

// Saturation thresholds (must match Enlight.cpp)
static constexpr uint16_t kSatHigh = 4085;
static constexpr uint16_t kSatLow  = 10;

// ================================================================
// Hardware objects  (pin constants come from player_pins.h via LightAir.h)
// ================================================================
static LightAir_SSD1306Display display(PLAYER_I2C_SDA, PLAYER_I2C_SCL);

static const char    kKeys[]    = { '<', '^', '>', 'A', 'V', 'B' };
static const uint8_t kRowPins[] = { PLAYER_SW_R1, PLAYER_SW_R2 };
static const uint8_t kColPins[] = { PLAYER_SW_C1, PLAYER_SW_C2, PLAYER_SW_C3 };
static LightAir_HWKeypad  keypad(kKeys, kRowPins, 2, kColPins, 3);
static LightAir_HWButton  trig1(PLAYER_TRIG_1);
static LightAir_HWButton  trig2(PLAYER_TRIG_2);
static LightAir_InputCtrl input;

static EnlightCalib  enlightCalib;
static Enlight*      enlight = nullptr;

static const EnlightConfig enlightCfg = {
    /* adcHost     */ EnlightDefaults::ADC_HOST,
    /* adcClk      */ EnlightDefaults::ADC_CLK,
    /* adcSdo      */ EnlightDefaults::ADC_SDO,
    /* adcSdi      */ EnlightDefaults::ADC_SDI,
    /* adcCs       */ EnlightDefaults::ADC_CS,
    /* adcClockHz  */ EnlightDefaults::ADC_CLOCK_HZ,
    /* adcCmdR     */ EnlightDefaults::ADC_CMD_R,
    /* adcCmdG     */ EnlightDefaults::ADC_CMD_G,
    /* adcCmdB     */ EnlightDefaults::ADC_CMD_B,
    /* ledHost     */ EnlightDefaults::LED_HOST,
    /* ledSdo      */ EnlightDefaults::LED_SDO,
    /* ledSdiOut   */ EnlightDefaults::LED_SDI_OUT,
    /* ledClockHz  */ EnlightDefaults::LED_CLOCK_HZ,
    /* ledFreqHz   */ EnlightDefaults::LED_FREQ_HZ,
    /* pdmAmpOff   */ EnlightDefaults::PDM_AMP_OFFSET,
    /* afeOn         */ EnlightDefaults::AFE_ON,
    /* taskCore      */ EnlightDefaults::TASK_CORE,
    /* afeStartupUs  */ EnlightDefaults::AFE_STARTUP_MICROS,
};

static WiFiServer tcpServer(TCP_PORT);
static WiFiClient tcpClient;

// ================================================================
// App state
// ================================================================
enum class DataMode : uint8_t { RAW = 0, ELABORATED = 1 };
enum class TrigMode : uint8_t { MANUAL = 0, AUTO = 1 };

static DataMode gDataMode    = DataMode::RAW;
static TrigMode gTrigMode    = TrigMode::MANUAL;
static uint8_t  gIntervalSec = 5;

static uint32_t gLastTrigMs  = 0;
static uint32_t gTxCount     = 0;

// ================================================================
// Display helpers
// ================================================================
// Vertical layout: 5 rows at 12 px spacing on a 64 px screen.
static constexpr uint8_t kLY[] = { 1, 13, 25, 37, 51 };
static constexpr uint8_t kLH   = 11;   // row height for highlight rect

// Print one row, optionally highlighted (white-on-black rect, black text).
static void dispRow(uint8_t row, const char* text, bool highlight = false) {
    if (highlight) {
        display.setColor(true);
        display.fillRect(0, kLY[row] - 1, DisplayDefaults::SCREEN_WIDTH, kLH);
        display.setColor(false);
        display.print(3, kLY[row], text);
        display.setColor(true);
    } else {
        display.setColor(true);
        display.print(3, kLY[row], text);
    }
}

static void dispFlush() { display.flush(); }

static void dispBegin() {
    display.clear();
}

// ================================================================
// Input helper — block until any keypad key is released
// ================================================================
static char waitKey() {
    while (true) {
        const InputReport& rpt = input.poll();
        for (uint8_t i = 0; i < rpt.keyEventCount; i++) {
            if (rpt.keyEvents[i].state == KeyState::RELEASED)
                return rpt.keyEvents[i].key;
        }
        delay(10);
    }
}

// ================================================================
// Menu screens  (</>: cycle, A: confirm)
// ================================================================
static void menuDataMode() {
    uint8_t sel = static_cast<uint8_t>(gDataMode);
    for (;;) {
        dispBegin();
        dispRow(0, "Data mode:");
        dispRow(1, "  RAW",        sel == 0);
        dispRow(2, "  ELABORATED", sel == 1);
        dispRow(4, "</> sel   A: ok");
        dispFlush();

        char k = waitKey();
        if (k == '<' || k == '>') sel ^= 1;
        else if (k == '^')        sel  = 0;
        else if (k == 'V')        sel  = 1;
        else if (k == 'A')        { gDataMode = static_cast<DataMode>(sel); return; }
    }
}

static void menuTrigMode() {
    uint8_t sel = static_cast<uint8_t>(gTrigMode);
    for (;;) {
        dispBegin();
        dispRow(0, "Trigger mode:");
        dispRow(1, "  MANUAL", sel == 0);
        dispRow(2, "  AUTO",   sel == 1);
        dispRow(4, "</> sel   A: ok");
        dispFlush();

        char k = waitKey();
        if (k == '<' || k == '>') sel ^= 1;
        else if (k == '^')        sel  = 0;
        else if (k == 'V')        sel  = 1;
        else if (k == 'A')        { gTrigMode = static_cast<TrigMode>(sel); return; }
    }
}

static void menuInterval() {
    uint8_t val = gIntervalSec;
    char buf[20];
    for (;;) {
        dispBegin();
        dispRow(0, "Auto interval:");
        snprintf(buf, sizeof(buf), "  %u second%s", val, val == 1 ? "" : "s");
        dispRow(2, buf, true);
        dispRow(4, "</> change  A: ok");
        dispFlush();

        char k = waitKey();
        if      (k == '<' && val > 1)  val--;
        else if (k == '>' && val < 10) val++;
        else if (k == 'A')             { gIntervalSec = val; return; }
    }
}

// ================================================================
// WiFi + TCP server startup
// ================================================================
static void wifiConnect() {
    char buf[22];
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t t0   = millis();
    uint8_t  dots = 0;
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 15000) {
            dispBegin();
            dispRow(0, "WiFi FAILED");
            dispRow(2, "Check SSID / PW");
            dispRow(4, "Reboot to retry");
            dispFlush();
            while (true) delay(1000);
        }
        // Animated progress dots
        char dotbuf[5] = {};
        for (uint8_t i = 0; i < (dots & 3); i++) dotbuf[i] = '.';
        dispBegin();
        dispRow(0, "Connecting WiFi");
        snprintf(buf, sizeof(buf), "%.20s", WIFI_SSID);
        dispRow(1, buf);
        dispRow(2, dotbuf);
        dispFlush();
        dots++;
        delay(500);
    }

    tcpServer.begin();

    IPAddress ip = WiFi.localIP();
    dispBegin();
    dispRow(0, "WiFi connected");
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    dispRow(1, buf);
    snprintf(buf, sizeof(buf), "Port %u", TCP_PORT);
    dispRow(2, buf);
    dispRow(4, "Waiting client..");
    dispFlush();
}

// ================================================================
// Running-screen display update
// ================================================================
static void updateRunDisplay(bool waitingClient) {
    char buf[22];
    dispBegin();

    // Row 0: IP and port — always visible so the user can connect.
    IPAddress ip = WiFi.localIP();
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d:%u",
             ip[0], ip[1], ip[2], ip[3], TCP_PORT);
    dispRow(0, buf);

    if (waitingClient) {
        dispRow(2, "Waiting for");
        dispRow(3, "TCP client...");
        return dispFlush();
    }

    dispRow(1, gDataMode == DataMode::RAW ? "Mode: RAW" : "Mode: ELAB");

    if (gTrigMode == TrigMode::MANUAL) {
        dispRow(2, "Trig: MANUAL");
        dispRow(3, "Press TRIG 1");
    } else {
        snprintf(buf, sizeof(buf), "Trig: AUTO %us", gIntervalSec);
        dispRow(2, buf);
        uint32_t elapsed = (millis() - gLastTrigMs) / 1000;
        uint32_t remain  = (gIntervalSec > elapsed) ? gIntervalSec - elapsed : 0;
        snprintf(buf, sizeof(buf), "Next in: %lus", (unsigned long)remain);
        dispRow(3, buf);
    }

    snprintf(buf, sizeof(buf), "Sent: %lu", (unsigned long)gTxCount);
    dispRow(4, buf);
    dispFlush();
}

// ================================================================
// Enlight measurement + TCP transmission
// ================================================================
static const char* statusStr(EnlightStatus s) {
    switch (s) {
        case EnlightStatus::IDLE:       return "IDLE";
        case EnlightStatus::RUNNING:    return "RUNNING";
        case EnlightStatus::LOW_POW:    return "LOW_POW";
        case EnlightStatus::NO_HIT:     return "NO_HIT";
        case EnlightStatus::PLAYER_HIT: return "PLAYER_HIT";
        case EnlightStatus::NEAR:       return "NEAR";
        default:                        return "UNKNOWN";
    }
}

// Decode one 12-bit ADC value from the DMA buffer (big-endian, bits [11:0]).
static inline uint16_t adcSample(const uint8_t* buf, uint32_t convIdx) {
    return (((uint16_t)buf[convIdx * 2] << 8) | buf[convIdx * 2 + 1]) & 0x0FFF;
}

static void takeMeasurement() {
    enlight->run(ENLIGHT_REPS);

    // Poll to completion (timeout 2 s; each DMA cycle ≈ 7.8 ms).
    EnlightResult res = {};
    uint32_t t0 = millis();
    do {
        res = enlight->poll();
        if (res.status != EnlightStatus::RUNNING) break;
        delay(1);  // yield to FreeRTOS scheduler
    } while (millis() - t0 < 2000);

    uint32_t ts = millis();

    if (!tcpClient || !tcpClient.connected()) return;

    char line[96];
    if (gDataMode == DataMode::RAW) {
        // Stream individual 12-bit ADC samples from the last DMA cycle.
        // Only the last cycle is retained; ENLIGHT_REPS=1 is recommended.
        const uint8_t*  buf    = enlight->rawAdcBuf();
        const uint32_t  trips  = enlight->adcConvsPerCycle() / ADC_CHANNELS;
        const int32_t*  sintab = enlight->rawSintab();
        const uint32_t  gp     = enlight->goertzPeriod();

        snprintf(line, sizeof(line), "RAW_BEGIN,%lu,%lu\n",
                 (unsigned long)ts, (unsigned long)trips);
        tcpClient.print(line);

        for (uint32_t t = 0; t < trips; t++) {
            const uint32_t base = t * ADC_CHANNELS + ADC_PIPELINE_DELAY;
            const uint16_t rv = adcSample(buf, base);
            const uint16_t gv = adcSample(buf, base + 1);
            const uint16_t bv = adcSample(buf, base + 2);
            const int sat = (rv >= kSatHigh || rv <= kSatLow ||
                             gv >= kSatHigh || gv <= kSatLow ||
                             bv >= kSatHigh || bv <= kSatLow) ? 1 : 0;
            const int32_t sinVal = sintab ? sintab[t % gp] : 0;
            snprintf(line, sizeof(line), "SAMPLE,%lu,%lu,%u,%u,%u,%d,%ld\n",
                     (unsigned long)ts, (unsigned long)t, rv, gv, bv, sat, (long)sinVal);
            tcpClient.print(line);
        }

        snprintf(line, sizeof(line), "RAW_END,%lu\n", (unsigned long)ts);
        tcpClient.print(line);
    } else {
        // rawMeasure() must be called before the next run(); grab it now.
        EnlightRawMeasure raw = enlight->rawMeasure();
        snprintf(line, sizeof(line),
            "ELAB,%lu,%s,%u,%lld,%lld,%lld,%lld,%lld,%lld\n",
            (unsigned long)ts,
            statusStr(res.status), res.id,
            raw.rout,  raw.gout,  raw.bout,
            raw.rnear, raw.gnear, raw.bnear);
        tcpClient.print(line);
    }

    gTxCount++;
}

// ================================================================
// setup / loop
// ================================================================
void setup() {
    Serial.begin(115200);

    display.begin();

    keypad.begin();
    trig1.begin();
    trig2.begin();
    input.registerKeypad(InputDefaults::KEYPAD_ID, keypad);
    input.registerButton(InputDefaults::TRIG_1_ID, trig1);
    input.registerButton(InputDefaults::TRIG_2_ID, trig2);

    // Enlight init
    enlight_calib_load(enlightCalib);
    enlight = new Enlight(enlightCfg, enlightCalib);
    if (!enlight->begin()) {
        dispBegin();
        dispRow(0, "Enlight init");
        dispRow(1, "FAILED");
        dispRow(3, "Check hardware");
        dispFlush();
        while (true) delay(1000);
    }

    // Setup menu
    menuDataMode();
    menuTrigMode();
    if (gTrigMode == TrigMode::AUTO) menuInterval();

    // WiFi + TCP server
    wifiConnect();

    gLastTrigMs = millis();
}

void loop() {
    static uint32_t lastDispMs = 0;

    // ---- Accept incoming client if none is active --------------------
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient c = tcpServer.available();
        if (c) {
            tcpClient   = c;
            gTxCount    = 0;
            gLastTrigMs = millis();
            Serial.printf("Client connected: %s\n",
                          tcpClient.remoteIP().toString().c_str());

            // Session header: mode/trigger settings, then column labels.
            char hdr[64];
            if (gTrigMode == TrigMode::AUTO) {
                snprintf(hdr, sizeof(hdr), "# mode=%s trig=AUTO%us\n",
                         gDataMode == DataMode::RAW ? "RAW" : "ELAB",
                         gIntervalSec);
            } else {
                snprintf(hdr, sizeof(hdr), "# mode=%s trig=MANUAL\n",
                         gDataMode == DataMode::RAW ? "RAW" : "ELAB");
            }
            tcpClient.print(hdr);

            // Column labels so each field is self-documenting.
            if (gDataMode == DataMode::RAW) {
                tcpClient.print(
                    "# RAW_BEGIN,timestamp_ms,sample_count\n"
                    "# SAMPLE,timestamp_ms,triple_idx,r(12bit),g(12bit),b(12bit),saturated\n"
                    "# RAW_END,timestamp_ms\n");
            } else {
                tcpClient.print(
                    "# ELAB,"
                    "timestamp_ms,"
                    "status,matched_id,"
                    "rout(far-R),gout(far-G),bout(far-B),"
                    "rnear(near-R),gnear(near-G),bnear(near-B)\n");
            }
        } else {
            if (millis() - lastDispMs > 500) {
                updateRunDisplay(true);
                lastDispMs = millis();
            }
            delay(10);
            return;
        }
    }

    // ---- Poll input --------------------------------------------------
    const InputReport& rpt = input.poll();

    bool trigFired = false;
    for (uint8_t i = 0; i < rpt.buttonCount; i++) {
        if (rpt.buttons[i].id    == InputDefaults::TRIG_1_ID &&
            rpt.buttons[i].state == ButtonState::RELEASED) {
            trigFired = true;
        }
    }

    // ---- Decide whether to measure -----------------------------------
    bool shouldMeasure = false;
    if (gTrigMode == TrigMode::MANUAL) {
        shouldMeasure = trigFired;
    } else {
        uint32_t elapsed = millis() - gLastTrigMs;
        shouldMeasure = trigFired || (elapsed >= (uint32_t)gIntervalSec * 1000u);
    }

    if (shouldMeasure) {
        gLastTrigMs = millis();
        takeMeasurement();
    }

    // ---- Refresh display every 200 ms --------------------------------
    if (millis() - lastDispMs > 200) {
        updateRunDisplay(false);
        lastDispMs = millis();
    }
}
