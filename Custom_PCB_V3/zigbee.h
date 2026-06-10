/***************************************************************************//**
 * @file
 * @brief Zigbee helper interface.
 ******************************************************************************/

#ifndef ZIGBEE_H
#define ZIGBEE_H

#include <stdbool.h>

/*
 * zigbee.h / zigbee.c contract
 * -------------------------
 * This module owns Zigbee network bring-up logic for an End Device / Sleepy End
 * Device (join, stored-network restore/rejoin, and a short fast parent-poll
 * window after a fresh join to help ZHA interview complete quickly).
 *
 * Typical usage:
 * - app_init(): call zigbee_init() once.
 * - main loop: call zigbee_process_action() frequently.
 * - Gate application traffic on zigbee_is_network_up().
 * - Gate EM4 entry on zigbee_ok_to_sleep().
 */

// Initialize Zigbee helper state and start join/steering if needed.
void zigbee_init(void);

// Run periodic Zigbee helper processing from the main loop.
void zigbee_process_action(void);

// True only when network, stack state and security context are all ready.
bool zigbee_is_network_up(void);

// True when it's safe for the application to enter deep sleep (EM4).
bool zigbee_ok_to_sleep(void);

#endif // ZIGBEE_H