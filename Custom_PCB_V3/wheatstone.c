/***************************************************************************//**
 * @file
 * @brief Wheatstone bridge supply control and IADC sampling.
 ******************************************************************************/

#include "wheatstone.h"
#include "em_iadc.h"
#include "em_cmu.h"
#include "em_gpio.h"

#define WHEATSTONE_BRIDGE_SUPPLY_PORT gpioPortC
#define WHEATSTONE_BRIDGE_SUPPLY_PIN  (0U)

#define WHEATSTONE_ADC_POS_PORT gpioPortC
#define WHEATSTONE_ADC_POS_PIN  (1U)
#define WHEATSTONE_ADC_NEG_PORT gpioPortC
#define WHEATSTONE_ADC_NEG_PIN  (2U)

/*
 * FILE STRUCTURE (wheatstone.c)
 * -----------------------------
 * This file owns the analog front end: a resistive Wheatstone bridge measured
 * differentially by the IADC. It exposes three calls used by app.c and keeps
 * everything else (pin routing, IADC bring-up) private.
 *
 * Hardware mapping (EFR32MG22E, port C):
 * - PC0: bridge excitation supply, driven as a push-pull GPIO output.
 * - PC1: IADC differential positive input.
 * - PC2: IADC differential negative input.
 *
 * Power strategy (matched to the EM4-reset-per-cycle design in app.c):
 * - wheatstone_init() configures the pins and turns the bridge supply ON so it
 *   can settle during Zigbee bring-up.
 * - wheatstone_read() runs one single-shot conversion, then turns the supply
 *   OFF. The IADC clocks are enabled only for the duration of the conversion.
 * - wheatstone_prepare_for_em4() guarantees the supply is OFF and the pins are
 *   parked before deep sleep.
 *
 * Measurement config: 1.2 V internal reference, 32x oversampling plus 16x
 * digital averaging to reduce noise on the small bridge differential signal.
 *
 * Call flow:
 * - app_init()           -> wheatstone_init()
 * - app_process_action() -> wheatstone_read()            (once per boot)
 * - enter_em4_for_ms()   -> wheatstone_prepare_for_em4()
 */

/**
 * @brief Drive the bridge excitation supply pin (PC0) high to power the bridge.
 */
static void bridge_power_on(void)
{
    GPIO_PinOutSet(WHEATSTONE_BRIDGE_SUPPLY_PORT, WHEATSTONE_BRIDGE_SUPPLY_PIN);
}

/**
 * @brief Drive the bridge excitation supply pin (PC0) low to unpower the bridge.
 */
static void bridge_power_off(void)
{
    GPIO_PinOutClear(WHEATSTONE_BRIDGE_SUPPLY_PORT, WHEATSTONE_BRIDGE_SUPPLY_PIN);
}

/**
 * @brief Enable and configure IADC0 for a single differential conversion.
 *
 * Brings up the IADC0 and FSRCO clocks, selects the 1.2 V internal reference
 * with 4x analog gain (full-scale differential input ~±0.3 V, suited to the
 * small bridge signal), 32x oversampling and 16x digital averaging, and routes
 * PC1 (pos) / PC2 (neg) as the differential input pair. Paired with
 * wheatstone_iadc_disable(), which gates these clocks back off after the read.
 */
static void wheatstone_iadc_enable(void)
{
    IADC_Init_t init = IADC_INIT_DEFAULT;
    IADC_AllConfigs_t initAllConfigs = IADC_ALLCONFIGS_DEFAULT;
    IADC_InitSingle_t initSingle = IADC_INITSINGLE_DEFAULT;
    IADC_SingleInput_t singleInput = IADC_SINGLEINPUT_DEFAULT;
    uint32_t srcClk;

    CMU_ClockEnable(cmuClock_IADC0, true);
    CMU_ClockEnable(cmuClock_FSRCO, true);
    CMU_ClockSelectSet(cmuClock_IADCCLK, cmuSelect_FSRCO);

    srcClk = CMU_ClockFreqGet(cmuClock_IADCCLK);
    init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, srcClk, 0);

    initAllConfigs.configs[0].reference = iadcCfgReferenceInt1V2;
    // 4x analog gain narrows full-scale to ~±0.3 V for the small bridge signal.
    initAllConfigs.configs[0].analogGain = iadcCfgAnalogGain4x;
    initAllConfigs.configs[0].adcClkPrescale =
    IADC_calcAdcClkPrescale(IADC0,
                            10000000,
                            srcClk,
                            0, 
                            iadcCfgModeNormal);

    // 32x oversampling plus digital averaging improves noise for bridge readings.
    initAllConfigs.configs[0].osrHighSpeed = iadcCfgOsrHighSpeed32x;
    initAllConfigs.configs[0].digAvg = IADC_CFG_DIGAVG_AVG16;

    IADC_init(IADC0, &init, &initAllConfigs);

    // Differential input: PC1 (pos) - PC2 (neg)
    singleInput.posInput = iadcPosInputPortCPin1;
    singleInput.negInput = iadcNegInputPortCPin2;
    IADC_initSingle(IADC0, &initSingle, &singleInput);
}

