/***************************************************************************//**
 * @file
 * @brief Application logic: measure the Wheatstone bridge and report it.
 *
 * Owns the top-level state machine, the ZCL "Report Attributes" send path,
 * and the no-progress safety watchdog. Zigbee network bring-up lives in
 * zigbee.c; ADC sampling lives in wheatstone.c; the EM4 deep-sleep entry
 * sequence lives in em4.c.
 ******************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "app/framework/include/af.h"
#include "zap-id.h"
#include "zap-command.h"
#include "wheatstone.h"
#include "zigbee.h"
#include "em4.h"
#include "sl_sleeptimer.h"
#include "em_emu.h"
#include "config/sl_zigbee_pro_leaf_stack_config.h"

/*
 * PROGRAM OVERVIEW
 * ----------------
 * This firmware:
 * 1) Boots and (re)joins a Zigbee network (see zigbee.c).
 * 2) Once joined, reads a Wheatstone bridge via IADC (wheatstone.c) and the
 *    SoC die temperature via the EMU sensor.
 * 3) Sends two ZCL "Report Attributes" to the coordinator (ZHA): die
 *    temperature (cluster 0x0402) and the ADC value (cluster 0x0B04).
 * 4) Waits for the ADC report's send callback as confirmation.
 * 5) Enters EM4 for 10 seconds, wakes via BURTC, and repeats.
 *
 * Notes:
 * - This project targets fast reporting + deep sleep (EM4 reset/hibernate).
 * - For Sleepy End Devices (SED), zigbee.c runs a short fast-poll window after
 *   a fresh join so the ZHA interview can complete before the device hibernates.
 *
 * High-level block diagram:
 *
 *   [Boot/Reset]
 *       |
 *       v
 *   [Join/Rejoin] -> [ZHA interview drain] -> [Read ADC + die temp]
 *                                          -> [Send temp + ADC reports]
 *                                          -> [Wait ADC TX callback]
 *                                          -> [EM4 10s]
 *       ^
 *       |
 *       +---------------------------------------------------------------+
 *
 * Top-level call graph (bare-metal):
 *
 *   main() [main.c]
 *     sl_main_init()
 *     app_init()
 *     while (1):
 *       sl_main_process_action()   // runs Zigbee stack and triggers callbacks
 *       app_process_action()
 *         zigbee_process_action()
 *         (when report_due) wheatstone_read()
 *                           read_die_temperature_centi_c()
 *         send_report()
 *           sl_zigbee_af_send_command_unicast_with_cb(..., temp_tx_ignore_cb)
 *           sl_zigbee_af_send_command_unicast_with_cb(..., report_tx_complete_cb)
 *         (async) report_tx_complete_cb()
 *         em4_request() -> em4_enter()  [em4.c]
 */

/*
 * FILE STRUCTURE (app.c)
 * ----------------------
 * - Optional binding helper (used only as a fallback send path)
 * - Publish retry timer helper
 * - Sensor read: read_die_temperature_centi_c()
 * - ZCL reporting: send_report() emits a best-effort temperature report and a
 *   tracked ADC report; report_tx_complete_cb() (ADC only) gates the sleep cycle
 * - app_init() / app_process_action() state machine
 *
 * EM4 deep-sleep entry (BURTC wake, request/enter latch) lives in em4.c.
 */


#define PUBLISH_RETRY_INTERVAL_MS 1000U
#define EM4_SLEEP_AFTER_PUBLISH_MS 10000U
#define EM4_SLEEP_WATCHDOG_MS 5000U
#define NO_PROGRESS_EM4_TIMEOUT_MS 60000U
#define ZIGBEE_ENDPOINT_ADC 1

static bool tx_done = false;
static sl_status_t tx_status = SL_STATUS_FAIL;
static bool tx_in_flight = false;
static sl_sleeptimer_timer_handle_t publish_retry_timer;
static bool report_due = true;
static bool binding_bootstrap_done = false;
static uint64_t watchdog_deadline_tick = 0U;
static bool watchdog_requested = false;

