/***************************************************************************//**
 * @file
 * @brief EM4 deep-sleep entry with BURTC wake-up.
 ******************************************************************************/

#include "em4.h"
#include "wheatstone.h"
#include "em_burtc.h"
#include "em_emu.h"
#include "em_cmu.h"
#include "sl_clock_manager.h"
#include "sl_device_clock.h"

/*
 * FILE STRUCTURE (em4.c)
 * ----------------------
 * This file owns the EM4 Shutoff entry sequence for the EFR32MG22E:
 *
 * - em4_request() / em4_pending(): a small latch the app state machine uses to
 *   defer EM4 entry until the Zigbee stack allows it.
 * - em4_enter(): the actual entry sequence -
 *     1) park board pins (wheatstone_prepare_for_em4()),
 *     2) disable unused LF oscillators in EM4, keep ULFRCO for BURTC,
 *     3) program a BURTC compare match `sleep_ms` in the future,
 *     4) EMU_EnterEM4() - the MCU resets on wake, so this never returns.
 * - clamp_burtc_clkdiv() / choose_burtc_clkdiv(): BURTC prescaler selection,
 *   trading wake-timing granularity for lower EM4 current on long sleeps.
 *
 * Clocking background: in EM4 the BURTC runs from EM4GRPACLK, which on this
 * board is the ULFRCO (~1 kHz, +/-5 %ish). Wake timing therefore has a few
 * percent of jitter, which is acceptable for this firmware's report cycles.
 *
 * Call flow:
 * - app_process_action() -> em4_request()  (after publish / watchdog)
 * - app_process_action() -> em4_pending() + zigbee_ok_to_sleep() -> em4_enter()
 */

// BURTC runs from EM4GRPACLK in EM4. Slowing the BURTC counter reduces EM4
// current, at the cost of wake timing granularity.
//
// For multi-day sleeps, a large divider is typically fine. Example with ULFRCO
// (~1 kHz):
//   clkDiv=32768 -> ~0.0305 Hz (one tick per ~32.768 s)
//
// Keep within Series 2 supported range: 1..32768.
#define BURTC_EM4_CLKDIV_LONG 32768U

static bool em4_requested = false;
static uint32_t em4_duration_ms = 0U;
static const char *em4_reason = NULL;

/**
 * @brief Clamp a BURTC prescaler to the Series 2 supported range [1..32768].
 */
static uint32_t clamp_burtc_clkdiv(uint32_t clkdiv)
{
    if (clkdiv > 32768U) {
        return 32768U;
    }
    return (clkdiv == 0U) ? 1U : clkdiv;
}

/**
 * @brief Pick a coarse BURTC prescaler for the requested sleep duration.
 *
 * Short sleeps get a fine tick for accurate wake timing; long sleeps get a
 * coarse tick for the lowest EM4 current. The buckets are intentionally rough.
 */
static uint32_t choose_burtc_clkdiv(uint32_t sleep_ms)
{
    if (sleep_ms <= (60U * 1000U)) { // <= 1 min
        return 1U;   // ~1 ms tick @ ~1 kHz ULFRCO
    }
    if (sleep_ms <= (10U * 60U * 1000U)) { // <= 10 min
        return 1024U;   // ~1 s tick @ ~1 kHz ULFRCO
    }
    if (sleep_ms <= (24U * 60U * 60U * 1000U)) { // <= 24 h
        return 8192U;   // ~8 s tick
    }
    return BURTC_EM4_CLKDIV_LONG; // ~33 s tick
}

/**
 * @brief Latch a deep-sleep request for the app state machine.
 *
 * Does not enter EM4 by itself; app_process_action() polls em4_pending() and
 * calls em4_enter() once zigbee_ok_to_sleep() agrees.
 */
void em4_request(uint32_t sleep_ms, const char *reason)
{
    em4_duration_ms = sleep_ms;
    em4_reason = reason;
    em4_requested = true;
}

/**
 * @brief Check whether an EM4 request is waiting to be entered.
 */
