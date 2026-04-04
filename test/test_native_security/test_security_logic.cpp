/**
 * Native unit tests for security/alarm logic
 *
 * Tests alarm state machine transitions, debounce, and queue logic
 * using minimal extracted logic — no ESP32 dependencies.
 */
#include <unity.h>
#include "ArduinoMock.h"

// ============================================================================
// Extracted alarm state machine (mirrors SecurityMonitor logic)
// ============================================================================

enum class AlarmState : uint8_t {
    DISARMED,
    ARMING,
    ARMED,
    PENDING,
    TRIGGERED
};

// Minimal alarm state machine — extracted from SecurityMonitor::processRadarData
// and SecurityMonitor::update() for testability without ESP32 deps
struct AlarmStateMachine {
    AlarmState state = AlarmState::DISARMED;
    unsigned long entryDelay = 30000;
    unsigned long exitDelay = 30000;
    unsigned long triggerTimeout = 900000;
    bool autoRearm = true;
    uint8_t alarmEnergyThreshold = 15;
    uint8_t alarmDebounceFrames = 3;

    // Internal state
    uint8_t debounceCount = 0;
    unsigned long exitDelayStart = 0;
    unsigned long entryDelayStart = 0;
    unsigned long triggerStartTime = 0;

    // Transition counters (for test assertions)
    int pendingCount = 0;
    int triggeredCount = 0;
    int armedCount = 0;

    void setArmed(bool armed, bool immediate, unsigned long now) {
        if (armed) {
            if (state == AlarmState::PENDING || state == AlarmState::TRIGGERED) return;
            if (state == AlarmState::ARMING || state == AlarmState::ARMED) return;
            if (immediate) {
                state = AlarmState::ARMED;
                armedCount++;
            } else {
                state = AlarmState::ARMING;
                exitDelayStart = now;
            }
            debounceCount = 0;
        } else {
            state = AlarmState::DISARMED;
            entryDelayStart = 0;
            exitDelayStart = 0;
        }
    }

    // Called from update() — handles ARMING→ARMED and TRIGGERED timeout
    void updateTimers(unsigned long now) {
        if (state == AlarmState::ARMING && (now - exitDelayStart) >= exitDelay) {
            state = AlarmState::ARMED;
            armedCount++;
        }
        if (state == AlarmState::TRIGGERED && triggerTimeout > 0 &&
            (now - triggerStartTime) >= triggerTimeout) {
            if (autoRearm) {
                state = AlarmState::ARMED;
                armedCount++;
            } else {
                state = AlarmState::DISARMED;
            }
        }
    }

    // Mirrors processRadarData alarm logic (behavior 0 = entry delay)
    void processDetection(uint16_t distance, uint8_t moveEnergy, uint8_t staticEnergy, unsigned long now) {
        bool qualifies = (state == AlarmState::ARMED && distance > 0 &&
            (moveEnergy >= alarmEnergyThreshold || staticEnergy >= alarmEnergyThreshold));

        if (qualifies) {
            debounceCount++;
        } else if (state == AlarmState::ARMED) {
            debounceCount = 0;
        }

        if (qualifies && debounceCount >= alarmDebounceFrames) {
            debounceCount = 0;
            state = AlarmState::PENDING;
            entryDelayStart = now;
            pendingCount++;
        }

        if (state == AlarmState::PENDING && (now - entryDelayStart) >= entryDelay) {
            state = AlarmState::TRIGGERED;
            triggerStartTime = now;
            triggeredCount++;
        }
    }
};

// ============================================================================
// Telegram queue logic (extracted)
// ============================================================================
struct TelegramQueueMock {
    static constexpr size_t QUEUE_SIZE = 16;
    char items[QUEUE_SIZE][512];
    size_t count = 0;
    uint16_t droppedMessages = 0;

    bool enqueue(const char* text) {
        if (count >= QUEUE_SIZE) {
            droppedMessages++;
            return false;
        }
        strncpy(items[count], text, 511);
        items[count][511] = '\0';
        count++;
        return true;
    }

    bool dequeue(char* out, size_t outLen) {
        if (count == 0) return false;
        strncpy(out, items[0], outLen - 1);
        out[outLen - 1] = '\0';
        // Shift (simple, not ring buffer — matches test simplicity)
        for (size_t i = 1; i < count; i++) {
            memcpy(items[i-1], items[i], 512);
        }
        count--;
        return true;
    }
};

// ============================================================================
// Test fixtures
// ============================================================================
static AlarmStateMachine* asm_ptr = nullptr;

void setUp(void) {
    setMockMillis(10000);
    if (asm_ptr) delete asm_ptr;
    asm_ptr = new AlarmStateMachine();
}

