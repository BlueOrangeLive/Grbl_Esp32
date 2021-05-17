/*
  Protocol.cpp - controls Grbl execution protocol and procedures
  Part of Grbl

  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud

    2018 -	Bart Dring This file was modifed for use on the ESP32
                    CPU. Do not use this with Grbl for atMega328P

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Grbl.h"

static void protocol_exec_rt_suspend();

static char    line[LINE_BUFFER_SIZE];     // Line to be executed. Zero-terminated.
static char    comment[LINE_BUFFER_SIZE];  // Line to be executed. Zero-terminated.
static uint8_t line_flags           = 0;
static uint8_t char_counter         = 0;
static uint8_t comment_char_counter = 0;

typedef struct {
    char buffer[LINE_BUFFER_SIZE];
    int  len;
    int  line_number;
} client_line_t;
client_line_t client_lines[CLIENT_COUNT];

static void empty_line(uint8_t client) {
    client_line_t* cl = &client_lines[client];
    cl->len           = 0;
    cl->buffer[0]     = '\0';
}
static void empty_lines() {
    for (uint8_t client = 0; client < CLIENT_COUNT; client++) {
        empty_line(client);
    }
}

Error add_char_to_line(char c, uint8_t client) {
    client_line_t* cl = &client_lines[client];
    // Simple editing for interactive input
    if (c == '\b') {
        // Backspace erases
        if (cl->len) {
            --cl->len;
            cl->buffer[cl->len] = '\0';
        }
        return Error::Ok;
    }
    if (cl->len == (LINE_BUFFER_SIZE - 1)) {
        return Error::Overflow;
    }
    if (c == '\r' || c == '\n') {
        cl->len = 0;
        cl->line_number++;
        return Error::Eol;
    }
    cl->buffer[cl->len++] = c;
    cl->buffer[cl->len]   = '\0';
    return Error::Ok;
}

Error execute_line(char* line, uint8_t client, WebUI::AuthenticationLevel auth_level) {
    Error result = Error::Ok;
    // Empty or comment line. For syncing purposes.
    if (line[0] == 0) {
        return Error::Ok;
    }
    // Grbl '$' or WebUI '[ESPxxx]' system command
    if (line[0] == '$' || line[0] == '[') {
        return system_execute_line(line, client, auth_level);
    }
    // Everything else is gcode. Block if in alarm or jog mode.
    if (sys.state == State::Alarm || sys.state == State::Jog) {
        return Error::SystemGcLock;
    }
    return gc_execute_line(line, client);
}

bool can_park() {
    return
#ifdef ENABLE_PARKING_OVERRIDE_CONTROL
        sys.override_ctrl == Override::ParkingMotion &&
#endif
        homing_enable->get() && !spindle->inLaserMode();
}

/*
  GRBL PRIMARY LOOP:
*/
void protocol_main_loop() {
    client_reset_read_buffer(CLIENT_ALL);
    empty_lines();
    //uint8_t client = CLIENT_SERIAL; // default client
    // Perform some machine checks to make sure everything is good to go.
#ifdef CHECK_LIMITS_AT_INIT
    if (hard_limits->get()) {
        if (limits_get_state()) {
            sys.state = State::Alarm;  // Ensure alarm state is active.
            report_feedback_message(Message::CheckLimits);
        }
    }
#endif
    // Check for and report alarm state after a reset, error, or an initial power up.
    // NOTE: Sleep mode disables the stepper drivers and position can't be guaranteed.
    // Re-initialize the sleep state as an ALARM mode to ensure user homes or acknowledges.
    if (sys.state == State::Alarm || sys.state == State::Sleep) {
        report_feedback_message(Message::AlarmLock);
        sys.state = State::Alarm;  // Ensure alarm state is set.
    } else {
        // Check if the safety door is open.
        sys.state = State::Idle;
        if (system_check_safety_door_ajar()) {
            rtSafetyDoor = true;
            protocol_execute_realtime();  // Enter safety door mode. Should return as IDLE state.
        }
        // All systems go!
        system_execute_startup(line);  // Execute startup script.
    }
    // ---------------------------------------------------------------------------------
    // Primary loop! Upon a system abort, this exits back to main() to reset the system.
    // This is also where Grbl idles while waiting for something to do.
    // ---------------------------------------------------------------------------------
    int c;
    for (;;) {
#ifdef ENABLE_SD_CARD
        if (SD_ready_next) {
            char fileLine[255];
            if (readFileLine(fileLine, 255)) {
                SD_ready_next = false;
                report_status_message(execute_line(fileLine, SD_client, SD_auth_level), SD_client);
            } else {
                char temp[50];
                sd_get_current_filename(temp);
                grbl_notifyf("SD print done", "%s print is successful", temp);
                closeFile();  // close file and clear SD ready/running flags
            }
        }
#endif
        // Receive one line of incoming serial data, as the data becomes available.
        // Filtering, if necessary, is done later in gc_execute_line(), so the
        // filtering is the same with serial and file input.
        uint8_t client = CLIENT_SERIAL;
        char*   line;
        for (client = 0; client < CLIENT_COUNT; client++) {
            while ((c = client_read(client)) != -1) {
                Error res = add_char_to_line(c, client);
                switch (res) {
                    case Error::Ok:
                        break;
                    case Error::Eol:
                        protocol_execute_realtime();  // Runtime command check point.
                        if (sys.abort) {
                            return;  // Bail to calling function upon system abort
                        }
                        line = client_lines[client].buffer;
#ifdef REPORT_ECHO_RAW_LINE_RECEIVED
                        report_echo_line_received(line, client);
#endif
                        // auth_level can be upgraded by supplying a password on the command line
                        report_status_message(execute_line(line, client, WebUI::AuthenticationLevel::LEVEL_GUEST), client);
                        empty_line(client);
                        break;
                    case Error::Overflow:
                        report_status_message(Error::Overflow, client);
                        empty_line(client);
                        break;
                    default:
                        break;
                }
            }  // while serial read
        }      // for clients
        // If there are no more characters in the serial read buffer to be processed and executed,
        // this indicates that g-code streaming has either filled the planner buffer or has
        // completed. In either case, auto-cycle start, if enabled, any queued moves.
        protocol_auto_cycle_start();
        protocol_execute_realtime();  // Runtime command check point.
        if (sys.abort) {
            return;  // Bail to main() program loop to reset system.
        }
        // check to see if we should disable the stepper drivers ... esp32 work around for disable in main loop.
        if (stepper_idle && stepper_idle_lock_time->get() != 0xff) {
            if (esp_timer_get_time() > stepper_idle_counter) {
                motors_set_disable(true);
            }
        }
    }
    return; /* Never reached */
}

