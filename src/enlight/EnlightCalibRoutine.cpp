#include "EnlightCalibRoutine.h"
#include "../nvs_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

using namespace InputDefaults;

static const char* TAG = "EnlightCalib";

EnlightCalibRoutine::EnlightCalibRoutine(Enlight&            e,
                                          LightAir_Display&   disp,
                                          LightAir_InputCtrl& input,
                                          uint8_t             keypadId)
    : _e(e), _disp(disp), _input(input), _keypadId(keypadId) {}

/* ============================================================
 *   Public entry point
 * ============================================================ */

void EnlightCalibRoutine::run() {
    step1();
    step2();
    step3();
    step4();
}

/* ============================================================
 *   Step 1 — phase offset + clear-target statistics
 * ============================================================ */

static int cmp_u32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

void EnlightCalibRoutine::step1() {
    showLines("Step 1: Phase",
              "Clear target in",
              "view.",
              "TRIG1 per shot.");

    uint32_t  phases[N_RUNS];
    long long sumR = 0, sumG = 0, sumB = 0;
    long long ssR  = 0, ssG  = 0, ssB  = 0;
    uint32_t  n = 0;

    while (n < N_RUNS) {
        char prompt[24];
        snprintf(prompt, sizeof(prompt), "Shot %lu/%lu - TRIG1",
                 (unsigned long)(n + 1), (unsigned long)N_RUNS);
        showLines("Clear target", prompt);
        waitTrig(TRIG_1_ID);

        EnlightRawMeasure m;
        if (!runOne(m)) {
            // Severe saturation — inform user and don't count this shot.
            showLines("Saturated!", "Try again.", "TRIG1 to retry.");
            delay(500);
            continue;
        }

        // Compute bestPhase for this shot from the captured ADC buffer.
        phases[n] = computeBestPhase();

        long long r = m.rout, g = m.gout, b = m.bout;
        sumR += r;  ssR += r * r;
        sumG += g;  ssG += g * g;
        sumB += b;  ssB += b * b;
        n++;
    }

    // Sort phase values and take the median.
    qsort(phases, n, sizeof(uint32_t), cmp_u32);
    const uint32_t bestPhase = phases[n / 2];

    // Compute per-channel averages; derive white-balance factors so that
    // rfact*R = G and bfact*B = G for a spectrally-flat (Clear) target.
    const long long avgR = sumR / n, avgG = sumG / n, avgB = sumB / n;
    const float rfact = (avgR > 0) ? (float)avgG / (float)avgR : 1.0f;
    const float bfact = (avgB > 0) ? (float)avgG / (float)avgB : 1.0f;

    // Persist phase offset and white-balance factors; apply phase immediately.
    EnlightCalib cal;
    enlight_calib_load(cal);
    cal.phaseOff = bestPhase;
    cal.rfact    = rfact;
    cal.bfact    = bfact;
    enlight_calib_save(cal);
    _e.buildSintab(bestPhase);

    // Show results.
    const float sdR = sqrtf(fmaxf(0.f, (float)(ssR / n) - (float)(avgR * avgR)));
    const float sdG = sqrtf(fmaxf(0.f, (float)(ssG / n) - (float)(avgG * avgG)));
    const float sdB = sqrtf(fmaxf(0.f, (float)(ssB / n) - (float)(avgB * avgB)));

    char l0[24], l1[24], l2[24], l3[24], l4[24], l5[24];
    snprintf(l0, sizeof(l0), "R:%.0f G:%.0f", (double)avgR, (double)avgG);
    snprintf(l1, sizeof(l1), "B:%.0f Ph:%lu", (double)avgB, (unsigned long)bestPhase);
    snprintf(l2, sizeof(l2), "sR:%.0f sG:%.0f", (double)sdR, (double)sdG);
    snprintf(l3, sizeof(l3), "sB:%.0f", (double)sdB);
    snprintf(l4, sizeof(l4), "rfact:%.4g", (double)rfact);
    snprintf(l5, sizeof(l5), "bfact:%.4g next", (double)bfact);
    showLines(l0, l1, l2, l3, l4, l5);
    waitTrig(TRIG_2_ID);
}

/* ============================================================
 *   Step 2 — void baseline
 * ============================================================ */

static int cmp_ll(const void* a, const void* b) {
    const long long x = *(const long long*)a;
    const long long y = *(const long long*)b;
    return (x > y) - (x < y);
}