/**
 * @brief Convert milliseconds to sleeptimer ticks.
 *
 * sl_sleeptimer_ms_to_tick() takes a uint16_t, so values above 65535 ms are
 * clamped to keep the conversion well-defined. The watchdog timeout (60000 ms)
 * fits, so no precision is lost in practice.
 */
static uint64_t ms_to_sleeptimer_ticks(uint32_t ms)
{
    if (ms > 0xFFFFU) {
        ms = 0xFFFFU;
    }

    return (uint64_t)sl_sleeptimer_ms_to_tick((uint16_t)ms);
}

/**
 * @brief (Re)arm the no-progress watchdog deadline.
 *
 * Records "now + NO_PROGRESS_EM4_TIMEOUT_MS" as the tick at which
 * app_process_action() will force a short EM4 reboot if nothing has completed
 * (join, interview, or a TX). Called once from app_init(); each EM4 wake is a
 * reset, so the deadline is effectively re-armed every boot.
 */
static void arm_watchdog(void)
{
    watchdog_deadline_tick = sl_sleeptimer_get_tick_count64()
                                 + ms_to_sleeptimer_ticks(NO_PROGRESS_EM4_TIMEOUT_MS);
    watchdog_requested = false;
}

/**
 * @brief Ensure a binding entry exists for this endpoint/cluster to the coordinator.
 *
 * This is optional for this firmware because we primarily send directly to NWK 0x0000.
 * The binding table is kept as a fallback send path when direct send fails.
 *
 * @return true if binding is confirmed present or successfully created/updated.
 */
static bool ensure_coordinator_binding(void)
{
    uint8_t i;
    uint8_t free_index = 0xFFU;
    uint8_t stale_index = 0xFFU;
    sl_802154_long_addr_t coordinator_eui64;
    sl_status_t st;

    if (SL_ZIGBEE_BINDING_TABLE_SIZE == 0) {
        // Binding fallback is not available; direct-to-coordinator is still used.
        return true;
    }

    st = sl_zigbee_lookup_eui64_by_node_id(0x0000U, coordinator_eui64);
    if (st != SL_STATUS_OK) {
        return false;
    }

    for (i = 0U; i < (uint8_t)SL_ZIGBEE_BINDING_TABLE_SIZE; i++) {
        sl_zigbee_binding_table_entry_t entry;
        st = sl_zigbee_get_binding(i, &entry);
        if (st != SL_STATUS_OK) {
            continue;
        }

        if (entry.type == 0U) {
            if (free_index == 0xFFU) {
                free_index = i;
            }
            continue;
        }

        if (entry.local != ZIGBEE_ENDPOINT_ADC) {
            continue;
        }

        if (entry.clusterId != ZCL_ELECTRICAL_MEASUREMENT_CLUSTER_ID) {
            continue;
        }

        if (memcmp(entry.identifier, coordinator_eui64, sizeof(entry.identifier)) == 0) {
            return true;
        }

        // Same cluster/endpoint but different EUI64: likely a coordinator change.
        if (stale_index == 0xFFU) {
            stale_index = i;
        }
    }

    {
        uint8_t target_index = (stale_index != 0xFFU) ? stale_index : free_index;
        const char *action = (stale_index != 0xFFU) ? "updated" : "created";

        if (target_index == 0xFFU) {
            return false;
        }

        sl_zigbee_binding_table_entry_t new_entry;
        memset(&new_entry, 0, sizeof(new_entry));
    #ifdef SL_ZIGBEE_UNICAST_BINDING
        new_entry.type = SL_ZIGBEE_UNICAST_BINDING;
    #else
        new_entry.type = 1U;
    #endif
        new_entry.local = ZIGBEE_ENDPOINT_ADC;
        new_entry.remote = ZIGBEE_ENDPOINT_ADC;
        new_entry.clusterId = ZCL_ELECTRICAL_MEASUREMENT_CLUSTER_ID;
        new_entry.networkIndex = 0U;
        memcpy(new_entry.identifier, coordinator_eui64, sizeof(new_entry.identifier));

        st = sl_zigbee_set_binding(target_index, &new_entry);
        if (st == SL_STATUS_OK) {
            return true;
        } else {
            return false;
        }
    }
}

