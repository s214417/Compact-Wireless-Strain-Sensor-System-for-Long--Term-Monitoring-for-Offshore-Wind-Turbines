/***************************************************************************//**
 * @file
 * @brief Zigbee helper implementation.
 ******************************************************************************/

#include "zigbee.h"
#include "app/framework/include/af.h"
#include "network-steering.h"
#include "sl_sleeptimer.h"
#include "stack/include/network-formation.h"
#include "stack/include/stack-info.h"
#include "stack/include/child.h"

// Provided by the Zigbee App Framework sleep module.
extern void sl_zigbee_af_force_end_device_to_stay_awake(bool stayAwake);

/* 
 * FILE STRUCTURE (zigbee.c)
 * ------------------------
 * This file implements a small Zigbee "bring-up" helper for a Zigbee End Device
 * / Sleepy End Device.
 *
 * Responsibilities:
 * - Join a network using the Network Steering plugin when no stored network
 *   exists.
 * - If a stored network exists: wait for the stack to come up, attempt a secure
 *   rejoin once, then fall back to leave + steering if needed.
 * - After a *fresh join* (no stored network / after leave), run a short fast
 *   parent-poll window so ZHA interview traffic queued at the parent is
 *   delivered quickly.
 * - Provide two simple queries for the application:
 *     - zigbee_is_network_up(): joined + security ready
 *     - zigbee_ok_to_sleep(): safe to enter EM4 (no interview poll window,
 *       nothing pending in the stack)
 *
 * Call flow (high level):
 * - app_init() -> zigbee_init()
 * - main loop -> zigbee_process_action()
 * - Zigbee stack calls callbacks in this file:
 *     - sl_zigbee_af_stack_status_cb()
 *     - sl_zigbee_af_network_steering_complete_cb()
 */

//------------------------------------------------------------------------------
// Timing configuration
//------------------------------------------------------------------------------
#define JOIN_RETRY_INTERVAL_MS 10000U
#define STORED_NETWORK_RESTORE_TIMEOUT_MS 8000U
#define STORED_NETWORK_REJOIN_TIMEOUT_MS  8000U

// For sleepy end devices, ZHA interview frames are delivered via the parent.
// After a fresh join, run a short fast-poll window so the interview completes.
#define ZIGBEE_INTERVIEW_POLL_WINDOW_MS 10000U
#define ZIGBEE_INTERVIEW_POLL_INTERVAL_MS 250U

// Guard against deadlock if the stack never reports it is OK to long-poll.
// (This can happen during extended ZHA interview/configure bursts, or if the
// device is not operating as a sleepy end device.)
#define ZIGBEE_OK_TO_LONG_POLL_TIMEOUT_MS 15000U

static sl_sleeptimer_timer_handle_t retry_timer;
static sl_sleeptimer_timer_handle_t startup_timer;

static bool module_initialized = false;
static bool steering_active = false;
static bool retry_pending = false;
static bool startup_timeout_pending = false;
static bool network_up = false;
static bool security_ready = false;
static bool had_stored_network = false;
static bool steering_after_leave_pending = false;
static bool stored_network_rejoin_attempted = false;

static bool fresh_join_expected = false;
static bool interview_poll_active = false;
static uint64_t interview_poll_deadline_tick = 0U;
static uint64_t interview_next_poll_tick = 0U;
static uint64_t poll_block_start_tick = 0U;

static void start_network_steering(const char *reason);
static void stop_network_steering_if_active(void);
static void arm_retry_timer(void);
static void arm_startup_timeout_ms(uint32_t timeout_ms);
static bool has_valid_pan_and_node(void);
static bool refresh_security_ready(const char *context);
static void handle_network_up(const char *context);
static void leave_network_and_steer(const char *reason);
static void stop_interview_poll_window(void);

/**
 * @brief Convert milliseconds to sleeptimer ticks.
 *
 * sl_sleeptimer_ms_to_tick() takes a uint16_t, so clamp to keep the conversion
 * well-defined.
 */
static uint64_t ms_to_ticks(uint32_t ms)
{
  if (ms > 0xFFFFU) {
    ms = 0xFFFFU;
  }

  return (uint64_t)sl_sleeptimer_ms_to_tick((uint16_t)ms);
}

/**
 * @brief Start a short fast parent-poll window after a fresh join.
 *
 * For Sleepy End Devices, ZHA interview requests are queued at the parent and
 * only delivered when the device polls. This window aggressively polls to
 * drain those frames quickly.
 */