void tearDown(void) {
    delete asm_ptr;
    asm_ptr = nullptr;
}

// ============================================================================
// ALARM STATE MACHINE TESTS
// ============================================================================

void test_initial_state_disarmed() {
    TEST_ASSERT_EQUAL(AlarmState::DISARMED, asm_ptr->state);
}

void test_arm_with_exit_delay() {
    asm_ptr->setArmed(true, false, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMING, asm_ptr->state);

    // Before exit delay expires
    setMockMillis(millis() + 29999);
    asm_ptr->updateTimers(millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMING, asm_ptr->state);

    // After exit delay expires
    setMockMillis(millis() + 2);
    asm_ptr->updateTimers(millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);
}

void test_arm_immediate() {
    asm_ptr->setArmed(true, true, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);
}

void test_disarm_from_armed() {
    asm_ptr->setArmed(true, true, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);

    asm_ptr->setArmed(false, false, millis());
    TEST_ASSERT_EQUAL(AlarmState::DISARMED, asm_ptr->state);
}

void test_disarm_from_triggered() {
    // Get to TRIGGERED state
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 0;
    asm_ptr->setArmed(true, true, millis());
    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);

    // Disarm should always work
    asm_ptr->setArmed(false, false, millis());
    TEST_ASSERT_EQUAL(AlarmState::DISARMED, asm_ptr->state);
}

void test_cannot_rearm_while_triggered() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 0;
    asm_ptr->setArmed(true, true, millis());
    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);

    // Try to re-arm — should be rejected
    asm_ptr->setArmed(true, true, millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);
}

void test_cannot_rearm_while_pending() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 30000; // Long entry delay
    asm_ptr->setArmed(true, true, millis());
    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);

    // Try to re-arm — should be rejected
    asm_ptr->setArmed(true, true, millis());
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);
}

void test_idempotent_arm() {
    asm_ptr->setArmed(true, true, millis());
    TEST_ASSERT_EQUAL(1, asm_ptr->armedCount);

    // Arming again should be no-op
    asm_ptr->setArmed(true, true, millis());
    TEST_ASSERT_EQUAL(1, asm_ptr->armedCount); // Count unchanged
}

// ============================================================================
// DEBOUNCE TESTS
// ============================================================================

void test_debounce_requires_consecutive_frames() {
    asm_ptr->alarmDebounceFrames = 3;
    asm_ptr->setArmed(true, true, millis());

    // 2 qualifying frames — not enough
    asm_ptr->processDetection(200, 50, 0, millis());
    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);

    // 3rd frame — should trigger PENDING
    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);
}

void test_debounce_resets_on_gap() {
    asm_ptr->alarmDebounceFrames = 3;
    asm_ptr->setArmed(true, true, millis());

    // 2 qualifying, then 1 non-qualifying, then 2 more — should NOT trigger
    asm_ptr->processDetection(200, 50, 0, millis());
    asm_ptr->processDetection(200, 50, 0, millis());
    asm_ptr->processDetection(200, 5, 0, millis()); // Below threshold
    asm_ptr->processDetection(200, 50, 0, millis());
    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);

    // 3rd consecutive — now it triggers
    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);
}

void test_debounce_below_threshold_ignored() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->alarmEnergyThreshold = 15;
    asm_ptr->setArmed(true, true, millis());

    // Energy below threshold — should not trigger
    asm_ptr->processDetection(200, 10, 10, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);

    // Energy at threshold — should trigger
    asm_ptr->processDetection(200, 15, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);
}

void test_debounce_zero_distance_ignored() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->setArmed(true, true, millis());

    // Zero distance = no target — should not trigger even with energy
    asm_ptr->processDetection(0, 50, 50, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);
}

void test_debounce_static_energy_also_qualifies() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->alarmEnergyThreshold = 15;
    asm_ptr->setArmed(true, true, millis());

    // Only static energy (no movement) — should still trigger
    asm_ptr->processDetection(300, 0, 20, millis());
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);
}

// ============================================================================
// ENTRY DELAY TESTS
// ============================================================================

void test_entry_delay_pending_to_triggered() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 5000;
    asm_ptr->setArmed(true, true, millis());

    unsigned long detectTime = millis();
    asm_ptr->processDetection(200, 50, 0, detectTime);
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);

    // Before entry delay expires
    setMockMillis(detectTime + 4999);
    asm_ptr->processDetection(0, 0, 0, millis()); // No detection, just time check
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);

    // After entry delay expires
    setMockMillis(detectTime + 5001);
    asm_ptr->processDetection(0, 0, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);
}

