#ifndef WHEATSTONE_H
#define WHEATSTONE_H

#include <stdbool.h>
#include <stdint.h>

/***************************************************************************//**
 * Configure the bridge supply / IADC pins and energize the bridge.
 *
 * Powers the bridge on so it can settle before wheatstone_read() samples it.
 ******************************************************************************/
void wheatstone_init(void);

/***************************************************************************//**
 * Ensure the Wheatstone-related GPIOs are in a low-leakage state.
 *
 * Intended to be called right before entering EM4.
 ******************************************************************************/
void wheatstone_prepare_for_em4(void);

/***************************************************************************//**
 * Read a single conversion result from the Wheatstone bridge.
 ******************************************************************************/
int32_t wheatstone_read(void);

#endif  // WHEATSTONE_H