static void maybe_start_interview_poll_window(const char *context)
{
  if (!fresh_join_expected) {
    return;
  }

  // Only start once, right after we come up from a fresh join.
  fresh_join_expected = false;
  interview_poll_active = true;
  interview_poll_deadline_tick = sl_sleeptimer_get_tick_count64()
                               + ms_to_ticks(ZIGBEE_INTERVIEW_POLL_WINDOW_MS);
  interview_next_poll_tick = 0U;

  // Only force "stay awake" when we truly did a fresh join (no stored network
  // tokens). If a stored network was restored on boot, allow normal low-power
  // behavior during any follow-on traffic.
  if (!had_stored_network) {
    // Keep the device awake and aggressively poll the parent so queued interview
    // traffic is delivered quickly.
    sl_zigbee_af_force_end_device_to_stay_awake(true);
  }
}

/**
 * @brief Stop the fast parent-poll window and clear its bookkeeping.
 */
static void stop_interview_poll_window(void)
{
  interview_poll_active = false;
  interview_poll_deadline_tick = 0U;
  interview_next_poll_tick = 0U;

  // Allow the stack to return to normal polling behavior after the interview
  // drain window so sl_zigbee_ok_to_long_poll() can eventually become true.
  sl_zigbee_af_force_end_device_to_stay_awake(false);
}

/**
 * @brief Re-read the stack's security state and cache it in security_ready.
 *
 * The network can be associated before the network key is installed, so we
 * treat a valid security context as the point where application traffic may
 * begin. ZHA may start interview reads immediately after join. Returns the
 * latched value for convenience.
 */
static bool refresh_security_ready(const char *context)
{
  sl_status_t sec_status;
  sl_zigbee_current_security_state_t sec_state;

  sec_status = sl_zigbee_get_current_security_state(&sec_state);
  if (sec_status == SL_STATUS_OK) {
    security_ready = true;
    return true;
  }

  security_ready = false;
  return false;
}

/**
 * @brief Sanity-check whether PAN ID and Node ID look valid.
 */
static bool has_valid_pan_and_node(void)
{
  uint16_t pan_id = sl_zigbee_get_pan_id();
  uint16_t node_id = sl_zigbee_get_node_id();

  return (pan_id != 0xFFFFU) && (node_id != 0xFFFFU) && (node_id != 0x0000U);
}

/**
 * @brief Timer callback: request a join/steering retry from the main loop.
 */
static void retry_timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  retry_pending = true;
}

/**
 * @brief Timer callback: indicate stored-network startup timed out.
 */
static void startup_timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  startup_timeout_pending = true;
}

/**
 * @brief Start Network Steering (join) if not already active.
 *
 * This is the primary join path when no stored network is available.
 */
static void start_network_steering(const char *reason)
{
  sl_status_t status;

  if (!module_initialized || steering_active) {
    return;
  }

  if (network_up) {
    return;
  }

  status = sl_zigbee_af_network_steering_start();
  if (status == SL_STATUS_OK) {
    steering_active = true;
  } else {
    // If steering cannot start yet (for example due to transient stack state),
    // retry later so we eventually attempt a full scan and get completion status.
    arm_retry_timer();
  }
}

/**
 * @brief Stop Network Steering if currently running.
 */
static void stop_network_steering_if_active(void)
{
  if (!steering_active) {
    return;
  }

  (void)sl_zigbee_af_network_steering_stop();

  steering_active = false;
}

/**
 * @brief Arm a periodic retry to restart network steering.
 */
static void arm_retry_timer(void)
{
  (void)sl_sleeptimer_stop_timer(&retry_timer);
  (void)sl_sleeptimer_start_timer_ms(&retry_timer,
                                     JOIN_RETRY_INTERVAL_MS,
                                     retry_timer_callback,
                                     NULL,
                                     0,
                                     0);
}

/**
 * @brief Arm a one-shot startup timeout.
 *
 * Used to detect when a stored network never transitions to NETWORK_UP so we
 * can attempt a rejoin or fall back to leaving and joining a new network.
 */
static void arm_startup_timeout_ms(uint32_t timeout_ms)
{
  startup_timeout_pending = false;
  (void)sl_sleeptimer_stop_timer(&startup_timer);
  (void)sl_sleeptimer_start_timer_ms(&startup_timer,
                                     timeout_ms,
                                     startup_timer_callback,
                                     NULL,
                                     0,
                                     0);
}

/**
 * @brief Transition internal state to "network up".
 *
 * Stops steering/timers, latches security readiness, and starts the
 * fresh-join interview fast-poll window if applicable.
 */