void test_disarm_during_entry_delay() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 30000;
    asm_ptr->setArmed(true, true, millis());

    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);

    // Disarm during entry delay (user enters code in time)
    asm_ptr->setArmed(false, false, millis());
    TEST_ASSERT_EQUAL(AlarmState::DISARMED, asm_ptr->state);
}

void test_zero_entry_delay_immediate_trigger() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 0;
    asm_ptr->setArmed(true, true, millis());

    asm_ptr->processDetection(200, 50, 0, millis());
    // With 0 entry delay, goes PENDING then immediately TRIGGERED in same call
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);
}

// ============================================================================
// TRIGGER TIMEOUT & AUTO-REARM TESTS
// ============================================================================

void test_trigger_timeout_auto_rearm() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 0;
    asm_ptr->triggerTimeout = 10000;
    asm_ptr->autoRearm = true;
    asm_ptr->setArmed(true, true, millis());

    unsigned long trigTime = millis();
    asm_ptr->processDetection(200, 50, 0, trigTime);
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);

    // Before timeout
    setMockMillis(trigTime + 9999);
    asm_ptr->updateTimers(millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);

    // After timeout — auto rearm
    setMockMillis(trigTime + 10001);
    asm_ptr->updateTimers(millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);
}

void test_trigger_timeout_no_rearm() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 0;
    asm_ptr->triggerTimeout = 10000;
    asm_ptr->autoRearm = false;
    asm_ptr->setArmed(true, true, millis());

    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);

    setMockMillis(millis() + 10001);
    asm_ptr->updateTimers(millis());
    TEST_ASSERT_EQUAL(AlarmState::DISARMED, asm_ptr->state);
}

void test_zero_trigger_timeout_stays_triggered() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 0;
    asm_ptr->triggerTimeout = 0; // Disabled
    asm_ptr->setArmed(true, true, millis());

    asm_ptr->processDetection(200, 50, 0, millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);

    // Even after long time — stays triggered (manual disarm required)
    setMockMillis(millis() + 999999);
    asm_ptr->updateTimers(millis());
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);
}

// ============================================================================
// DETECTION WHILE DISARMED — NO STATE CHANGE
// ============================================================================

void test_detection_while_disarmed_no_effect() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->processDetection(200, 99, 99, millis());
    TEST_ASSERT_EQUAL(AlarmState::DISARMED, asm_ptr->state);
    TEST_ASSERT_EQUAL(0, asm_ptr->pendingCount);
}

void test_detection_while_arming_no_effect() {
    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->setArmed(true, false, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMING, asm_ptr->state);

    // Detection during exit delay — should NOT trigger
    asm_ptr->processDetection(200, 99, 99, millis());
    TEST_ASSERT_EQUAL(AlarmState::ARMING, asm_ptr->state);
}

// ============================================================================
// FULL CYCLE TEST
// ============================================================================

void test_full_alarm_cycle() {
    unsigned long t = millis();

    // 1. Arm with exit delay
    asm_ptr->exitDelay = 5000;
    asm_ptr->entryDelay = 3000;
    asm_ptr->alarmDebounceFrames = 2;
    asm_ptr->triggerTimeout = 10000;
    asm_ptr->autoRearm = true;

    asm_ptr->setArmed(true, false, t);
    TEST_ASSERT_EQUAL(AlarmState::ARMING, asm_ptr->state);

    // 2. Exit delay expires
    t += 5001;
    setMockMillis(t);
    asm_ptr->updateTimers(t);
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);

    // 3. Detection — 1st frame (debounce not met)
    t += 100;
    setMockMillis(t);
    asm_ptr->processDetection(150, 40, 0, t);
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);

    // 4. Detection — 2nd frame (debounce met → PENDING)
    t += 100;
    setMockMillis(t);
    asm_ptr->processDetection(140, 45, 0, t);
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);

    // 5. Entry delay running
    t += 2000;
    setMockMillis(t);
    asm_ptr->processDetection(0, 0, 0, t);
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);

    // 6. Entry delay expired → TRIGGERED
    t += 1100;
    setMockMillis(t);
    asm_ptr->processDetection(0, 0, 0, t);
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);

    // 7. Trigger timeout → auto rearm to ARMED
    t += 10001;
    setMockMillis(t);
    asm_ptr->updateTimers(t);
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);
}

// ============================================================================
// TELEGRAM QUEUE TESTS
// ============================================================================