// Block until all buffered steps are executed or in a cycle state. Works with feed hold
// during a synchronize call, if it should happen. Also, waits for clean cycle end.
void protocol_buffer_synchronize() {
    // If system is queued, ensure cycle resumes if the auto start flag is present.
    protocol_auto_cycle_start();
    do {
        protocol_execute_realtime();  // Check and execute run-time commands
        if (sys.abort) {
            return;  // Check for system abort
        }
    } while (plan_get_current_block() || (sys.state == State::Cycle));
}

// Auto-cycle start triggers when there is a motion ready to execute and if the main program is not
// actively parsing commands.
// NOTE: This function is called from the main loop, buffer sync, and mc_line() only and executes
// when one of these conditions exist respectively: There are no more blocks sent (i.e. streaming
// is finished, single commands), a command that needs to wait for the motions in the buffer to
// execute calls a buffer sync, or the planner buffer is full and ready to go.
void protocol_auto_cycle_start() {
    if (plan_get_current_block() != NULL) {  // Check if there are any blocks in the buffer.
        rtCycleStart = true;                 // If so, execute them!
    }
}

// This function is the general interface to Grbl's real-time command execution system. It is called
// from various check points in the main program, primarily where there may be a while loop waiting
// for a buffer to clear space or any point where the execution time from the last check point may
// be more than a fraction of a second. This is a way to execute realtime commands asynchronously
// (aka multitasking) with grbl's g-code parsing and planning functions. This function also serves
// as an interface for the interrupts to set the system realtime flags, where only the main program
// handles them, removing the need to define more computationally-expensive volatile variables. This
// also provides a controlled way to execute certain tasks without having two or more instances of
// the same task, such as the planner recalculating the buffer upon a feedhold or overrides.
void protocol_execute_realtime() {
    protocol_exec_rt_system();
    if (sys.suspend.value) {
        protocol_exec_rt_suspend();
    }
}