void EnlightCalibRoutine::step2() {
    showLines("Step 2: Baseline",
              "No target (void).",
              "Keep clear.",
              "TRIG1 to start.");
    waitTrig(TRIG_1_ID);

    long long sRF[N_RUNS], sGF[N_RUNS], sBF[N_RUNS];
    long long sRN[N_RUNS], sGN[N_RUNS], sBN[N_RUNS];

    long long sumRF = 0, ssRF = 0;
    long long sumGF = 0, ssGF = 0;
    long long sumBF = 0, ssBF = 0;
    long long sumRN = 0, ssRN = 0;
    long long sumGN = 0, ssGN = 0;
    long long sumBN = 0, ssBN = 0;
    uint32_t n = 0;

    while (n < N_RUNS) {
        char rem[20];
        snprintf(rem, sizeof(rem), "Rem: %lu", (unsigned long)(N_RUNS - n));
        showLines("Void", rem);
        delay(DELAY_MS);

        EnlightRawMeasure m;
        runOne(m);  // no saturation rejection in step 2

        sRF[n] = m.rout;  sGF[n] = m.gout;  sBF[n] = m.bout;
        sRN[n] = m.rnear; sGN[n] = m.gnear; sBN[n] = m.bnear;

        long long rf = m.rout, gf = m.gout, bf = m.bout;
        long long rn = m.rnear, gn = m.gnear, bn = m.bnear;

        sumRF += rf;  ssRF += rf * rf;
        sumGF += gf;  ssGF += gf * gf;
        sumBF += bf;  ssBF += bf * bf;
        sumRN += rn;  ssRN += rn * rn;
        sumGN += gn;  ssGN += gn * gn;
        sumBN += bn;  ssBN += bn * bn;
        n++;
    }

    // Sort each channel to find medians.
    qsort(sRF, n, sizeof(long long), cmp_ll);
    qsort(sGF, n, sizeof(long long), cmp_ll);
    qsort(sBF, n, sizeof(long long), cmp_ll);
    qsort(sRN, n, sizeof(long long), cmp_ll);
    qsort(sGN, n, sizeof(long long), cmp_ll);
    qsort(sBN, n, sizeof(long long), cmp_ll);

    const uint32_t mid = n / 2;

    // Persist medians as per-cycle baselines (normalized by REPS, negative values clamped to 0).
    EnlightCalib cal;
    enlight_calib_load(cal);
    cal.rcal     = (uint32_t)(sRF[mid] > 0 ? sRF[mid] / REPS : 0);
    cal.gcal     = (uint32_t)(sGF[mid] > 0 ? sGF[mid] / REPS : 0);
    cal.bcal     = (uint32_t)(sBF[mid] > 0 ? sBF[mid] / REPS : 0);
    cal.rcalNear = (uint32_t)(sRN[mid] > 0 ? sRN[mid] / REPS : 0);
    cal.gcalNear = (uint32_t)(sGN[mid] > 0 ? sGN[mid] / REPS : 0);
    cal.bcalNear = (uint32_t)(sBN[mid] > 0 ? sBN[mid] / REPS : 0);
    enlight_calib_save(cal);

    // Compute averages and stdevs for display.
    const float aRF = sumRF / n, aGF = sumGF / n, aBF = sumBF / n;
    const float aRN = sumRN / n, aGN = sumGN / n, aBN = sumBN / n;
    const float sdRF = sqrtf(fmaxf(0.f, ssRF / n - (aRF * aRF)));
    const float sdGF = sqrtf(fmaxf(0.f, ssGF / n - (aGF * aGF)));
    const float sdBF = sqrtf(fmaxf(0.f, ssBF / n - (aBF * aBF)));
    const float sdRN = sqrtf(fmaxf(0.f, ssRN / n - (aRN * aRN)));
    const float sdGN = sqrtf(fmaxf(0.f, ssGN / n - (aGN * aGN)));
    const float sdBN = sqrtf(fmaxf(0.f, ssBN / n - (aBN * aBN)));

    char l0[24], l1[24], l2[24], l3[24], l4[24], l5[24];
    snprintf(l0, sizeof(l0), "F R:%.0f G:%.0f", (double)aRF, (double)aGF);
    snprintf(l1, sizeof(l1), "F B:%.0f", (double)aBF);
    snprintf(l2, sizeof(l2), "N R:%.0f G:%.0f", (double)aRN, (double)aGN);
    snprintf(l3, sizeof(l3), "N B:%.0f", (double)aBN);
    snprintf(l4, sizeof(l4), "s %.0f %.0f %.0f",
             (double)sqrtf(sdRF*sdRF+sdGF*sdGF+sdBF*sdBF),
             (double)sqrtf(sdRN*sdRN+sdGN*sdGN+sdBN*sdBN),
             0.0);
    // Re-use l4 for individual stdevs of far channels; show near stdev on l5.
    snprintf(l4, sizeof(l4), "sF:%.0f %.0f %.0f", (double)sdRF, (double)sdGF, (double)sdBF);
    snprintf(l5, sizeof(l5), "sN:%.0f %.0f next", (double)sdRN, (double)sdGN);
    showLines(l0, l1, l2, l3, l4, l5);
    waitTrig(TRIG_2_ID);
}