/**
 * @brief Reset IADC0 and gate its clocks off to save current between reads.
 */
static void wheatstone_iadc_disable(void)
{
    IADC_reset(IADC0);
    CMU_ClockEnable(cmuClock_IADC0, false);
    CMU_ClockEnable(cmuClock_FSRCO, false);
}

/**
 * @brief Configure the bridge supply GPIO and the IADC input pins, then
 *        energize the bridge.
 *
 * The supply is turned on here (not in wheatstone_read()) so the bridge has
 * the whole join/interview window to settle before the single conversion. It
 * is turned back off after the measurement and before EM4 to avoid biasing
 * the bridge during deep sleep. Runs once from app_init().
 */
void wheatstone_init(void)
{
    // Keep GPIO clock enabled for analog pin configuration.
    CMU_ClockEnable(cmuClock_GPIO, true);

    // Bridge excitation supply (PC0): drive low by default to keep the bridge off.
    GPIO_PinOutClear(WHEATSTONE_BRIDGE_SUPPLY_PORT, WHEATSTONE_BRIDGE_SUPPLY_PIN);
    GPIO_PinModeSet(WHEATSTONE_BRIDGE_SUPPLY_PORT,
                    WHEATSTONE_BRIDGE_SUPPLY_PIN,
                    gpioModePushPull,
                    0);

    // IADC inputs (PC1/PC2): disable digital input buffers for lowest leakage.
    GPIO_PinModeSet(WHEATSTONE_ADC_POS_PORT, WHEATSTONE_ADC_POS_PIN, gpioModeDisabled, 0);
    GPIO_PinModeSet(WHEATSTONE_ADC_NEG_PORT, WHEATSTONE_ADC_NEG_PIN, gpioModeDisabled, 0);

    // Route PC1 (odd0) and PC2 (even1) to IADC0.
    // Also explicitly un-route PC0 (even0) so it remains a GPIO output.
    GPIO->CDBUSALLOC = (GPIO->CDBUSALLOC
                        & ~(_GPIO_CDBUSALLOC_CDEVEN0_MASK
                            | _GPIO_CDBUSALLOC_CDODD0_MASK
                            | _GPIO_CDBUSALLOC_CDEVEN1_MASK))
                       | GPIO_CDBUSALLOC_CDEVEN0_TRISTATE
                       | GPIO_CDBUSALLOC_CDODD0_ADC0
                       | GPIO_CDBUSALLOC_CDEVEN1_ADC0;

    // Energize the bridge now so it settles during network bring-up; the
    // single conversion in wheatstone_read() then samples a stable voltage.
    bridge_power_on();
}

/**
 * @brief Park the bridge GPIOs in a low-leakage state right before EM4.
 *
 * Forces the supply off and re-applies the intended pin modes so nothing is
 * biased or floating during deep sleep. Called from enter_em4_for_ms() (app.c).
 */
void wheatstone_prepare_for_em4(void)
{
    // Ensure the bridge is not biased during EM4.
    bridge_power_off();

    // Re-apply the intended pin modes right before EM4.
    GPIO_PinModeSet(WHEATSTONE_BRIDGE_SUPPLY_PORT,
                    WHEATSTONE_BRIDGE_SUPPLY_PIN,
                    gpioModePushPull,
                    0);
    GPIO_PinModeSet(WHEATSTONE_ADC_POS_PORT, WHEATSTONE_ADC_POS_PIN, gpioModeDisabled, 0);
    GPIO_PinModeSet(WHEATSTONE_ADC_NEG_PORT, WHEATSTONE_ADC_NEG_PIN, gpioModeDisabled, 0);
}

/**
 * @brief Run one differential conversion (PC1-PC2) and return the raw IADC value.
 *
 * The bridge supply is already on (enabled in wheatstone_init()). This only
 * gates the IADC clocks around the conversion, busy-waits for the result, and
 * then powers the bridge off so it draws nothing until the next EM4 wake/reset.
 * NOTE: the supply is left off on return, so a second call within the same
 * boot would sample an unpowered bridge. This firmware reads exactly once per
 * boot, so that does not occur in practice.
 */
int32_t wheatstone_read(void)
{
    wheatstone_iadc_enable();

    IADC_command(IADC0, iadcCmdStartSingle);

    // Busy-wait until the single-conversion FIFO has valid data.
    while (!(IADC_getStatus(IADC0) & IADC_STATUS_SINGLEFIFODV));
    IADC_Result_t result = IADC_pullSingleFifoResult(IADC0);

    wheatstone_iadc_disable();

    bridge_power_off();
    return (int32_t)result.data;
}