// Executes run-time commands, when required. This function primarily operates as Grbl's state
// machine and controls the various real-time features Grbl has to offer.
// NOTE: Do not alter this unless you know exactly what you are doing!
static void protocol_do_alarm() {
    switch (sys_rt_exec_alarm) {
        case ExecAlarm::None:
            return;
        // System alarm. Everything has shutdown by something that has gone severely wrong. Report
        case ExecAlarm::HardLimit:
        case ExecAlarm::SoftLimit:
            sys.state = State::Alarm;  // Set system alarm state
            report_alarm_message(sys_rt_exec_alarm);
            report_feedback_message(Message::CriticalEvent);
            rtReset = false;  // Disable any existing reset
            do {
                // Block everything, except reset and status reports, until user issues reset or power
                // cycles. Hard limits typically occur while unattended or not paying attention. Gives
                // the user and a GUI time to do what is needed before resetting, like killing the
                // incoming stream. The same could be said about soft limits. While the position is not
                // lost, continued streaming could cause a serious crash if by chance it gets executed.
            } while (!rtReset);
            break;
        default:
            sys.state = State::Alarm;  // Set system alarm state
            report_alarm_message(sys_rt_exec_alarm);
            break;
    }
    sys_rt_exec_alarm = ExecAlarm::None;
}

static void protocol_start_holding() {
    if (!(sys.suspend.bit.motionCancel || sys.suspend.bit.jogCancel)) {  // Block, if already holding.
        st_update_plan_block_parameters();                               // Notify stepper module to recompute for hold deceleration.
        sys.step_control             = {};
        sys.step_control.executeHold = true;  // Initiate suspend state with active flag.
    }
}

static void protocol_cancel_jogging() {
    if (!sys.suspend.bit.motionCancel) {
        sys.suspend.bit.jogCancel = true;
    }
}

static void protocol_hold_complete() {
    sys.suspend.value            = 0;
    sys.suspend.bit.holdComplete = true;
}

static void protocol_do_motion_cancel() {
    // Execute and flag a motion cancel with deceleration and return to idle. Used primarily by probing cycle
    // to halt and cancel the remainder of the motion.
    rtMotionCancel = false;
    rtCycleStart   = false;  // Cancel any pending start

    // MOTION_CANCEL only occurs during a CYCLE, but a HOLD and SAFETY_DOOR may have been initiated
    // beforehand. Motion cancel affects only a single planner block motion, while jog cancel
    // will handle and clear multiple planner block motions.
    switch (sys.state) {
        case State::Alarm:
        case State::CheckMode:
            return;  // Do not set motionCancel

        case State::Idle:
            protocol_hold_complete();
            break;

        case State::Cycle:
            protocol_start_holding();
            break;

        case State::Jog:
            protocol_start_holding();
            protocol_cancel_jogging();
            // When jogging, we do not set motionCancel, hence return not break
            return;

        case State::Sleep:
        case State::Hold:
        case State::Homing:
        case State::SafetyDoor:
            break;
    }
    sys.suspend.bit.motionCancel = true;
}

static void protocol_do_feedhold() {
    // Execute a feed hold with deceleration, if required. Then, suspend system.
    rtFeedHold   = false;
    rtCycleStart = false;  // Cancel any pending start
    switch (sys.state) {
        case State::Alarm:
        case State::CheckMode:
        case State::SafetyDoor:
        case State::Sleep:
            return;  // Do not change the state to Hold

        case State::Hold:
        case State::Homing:
            break;

        case State::Idle:
            protocol_hold_complete();
            break;

        case State::Cycle:
            protocol_start_holding();
            break;

        case State::Jog:
            protocol_start_holding();
            protocol_cancel_jogging();
            return;  // Do not change the state to Hold
    }
    sys.state = State::Hold;
}