/**
 * @brief Sleeptimer callback: allow another publish attempt.
 */
static void publish_retry_timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data)
{
    (void)handle;
    (void)data;
    report_due = true;
}

/**
 * @brief Schedule a publish retry using the sleeptimer.
 */
static void schedule_publish_retry(void)
{
    sl_status_t st;

    report_due = false;
    (void)sl_sleeptimer_stop_timer(&publish_retry_timer);
    st = sl_sleeptimer_start_timer_ms(&publish_retry_timer,
                                      PUBLISH_RETRY_INTERVAL_MS,
                                      publish_retry_timer_callback,
                                      NULL,
                                      0,
                                      0);
    if (st != SL_STATUS_OK) {
        // If timer setup fails, retry on next main loop iteration.
        report_due = true;
    }
}

/**
 * @brief APS completion callback for the report we sent.
 *
 * Called asynchronously by the stack (from inside sl_main_process_action())
 * once the report's APS PDU has been ACKed or has failed. It does no work
 * itself: it just latches the outcome into tx_status/tx_done so the main-loop
 * state machine can act on it. The tx_in_flight guard rejects any stale or
 * duplicate callback that arrives when we are not expecting one.
 */
static void report_tx_complete_cb(sl_zigbee_outgoing_message_type_t type,
                                  uint16_t indexOrDestination,
                                  sl_zigbee_aps_frame_t *apsFrame,
                                  uint16_t msgLen,
                                  uint8_t *message,
                                  sl_status_t status)
{
    if (!tx_in_flight) {
        return;
    }

    tx_status = status;
    tx_done = true;
}

/**
 * @brief Read the on-die temperature sensor, in centi-degrees Celsius.
 *
 * Uses the always-on EMU temperature sensor (EMU_TemperatureGet() returns the
 * die temperature in degrees C as a float). The ZCL Temperature Measurement
 * MeasuredValue attribute is INT16S in units of 0.01 C, so we scale by 100,
 * round to nearest, and clamp to the INT16S range.
 *
 * NOTE: this is the SoC die temperature, not an external ambient sensor. It
 * reads high relative to ambient whenever the radio/CPU have been active.
 */
static int16_t read_die_temperature_centi_c(void)
{
    float scaled = EMU_TemperatureGet() * 100.0f;

    scaled += (scaled >= 0.0f) ? 0.5f : -0.5f; // round to nearest
    if (scaled > 32767.0f) {
        scaled = 32767.0f;
    } else if (scaled < -32768.0f) {
        scaled = -32768.0f;
    }

    return (int16_t)scaled;
}

/**
 * @brief No-op TX callback for the best-effort temperature report.
 *
 * The temperature report is fire-and-forget: only the ADC report gates the
 * sleep cycle. Routing the temperature completion to its own do-nothing
 * callback keeps it from ever touching tx_done/tx_in_flight (which would
 * otherwise risk a false completion for the tracked ADC report).
 */
static void temp_tx_ignore_cb(sl_zigbee_outgoing_message_type_t type,
                              uint16_t indexOrDestination,
                              sl_zigbee_aps_frame_t *apsFrame,
                              uint16_t msgLen,
                              uint8_t *message,
                              sl_status_t status)
{
    (void)type;
    (void)indexOrDestination;
    (void)apsFrame;
    (void)msgLen;
    (void)message;
    (void)status;
}

/**
 * @brief Strip slow APS options off the current command so the TX callback is
 *        prompt (no multi-second retry / route-discovery loop).
 */
static void shorten_aps_callback_latency(void)
{
    sl_zigbee_aps_frame_t *aps_frame = sl_zigbee_af_get_command_aps_frame();
    if (aps_frame != NULL) {
        aps_frame->options &= ~(SL_ZIGBEE_APS_OPTION_RETRY
                                | SL_ZIGBEE_APS_OPTION_FORCE_ROUTE_DISCOVERY
                                | SL_ZIGBEE_APS_OPTION_ENABLE_ADDRESS_DISCOVERY);
    }
}

