/**
 * Native unit tests for LD2412 UART parser
 * Tests the REAL production LD2412 class — not a copy.
 * Runs on PC, no hardware required.
 */
#include <unity.h>
#include "ArduinoMock.h"
#include "LD2412.h"

// ============================================================================
// Helper: build a valid basic-mode data frame
// Frame = Header(4) + Length(2) + Payload(length) + Footer(4)
// ============================================================================
static std::vector<uint8_t> buildBasicFrame(
    uint8_t state, uint16_t movDist, uint8_t movEn,
    uint16_t statDist, uint8_t statEn)
{
    // Real LD2412 basic frame payload = 11 bytes (verified on HW: totalLen=21)
    // Header(4) + Length(2) + Payload(11) + Footer(4) = 21 bytes
    // Payload: dataType(1) + head(1) + state(1) + movDist(2) + movEn(1) +
    //          statDist(2) + statEn(1) + detDist(2) = 11 bytes
    const uint8_t payloadLen = 11;
    std::vector<uint8_t> frame;

    // Header
    frame.push_back(0xF4);
    frame.push_back(0xF3);
    frame.push_back(0xF2);
    frame.push_back(0xF1);

    // Length (little-endian)
    frame.push_back(payloadLen);
    frame.push_back(0x00);

    // Payload (11 bytes)
    frame.push_back(0x02);  // Data type: basic
    frame.push_back(0xAA);  // Head marker
    frame.push_back(state);
    frame.push_back(movDist & 0xFF);
    frame.push_back((movDist >> 8) & 0xFF);
    frame.push_back(movEn);
    frame.push_back(statDist & 0xFF);
    frame.push_back((statDist >> 8) & 0xFF);
    frame.push_back(statEn);
    frame.push_back(0x00);  // Detection distance low
    frame.push_back(0x00);  // Detection distance high

    // Footer
    frame.push_back(0xF8);
    frame.push_back(0xF7);
    frame.push_back(0xF6);
    frame.push_back(0xF5);

    // Parser's VERIFY_FOOTER state requires one more byte to be popped
    // from the ring buffer (it triggers on next ringPop). On real HW,
    // this is the first byte of the next frame. Add a dummy byte.
    frame.push_back(0x00);

    return frame;  // total 22 bytes (21 frame + 1 trigger)
}

// ============================================================================
// Test fixture — MockSerial fed to real LD2412
// ============================================================================
static MockSerialClass mockSerial;
static LD2412* radar = nullptr;

void setUp(void) {
    mockSerial.clearAll();
    setMockMillis(10000);  // Start with reasonable uptime
    if (radar) delete radar;
    radar = new LD2412(mockSerial);
    // Set threshold to 0 so readSerial always processes
    radar->setSerialRefreshThres(0);
}

void tearDown(void) {
    delete radar;
    radar = nullptr;
}

// ============================================================================
// PARSER TESTS — using real LD2412 class
// ============================================================================

void test_valid_basic_frame() {
    auto frame = buildBasicFrame(3, 150, 80, 250, 40);
    mockSerial.pushToRx(frame.data(), frame.size());

    RadarSnapshot snap = radar->readSnapshot();

    TEST_ASSERT_TRUE(snap.valid);
    TEST_ASSERT_EQUAL(3, snap.state);
    TEST_ASSERT_EQUAL(150, snap.movingDistance);
    TEST_ASSERT_EQUAL(80, snap.movingEnergy);
    TEST_ASSERT_EQUAL(250, snap.staticDistance);
    TEST_ASSERT_EQUAL(40, snap.staticEnergy);
}

void test_snapshot_consistency() {
    // All fields must come from the same frame
    auto frame = buildBasicFrame(1, 100, 50, 200, 30);
    mockSerial.pushToRx(frame.data(), frame.size());

    RadarSnapshot snap = radar->readSnapshot();

    TEST_ASSERT_TRUE(snap.valid);
    // Verify all fields are from the same frame (atomic read)
    TEST_ASSERT_EQUAL(1, snap.state);
    TEST_ASSERT_EQUAL(100, snap.movingDistance);
    TEST_ASSERT_EQUAL(50, snap.movingEnergy);
    TEST_ASSERT_EQUAL(200, snap.staticDistance);
    TEST_ASSERT_EQUAL(30, snap.staticEnergy);
}

