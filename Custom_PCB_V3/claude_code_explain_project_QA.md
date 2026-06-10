# Claude Code — Project Q&A Log

---

## Entry 1

**Timestamp:** 2026-06-09

**Original question:**

> I have this project written here in vscode and simplicity studio called Custom_PCB_V3. The project is for a custom PCB i have made that is centered around my EFR32MG22E MCU. the PCBs task is to read the wheatstone bridge with its IADC and send it to a Home Assistant (HA) over a zigbee connection.
>
> You are analyzing this embedded Zigbee project.
>
> Task:
> 1. Read every meaningful source file in the project.
> 2. Explain each function in plain language.
> 3. Explain how the whole program works from startup to main loop, including Zigbee bring-up, sensor sampling, low-power behavior, and any callback flow.
> 4. Call out the order of execution, important data flow, state machines, timers, and callback relationships.
> 5. Identify generated code and build output, but do not spend time explaining files under `autogen/`, `cmake_gcc/build/`, or other generated directories unless they are directly needed to understand the program.
> 6. If a function is a wrapper, callback, or helper, say what it wraps or helps and why it exists.
> 7. If something is unclear from the code, say so instead of guessing.

---

## Answer

---

# Custom_PCB_V3 — Full Project Analysis

## Project Overview

This firmware runs on a custom PCB centered on a Silicon Labs **EFR32MG22E** SoC (Series 2, multiprotocol). The board's single job is:

1. Join a Zigbee network managed by Home Assistant (ZHA integration).
2. Read a **Wheatstone bridge** via the on-chip **IADC** (Incremental ADC with differential mode).
3. Send the ADC reading to the coordinator as a **ZCL "Report Attributes"** message on the Electrical Measurement cluster.
4. Enter **EM4 Shutoff** (deepest sleep — equivalent to a hibernate/reset) for 10 seconds, then wake via BURTC compare and repeat.

The device is configured as a **Sleepy End Device (SED)** with ZigBee 3.0 security. It is a bare-metal application (no RTOS) built on Silicon Labs' Simplicity SDK / App Framework.

---

## File-by-file Analysis

---

### `main.c` — Startup and superloop

**Role:** Generated entry point wired to SDK init and the application hooks.

#### `main()`

- **What it does:** Calls `sl_main_init()` to bring up all SDK components (clocks, Zigbee stack, peripherals, power manager), then calls `app_init()` once, then spins in a superloop calling `sl_main_process_action()` and `app_process_action()` forever.
- **When it runs:** From reset; never returns.
- **Depends on:** SDK `sl_main_init.h`, `sl_main_process_action.h`, `app.h`.
- **State it changes:** Starts the entire system; delegates all state changes to the functions it calls.

The `#ifdef SL_CATALOG_KERNEL_PRESENT` branch is dead code here — no RTOS is used, so the `else` branch always compiles.

`sl_main_process_action()` is the SDK's heartbeat. On every call it runs the Zigbee stack tick, fires any sleeptimer callbacks whose deadlines have passed, lets the power manager decide whether to drop into EM1/EM2 when idle, and invokes any other registered component "process action" hooks. It is the only place the Zigbee stack gets CPU time.

---

### `app.h` — Application interface

**Role:** Declares exactly two functions: `app_init()` and `app_process_action()`. Both are called directly from `main.c`. No state is exposed through the header — all state in `app.c` is file-scoped.

---

### `app.c` — Application state machine and EM4 management

**Role:** Top-level application logic. Owns the main state machine, the ZCL reporting path, the EM4 entry sequence, and the safety watchdog.

#### Key module-level state variables

| Variable | Purpose |
|---|---|
| `tx_confirm_received` | Set to `true` by `report_tx_complete_cb()` when the APS TX callback arrives. |
| `tx_confirm_sl_status` | Status code from the TX callback. |
| `tx_confirm_pending` | `true` while a send is in flight (no callback yet). |
| `tx_start_tick` | Tick count at send time, used to measure TX round-trip latency. |
| `tx_timing_active` | Guards the latency measurement against spurious callbacks. |
| `publish_retry_timer` | Sleeptimer handle for re-trying a failed send. |
| `publish_retry_due` | Set `true` on boot and by the retry timer; gates the "read ADC + send" path. |
| `em4_entry_requested` | Set when the state machine wants to enter EM4. Actual entry waits for `zigbee_ok_to_sleep()`. |
| `em4_sleep_ms_requested` | Duration passed to `enter_em4_for_ms()`. |
| `em4_reason` | String label for the EM4 reason (for diagnostics). |
| `binding_bootstrap_done` | Latched once the binding table entry for the coordinator has been confirmed/created. |
| `no_progress_em4_deadline_tick` | Absolute tick count for the 60-second no-progress watchdog deadline. |
| `watchdog_em4_requested` | Set when the watchdog fires to prevent it from re-firing on the same boot. |

---

#### `clamp_burtc_clkdiv(uint32_t clkdiv)` — static helper

- **What it does:** Clamps a BURTC clock prescaler value to the Series 2 hardware-supported range [1..32768].
- **When it runs:** Called only from `choose_burtc_clkdiv()` → `enter_em4_for_ms()`.
- **Why it exists:** Defensive guard before writing to BURTC hardware registers.

---

#### `choose_burtc_clkdiv(uint32_t sleep_ms)` — static helper

- **What it does:** Selects a coarse BURTC prescaler based on the requested sleep duration, trading timing granularity for lower EM4 current (a higher prescaler means the BURTC counter ticks more slowly, reducing BURTC oscillator switching current marginally and making the compare logic fire less often in very deep sleep).
  - ≤ 1 min → prescaler 1 (≈1 ms/tick at 1 kHz ULFRCO)
  - ≤ 10 min → prescaler 1024 (≈1 s/tick)
  - ≤ 24 h → prescaler 8192 (≈8 s/tick)
  - else → prescaler 32768 (≈33 s/tick, for multi-day sleeps)