static void protocol_do_safety_door() {
    // Execute a safety door stop with a feed hold and disable spindle/coolant.
    // NOTE: Safety door differs from feed holds by stopping everything no matter state, disables powered
    // devices (spindle/coolant), and blocks resuming until switch is re-engaged.

    rtCycleStart = false;  // Cancel any pending start
    report_feedback_message(Message::SafetyDoorAjar);
    switch (sys.state) {
        case State::Alarm:
        case State::CheckMode:
        case State::Sleep:
            rtSafetyDoor = false;
            return;  // Do not change the state to SafetyDoor

        case State::Hold:
            break;
        case State::Homing:
            sys_rt_exec_alarm = ExecAlarm::HomingFailDoor;
            break;
        case State::SafetyDoor:
            if (!sys.suspend.bit.jogCancel && sys.suspend.bit.initiateRestore) {  // Actively restoring
#ifdef PARKING_ENABLE
                // Set hold and reset appropriate control flags to restart parking sequence.
                if (sys.step_control.executeSysMotion) {
                    st_update_plan_block_parameters();  // Notify stepper module to recompute for hold deceleration.
                    sys.step_control                  = {};
                    sys.step_control.executeHold      = true;
                    sys.step_control.executeSysMotion = true;
                    sys.suspend.bit.holdComplete      = false;
                }  // else NO_MOTION is active.
#endif
                sys.suspend.bit.retractComplete = false;
                sys.suspend.bit.initiateRestore = false;
                sys.suspend.bit.restoreComplete = false;
                sys.suspend.bit.restartRetract  = true;
            }
            break;
        case State::Idle:
            protocol_hold_complete();
            break;
        case State::Cycle:
            protocol_start_holding();
            break;
        case State::Jog:
            protocol_start_holding();
            protocol_cancel_jogging();
            break;
    }
    if (!sys.suspend.bit.jogCancel) {
        // If jogging, leave the safety door event pending until the jog cancel completes
        rtSafetyDoor = false;
        sys.state    = State::SafetyDoor;
    }
    // NOTE: This flag doesn't change when the door closes, unlike sys.state. Ensures any parking motions
    // are executed if the door switch closes and the state returns to HOLD.
    sys.suspend.bit.safetyDoorAjar = true;
}

static void protocol_do_sleep() {
    rtSleep = false;
    switch (sys.state) {
        case State::Alarm:
            sys.suspend.bit.retractComplete = true;
            sys.suspend.bit.holdComplete    = true;
            break;

        case State::Idle:
            protocol_hold_complete();
            break;

        case State::Cycle:
        case State::Jog:
            protocol_start_holding();
            // Unlike other hold events, sleep does not set jogCancel
            break;

        case State::CheckMode:
        case State::Sleep:
        case State::Hold:
        case State::Homing:
        case State::SafetyDoor:
            break;
    }
    sys.state = State::Sleep;
}

static void protocol_do_initiate_cycle() {
    // Start cycle only if queued motions exist in planner buffer and the motion is not canceled.
    sys.step_control = {};  // Restore step control to normal operation
    if (plan_get_current_block() && !sys.suspend.bit.motionCancel) {
        sys.suspend.value = 0;  // Break suspend state.
        sys.state         = State::Cycle;
        st_prep_buffer();  // Initialize step segment buffer before beginning cycle.
        st_wake_up();
    } else {                    // Otherwise, do nothing. Set and resume IDLE state.
        sys.suspend.value = 0;  // Break suspend state.
        sys.state         = State::Idle;
    }
}

// The handlers for rtFeedHold, rtMotionCancel, and rtsDafetyDoor clear rtCycleStart to
// ensure that auto-cycle-start does not resume a hold without explicit user input.
static void protocol_do_cycle_start() {
    rtCycleStart = false;

    // Execute a cycle start by starting the stepper interrupt to begin executing the blocks in queue.

    // Resume door state when parking motion has retracted and door has been closed.
    switch (sys.state) {
        case State::SafetyDoor:
            if (!sys.suspend.bit.safetyDoorAjar) {
                if (sys.suspend.bit.restoreComplete) {
                    sys.state = State::Idle;  // Set to IDLE to immediately resume the cycle.
                } else if (sys.suspend.bit.retractComplete) {
                    // Flag to re-energize powered components and restore original position, if disabled by SAFETY_DOOR.
                    // NOTE: For a safety door to resume, the switch must be closed, as indicated by HOLD state, and
                    // the retraction execution is complete, which implies the initial feed hold is not active. To
                    // restore normal operation, the restore procedures must be initiated by the following flag. Once,
                    // they are complete, it will call CYCLE_START automatically to resume and exit the suspend.
                    sys.suspend.bit.initiateRestore = true;
                }
            }
            break;
        case State::Idle:
            protocol_do_initiate_cycle();
            break;
        case State::Hold:
            // Cycle start only when IDLE or when a hold is complete and ready to resume.
            if (sys.suspend.bit.holdComplete) {
                if (sys.spindle_stop_ovr.value) {
                    sys.spindle_stop_ovr.bit.restoreCycle = true;  // Set to restore in suspend routine and cycle start after.
                } else {
                    protocol_do_initiate_cycle();
                }
            }
            break;
        case State::Alarm:
        case State::CheckMode:
        case State::Sleep:
        case State::Cycle:
        case State::Homing:
        case State::Jog:
            break;
    }
}