void test_garbage_before_frame() {
    // Push garbage, then a valid frame
    uint8_t garbage[] = {0x00, 0xFF, 0xAA, 0xF4, 0x00, 0xF3, 0xBA, 0xAD};
    mockSerial.pushToRx(garbage, sizeof(garbage));

    auto frame = buildBasicFrame(2, 0, 0, 300, 90);
    mockSerial.pushToRx(frame.data(), frame.size());

    RadarSnapshot snap = radar->readSnapshot();

    TEST_ASSERT_TRUE(snap.valid);
    TEST_ASSERT_EQUAL(2, snap.state);
    TEST_ASSERT_EQUAL(300, snap.staticDistance);
    TEST_ASSERT_EQUAL(90, snap.staticEnergy);

    // Resync count should be > 0
    TEST_ASSERT_GREATER_THAN(0, radar->getStatistics().resyncCount);
}

void test_corrupted_footer_rejected() {
    // Build frame but corrupt the footer
    auto frame = buildBasicFrame(1, 100, 50, 200, 30);
    // Footer is last 4 bytes
    frame[frame.size() - 4] = 0x00;
    frame[frame.size() - 3] = 0x00;
    frame[frame.size() - 2] = 0x00;
    frame[frame.size() - 1] = 0x00;

    mockSerial.pushToRx(frame.data(), frame.size());

    RadarSnapshot snap = radar->readSnapshot();
    // Should fail — invalid footer
    TEST_ASSERT_FALSE(snap.valid);
    TEST_ASSERT_EQUAL(0, radar->getStatistics().validFrames);
}

void test_invalid_length_rejected() {
    uint8_t frame[] = {
        0xF4, 0xF3, 0xF2, 0xF1,
        0xFF, 0x00,  // Invalid length = 255 (exceeds buffer)
        0x01
    };
    mockSerial.pushToRx(frame, sizeof(frame));

    RadarSnapshot snap = radar->readSnapshot();
    TEST_ASSERT_FALSE(snap.valid);
    TEST_ASSERT_EQUAL(0, radar->getStatistics().validFrames);
}

void test_fragmented_frame() {
    auto frame = buildBasicFrame(1, 100, 50, 200, 30);

    // Feed first half
    size_t half = frame.size() / 2;
    mockSerial.pushToRx(frame.data(), half);

    RadarSnapshot snap1 = radar->readSnapshot();
    TEST_ASSERT_FALSE(snap1.valid);  // Not complete yet

    // Feed second half
    setMockMillis(millis() + 10);  // Advance time past refresh threshold
    mockSerial.pushToRx(frame.data() + half, frame.size() - half);

    RadarSnapshot snap2 = radar->readSnapshot();
    TEST_ASSERT_TRUE(snap2.valid);
    TEST_ASSERT_EQUAL(100, snap2.movingDistance);
}

void test_multiple_consecutive_frames() {
    auto f1 = buildBasicFrame(1, 10, 20, 0, 0);
    auto f2 = buildBasicFrame(2, 0, 0, 300, 90);
    auto f3 = buildBasicFrame(3, 50, 60, 70, 80);

    mockSerial.pushToRx(f1.data(), f1.size());
    mockSerial.pushToRx(f2.data(), f2.size());
    mockSerial.pushToRx(f3.data(), f3.size());

    // readSnapshot processes all available data, returns last frame
    RadarSnapshot snap = radar->readSnapshot();
    TEST_ASSERT_TRUE(snap.valid);
    TEST_ASSERT_EQUAL(3, radar->getStatistics().validFrames);

    // Last frame's data should be in the snapshot
    TEST_ASSERT_EQUAL(3, snap.state);
    TEST_ASSERT_EQUAL(50, snap.movingDistance);
}