- **When it runs:** At every EM4 entry, before BURTC init.
- **Note:** With the default `EM4_SLEEP_AFTER_PUBLISH_MS = 10000` (10 s), this always returns prescaler 1.

---

#### `enter_em4_for_ms(uint32_t sleep_ms, const char *reason)` — static

- **What it does:** The EM4 entry sequence. Execution does not return from this function — the MCU wakes from EM4 as a full reset.
  1. Calls `wheatstone_prepare_for_em4()` to ensure bridge supply is off and pins are in low-leakage state.
  2. Configures `EMU_EM4Init_TypeDef` with `em4State = emuEM4Shutoff`. Sets `retainLfxo = true` and `retainLfrco = true` (confusing naming: on Series 2, these fields actually **disable** those oscillators on EM4 entry), and `retainUlfrco = false` (keep ULFRCO running — required for BURTC).
  3. Computes `wake_ticks` from `sleep_ms`, ULFRCO frequency (1000 Hz), and the chosen BURTC prescaler using 64-bit integer math to avoid overflow.
  4. Enables the BURTC bus clock, resets BURTC, re-initializes it with `em4comp = true` (wake on compare match), reads the current counter, sets compare to `current + wake_ticks`.
  5. Clears all BURTC interrupt flags (note: `BURTC_IntEnable(BURTC_IEN_COMP)` is commented out — the BURTC compare wakes via the EM4 wakeup path even without the CPU interrupt enabled, which is correct for EM4 Shutoff).
  6. Calls `EMU_EnterEM4()`. The MCU enters EM4 Shutoff. After `sleep_ms` milliseconds (approximately), BURTC compare fires and the MCU resets.
  7. The `while(1)` after `EMU_EnterEM4()` is a safety trap; it can never be reached.
- **When it runs:** From `app_process_action()`, after TX confirmation or watchdog timeout.
- **Depends on:** `wheatstone_prepare_for_em4()`, `BURTC_*` EMLIB functions, `EMU_EnterEM4()`, `sl_clock_manager_enable_bus_clock()`.

**Important EM4 naming quirk:** In `EMU_EM4Init_TypeDef` on EFR32 Series 2, the fields `retainLfxo`, `retainLfrco`, `retainUlfrco` are named "retain" but mean **"disable upon EM4 entry"** when set to `true`. So `retainLfxo = true` turns LFXO off in EM4. The code comment explains this correctly.

---

#### `ensure_reporting_binding_to_coordinator()` — static

- **What it does:** Scans the binding table for an existing entry matching endpoint 1, `ZCL_ELECTRICAL_MEASUREMENT_CLUSTER_ID`, and the coordinator's EUI64. If found, returns `true`. If not found, creates (or overwrites a stale) binding entry. Returns `false` if the coordinator EUI64 is not yet known or the table is full.
- **When it runs:** Once per boot, from `app_process_action()`, immediately after the network comes up.
- **Why it exists:** The primary send path uses direct NWK unicast to 0x0000 (coordinator). The binding entry is a **fallback** — if the direct send fails, `send_adc_report_to_zha()` falls back to `sl_zigbee_af_send_command_unicast_to_bindings_with_cb()`. Without a valid binding, that fallback is a no-op.
- **State it changes:** Writes the Zigbee binding table in NVM via `sl_zigbee_set_binding()`. Sets `binding_bootstrap_done = true` in the caller when it returns `true`.

---

#### `publish_retry_timer_callback(handle, data)` — static, sleeptimer callback

- **What it does:** Sets `publish_retry_due = true`.
- **When it runs:** 1000 ms after `schedule_publish_retry()` is called, from inside `sl_main_process_action()` (sleeptimer fires in the SDK tick).
- **Why it exists:** Decouples the retry scheduling (can happen inside a callback) from the actual send attempt (must happen in the main loop).

---

#### `schedule_publish_retry()` — static

- **What it does:** Clears `publish_retry_due`, stops any running retry timer, then starts a new 1-second one-shot sleeptimer that will set `publish_retry_due = true` when it fires.
- **When it runs:** When a send fails (either `send_adc_report_to_zha()` returns `false`, or the TX callback returns a non-OK status).
- **Fallback:** If the sleeptimer fails to start, sets `publish_retry_due = true` immediately so the next main loop iteration retries right away.

---

#### `report_tx_complete_cb(type, indexOrDestination, apsFrame, msgLen, message, status)` — static, APS TX callback

- **What it does:** Called by the Zigbee stack after the APS layer has finished sending the report command (either ACKed by the coordinator, or timed out/failed). Checks `tx_confirm_pending` first to guard against spurious late callbacks. Sets `tx_confirm_sl_status = status` and `tx_confirm_received = true`.
- **When it runs:** Asynchronously, inside `sl_main_process_action()`, sometime after `sl_zigbee_af_send_command_unicast_with_cb()` is called.
- **Why it exists:** The App Framework's `_with_cb` send variants require the callback to be a named function pointer. This callback bridges from the async APS stack layer back into the main-loop state machine via the `tx_confirm_received` flag.
- **State it changes:** `tx_confirm_sl_status`, `tx_confirm_received`.

---

#### `send_adc_report_to_zha(uint16_t adc_value)` — static