/**
 * @brief Report the ADC value and die temperature to the coordinator.
 *
 * Sends two ZCL "Report Attributes" unicasts to NWK 0x0000 in the same cycle:
 *   1. Temperature Measurement (0x0402) MeasuredValue - best-effort, untracked.
 *   2. Electrical Measurement (0x0B04) RMS Voltage     - tracked; its TX
 *      callback (report_tx_complete_cb) is what gates EM4 entry.
 *
 * They are separate commands because a single Report Attributes command can
 * only carry attributes from one cluster.
 *
 * @return true if the (tracked) ADC report was successfully queued.
 */
static bool send_report(uint16_t adc_value, int16_t temp_centi)
{
    uint8_t record[5];
    sl_status_t send_status;
    sl_zigbee_af_status_t status;

    // Update both attribute caches up front.
    status = sl_zigbee_af_write_server_attribute(
        ZIGBEE_ENDPOINT_ADC,
        ZCL_ELECTRICAL_MEASUREMENT_CLUSTER_ID,
        ZCL_RMS_VOLTAGE_ATTRIBUTE_ID,
        (uint8_t *)&adc_value,
        ZCL_INT16U_ATTRIBUTE_TYPE);
    if (status != SL_ZIGBEE_ZCL_STATUS_SUCCESS) {
        return false;
    }

    status = sl_zigbee_af_write_server_attribute(
        ZIGBEE_ENDPOINT_ADC,
        ZCL_TEMP_MEASUREMENT_CLUSTER_ID,
        ZCL_TEMP_MEASURED_VALUE_ATTRIBUTE_ID,
        (uint8_t *)&temp_centi,
        ZCL_INT16S_ATTRIBUTE_TYPE);
    if (status != SL_ZIGBEE_ZCL_STATUS_SUCCESS) {
        return false;
    }

    // --- Temperature report (cluster 0x0402), best-effort / not tracked. ---
    record[0] = (uint8_t)(ZCL_TEMP_MEASURED_VALUE_ATTRIBUTE_ID & 0xFFU);
    record[1] = (uint8_t)((ZCL_TEMP_MEASURED_VALUE_ATTRIBUTE_ID >> 8) & 0xFFU);
    record[2] = ZCL_INT16S_ATTRIBUTE_TYPE;
    record[3] = (uint8_t)((uint16_t)temp_centi & 0xFFU);
    record[4] = (uint8_t)(((uint16_t)temp_centi >> 8) & 0xFFU);

    sl_zigbee_af_set_command_endpoints(ZIGBEE_ENDPOINT_ADC, ZIGBEE_ENDPOINT_ADC);
    sl_zigbee_af_fill_command_global_server_to_client_report_attributes(
        ZCL_TEMP_MEASUREMENT_CLUSTER_ID,
        record,
        sizeof(record));
    shorten_aps_callback_latency();
    // Ignore the queue result: the ADC report below is what drives the cycle.
    (void)sl_zigbee_af_send_command_unicast_with_cb(SL_ZIGBEE_OUTGOING_DIRECT,
                                                    0x0000U,
                                                    temp_tx_ignore_cb);

    // --- ADC report (cluster 0x0B04), tracked; its callback gates EM4. ---
    record[0] = (uint8_t)(ZCL_RMS_VOLTAGE_ATTRIBUTE_ID & 0xFFU);
    record[1] = (uint8_t)((ZCL_RMS_VOLTAGE_ATTRIBUTE_ID >> 8) & 0xFFU);
    record[2] = ZCL_INT16U_ATTRIBUTE_TYPE;
    record[3] = (uint8_t)(adc_value & 0xFFU);
    record[4] = (uint8_t)((adc_value >> 8) & 0xFFU);

    sl_zigbee_af_set_command_endpoints(ZIGBEE_ENDPOINT_ADC, ZIGBEE_ENDPOINT_ADC);
    sl_zigbee_af_fill_command_global_server_to_client_report_attributes(
        ZCL_ELECTRICAL_MEASUREMENT_CLUSTER_ID,
        record,
        sizeof(record));
    shorten_aps_callback_latency();

    // Arm callback tracking before send in case the callback is immediate.
    tx_done = false;
    tx_status = SL_STATUS_FAIL;
    tx_in_flight = true;

    // Coordinator NWK is always 0x0000; send directly first for deterministic behavior.
    send_status = sl_zigbee_af_send_command_unicast_with_cb(SL_ZIGBEE_OUTGOING_DIRECT,
                                                             0x0000U,
                                                             report_tx_complete_cb);
    if (send_status != SL_STATUS_OK) {
        // Keep binding path available as fallback.
        send_status = sl_zigbee_af_send_command_unicast_to_bindings_with_cb(report_tx_complete_cb);
        if (send_status != SL_STATUS_OK) {
            tx_in_flight = false;
            return false;
        }
    }

    return true;
}