static void protocol_do_cycle_stop() {
    rtCycleStop = false;

    switch (sys.state) {
        case State::Hold:
        case State::SafetyDoor:
        case State::Sleep:
            // Reinitializes the cycle plan and stepper system after a feed hold for a resume. Called by
            // realtime command execution in the main program, ensuring that the planner re-plans safely.
            // NOTE: Bresenham algorithm variables are still maintained through both the planner and stepper
            // cycle reinitializations. The stepper path should continue exactly as if nothing has happened.
            // NOTE: rtCycleStop is set by the stepper subsystem when a cycle or feed hold completes.
            if (!sys.soft_limit && !sys.suspend.bit.jogCancel) {
                // Hold complete. Set to indicate ready to resume.  Remain in HOLD or DOOR states until user
                // has issued a resume command or reset.
                plan_cycle_reinitialize();
                if (sys.step_control.executeHold) {
                    sys.suspend.bit.holdComplete = true;
                }
                sys.step_control.executeHold      = false;
                sys.step_control.executeSysMotion = false;
                break;
            }
            // Fall through
        case State::Alarm:
        case State::CheckMode:
        case State::Idle:
        case State::Cycle:
        case State::Homing:
        case State::Jog:
            // Motion complete. Includes CYCLE/JOG/HOMING states and jog cancel/motion cancel/soft limit events.
            // NOTE: Motion and jog cancel both immediately return to idle after the hold completes.
            if (sys.suspend.bit.jogCancel) {  // For jog cancel, flush buffers and sync positions.
                sys.step_control = {};
                plan_reset();
                st_reset();
                gc_sync_position();
                plan_sync_position();
            }
            if (sys.suspend.bit.safetyDoorAjar) {  // Only occurs when safety door opens during jog.
                sys.suspend.bit.jogCancel    = false;
                sys.suspend.bit.holdComplete = true;
                sys.state                    = State::SafetyDoor;
            } else {
                sys.suspend.value = 0;
                sys.state         = State::Idle;
            }
            break;
    }
}

static void protocol_execute_overrides() {
    // Execute overrides.
    if ((sys_rt_f_override != sys.f_override) || (sys_rt_r_override != sys.r_override)) {
        sys.f_override         = sys_rt_f_override;
        sys.r_override         = sys_rt_r_override;
        sys.report_ovr_counter = 0;  // Set to report change immediately
        plan_update_velocity_profile_parameters();
        plan_cycle_reinitialize();
    }

    // NOTE: Unlike motion overrides, spindle overrides do not require a planner reinitialization.
    if (sys_rt_s_override != sys.spindle_speed_ovr) {
        sys.step_control.updateSpindleRpm = true;
        sys.spindle_speed_ovr             = sys_rt_s_override;
        sys.report_ovr_counter            = 0;  // Set to report change immediately
        // If spindle is on, tell it the RPM has been overridden
        if (gc_state.modal.spindle != SpindleState::Disable) {
            spindle->set_rpm(gc_state.spindle_speed);
        }
    }

    if (sys_rt_exec_accessory_override.bit.spindleOvrStop) {
        sys_rt_exec_accessory_override.bit.spindleOvrStop = false;
        // Spindle stop override allowed only while in HOLD state.
        // NOTE: Report counters are set in spindle_set_state() when spindle stop is executed.
        if (sys.state == State::Hold) {
            if (sys.spindle_stop_ovr.value == 0) {
                sys.spindle_stop_ovr.bit.initiate = true;
            } else if (sys.spindle_stop_ovr.bit.enabled) {
                sys.spindle_stop_ovr.bit.restore = true;
            }
        }
    }

    // NOTE: Since coolant state always performs a planner sync whenever it changes, the current
    // run state can be determined by checking the parser state.
    if (sys_rt_exec_accessory_override.bit.coolantFloodOvrToggle) {
        sys_rt_exec_accessory_override.bit.coolantFloodOvrToggle = false;
#ifdef COOLANT_FLOOD_PIN
        if (sys.state == State::Idle || sys.state == State::Cycle || sys.state == State::Hold) {
            gc_state.modal.coolant.Flood = !gc_state.modal.coolant.Flood;
            coolant_set_state(gc_state.modal.coolant);  // Report counter set in coolant_set_state().
        }
#endif
    }
    if (sys_rt_exec_accessory_override.bit.coolantMistOvrToggle) {
        sys_rt_exec_accessory_override.bit.coolantMistOvrToggle = false;
#ifdef COOLANT_MIST_PIN
        if (sys.state == State::Idle || sys.state == State::Cycle || sys.state == State::Hold) {
            gc_state.modal.coolant.Mist = !gc_state.modal.coolant.Mist;
            coolant_set_state(gc_state.modal.coolant);  // Report counter set in coolant_set_state().
        }
#endif
    }
}