- **What it does:** Builds and enqueues a ZCL "Report Attributes" message for the `ZCL_ELECTRICAL_MEASUREMENT_CLUSTER_ID` cluster:
  1. Writes `adc_value` into the ZCL attribute cache via `sl_zigbee_af_write_server_attribute()`.
  2. Manually builds the 5-byte attribute record (attribute ID little-endian + type byte + value little-endian).
  3. Uses the App Framework command builder (`sl_zigbee_af_fill_command_global_server_to_client_report_attributes`) to construct the ZCL frame.
  4. Strips APS retry, forced route discovery, and address discovery options from the APS frame — this keeps the TX callback fast and avoids multi-second retry loops that would delay EM4 entry.
  5. Arms `tx_confirm_pending`, records the start tick, then calls `sl_zigbee_af_send_command_unicast_with_cb()` targeting NWK 0x0000 (coordinator) directly.
  6. If the direct send fails, falls back to `sl_zigbee_af_send_command_unicast_to_bindings_with_cb()`.
  7. Returns `true` if the message was queued successfully.
- **When it runs:** From `app_process_action()`, when the network is up and `publish_retry_due` is true.
- **State it changes:** `tx_confirm_pending = true`, `tx_confirm_received = false`, `tx_start_tick`, `tx_timing_active`.

---

#### `app_init()` — public

- **What it does:** Calls `wheatstone_init()`, `zigbee_init()`, and `arm_no_progress_em4_watchdog()`.
- **When it runs:** Once, from `main()`, before the superloop starts.
- **Note:** `publish_retry_due` is initialized to `true` at declaration, so the first loop iteration will immediately attempt a read + send (assuming network is already up). If the network is not yet up, the state machine skips the send.

---

#### `app_process_action()` — public

The main state machine, called every superloop iteration. The six-step priority order is:

```
Priority 1: EM4 entry requested?
  → Yes: is zigbee_ok_to_sleep()?
    → Yes: enter EM4 (reboot after sleep)
    → No: return (keep spinning)

Priority 2: TX callback arrived?
  → Yes + status OK: request EM4 for 10s ("publish")
  → Yes + status fail: schedule 1s retry
  → Either way: return

Priority 3: No-progress watchdog expired?
  → Yes: request EM4 for 5s ("watchdog"), return

Priority 4: TX still pending?
  → Yes: return (wait for callback)

Priority 5: Network not up?
  → Yes: return (wait for zigbee.c to bring network up)

Priority 6: publish_retry_due?
  → No: return
  → Yes: read ADC, clamp, encode, send ZCL report
         → send OK: wait for TX callback
         → send fail: schedule_publish_retry()
```

- **When it runs:** Every superloop iteration after `sl_main_process_action()`.
- **State it changes:** Most application state variables.

---

#### `arm_no_progress_em4_watchdog()` — static

- **What it does:** Sets `no_progress_em4_deadline_tick` to now + 60 seconds (in sleeptimer ticks) and clears `watchdog_em4_requested`.
- **When it runs:** Once from `app_init()`.
- **Why it exists:** Safety net. If the device gets stuck — e.g., never joins, TX callback never arrives — it will force a short EM4 cycle (effectively a reboot) after 60 seconds of no progress. This prevents permanent deadlock.

---

#### `ms_to_sleeptimer_ticks(uint32_t ms)` — static helper

- **What it does:** Converts milliseconds to sleeptimer ticks, clamping to 16-bit max before calling `sl_sleeptimer_ms_to_tick()`. This is a local copy of the same helper pattern used in `zigbee.c`.
- **Why it exists:** `sl_sleeptimer_ms_to_tick()` takes a `uint16_t`, so inputs larger than 65535 ms must be clamped. For the watchdog at 60000 ms this is fine; 65535 ms ≈ 65.5 s.

---

### `zigbee.h` — Zigbee helper interface

**Role:** Declares four functions. All application state and Zigbee join logic is encapsulated in `zigbee.c`.

```c
void zigbee_init(void);
void zigbee_process_action(void);
bool zigbee_is_network_up(void);
bool zigbee_ok_to_sleep(void);
```

---

### `zigbee.c` — Network join/rejoin and sleep gating

**Role:** A self-contained Zigbee bring-up helper. It owns all network state and makes two decisions visible to the application: "is the network ready for traffic?" and "is it safe to enter EM4?".

#### Key module-level state

| Variable | Purpose |
|---|---|
| `retry_timer` | Sleeptimer handle for retrying network steering after failure. |
| `startup_timer` | Sleeptimer handle for detecting stored-network restore/rejoin timeout. |
| `module_initialized` | Guards against callbacks firing before `zigbee_init()` runs. |
| `steering_active` | True while Network Steering plugin is running a scan. |
| `retry_pending` | Set by `retry_timer_callback()`, consumed in `zigbee_process_action()`. |
| `startup_timeout_pending` | Set by `startup_timer_callback()`, triggers rejoin or leave+steer. |
| `network_up` | Internal latch: set when NETWORK_UP or detected by polling. |
| `security_ready` | True when `sl_zigbee_get_current_security_state()` returns OK. |
| `had_stored_network` | True if NVM tokens had a valid network on boot. |
| `steering_after_leave_pending` | True after `sl_zigbee_leave_network()` is called — triggers steering once leave completes. |
| `stored_network_rejoin_attempted` | Prevents double-rejoin attempts per boot. |
| `fresh_join_expected` | Signals that the next NETWORK_UP is a fresh join and the interview poll window should start. |
| `interview_poll_active` | True while aggressively polling parent for queued ZHA frames. |
| `interview_poll_deadline_tick` | Absolute tick when the 10-second interview poll window expires. |
| `interview_next_poll_tick` | Absolute tick for the next 250 ms parent poll. |
| `ok_to_long_poll_block_start_tick` | Timestamp when `sl_zigbee_ok_to_long_poll()` first returned false; used for the 15-second override. |

---

#### `ms_to_ticks(uint32_t ms)` — static helper

Same purpose as the copy in `app.c`: converts ms to sleeptimer ticks with 16-bit clamping.

---

#### `maybe_start_interview_poll_window(const char *context)` — static