/* ============================================================
 *   Step 3 — white diffusing surface (contact … ~5 m)
 * ============================================================ */

void EnlightCalibRoutine::step3() {
    showLines("Step 3: White",
              "Enlight white wall",
              "contact to ~5m.",
              "TRIG1 to start.");
    waitTrig(TRIG_1_ID);

    uint32_t maxNear = 0, maxFar = 0, maxRawPerCycle = 0;
    uint32_t n = 0;

    while (n < N_RUNS) {
        char rem[20];
        snprintf(rem, sizeof(rem), "Rem: %lu", (unsigned long)(N_RUNS - n));
        showLines("White wall", rem);
        delay(DELAY_MS);

        EnlightRawMeasure m;
        runOne(m);  // no saturation rejection

        const uint32_t nearPow = (uint32_t)(llabs(m.rnear) + llabs(m.gnear) + llabs(m.bnear));
        const uint32_t farPow  = (uint32_t)(llabs(m.rout)  + llabs(m.gout)  + llabs(m.bout));
        // Raw sum per cycle: divide by REPS so limpow is independent of repetition count.
        const uint32_t rawPerCycle = (m.rawsum > 0) ? (uint32_t)(m.rawsum / REPS) : 0;

        if (nearPow     > maxNear)         maxNear        = nearPow;
        if (farPow      > maxFar)          maxFar         = farPow;
        if (rawPerCycle > maxRawPerCycle)  maxRawPerCycle = rawPerCycle;
        n++;
    }

    // Persist Max Near White, Max Far White, and limpow.
    // limpow is the highest per-cycle raw ADC sum seen over the white surface;
    // classify() compares _rawsum against limpow * cycles so the threshold
    // scales automatically with the in-game repetition count.
    EnlightCalib cal;
    enlight_calib_load(cal);
    cal.maxNearWhite = maxNear;
    cal.maxFarWhite  = maxFar;
    cal.limpow       = maxRawPerCycle;
    enlight_calib_save(cal);

    // Show results.
    char l0[24], l1[24], l2[24];
    snprintf(l0, sizeof(l0), "NearMax: %lu", (unsigned long)maxNear);
    snprintf(l1, sizeof(l1), "FarMax:  %lu", (unsigned long)maxFar);
    snprintf(l2, sizeof(l2), "limpow:  %lu", (unsigned long)maxRawPerCycle);
    showLines(l0, l1, l2, nullptr, nullptr, "TRIG2: done");
    waitTrig(TRIG_2_ID);
}

/* ============================================================
 *   Step 4 — calibration summary (paged NVS readback)
 * ============================================================ */