void test_telegram_queue_basic() {
    TelegramQueueMock q;
    TEST_ASSERT_TRUE(q.enqueue("Hello"));
    TEST_ASSERT_EQUAL(1, q.count);

    char buf[512];
    TEST_ASSERT_TRUE(q.dequeue(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Hello", buf);
    TEST_ASSERT_EQUAL(0, q.count);
}

void test_telegram_queue_overflow_drops() {
    TelegramQueueMock q;
    // Fill queue
    for (size_t i = 0; i < TelegramQueueMock::QUEUE_SIZE; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "msg%zu", i);
        TEST_ASSERT_TRUE(q.enqueue(msg));
    }
    TEST_ASSERT_EQUAL(16, q.count);
    TEST_ASSERT_EQUAL(0, q.droppedMessages);

    // 17th message should be dropped
    TEST_ASSERT_FALSE(q.enqueue("overflow"));
    TEST_ASSERT_EQUAL(1, q.droppedMessages);

    // Multiple overflows tracked
    TEST_ASSERT_FALSE(q.enqueue("overflow2"));
    TEST_ASSERT_EQUAL(2, q.droppedMessages);
}

void test_telegram_queue_fifo_order() {
    TelegramQueueMock q;
    q.enqueue("first");
    q.enqueue("second");
    q.enqueue("third");

    char buf[512];
    q.dequeue(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("first", buf);
    q.dequeue(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("second", buf);
    q.dequeue(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("third", buf);
}

void test_telegram_queue_dequeue_empty() {
    TelegramQueueMock q;
    char buf[512];
    TEST_ASSERT_FALSE(q.dequeue(buf, sizeof(buf)));
}

// ============================================================================
// MILLIS OVERFLOW TESTS
// ============================================================================

void test_millis_overflow_exit_delay() {
    // Start near overflow boundary
    unsigned long t = 0xFFFFF000; // ~4.2 billion - 4096
    setMockMillis(t);

    asm_ptr->exitDelay = 5000;
    asm_ptr->setArmed(true, false, t);
    TEST_ASSERT_EQUAL(AlarmState::ARMING, asm_ptr->state);

    // After overflow (wraps around)
    t += 5001; // Wraps past 0
    setMockMillis(t);
    asm_ptr->updateTimers(t);
    TEST_ASSERT_EQUAL(AlarmState::ARMED, asm_ptr->state);
}

void test_millis_overflow_entry_delay() {
    unsigned long t = 0xFFFFF000;
    setMockMillis(t);

    asm_ptr->alarmDebounceFrames = 1;
    asm_ptr->entryDelay = 5000;
    asm_ptr->setArmed(true, true, t);

    asm_ptr->processDetection(200, 50, 0, t);
    TEST_ASSERT_EQUAL(AlarmState::PENDING, asm_ptr->state);

    t += 5001; // Wraps past 0
    setMockMillis(t);
    asm_ptr->processDetection(0, 0, 0, t);
    TEST_ASSERT_EQUAL(AlarmState::TRIGGERED, asm_ptr->state);
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char **argv) {
    UNITY_BEGIN();

    // Alarm state machine
    RUN_TEST(test_initial_state_disarmed);
    RUN_TEST(test_arm_with_exit_delay);
    RUN_TEST(test_arm_immediate);
    RUN_TEST(test_disarm_from_armed);
    RUN_TEST(test_disarm_from_triggered);
    RUN_TEST(test_cannot_rearm_while_triggered);
    RUN_TEST(test_cannot_rearm_while_pending);
    RUN_TEST(test_idempotent_arm);

    // Debounce
    RUN_TEST(test_debounce_requires_consecutive_frames);
    RUN_TEST(test_debounce_resets_on_gap);
    RUN_TEST(test_debounce_below_threshold_ignored);
    RUN_TEST(test_debounce_zero_distance_ignored);
    RUN_TEST(test_debounce_static_energy_also_qualifies);

    // Entry delay
    RUN_TEST(test_entry_delay_pending_to_triggered);
    RUN_TEST(test_disarm_during_entry_delay);
    RUN_TEST(test_zero_entry_delay_immediate_trigger);

    // Trigger timeout
    RUN_TEST(test_trigger_timeout_auto_rearm);
    RUN_TEST(test_trigger_timeout_no_rearm);
    RUN_TEST(test_zero_trigger_timeout_stays_triggered);

    // Detection in non-armed states
    RUN_TEST(test_detection_while_disarmed_no_effect);
    RUN_TEST(test_detection_while_arming_no_effect);

    // Full cycle
    RUN_TEST(test_full_alarm_cycle);

    // Telegram queue
    RUN_TEST(test_telegram_queue_basic);
    RUN_TEST(test_telegram_queue_overflow_drops);
    RUN_TEST(test_telegram_queue_fifo_order);
    RUN_TEST(test_telegram_queue_dequeue_empty);

    // Millis overflow
    RUN_TEST(test_millis_overflow_exit_delay);
    RUN_TEST(test_millis_overflow_entry_delay);

    return UNITY_END();
}