static void handle_network_up(const char *context)
{
  if (network_up) {
    return;
  }

  network_up = true;
  stop_network_steering_if_active();
  retry_pending = false;
  startup_timeout_pending = false;
  (void)sl_sleeptimer_stop_timer(&retry_timer);
  (void)sl_sleeptimer_stop_timer(&startup_timer);
  (void)refresh_security_ready(context);

  // For a fresh join, keep the device awake and fast-poll the parent for a
  // short window so ZHA interview frames queued at the parent are delivered.
  maybe_start_interview_poll_window(context);
}

/**
 * @brief Leave the current network and start steering to join a new one.
 *
 * This is used when a stored network restore/rejoin fails and we want to join
 * a new HA network from scratch.
 */
static void leave_network_and_steer(const char *reason)
{
  sl_status_t st;

  // Leave clears stored network tokens so steering can join a fresh HA network.
  st = sl_zigbee_leave_network(SL_ZIGBEE_LEAVE_NWK_WITH_NO_OPTION);
  if (st == SL_STATUS_OK || st == SL_STATUS_IN_PROGRESS) {
    steering_after_leave_pending = true;
  } else {
    start_network_steering("leave failed fallback");
  }

  had_stored_network = false;
  stored_network_rejoin_attempted = false;
  network_up = false;
  security_ready = false;

  // Next time we join, it's expected to be a fresh join.
  fresh_join_expected = true;
  stop_interview_poll_window();
}

/**
 * @brief Initialize the Zigbee join/rejoin helper.
 *
 * Called once from app_init(). Progress continues via stack callbacks and
 * zigbee_process_action().
 */
void zigbee_init(void)
{
  sl_zigbee_network_status_t net_status;

  module_initialized = true;
  (void)sl_sleeptimer_stop_timer(&retry_timer);
  (void)sl_sleeptimer_stop_timer(&startup_timer);

  retry_pending = false;
  startup_timeout_pending = false;
  steering_active = false;
  steering_after_leave_pending = false;
  network_up = false;
  security_ready = false;
  had_stored_network = false;
  stored_network_rejoin_attempted = false;
  fresh_join_expected = false;
  stop_interview_poll_window();

  net_status = sl_zigbee_network_state();
  if ((net_status == SL_ZIGBEE_JOINED_NETWORK || net_status == SL_ZIGBEE_JOINED_NETWORK_NO_PARENT)
      && has_valid_pan_and_node()) {
    had_stored_network = true;
    arm_startup_timeout_ms(STORED_NETWORK_RESTORE_TIMEOUT_MS);
    return;
  }

  fresh_join_expected = true;
  start_network_steering("init no stored network");
}

/**
 * @brief Periodic processing for join/rejoin and interview polling.
 *
 * Call this frequently from the main loop.
 */
void zigbee_process_action(void)
{
  if (!module_initialized) {
    return;
  }

  if (interview_poll_active) {
    uint64_t now = sl_sleeptimer_get_tick_count64();
    if (now >= interview_poll_deadline_tick) {
      stop_interview_poll_window();
    } else {
      if (interview_next_poll_tick == 0U || now >= interview_next_poll_tick) {
        (void)sl_zigbee_poll_for_data();
        interview_next_poll_tick = now + ms_to_ticks(ZIGBEE_INTERVIEW_POLL_INTERVAL_MS);
      }
    }
  }

  if (steering_after_leave_pending
      && !steering_active
      && !network_up
      && sl_zigbee_network_state() == SL_ZIGBEE_NO_NETWORK) {
    steering_after_leave_pending = false;
    start_network_steering("leave complete poll");
    return;
  }

  // If for any reason the callback is missed, detect a joined network via polling
  // so the app can still proceed to publish.
  if (!network_up
      && sl_zigbee_stack_is_up()
      && sl_zigbee_network_state() == SL_ZIGBEE_JOINED_NETWORK
      && has_valid_pan_and_node()) {
    handle_network_up("poll");
  }

  if (network_up) {
    refresh_security_ready("process action");
    return;
  }

  if (startup_timeout_pending && had_stored_network) {
    startup_timeout_pending = false;

    // Stage 0: stored network exists but never came up; try a secure rejoin first.
    if (!stored_network_rejoin_attempted) {
      sl_status_t rejoin_status = sl_zigbee_find_and_rejoin_network(true,
                                                                    SL_ZIGBEE_ALL_802_15_4_CHANNELS_MASK,
                                                                    SL_ZIGBEE_REJOIN_DUE_TO_APP_EVENT_1,
                                                                    SL_ZIGBEE_DEVICE_TYPE_UNCHANGED);

      if (rejoin_status == SL_STATUS_OK) {
        stored_network_rejoin_attempted = true;
        arm_startup_timeout_ms(STORED_NETWORK_REJOIN_TIMEOUT_MS);
        return;
      }

      // If rejoin can't even start, fall through to join a fresh network.
    }

    // Stage 1: rejoin did not bring the network up -> leave + steering to join new HA.
    leave_network_and_steer("startup fallback");
    return;
  }

  if (has_valid_pan_and_node() && !security_ready) {
    refresh_security_ready("waiting join path");
  }

  if (!retry_pending || steering_active || network_up) {
    return;
  }

  retry_pending = false;
  start_network_steering("retry timer");
}