- **What it does:** If `fresh_join_expected` is true, arms a 10-second fast-poll window: sets `interview_poll_active = true`, records the deadline tick, and if the join was truly fresh (not a stored-network restore) calls `sl_zigbee_af_force_end_device_to_stay_awake(true)` to prevent the SED from sleeping between main loop iterations.
- **When it runs:** From `handle_network_up()`, immediately after the network comes up.
- **Why it exists:** ZHA enqueues device interview (descriptor read, attribute discovery, etc.) frames at the parent router. A Sleepy End Device normally only polls at long intervals, so those frames sit in the parent's queue. By forcing fast polling (every 250 ms) for 10 seconds, the firmware drains the interview queue before entering EM4, which allows ZHA to complete the interview and mark the device as "available."

---

#### `reset_interview_poll_window()` — static

- **What it does:** Clears `interview_poll_active` and all poll deadline/interval ticks. Calls `sl_zigbee_af_force_end_device_to_stay_awake(false)` to restore normal SED sleep behavior.
- **When it runs:** When the 10-second window expires (in `zigbee_process_action()`), or when a NETWORK_DOWN/NOT_JOINED event fires, or in `leave_network_and_steer()`.

---

#### `update_security_ready(const char *context)` — static

- **What it does:** Calls `sl_zigbee_get_current_security_state()`. If it returns `SL_STATUS_OK`, the device has an active security context (network key installed), so sets `security_ready = true`. Otherwise sets `security_ready = false`.
- **When it runs:** In `handle_network_up()` and periodically in `zigbee_process_action()`.
- **Why it exists:** The network may be "up" (associated with a parent) before the security context is fully established. Gating application traffic on `security_ready` prevents sending before the network key is ready.

---

#### `has_valid_pan_and_node()` — static