/***************************************************************************//**
 * Initialize application.
 ******************************************************************************/
void app_init(void)
{
    // Initialize the Wheatstone bridge / IADC
    wheatstone_init();
    zigbee_init();
    sl_zigbee_set_radio_power(-26);  // dBm, int8_t; EFR32MG22 range: -26..+6 dBm normal mode
    arm_watchdog();
}

/***************************************************************************//**
 * App ticking function.
 ******************************************************************************/
void app_process_action(void)
{
    zigbee_process_action();

    // State machine, evaluated top-to-bottom every tick.
    //
    // 1) EM4 requested      -> enter EM4 once zigbee_ok_to_sleep() agrees.
    // 2) TX callback arrived -> success: schedule a 10 s EM4 cycle ("publish").
    //                           failure: schedule a 1 s publish retry.
    // 3) No-progress watchdog expired -> force a short EM4 reboot ("watchdog").
    // 4) TX still in flight  -> wait for the callback.
    // 5) Otherwise           -> (once) ensure the coordinator binding, then,
    //                           when report_due, read the ADC and send a report.
    //
    // Note: there is no "wait until joined" gate here. Before the network is up
    // the send in step 5 simply fails and falls into the 1 s retry path.

    if (em4_pending()) {
        if (zigbee_ok_to_sleep()) {
            em4_enter(); // never returns; MCU wakes via reset
        }
        return;
    }

    if (tx_done) {
        sl_status_t tx_status_local;

        tx_done = false;
        tx_status_local = tx_status;
        tx_in_flight = false;

        if (tx_status_local == SL_STATUS_OK) {
            em4_request(EM4_SLEEP_AFTER_PUBLISH_MS, "publish");
        } else {
            schedule_publish_retry();
        }
        return;
    }

    // Safety watchdog: if we get stuck for too long (join/interview/TX never
    // completes), force a short EM4 cycle to reboot and try again.
    if (!em4_pending() && !watchdog_requested) {
        uint64_t now = sl_sleeptimer_get_tick_count64();
        if (now >= watchdog_deadline_tick) {
            watchdog_requested = true;
            em4_request(EM4_SLEEP_WATCHDOG_MS, "watchdog");
            return;
        }
    }

    if (tx_in_flight) {
        return;
    }

    // Binding is not required for direct-to-coordinator reporting, but keeping
    // it valid makes the binding fallback path usable if direct send fails.
    if (!binding_bootstrap_done) {
        binding_bootstrap_done = ensure_coordinator_binding();
    }

    if (!report_due) {
        return;
    }

    int32_t adc_raw = 0;
    uint16_t adc_attr = 0U;

    adc_raw = (int32_t)wheatstone_read();
    if (adc_raw < -32767) {
        adc_raw = -32767;
    } else if (adc_raw > 32767) {
        adc_raw = 32767;
    }

    // Map signed range [-32767..32767] into an unsigned attribute.
    // This avoids negative ZCL types while still preserving sign information.
    // Decode on the receiver side with: adc_raw = (int32_t)adc_attr - 32767.
    adc_attr = (uint16_t)(adc_raw + 32767);

    // Sample the SoC die temperature in the same cycle and report both together.
    int16_t temp_centi = read_die_temperature_centi_c();

    if (!send_report(adc_attr, temp_centi)) {
        schedule_publish_retry();
        return;
    }
}
