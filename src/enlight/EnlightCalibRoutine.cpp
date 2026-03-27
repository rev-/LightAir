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
}

/* ============================================================
 *   Step 1 — phase offset + clear-target statistics
 * ============================================================ */

void EnlightCalibRoutine::step1() {
    showLines("Step 1: Phase",
              "Clear target in",
              "view. Hold still.",
              "TRIG1 to start.");
    waitTrig(TRIG_1_ID);

    float sumR = 0.f, sumG = 0.f, sumB = 0.f;
    float ssR  = 0.f, ssG  = 0.f, ssB  = 0.f;
    uint32_t n = 0;

    while (n < N_RUNS) {
        char rem[20];
        snprintf(rem, sizeof(rem), "Rem: %lu", (unsigned long)(N_RUNS - n));
        showLines("Clear target", rem);
        delay(DELAY_MS);

        EnlightRawMeasure m;
        if (!runOne(m)) continue;  // severe saturation — discard, don't count

        float r = (float)m.rout, g = (float)m.gout, b = (float)m.bout;
        sumR += r;  ssR += r * r;
        sumG += g;  ssG += g * g;
        sumB += b;  ssB += b * b;
        n++;
    }

    const float avgR = sumR / n, avgG = sumG / n, avgB = sumB / n;
    const float sdR  = sqrtf(fmaxf(0.f, ssR / n - avgR * avgR));
    const float sdG  = sqrtf(fmaxf(0.f, ssG / n - avgG * avgG));
    const float sdB  = sqrtf(fmaxf(0.f, ssB / n - avgB * avgB));

    showLines("Scanning phase", "Please wait...");

    const uint32_t bestPhase = scanBestPhase();

    // Persist and immediately apply the optimal phase offset.
    EnlightCalib cal;
    enlight_calib_load(cal);
    cal.phaseOff = bestPhase;
    enlight_calib_save(cal);
    _e.buildSintab(bestPhase);

    // Show results.
    char l0[24], l1[24], l2[24], l3[24], l4[24];
    snprintf(l0, sizeof(l0), "R:%.0f G:%.0f", (double)avgR, (double)avgG);
    snprintf(l1, sizeof(l1), "B:%.0f", (double)avgB);
    snprintf(l2, sizeof(l2), "sR:%.0f sG:%.0f", (double)sdR, (double)sdG);
    snprintf(l3, sizeof(l3), "sB:%.0f", (double)sdB);
    snprintf(l4, sizeof(l4), "Phase: %lu", (unsigned long)bestPhase);
    showLines(l0, l1, l2, l3, l4, "TRIG2: next");
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

    float sumRF = 0.f, ssRF = 0.f;
    float sumGF = 0.f, ssGF = 0.f;
    float sumBF = 0.f, ssBF = 0.f;
    float sumRN = 0.f, ssRN = 0.f;
    float sumGN = 0.f, ssGN = 0.f;
    float sumBN = 0.f, ssBN = 0.f;
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

        float rf = (float)m.rout,  gf = (float)m.gout,  bf = (float)m.bout;
        float rn = (float)m.rnear, gn = (float)m.gnear, bn = (float)m.bnear;

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

    // Persist medians as baselines (negative values clamped to 0).
    EnlightCalib cal;
    enlight_calib_load(cal);
    cal.rcal     = (uint32_t)(sRF[mid] > 0 ? sRF[mid] : 0);
    cal.gcal     = (uint32_t)(sGF[mid] > 0 ? sGF[mid] : 0);
    cal.bcal     = (uint32_t)(sBF[mid] > 0 ? sBF[mid] : 0);
    cal.rcalNear = (uint32_t)(sRN[mid] > 0 ? sRN[mid] : 0);
    cal.gcalNear = (uint32_t)(sGN[mid] > 0 ? sGN[mid] : 0);
    cal.bcalNear = (uint32_t)(sBN[mid] > 0 ? sBN[mid] : 0);
    enlight_calib_save(cal);

    // Compute averages and stdevs for display.
    const float aRF = sumRF / n, aGF = sumGF / n, aBF = sumBF / n;
    const float aRN = sumRN / n, aGN = sumGN / n, aBN = sumBN / n;
    const float sdRF = sqrtf(fmaxf(0.f, ssRF / n - aRF * aRF));
    const float sdGF = sqrtf(fmaxf(0.f, ssGF / n - aGF * aGF));
    const float sdBF = sqrtf(fmaxf(0.f, ssBF / n - aBF * aBF));
    const float sdRN = sqrtf(fmaxf(0.f, ssRN / n - aRN * aRN));
    const float sdGN = sqrtf(fmaxf(0.f, ssGN / n - aGN * aGN));
    const float sdBN = sqrtf(fmaxf(0.f, ssBN / n - aBN * aBN));

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

    uint32_t maxNear = 0, maxFar = 0;
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

        if (nearPow > maxNear) maxNear = nearPow;
        if (farPow  > maxFar)  maxFar  = farPow;
        n++;
    }

    // Persist Max Near White and Max Far White.
    EnlightCalib cal;
    enlight_calib_load(cal);
    cal.maxNearWhite = maxNear;
    cal.maxFarWhite  = maxFar;
    enlight_calib_save(cal);

    // Show results.
    char l0[24], l1[24];
    snprintf(l0, sizeof(l0), "NearMax: %lu", (unsigned long)maxNear);
    snprintf(l1, sizeof(l1), "FarMax:  %lu", (unsigned long)maxFar);
    showLines(l0, l1, nullptr, nullptr, nullptr, "TRIG2: done");
    waitTrig(TRIG_2_ID);
}

/* ============================================================
 *   Helpers
 * ============================================================ */

bool EnlightCalibRoutine::runOne(EnlightRawMeasure& out) {
    _e.run(REPS);
    while (_e.poll().status == EnlightStatus::RUNNING)
        delay(1);
    out = _e.rawMeasure();
    if (out.totalSamples == 0) return false;
    return (float)out.satCount / (float)out.totalSamples <= SAT_THRESH;
}

uint32_t EnlightCalibRoutine::scanBestPhase() {
    const uint32_t gp = _e.goertzPeriod();
    uint32_t bestPhase = 0;
    long long bestVal  = 0;
    bool found = false;

    for (uint32_t p = 0; p < gp; p++) {
        _e.buildSintab(p);
        long long sumVal = 0;
        for (uint32_t r = 0; r < SCAN_REPS; r++) {
            EnlightRawMeasure m;
            _e.run(REPS);
            while (_e.poll().status == EnlightStatus::RUNNING)
                delay(1);
            m = _e.rawMeasure();
            sumVal += m.rout + m.gout + m.bout;
        }
        if (!found || sumVal > bestVal) {
            bestVal  = sumVal;
            bestPhase = p;
            found    = true;
        }
    }
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
                (rep.buttons[i].state == ButtonState::RELEASED ||
                 rep.buttons[i].state == ButtonState::RELEASED_HELD))
                return;
        }
        delay(10);
    }
}