- **What it does:** Reads the current PAN ID and Node ID. Returns `true` only if neither is `0xFFFF` (invalid/unset) and the node ID is not `0x0000` (which would mean the device thinks it's a coordinator).
- **When it runs:** Throughout `zigbee_init()`, `zigbee_process_action()`, and `zigbee_is_network_up()` as a sanity check.

---

#### `retry_timer_callback(handle, data)` — static, sleeptimer callback

- **What it does:** Sets `retry_pending = true`.
- **When it runs:** 10 seconds after `arm_retry_timer()`.
- **Why it exists:** Decouples the timer interrupt from the steering restart so steering is always restarted in the main-loop context, not in an ISR/callback context.

---

#### `startup_timer_callback(handle, data)` — static, sleeptimer callback

- **What it does:** Sets `startup_timeout_pending = true`.
- **When it runs:** 8 seconds after `arm_startup_timeout_ms()` is called.
- **Why it exists:** Same decoupling pattern. Signals the main loop to take action on a stored-network that never came up.

---

#### `start_network_steering(const char *reason)` — static

- **What it does:** Calls `sl_zigbee_af_network_steering_start()` to begin a Zigbee channel scan and join attempt. Sets `steering_active = true` on success. On failure (e.g., stack not ready), calls `arm_retry_timer()` to retry in 10 seconds.
- **When it runs:** From `zigbee_init()` (no stored network), from `sl_zigbee_af_stack_status_cb()` (on NETWORK_DOWN or NOT_JOINED), and from `zigbee_process_action()` (retry timer, post-leave).
- **Guard:** Does nothing if `module_initialized` is false, `steering_active` is true, or `network_up` is true.

---

#### `stop_network_steering_if_active()` — static

- **What it does:** Calls `sl_zigbee_af_network_steering_stop()` and clears `steering_active`.
- **When it runs:** In `handle_network_up()` when the network comes up while steering was still running.

---

#### `arm_retry_timer()` — static

- **What it does:** Stops any existing retry timer, then starts a new 10-second one-shot sleeptimer that will set `retry_pending = true`.
- **When it runs:** After a steering failure, or if `start_network_steering()` fails immediately.

---

#### `arm_startup_timeout_ms(uint32_t timeout_ms)` — static

- **What it does:** Clears `startup_timeout_pending`, stops any existing startup timer, then starts a new one-shot sleeptimer for `timeout_ms` ms.
- **When it runs:** From `zigbee_init()` when a stored network is detected (8-second restore timeout), and from `zigbee_process_action()` after a rejoin attempt (another 8-second window).

---

#### `handle_network_up(const char *context)` — static

- **What it does:** Single entry point that transitions the module to the "network up" state:
  1. Guards against re-entry (`if (network_up) return`).
  2. Sets `network_up = true`.
  3. Calls `stop_network_steering_if_active()`.
  4. Clears `retry_pending` and `startup_timeout_pending`.
  5. Stops both timers.
  6. Calls `update_security_ready()`.
  7. Calls `maybe_start_interview_poll_window()`.
- **When it runs:** From `sl_zigbee_af_stack_status_cb()` on `SL_STATUS_NETWORK_UP`, and from `zigbee_process_action()` as a polling fallback.

---

#### `leave_network_and_steer(const char *reason)` — static

- **What it does:** Calls `sl_zigbee_leave_network(SL_ZIGBEE_LEAVE_NWK_WITH_NO_OPTION)` to broadcast a Leave Request and clear NVM tokens. On success, sets `steering_after_leave_pending = true` so that when the subsequent `SL_STATUS_NETWORK_DOWN` or `SL_STATUS_NOT_JOINED` callback arrives, steering is started immediately. On immediate failure, starts steering directly.
- **When it runs:** From `zigbee_process_action()` when stored-network rejoin fails (Stage 1 fallback).
- **State it changes:** Resets `had_stored_network`, `stored_network_rejoin_attempted`, `network_up`, `security_ready`, sets `fresh_join_expected = true`, resets interview poll window.

---

#### `zigbee_init()` — public

- **What it does:** Initializes all module state to clean defaults. Checks the stack's `sl_zigbee_network_state()`:
  - **If a valid stored network exists** (`JOINED_NETWORK` or `JOINED_NETWORK_NO_PARENT` + valid PAN/node): sets `had_stored_network = true`, arms an 8-second startup timeout, and **returns without starting steering**. The stack is expected to bring the network up on its own (it tries to associate with the stored parent); the startup timer detects if that never happens.
  - **If no stored network**: sets `fresh_join_expected = true`, starts Network Steering immediately.
- **When it runs:** Once, from `app_init()`.

---

#### `zigbee_process_action()` — public

Called every superloop iteration. Handles four distinct concerns in order:

1. **Interview fast-poll window:** If `interview_poll_active`, checks if the deadline has passed (stop window) or if it's time for the next 250 ms parent poll (`sl_zigbee_poll_for_data()`).

2. **Post-leave steering:** If `steering_after_leave_pending` and the stack has reached `SL_ZIGBEE_NO_NETWORK`, starts steering.

3. **Fallback network-up detection:** If the stack is up and joined but `network_up` is still false (callback was missed), calls `handle_network_up("poll")`.

4. **Stored-network timeout handling:**
   - **Stage 0 — Secure rejoin:** If `startup_timeout_pending` and no rejoin has been tried yet, calls `sl_zigbee_find_and_rejoin_network(true, ALL_CHANNELS, ...)`. Arms another 8-second window. If the rejoin API itself fails, falls through to Stage 1.
   - **Stage 1 — Leave + steer:** If the stored network never came up even after a rejoin attempt, calls `leave_network_and_steer("startup fallback")` to wipe NVM and join a new network from scratch.

5. **Retry timer:** If `retry_pending` is true (and not already steering or up), calls `start_network_steering("retry timer")`.

---

#### `zigbee_is_network_up()` — public

- **What it does:** Returns `true` only when ALL of the following are true: internal `network_up` flag, `security_ready`, `has_valid_pan_and_node()`, `sl_zigbee_stack_is_up()`, and the stack reports `SL_ZIGBEE_JOINED_NETWORK`.
- **Why the multi-check:** Belt-and-suspenders. The internal `network_up` flag is set by callbacks; the external stack queries detect race conditions where the stack state diverges before the callback fires.

---

#### `zigbee_ok_to_sleep()` — public

- **What it does:** Returns `false` (block EM4 entry) in two cases:
  1. `interview_poll_active` is true — must drain the ZHA interview queue first.
  2. `sl_zigbee_ok_to_long_poll()` returns false — the stack has pending inbound traffic. However, if this condition persists for more than **15 seconds**, it is overridden and `true` is returned. This prevents the device from being permanently blocked from sleeping by a chatty coordinator.
- **Returns `true`:** When neither blocking condition applies, allowing `app_process_action()` to call `enter_em4_for_ms()`.

---

#### `sl_zigbee_af_stack_status_cb(sl_status_t status)` — public, Zigbee stack callback

- **What it does:** Reacts to Zigbee stack state transitions:
  - `SL_STATUS_NETWORK_UP` → `handle_network_up("stack status")`
  - `SL_STATUS_NETWORK_DOWN` → resets `network_up`, `security_ready`, `steering_active`; stops startup timer; resets interview window; forces stay-awake off. If `steering_after_leave_pending`, starts steering (leave just completed); otherwise starts steering to recover ("network down").
  - `SL_STATUS_NOT_JOINED` → same as `NETWORK_DOWN` but triggered by a different path (e.g., parent timeout).
- **When it runs:** Called by the App Framework from inside `sl_main_process_action()` whenever the stack state machine changes.

---

#### `sl_zigbee_af_network_steering_complete_cb(status, totalBeacons, joinAttempts, finalState)` — public, App Framework callback

- **What it does:** Called when the Network Steering plugin finishes (either successfully found and joined a network, or exhausted all retries). Clears `steering_active`. On failure (non-OK status), arms the 10-second retry timer.
- **Note:** On success, the code does nothing explicitly — it waits for the `SL_STATUS_NETWORK_UP` callback to confirm the actual join. This is correct because steering completion ≠ network up (security setup still happens after steering reports success).

---

### `wheatstone.h` — Bridge hardware interface

**Role:** Declares three functions. All hardware state is internal to `wheatstone.c`.

```c
void wheatstone_init(void);
void wheatstone_prepare_for_em4(void);
int32_t wheatstone_read(void);
```

---

### `wheatstone.c` — Bridge hardware abstraction

**Role:** Owns all GPIO and IADC configuration for the Wheatstone bridge measurement.

**Pin assignments:**
| Signal | Port/Pin | Direction |
|---|---|---|
| Bridge supply (excitation) | PC0 | Output push-pull, driven high during measurement |
| IADC positive input | PC1 | Analog (disabled digital mode) |
| IADC negative input | PC2 | Analog (disabled digital mode) |

---

#### `wheatstone_bridge_supply_set_enabled(bool enabled)` — static

- **What it does:** Sets PC0 high (bridge powered) or low (bridge off) via `GPIO_PinOutSet` / `GPIO_PinOutClear`.
- **When it runs:** From `wheatstone_read()` (on + off) and `wheatstone_prepare_for_em4()` (force off).

---

#### `wheatstone_iadc_enable()` — static

- **What it does:** Full IADC startup sequence:
  1. Enables `cmuClock_IADC0` and `cmuClock_FSRCO` (20 MHz free-running RC oscillator).
  2. Selects FSRCO as the IADC clock source.
  3. Computes `srcClkPrescale` to hit the target clock rate.
  4. Configures the analog config: **1.2 V internal reference** (the commented-out `4x gain` line would improve resolution for very small signals, but is currently disabled), **ADC clock target 10 MHz**, **32x oversampling (OSR)**, **16x digital averaging**.
  5. Configures differential single-shot input: PC1 positive, PC2 negative.
  6. Calls `IADC_init()` and `IADC_initSingle()`.
- **Effective resolution enhancement:** 32x OSR + 16x digital average provides a total effective oversampling of 512x, improving SNR by approximately log2(512)/2 ≈ 4.5 bits above the base 12-bit ADC. This is significant for a low-signal-level Wheatstone bridge differential measurement.
- **Note:** The gain 4x analog option is commented out. If re-enabled, it would shift the full-scale range from ±1.2 V to ±0.3 V differential, matching a lower-output-voltage bridge while improving noise floor.

---

#### `wheatstone_iadc_disable()` — static

- **What it does:** Calls `IADC_reset(IADC0)` to clear all IADC registers and state, then disables the IADC0 and FSRCO clocks.
- **When it runs:** After each measurement, from `wheatstone_read()`.
- **Why it exists:** Reduces active current between measurements. The IADC and FSRCO are not needed outside of a measurement window.

---

#### `wheatstone_init()` — public

- **What it does:**
  1. Enables GPIO clock (`cmuClock_GPIO`).
  2. Drives PC0 low and configures it as push-pull output (bridge supply off by default).
  3. Configures PC1 and PC2 as `gpioModeDisabled` (disables digital input buffer for minimum leakage — correct for analog inputs).
  4. Routes PC1 and PC2 to IADC0 via `GPIO->CDBUSALLOC`: sets `CDODD0 = ADC0` (PC1) and `CDEVEN1 = ADC0` (PC2), and explicitly sets `CDEVEN0 = TRISTATE` to un-route PC0 (keeping it as a GPIO output).
- **When it runs:** Once, from `app_init()`.

---

#### `wheatstone_prepare_for_em4()` — public

- **What it does:** Forces PC0 low (bridge off) and re-applies the same GPIO modes as `wheatstone_init()` for all three pins. This is a defensive measure before EM4 entry.
- **When it runs:** From `enter_em4_for_ms()`, before the EM4 entry sequence.
- **Why it exists:** On Series 2 EFR32, EM4 Shutoff with **no pin retention** leaves GPIO state undefined during EM4 (pins tristate, or retain last driven state depending on silicon configuration). By explicitly driving PC0 low and setting PC1/PC2 to disabled before EM4, the bridge supply is guaranteed off during sleep, preventing unnecessary power draw through the bridge resistors.

---

#### `wheatstone_read()` — public

- **What it does:** Single-shot blocking measurement:
  1. Powers the bridge (PC0 high).
  2. Calls `wheatstone_iadc_enable()` — clocks on, IADC configured.
  3. Sends `iadcCmdStartSingle` to trigger one differential conversion.
  4. **Busy-waits** (polls) `IADC_STATUS_SINGLEFIFODV` until the FIFO has a result.
  5. Pulls the result with `IADC_pullSingleFifoResult()`.
  6. Calls `wheatstone_iadc_disable()` — clocks off, IADC reset.
  7. Powers off the bridge (PC0 low).
  8. Returns `(int32_t)result.data`.
- **When it runs:** Once per boot, from `app_process_action()` when the network is up and `publish_retry_due` is true.
- **Result format:** Signed 32-bit. On a 12-bit IADC with OSR/averaging, the effective output is typically in a wider range. The caller in `app_process_action()` clamps to `[-32767..32767]` and maps to an unsigned 16-bit attribute as `adc_attr = adc_raw + 32767`.

---

### Config files (relevant subset)

| File | Key settings |
|---|---|
| `zigbee_device_config.h` | Device type: **Sleepy End Device**, Security: **ZigBee 3.0** |
| `zigbee_sleep_config.h` | Min sleep: 5 ms, `STAY_AWAKE_WHEN_NOT_JOINED = 1` (device stays awake during join scan) |
| `sl_zigbee_pro_leaf_stack_config.h` | Poll timeout: 256 min, binding table: 3 entries, neighbor table: 1, route table: 0 (pure end device, no routing), packet buffer heap: 4096 bytes |
| `network-steering-config.h` | Channel mask: `0x02108800` (channels 11, 15, 20, 25), TX power: 3 dBm, scan duration exponent: 4 (~310 ms/channel), auto-start enabled |
| `sl_sleeptimer_config.h` | Sleeptimer peripheral: **RTCC** (separate from BURTC which is used for EM4 wake) |
| `sl_power_manager_config.h` | EM4 pin retention: **DISABLE** (no GPIO retention through EM4), EM2 debug: off, DVDD ramp: enabled |
| `sl_clock_manager_oscillator_config.h` | HFXO: 38.4 MHz crystal, CTUNE=121; LFXO: **disabled** (no 32.768 kHz crystal on this PCB); HFRCO: 80 MHz |
| `reporting-config.h` | Reporting table: 5 entries |

---

## Full Program Flow — Step by Step

### Phase 0: Boot / Reset (from EM4 or cold power-on)

1. **Reset vector** → `main()`.
2. `sl_main_init()` runs the SDK component init chain:
   - Enables clocks (HFXO 38.4 MHz, HFRCO 80 MHz).
   - Initializes RTCC (used by sleeptimer).
   - Initializes Power Manager (sets EM4 pin retention to DISABLE).
   - Initializes NVM3 / token manager (loads stored network credentials).
   - Initializes the Zigbee stack (registers endpoints, ZCL clusters, etc.).
   - Note: BURTC is **not** initialized here — it is initialized only in `enter_em4_for_ms()`.
3. `app_init()`:
   - `wheatstone_init()`: configures PC0 push-pull output (low), PC1/PC2 analog disabled, routes PC1/PC2 to IADC.
   - `zigbee_init()`: checks NVM for stored network credentials.
     - **If stored network found:** sets `had_stored_network = true`, arms 8s startup timer. Stack will try to restore the network automatically.
     - **If no stored network:** sets `fresh_join_expected = true`, calls `sl_zigbee_af_network_steering_start()`.
   - `arm_no_progress_em4_watchdog()`: sets 60-second deadline.

### Phase 1: Network Bring-Up

The superloop starts:
- `sl_main_process_action()` drives the Zigbee stack (radio scanning, beacon processing, association, authentication, key exchange).
- `app_process_action()` → `zigbee_process_action()` polls for progress.

**Scenario A — No stored network (first boot or after leave):**
- Network Steering scans channels 11, 15, 20, 25 for ZHA networks with open permit-join.
- On success, the App Framework calls `sl_zigbee_af_network_steering_complete_cb()` → sets `steering_active = false`.
- Stack calls `sl_zigbee_af_stack_status_cb(SL_STATUS_NETWORK_UP)` → `handle_network_up()`:
  - `network_up = true`, stops timers, calls `update_security_ready()`, calls `maybe_start_interview_poll_window()` → `interview_poll_active = true`, `sl_zigbee_af_force_end_device_to_stay_awake(true)`.
- For 10 seconds, `zigbee_process_action()` polls parent every 250 ms via `sl_zigbee_poll_for_data()` to drain ZHA interview frames.
- After 10 seconds, `reset_interview_poll_window()` → `interview_poll_active = false`, `sl_zigbee_af_force_end_device_to_stay_awake(false)`.
- `zigbee_ok_to_sleep()` now returns `true` (once `sl_zigbee_ok_to_long_poll()` is also true).

**Scenario B — Stored network:**
- Stack attempts to associate with the stored parent on the stored channel.
- If successful within 8 seconds: `SL_STATUS_NETWORK_UP` callback → same path as Scenario A (but `maybe_start_interview_poll_window()` does not force stay-awake because `had_stored_network = true`).
- If not successful within 8 seconds: `startup_timer_callback()` fires, `startup_timeout_pending = true`. In `zigbee_process_action()`:
  - Stage 0: tries `sl_zigbee_find_and_rejoin_network()` (secure rejoin). Arms another 8s timer.
  - If rejoin also fails after 8s: Stage 1: `leave_network_and_steer()` — wipes NVM, broadcasts Leave, then starts Network Steering for a fresh join.

### Phase 2: ADC Read and ZCL Report

Once `zigbee_is_network_up()` returns true and `publish_retry_due` is true:

1. `app_process_action()` (Priority 5 passes, Priority 6 reached):
2. `ensure_reporting_binding_to_coordinator()` — looks up coordinator EUI64, creates binding if needed. Sets `binding_bootstrap_done = true`. (Only on first iteration per boot.)
3. `wheatstone_read()`:
   - PC0 → high (bridge energized).
   - IADC enabled: FSRCO clock, 1.2V internal reference, 32x OSR, 16x averaging, differential PC1−PC2.
   - Single conversion triggered, busy-wait poll until FIFO has data.
   - Result pulled, IADC disabled, PC0 → low (bridge off).
   - Returns signed 32-bit raw IADC result.
4. Clamp raw result to `[-32767..32767]`.
5. Encode: `adc_attr = (uint16_t)(adc_raw + 32767)` → maps to `[0..65534]`. HA decodes with `adc_raw = (int32_t)adc_attr - 32767`.
6. `send_adc_report_to_zha(adc_attr)`:
   - Writes attribute to ZCL cache.
   - Builds 5-byte attribute record.
   - Strips APS retry/route discovery options (for fast callback).
   - Arms `tx_confirm_pending = true`.
   - Sends unicast to NWK 0x0000. On failure: binding fallback. On both failures: returns `false` → `schedule_publish_retry()`.

### Phase 3: TX Callback and EM4

Sometime later (within a few hundred milliseconds normally), inside `sl_main_process_action()`:
- The APS layer fires `report_tx_complete_cb()`.
- Sets `tx_confirm_received = true`, `tx_confirm_sl_status = status`.

Next `app_process_action()` iteration (Priority 2):
- **TX OK:** `em4_sleep_ms_requested = 10000`, `em4_reason = "publish"`, `em4_entry_requested = true`.
- **TX failed:** `schedule_publish_retry()` → 1-second retry, try again.

On EM4 entry (Priority 1, when `zigbee_ok_to_sleep()` returns `true`):
- `enter_em4_for_ms(10000, "publish")`:
  - `wheatstone_prepare_for_em4()` → PC0 low, pins safe.
  - BURTC configured: ULFRCO (~1 kHz), prescaler 1 (for 10s sleep), compare at current + 10000 ticks.
  - `EMU_EnterEM4()` — MCU enters EM4 Shutoff. SRAM lost. Radio off.
- After ~10 seconds: BURTC compare fires → MCU reset → back to Phase 0.

### Watchdog path

If 60 seconds pass without a TX completing (e.g., stuck in join loop, TX never queued):
- `no_progress_em4_deadline_tick` expires in `app_process_action()` (Priority 3).
- `em4_sleep_ms_requested = 5000`, `em4_reason = "watchdog"`, `em4_entry_requested = true`.
- Same EM4 entry sequence, but only 5 seconds before reboot-and-retry.

---

## Important Design Choices, Risks, and Low-Power Behaviors

### Design Choices

1. **EM4 Shutoff as the primary sleep mode.** EM4 Shutoff is a hardware reset — RAM is lost, all peripherals are off. The firmware is designed around this: every boot is a fresh execution, and all persistent state is in NVM (Zigbee network tokens) or is re-derived on boot (ADC reading). This is a very power-efficient approach for a device that only needs to report every 10 seconds.

2. **Signed-to-unsigned ZCL encoding.** The raw IADC result is signed (differential ADC), but the ZCL attribute type used is `INT16U` (unsigned 16-bit). The offset-by-32767 encoding preserves the sign across the unsigned ZCL channel. The HA integration must decode with `adc_raw = adc_attr - 32767`. If this decode is not configured on the HA side, readings will appear as a large positive number instead of a small signed one.

3. **Direct NWK 0x0000 send with binding fallback.** Avoids the overhead of route discovery (ZHA coordinator is always at NWK 0x0000). The binding table is maintained purely as a fallback; the primary path is always the direct unicast.

4. **APS retry disabled.** `SL_ZIGBEE_APS_OPTION_RETRY` is stripped from the APS frame options. This means the stack will not retry the APS PDU on MAC ACK failure. The application-level retry (1-second schedule) handles failures instead. This keeps the TX callback latency short and predictable.

5. **IADC is fully power-gated between measurements.** Both the IADC0 clock and FSRCO are disabled after each reading. The bridge excitation supply (PC0) is also driven only during measurement. This eliminates any standby current from those circuits.

6. **Adaptive BURTC prescaler.** Prepared for future extension to longer sleep periods. The current `EM4_SLEEP_AFTER_PUBLISH_MS = 10000` always lands in the "≤1 min → prescaler 1" bucket, but changing that constant to hours or days would automatically select a more current-efficient prescaler.

7. **RTCC for sleeptimer, BURTC independently for EM4 wake.** These are two separate peripherals. The RTCC (sleeptimer) cannot wake from EM4 Shutoff because EM4 Shutoff cuts power to most domains. BURTC runs from EM4GRPACLK (ULFRCO) which survives EM4 Shutoff. This is why the code explicitly enables the BURTC bus clock in `enter_em4_for_ms()` even though it is not the sleeptimer backend.

### Risks and Potential Issues

1. **Busy-wait ADC conversion.** `wheatstone_read()` spins on the `SINGLEFIFODV` flag. With 32x OSR, 16x averaging, and a 10 MHz ADC clock, the conversion takes roughly `(32 × 16 × conversion_cycles) / 10 MHz` ≈ several milliseconds. This is not a problem for throughput (only one measurement per 10-second EM4 cycle), but it is not power-optimal if the MCU could drop to EM1 during the conversion. The current design keeps the MCU in EM0 for the entire ADC measurement.
- User answer: This is okay, this is how I want it.

2. **No pin retention through EM4.** `SL_POWER_MANAGER_INIT_EMU_EM4_PIN_RETENTION_MODE = DISABLE`. This means all GPIOs tristate during EM4 (no retained drive state). The bridge supply PC0 must have either a pulldown on the PCB or be guaranteed low by the reset-state GPIO mode. `wheatstone_prepare_for_em4()` explicitly drives PC0 low before EM4 entry, which mitigates this, but if there is a hardware fault or a rapid EM4 entry path not routed through `enter_em4_for_ms()`, PC0 could float.
- User answer: This is not a problem, nothing should be fixed here.

3. **`STAY_AWAKE_WHEN_NOT_JOINED = 1`** means the device draws active current (SED stays awake) during the entire network join/steering phase. For a battery-powered device, a long scan (many channels, slow to find HA network) can drain the battery. Channel mask `0x02108800` limits scanning to only 4 channels, which reduces this risk.
- User answer: This is not a problem, nothing should be fixed here.

4. **60-second watchdog is not re-armed after the first successful TX.** `arm_no_progress_em4_watchdog()` is only called once in `app_init()`. Once the watchdog fires and triggers a 5-second EM4 cycle, the next boot will call `app_init()` again and re-arm it. This is correct for the EM4-reset pattern, but means the watchdog provides a maximum 60-second "first TX" deadline, not a per-cycle deadline.
- User answer: this is correct beheviour.

5. **ZHA interview poll window does not apply to stored-network restores.** `maybe_start_interview_poll_window()` only forces stay-awake when `had_stored_network = false`. On a stored-network restore, interview traffic may still be pending, but the device reverts to SED polling behavior immediately. For a normally-joined device this is fine (ZHA already completed its interview on first join), but if the coordinator is reset and loses device state, a rejoin may require a new interview that the device will not fast-poll for.
- User answer: this is correct beheviour, nothing should be fixed here.

6. **ULFRCO accuracy.** The ULFRCO on EFR32 Series 2 is a low-power RC oscillator with ±5% accuracy at room temperature and wider spread over temperature. For a 10-second EM4 sleep, this means ±0.5 seconds of wake-up jitter — which is acceptable. For longer sleeps (if `EM4_SLEEP_AFTER_PUBLISH_MS` is increased), the timing error scales proportionally.
- User answer: This is okay beheviour, since it is not critical that it wakes up at correctly the right time

7. **The `retainLfxo`/`retainLfrco` naming confusion** in `EMU_EM4Init_TypeDef`. The code comments correctly explain that these fields mean "disable on EM4 entry" despite the "retain" naming. This is a Silicon Labs SDK naming quirk that could confuse anyone maintaining this code without the comment.
- User answer: The comments should maybe be updated here to better explain what is happening and why

8. **Unclear:** It is not obvious from the source code what interval HA/ZHA expects between attribute reports. The firmware hardcodes 10 seconds of EM4 sleep after each successful TX. If ZHA's reporting configuration expects a different interval, there may be "stale value" warnings on the HA side. The ZCL reporting plugin (`reporting-config.h`, table size 5) is included in the project but the firmware sends reports manually rather than using the ZCL reporting engine — the reporting plugin table entries are not configured anywhere visible in this code.
- User answer: This is not a problem, HA handles this fine