void protocol_exec_rt_system() {
    protocol_do_alarm();  // If there is a hard or soft limit, this will block until rtReset is set

    if (rtReset) {
        if (sys.state == State::Homing) {
            sys_rt_exec_alarm = ExecAlarm::HomingFailReset;
        }
        // Execute system abort.
        sys.abort = true;  // Only place this is set true.
        return;            // Nothing else to do but exit.
    }

    if (rtStatusReport) {
        rtStatusReport = false;
        report_realtime_status(CLIENT_ALL);
    }

    if (rtMotionCancel) {
        protocol_do_motion_cancel();
    }

    if (rtFeedHold) {
        protocol_do_feedhold();
    }

    if (rtSafetyDoor) {
        protocol_do_safety_door();
    }

    if (rtSleep) {
        protocol_do_sleep();
    }

    if (rtCycleStart) {
        protocol_do_cycle_start();
    }

    if (rtCycleStop) {
        protocol_do_cycle_stop();
    }

    protocol_execute_overrides();

#ifdef DEBUG
    if (sys_rt_exec_debug) {
        sys_rt_exec_debug = false;
        report_realtime_debug();
    }
#endif
    // Reload step segment buffer
    switch (sys.state) {
        case State::Alarm:
        case State::CheckMode:
        case State::Idle:
        case State::Sleep:
            break;
        case State::Cycle:
        case State::Hold:
        case State::SafetyDoor:
        case State::Homing:
        case State::Jog:
            st_prep_buffer();
            break;
    }
}