void EnlightCalibRoutine::step4() {
    // Read all calibration values fresh from NVS.
    EnlightCalib cal;
    enlight_calib_load(cal);

    // Build one formatted line per calibration value.
    struct CalEntry { char line[22]; };
    const uint8_t N_ENTRIES    = 13;
    const uint8_t ROWS_PER_PAGE = 5;
    const uint8_t N_PAGES      = (N_ENTRIES + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;

    CalEntry entries[N_ENTRIES];
    snprintf(entries[0].line,  sizeof(entries[0].line),  "phsOff: %lu",  (unsigned long)cal.phaseOff);
    snprintf(entries[1].line,  sizeof(entries[1].line),  "rcal:   %lu",  (unsigned long)cal.rcal);
    snprintf(entries[2].line,  sizeof(entries[2].line),  "gcal:   %lu",  (unsigned long)cal.gcal);
    snprintf(entries[3].line,  sizeof(entries[3].line),  "bcal:   %lu",  (unsigned long)cal.bcal);
    snprintf(entries[4].line,  sizeof(entries[4].line),  "rcalN:  %lu",  (unsigned long)cal.rcalNear);
    snprintf(entries[5].line,  sizeof(entries[5].line),  "gcalN:  %lu",  (unsigned long)cal.gcalNear);
    snprintf(entries[6].line,  sizeof(entries[6].line),  "bcalN:  %lu",  (unsigned long)cal.bcalNear);
    snprintf(entries[7].line,  sizeof(entries[7].line),  "limpow: %lu",  (unsigned long)cal.limpow);
    snprintf(entries[8].line,  sizeof(entries[8].line),  "rfact:  %.4g", (double)cal.rfact);
    snprintf(entries[9].line,  sizeof(entries[9].line),  "bfact:  %.4g", (double)cal.bfact);
    snprintf(entries[10].line, sizeof(entries[10].line), "nRatMx: %.4g", (double)cal.nearRatioMax);
    snprintf(entries[11].line, sizeof(entries[11].line), "mxNrW:  %lu",  (unsigned long)cal.maxNearWhite);
    snprintf(entries[12].line, sizeof(entries[12].line), "mxFrW:  %lu",  (unsigned long)cal.maxFarWhite);

    uint8_t page = 0;
    for (;;) {
        // Render the current page.
        _disp.clear();
        _disp.setColor(true);

        const uint8_t base = page * ROWS_PER_PAGE;
        for (uint8_t r = 0; r < ROWS_PER_PAGE; r++) {
            const uint8_t idx = base + r;
            if (idx >= N_ENTRIES) break;
            _disp.print(0, r * 10, entries[idx].line);
        }

        char footer[22];
        snprintf(footer, sizeof(footer), "^V pg%u/%u TRIG2:done", page + 1, N_PAGES);
        _disp.print(0, 50, footer);
        _disp.flush();

        // Wait for TRIG2 (exit) or ^ / V (page change).
        bool redraw = false;
        while (!redraw) {
            const InputReport& rep = _input.poll();

            for (uint8_t i = 0; i < rep.buttonCount; i++) {
                if (rep.buttons[i].id == TRIG_2_ID &&
                    (rep.buttons[i].state == ButtonState::HELD))
                    return;
            }

            for (uint8_t i = 0; i < rep.keyEventCount; i++) {
                const InputReport::KeyEntry& ke = rep.keyEvents[i];
                if (ke.keypadId != _keypadId) continue;
                if (ke.state != KeyState::RELEASED &&
                    ke.state != KeyState::RELEASED_HELD) continue;
                if (ke.key == '^' && page > 0)           { page--; redraw = true; }
                if (ke.key == 'V' && page < N_PAGES - 1) { page++; redraw = true; }
            }

            delay(10);
        }
    }
}

/* ============================================================
 *   Helpers
 * ============================================================ */

bool EnlightCalibRoutine::runOne(EnlightRawMeasure& out) {
    _e.run();
    while (_e.poll().status == EnlightStatus::RUNNING)
        delay(1);
    out = _e.rawMeasure();
    if (out.totalSamples == 0) return false;
    return (float)out.satCount / (float)out.totalSamples <= SAT_THRESH;
}

uint32_t EnlightCalibRoutine::computeBestPhase() {
    const uint32_t gp       = _e.goertzPeriod();
    const uint32_t maxOff   = gp / 4;            // scan 0 … 90°
    const uint8_t* buf      = _e.rawAdcBuf();
    const uint32_t nTriples = _e.adcConvsPerCycle() / ADC_CHANNELS;

    // Precompute one period of integer sine values (same scale as Enlight's sintab).
    int32_t* sinLut = (int32_t*)malloc(gp * sizeof(int32_t));
    if (!sinLut) return 0;
    for (uint32_t i = 0; i < gp; i++)
        sinLut[i] = (int32_t)roundf((float)SIN_MAG * sinf(2.0f * (float)M_PI * i / (float)gp));

    uint32_t  bestPhase = 0;
    long long bestVal   = 0;
    bool      found     = false;

    for (uint32_t p = 0; p <= maxOff; p++) {
        long long sum = 0;
        for (uint32_t t = 0; t < nTriples; t++) {
            // Each DMA cycle holds an exact integer number of periods, so
            // triple t has phase (t % gp) within that cycle.
            const uint32_t base = t * ADC_CHANNELS + ADC_PIPELINE_DELAY;
            const long long rv = (((uint16_t)buf[base*2]     << 8) | buf[base*2+1])     & 0x0FFF;
            const long long gv = (((uint16_t)buf[(base+1)*2] << 8) | buf[(base+1)*2+1]) & 0x0FFF;
            const long long bv = (((uint16_t)buf[(base+2)*2] << 8) | buf[(base+2)*2+1]) & 0x0FFF;
            sum += (rv + gv + bv) * sinLut[(t % gp + p) % gp];
        }
        if (!found || sum > bestVal) {
            bestVal   = sum;
            bestPhase = p;
            found     = true;
        }
    }

    free(sinLut);
    return bestPhase;
}

void EnlightCalibRoutine::showLines(const char* l0, const char* l1,
                                     const char* l2, const char* l3,
                                     const char* l4, const char* l5) {
    _disp.clear();
    _disp.setColor(true);
    if (l0) _disp.print(0,  0, l0);
    if (l1) _disp.print(0, 10, l1);
    if (l2) _disp.print(0, 20, l2);
    if (l3) _disp.print(0, 30, l3);
    if (l4) _disp.print(0, 40, l4);
    if (l5) _disp.print(0, 50, l5);
    _disp.flush();
}

void EnlightCalibRoutine::waitTrig(uint8_t trigId) {
    while (true) {
        const InputReport& rep = _input.poll();
        for (uint8_t i = 0; i < rep.buttonCount; i++) {
            if (rep.buttons[i].id == trigId &&
                (rep.buttons[i].state == ButtonState::PRESSED ||
                 rep.buttons[i].state == ButtonState::HELD))
                return;
        }
        delay(10);
    }
}