/**
 * @brief Check whether the device is joined and security is ready.
 */
bool zigbee_is_network_up(void)
{
  sl_zigbee_network_status_t net_status = sl_zigbee_network_state();

  return (network_up
          && security_ready
          && has_valid_pan_and_node()
          && sl_zigbee_stack_is_up()
          && net_status == SL_ZIGBEE_JOINED_NETWORK);
}

/**
 * @brief Check whether it is safe for the application to enter EM4.
 */
bool zigbee_ok_to_sleep(void)
{
  // While the fresh-join interview polling window is active, do not allow EM4.
  if (interview_poll_active) {
    return false;
  }

  // Also avoid sleeping if the stack says we should keep polling.
  // (This macro checks for pending ACKed messages / parent connectivity.)
  if (!sl_zigbee_ok_to_long_poll()) {
    uint64_t now = sl_sleeptimer_get_tick_count64();

    if (poll_block_start_tick == 0U) {
      poll_block_start_tick = now;
    }

    // Don't block forever; if the coordinator keeps traffic pending, we still
    // need to make forward progress (this firmware uses EM4 reset cycles).
    if ((now - poll_block_start_tick)
        < ms_to_ticks(ZIGBEE_OK_TO_LONG_POLL_TIMEOUT_MS)) {
      return false;
    }
  } else {
    poll_block_start_tick = 0U;
  }

  return true;
}

/**
 * @brief Zigbee stack status callback.
 *
 * Tracks join/leave transitions and (re)starts steering as needed.
 */
void sl_zigbee_af_stack_status_cb(sl_status_t status)
{
  if (!module_initialized) {
    return;
  }

  switch (status) {
    case SL_STATUS_NETWORK_UP:
      handle_network_up("stack status");
      break;

    case SL_STATUS_NETWORK_DOWN:
      network_up = false;
      security_ready = false;
      steering_active = false;
      startup_timeout_pending = false;
      stored_network_rejoin_attempted = false;
      (void)sl_sleeptimer_stop_timer(&startup_timer);
      stop_interview_poll_window();
      sl_zigbee_af_force_end_device_to_stay_awake(false);
      if (steering_after_leave_pending) {
        steering_after_leave_pending = false;
        start_network_steering("leave complete");
        break;
      }

      start_network_steering("network down");
      break;

    case SL_STATUS_NOT_JOINED:
      network_up = false;
      security_ready = false;
      startup_timeout_pending = false;
      stored_network_rejoin_attempted = false;
      (void)sl_sleeptimer_stop_timer(&startup_timer);
      stop_interview_poll_window();
      sl_zigbee_af_force_end_device_to_stay_awake(false);
      if (steering_after_leave_pending) {
        steering_after_leave_pending = false;
        start_network_steering("leave complete");
        break;
      }

      start_network_steering("not joined");
      break;

    default:
      break;
  }
}

/**
 * @brief Network Steering completion callback.
 *
 * Successful steering indicates a join attempt succeeded; we still wait for the
 * NETWORK_UP stack status before reporting "network up" to the application.
 */
void sl_zigbee_af_network_steering_complete_cb(sl_status_t status,
                                               uint8_t totalBeacons,
                                               uint8_t joinAttempts,
                                               uint8_t finalState)
{
  steering_active = false;

  (void)joinAttempts;
  (void)finalState;

  if (network_up) {
    return;
  }

  // On success, do nothing here: wait for the NETWORK_UP stack status to
  // confirm the join. Only a steering failure needs handling (retry later).
  if (status != SL_STATUS_OK) {
    network_up = false;
    arm_retry_timer();
  }
}