bool em4_pending(void)
{
    return em4_requested;
}

/**
 * @brief Enter EM4 for the requested duration, then reboot.
 *
 * EM4 behaves like hibernate/reset: execution restarts from reset and RAM is
 * lost. The wake-up is done using BURTC compare.
 */
void em4_enter(void)
{
    uint32_t sleep_ms = em4_duration_ms;

    (void)em4_reason; // Reason label is kept for future diagnostics/logging.
    em4_requested = false;

    // With the bridge powered from a GPIO, pin state during EM4 matters.
    // Park pins and ensure the bridge supply is off before entering EM4.
    wheatstone_prepare_for_em4();

    // Minimize EM4 current: explicitly disable unused LF oscillators in EM4.
    // Note: in EMU_EM4Init_TypeDef on Series 2, retain{Lfxo,Lfrco,Ulfrco} fields
    // are documented as "Disable <osc> upon EM4 entry".
    // We keep ULFRCO running because BURTC compare wake relies on the EM4 Group A clock.
    {
        EMU_EM4Init_TypeDef em4_init = EMU_EM4INIT_DEFAULT;
        em4_init.em4State = emuEM4Shutoff;
        em4_init.retainLfxo = true;   // Disable LFXO in EM4
        em4_init.retainLfrco = true;  // Disable LFRCO in EM4
        em4_init.retainUlfrco = false; // Keep ULFRCO in EM4 (needed for BURTC wake)
        EMU_EM4Init(&em4_init);
    }

    BURTC_Init_TypeDef burtc_init = BURTC_INIT_DEFAULT;

    // Configure BURTC prescaler to reduce current in EM4.
    // Note: clkDiv is an integer (1..32768) on Series 2.
    burtc_init.clkDiv = clamp_burtc_clkdiv(choose_burtc_clkdiv(sleep_ms));

    // Compute compare ticks based on the actual EM4GRPACLK frequency and our BURTC divider.
    // Use a ratio-based calculation to stay correct even when EM4GRPACLK < clkDiv.
    uint32_t em4grpclk_hz = CMU_ClockFreqGet(cmuClock_EM4GRPACLK);
    if (em4grpclk_hz == 0U) { em4grpclk_hz = 1000U; } // defensive fallback
    uint32_t clkdiv = (burtc_init.clkDiv == 0U) ? 1U : burtc_init.clkDiv;

    // ticks = ceil(sleep_ms * em4grpclk_hz / (1000 * clkDiv))
    uint64_t numerator = (uint64_t)sleep_ms * (uint64_t)em4grpclk_hz;
    uint64_t denominator = 1000ULL * (uint64_t)clkdiv;
    uint64_t ticks_64 = (numerator + (denominator - 1ULL)) / denominator;
    uint32_t wake_ticks = (ticks_64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)ticks_64;

    if (wake_ticks == 0U) {
        wake_ticks = 1U;
    }

    /* Ensure BURTC bus clock is enabled even when sleeptimer backend is RTCC. */
    sl_clock_manager_enable_bus_clock(SL_BUS_CLOCK_BURTC);

    BURTC_Reset();
    burtc_init.em4comp = true;
    burtc_init.em4overflow = false;
    BURTC_Init(&burtc_init);
    BURTC_CounterReset();
    uint32_t cur = BURTC_CounterGet();
    if (cur == 0xFFFFFFFFU) {
        /* defensive fallback */
        cur = 0U;
    }
    uint32_t comp = cur + wake_ticks;
    if (comp == cur) {
        comp = cur + 1U;
    }
    BURTC_CompareSet(0U, comp);
    BURTC_IntClear(_BURTC_IF_MASK);
    BURTC_IntDisable(_BURTC_IEN_MASK);
    // The BURTC compare wakes the device through the EM4 wake-up path; the CPU
    // interrupt (BURTC_IntEnable) is not needed for EM4 Shutoff.

    EMU_EnterEM4();

    // EM4 does not return; this is a safety trap.
    while (1) {
    }
}