void test_no_data_returns_invalid() {
    RadarSnapshot snap = radar->readSnapshot();
    TEST_ASSERT_FALSE(snap.valid);
    TEST_ASSERT_EQUAL(-1, snap.state);
}

void test_state_machine_initial_state() {
    // Verify initial state is DISCONNECTED
    TEST_ASSERT_EQUAL(UARTState::DISCONNECTED, radar->getUARTState());

    // Note: DISCONNECTED → RUNNING transition requires serial.available() > 0
    // during updateUARTState(), which runs AFTER draining serial into ring buffer.
    // On real HW, continuous radar stream ensures this. In native mock, the serial
    // is fully drained before state check, so state stays DISCONNECTED.
    // This is a mock limitation — frame parsing works correctly regardless.
    auto frame = buildBasicFrame(0, 0, 0, 0, 0);
    mockSerial.pushToRx(frame.data(), frame.size());

    RadarSnapshot snap = radar->readSnapshot();
    TEST_ASSERT_TRUE(snap.valid);  // Parser works even in DISCONNECTED state
}

void test_health_score_after_valid_frames() {
    auto f1 = buildBasicFrame(0, 0, 0, 0, 0);
    auto f2 = buildBasicFrame(0, 0, 0, 0, 0);
    mockSerial.pushToRx(f1.data(), f1.size());
    mockSerial.pushToRx(f2.data(), f2.size());

    radar->readSnapshot();
    setMockMillis(millis() + 10);
    radar->readSnapshot();

    uint8_t health = radar->getHealthScore();
    TEST_ASSERT_GREATER_OR_EQUAL(50, health);  // Should be reasonable after valid frames
}

void test_statistics_tracking() {
    auto frame = buildBasicFrame(1, 100, 50, 200, 30);
    mockSerial.pushToRx(frame.data(), frame.size());
    radar->readSnapshot();

    const UARTStatistics& stats = radar->getStatistics();
    TEST_ASSERT_EQUAL(1, stats.validFrames);
    TEST_ASSERT_EQUAL(22, stats.bytesReceived);  // 21 byte frame + 1 trigger byte
}

void test_zero_length_frame_rejected() {
    uint8_t frame[] = {
        0xF4, 0xF3, 0xF2, 0xF1,
        0x00, 0x00,  // Length = 0 (invalid, < 5)
        0xF8, 0xF7, 0xF6, 0xF5
    };
    mockSerial.pushToRx(frame, sizeof(frame));
    RadarSnapshot snap = radar->readSnapshot();
    TEST_ASSERT_FALSE(snap.valid);
}

void test_static_only_detection() {
    auto frame = buildBasicFrame(2, 0, 0, 450, 65);
    mockSerial.pushToRx(frame.data(), frame.size());

    RadarSnapshot snap = radar->readSnapshot();
    TEST_ASSERT_TRUE(snap.valid);
    TEST_ASSERT_EQUAL(2, snap.state);
    TEST_ASSERT_EQUAL(0, snap.movingDistance);
    TEST_ASSERT_EQUAL(450, snap.staticDistance);
    TEST_ASSERT_EQUAL(65, snap.staticEnergy);
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_valid_basic_frame);
    RUN_TEST(test_snapshot_consistency);
    RUN_TEST(test_garbage_before_frame);
    RUN_TEST(test_corrupted_footer_rejected);
    RUN_TEST(test_invalid_length_rejected);
    RUN_TEST(test_fragmented_frame);
    RUN_TEST(test_multiple_consecutive_frames);
    RUN_TEST(test_no_data_returns_invalid);
    RUN_TEST(test_state_machine_initial_state);
    RUN_TEST(test_health_score_after_valid_frames);
    RUN_TEST(test_statistics_tracking);
    RUN_TEST(test_zero_length_frame_rejected);
    RUN_TEST(test_static_only_detection);

    return UNITY_END();
}
