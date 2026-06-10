/***************************************************************************//**
 * @file
 * @brief EM4 deep-sleep interface (request, gate, enter).
 ******************************************************************************/

#ifndef EM4_H
#define EM4_H

#include <stdbool.h>
#include <stdint.h>

/*
 * em4.h / em4.c contract
 * ----------------------
 * This module owns everything about entering EM4 Shutoff (deepest sleep) and
 * waking from it via BURTC compare. On the EFR32 Series 2, EM4 Shutoff behaves
 * like hibernate/reset: RAM is lost and execution restarts from the reset
 * vector, so em4_enter() never returns.
 *
 * The request/enter split exists because EM4 entry must wait until the Zigbee
 * stack says it is safe (see zigbee_ok_to_sleep()). The app state machine:
 * - calls em4_request() when it wants to sleep (after a publish, or from the
 *   no-progress watchdog),
 * - polls em4_pending() each tick,
 * - and calls em4_enter() once the Zigbee gate opens.
 *
 * Typical usage (see app_process_action() in app.c):
 *   em4_request(10000U, "publish");
 *   ...
 *   if (em4_pending() && zigbee_ok_to_sleep()) {
 *       em4_enter();  // never returns
 *   }
 */

// Latch a deep-sleep request: duration and a human-readable reason label.
// The reason is kept for diagnostics only; it is not interpreted.
void em4_request(uint32_t sleep_ms, const char *reason);

// True while a request made with em4_request() has not yet been entered.
bool em4_pending(void);

// Park the board pins, arm the BURTC wake-up for the requested duration, and
// enter EM4 Shutoff. Never returns: the MCU wakes up through a full reset.
void em4_enter(void);

#endif // EM4_H