// Handles Grbl system suspend procedures, such as feed hold, safety door, and parking motion.
// The system will enter this loop, create local variables for suspend tasks, and return to
// whatever function that invoked the suspend, such that Grbl resumes normal operation.
// This function is written in a way to promote custom parking motions. Simply use this as a
// template
static void protocol_exec_rt_suspend() {
#ifdef PARKING_ENABLE
    // Declare and initialize parking local variables
    float             restore_target[MAX_N_AXIS];
    float             retract_waypoint = PARKING_PULLOUT_INCREMENT;
    plan_line_data_t  plan_data;
    plan_line_data_t* pl_data = &plan_data;
    memset(pl_data, 0, sizeof(plan_line_data_t));
    pl_data->motion                = {};
    pl_data->motion.systemMotion   = 1;
    pl_data->motion.noFeedOverride = 1;
#    ifdef USE_LINE_NUMBERS
    pl_data->line_number = PARKING_MOTION_LINE_NUMBER;
#    endif
#endif
    plan_block_t* block = plan_get_current_block();
    CoolantState  restore_coolant;
    SpindleState  restore_spindle;
    float         restore_spindle_speed;
    if (block == NULL) {
        restore_coolant       = gc_state.modal.coolant;
        restore_spindle       = gc_state.modal.spindle;
        restore_spindle_speed = gc_state.spindle_speed;
    } else {
        restore_coolant       = block->coolant;
        restore_spindle       = block->spindle;
        restore_spindle_speed = block->spindle_speed;
    }
#ifdef DISABLE_LASER_DURING_HOLD
    if (spindle->inLaserMode()) {
        sys_rt_exec_accessory_override.bit.spindleOvrStop = true;
    }
#endif

    while (sys.suspend.value) {
        if (sys.abort) {
            return;
        }
        // if a jogCancel comes in and we have a jog "in-flight" (parsed and handed over to mc_line()),
        //  then we need to cancel it before it reaches the planner.  otherwise we may try to move way out of
        //  normal bounds, especially with senders that issue a series of jog commands before sending a cancel.
        if (sys.suspend.bit.jogCancel && sys_pl_data_inflight != NULL && ((plan_line_data_t*)sys_pl_data_inflight)->is_jog) {
            sys_pl_data_inflight = NULL;
        }
        // Block until initial hold is complete and the machine has stopped motion.
        if (sys.suspend.bit.holdComplete) {
            // Parking manager. Handles de/re-energizing, switch state checks, and parking motions for
            // the safety door and sleep states.
            if (sys.state == State::SafetyDoor || sys.state == State::Sleep) {
                // Handles retraction motions and de-energizing.
#ifdef PARKING_ENABLE
                float* parking_target = system_get_mpos();
#endif
                if (!sys.suspend.bit.retractComplete) {
                    // Ensure any prior spindle stop override is disabled at start of safety door routine.
                    sys.spindle_stop_ovr.value = 0;  // Disable override
#ifndef PARKING_ENABLE
                    spindle->set_state(SpindleState::Disable, 0);  // De-energize
                    coolant_off();
#else
                    // Get current position and store restore location and spindle retract waypoint.
                    if (!sys.suspend.bit.restartRetract) {
                        memcpy(restore_target, parking_target, sizeof(restore_target[0]) * number_axis->get());
                        retract_waypoint += restore_target[PARKING_AXIS];
                        retract_waypoint = MIN(retract_waypoint, PARKING_TARGET);
                    }
                    // Execute slow pull-out parking retract motion. Parking requires homing enabled, the
                    // current location not exceeding the parking target location, and laser mode disabled.
                    // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
                    if (can_park() && parking_target[PARKING_AXIS] < PARKING_TARGET) {
                        // Retract spindle by pullout distance. Ensure retraction motion moves away from
                        // the workpiece and waypoint motion doesn't exceed the parking target location.
                        if (parking_target[PARKING_AXIS] < retract_waypoint) {
                            parking_target[PARKING_AXIS] = retract_waypoint;
                            pl_data->feed_rate           = PARKING_PULLOUT_RATE;
                            pl_data->coolant             = restore_coolant;
                            pl_data->spindle             = restore_spindle;
                            pl_data->spindle_speed       = restore_spindle_speed;
                            mc_parking_motion(parking_target, pl_data);
                        }
                        // NOTE: Clear accessory state after retract and after an aborted restore motion.
                        pl_data->spindle               = SpindleState::Disable;
                        pl_data->coolant               = {};
                        pl_data->motion                = {};
                        pl_data->motion.systemMotion   = 1;
                        pl_data->motion.noFeedOverride = 1;
                        pl_data->spindle_speed         = 0.0;
                        spindle->set_state(pl_data->spindle, 0);  // De-energize
                        coolant_set_state(pl_data->coolant);
                        // Execute fast parking retract motion to parking target location.
                        if (parking_target[PARKING_AXIS] < PARKING_TARGET) {
                            parking_target[PARKING_AXIS] = PARKING_TARGET;
                            pl_data->feed_rate           = PARKING_RATE;
                            mc_parking_motion(parking_target, pl_data);
                        }
                    } else {
                        // Parking motion not possible. Just disable the spindle and coolant.
                        // NOTE: Laser mode does not start a parking motion to ensure the laser stops immediately.
                        spindle->set_state(SpindleState::Disable, 0);  // De-energize
                        coolant_off();
                    }
#endif
                    sys.suspend.bit.restartRetract  = false;
                    sys.suspend.bit.retractComplete = true;
                } else {
                    if (sys.state == State::Sleep) {
                        report_feedback_message(Message::SleepMode);
                        // Spindle and coolant should already be stopped, but do it again just to be sure.
                        spindle->set_state(SpindleState::Disable, 0);  // De-energize
                        coolant_off();
                        st_go_idle();  // Disable steppers
                        while (!(sys.abort)) {
                            protocol_exec_rt_system();  // Do nothing until reset.
                        }
                        return;  // Abort received. Return to re-initialize.
                    }
                    // Allows resuming from parking/safety door. Actively checks if safety door is closed and ready to resume.
                    if (sys.state == State::SafetyDoor) {
                        if (!(system_check_safety_door_ajar())) {
                            sys.suspend.bit.safetyDoorAjar = false;  // Reset door ajar flag to denote ready to resume.
                        }
                    }
                    // Handles parking restore and safety door resume.
                    if (sys.suspend.bit.initiateRestore) {
#ifdef PARKING_ENABLE
                        // Execute fast restore motion to the pull-out position. Parking requires homing enabled.
                        // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
                        if (can_park()) {
                            // Check to ensure the motion doesn't move below pull-out position.
                            if (parking_target[PARKING_AXIS] <= PARKING_TARGET) {
                                parking_target[PARKING_AXIS] = retract_waypoint;
                                pl_data->feed_rate           = PARKING_RATE;
                                mc_parking_motion(parking_target, pl_data);
                            }
                        }
#endif
                        // Delayed Tasks: Restart spindle and coolant, delay to power-up, then resume cycle.
                        if (gc_state.modal.spindle != SpindleState::Disable) {
                            // Block if safety door re-opened during prior restore actions.
                            if (!sys.suspend.bit.restartRetract) {
                                if (spindle->inLaserMode()) {
                                    // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
                                    sys.step_control.updateSpindleRpm = true;
                                } else {
                                    spindle->set_state(restore_spindle, (uint32_t)restore_spindle_speed);
                                    // restore delay is done in the spindle class
                                    //delay_sec(int32_t(1000.0 * spindle_delay_spinup->get()), DwellMode::SysSuspend);
                                }
                            }
                        }
                        if (gc_state.modal.coolant.Flood || gc_state.modal.coolant.Mist) {
                            // Block if safety door re-opened during prior restore actions.
                            if (!sys.suspend.bit.restartRetract) {
                                // NOTE: Laser mode will honor this delay. An exhaust system is often controlled by this pin.
                                coolant_set_state(restore_coolant);
                                delay_msec(int32_t(1000.0 * coolant_start_delay->get()), DwellMode::SysSuspend);
                            }
                        }
#ifdef PARKING_ENABLE
                        // Execute slow plunge motion from pull-out position to resume position.
                        if (can_park()) {
                            // Block if safety door re-opened during prior restore actions.
                            if (!sys.suspend.bit.restartRetract) {
                                // Regardless if the retract parking motion was a valid/safe motion or not, the
                                // restore parking motion should logically be valid, either by returning to the
                                // original position through valid machine space or by not moving at all.
                                pl_data->feed_rate     = PARKING_PULLOUT_RATE;
                                pl_data->spindle       = restore_spindle;
                                pl_data->coolant       = restore_coolant;
                                pl_data->spindle_speed = restore_spindle_speed;
                                mc_parking_motion(restore_target, pl_data);
                            }
                        }
#endif
                        if (!sys.suspend.bit.restartRetract) {
                            sys.suspend.bit.restoreComplete = true;
                            rtCycleStart                    = true;  // Set to resume program.
                        }
                    }
                }
            } else {
                // Feed hold manager. Controls spindle stop override states.
                // NOTE: Hold ensured as completed by condition check at the beginning of suspend routine.
                if (sys.spindle_stop_ovr.value) {
                    // Handles beginning of spindle stop
                    if (sys.spindle_stop_ovr.bit.initiate) {
                        if (gc_state.modal.spindle != SpindleState::Disable) {
                            spindle->set_state(SpindleState::Disable, 0);  // De-energize
                            sys.spindle_stop_ovr.value       = 0;
                            sys.spindle_stop_ovr.bit.enabled = true;  // Set stop override state to enabled, if de-energized.
                        } else {
                            sys.spindle_stop_ovr.value = 0;  // Clear stop override state
                        }
                        // Handles restoring of spindle state
                    } else if (sys.spindle_stop_ovr.bit.restore || sys.spindle_stop_ovr.bit.restoreCycle) {
                        if (gc_state.modal.spindle != SpindleState::Disable) {
                            report_feedback_message(Message::SpindleRestore);
                            if (spindle->inLaserMode()) {
                                // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
                                sys.step_control.updateSpindleRpm = true;
                            } else {
                                spindle->set_state(restore_spindle, (uint32_t)restore_spindle_speed);
                            }
                        }
                        if (sys.spindle_stop_ovr.bit.restoreCycle) {
                            rtCycleStart = true;  // Set to resume program.
                        }
                        sys.spindle_stop_ovr.value = 0;  // Clear stop override state
                    }
                } else {
                    // Handles spindle state during hold. NOTE: Spindle speed overrides may be altered during hold state.
                    // NOTE: sys.step_control.updateSpindleRpm is automatically reset upon resume in step generator.
                    if (sys.step_control.updateSpindleRpm) {
                        spindle->set_state(restore_spindle, (uint32_t)restore_spindle_speed);
                        sys.step_control.updateSpindleRpm = false;
                    }
                }
            }
        }
        protocol_exec_rt_system();
    }
}
