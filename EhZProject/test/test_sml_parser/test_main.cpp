#include <unity.h>
#include <stdio.h>
#include <vector>

#include "SmlParser.h"
#include "test_data.h"

// ---------------------------------------------------------------------------
// Unity boilerplate
// ---------------------------------------------------------------------------
void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Test 1 – searchSequence: sequence present
// ---------------------------------------------------------------------------
void test_searchSequence_found(void) {
    const uint8_t data[] = { 0x00, 0x01, 0x02, 0xAB, 0xCD, 0xEF, 0x03, 0x04 };
    const uint8_t seq[]  = { 0xAB, 0xCD, 0xEF };
    int idx = SmlParser::searchSequence(seq, 3, data, 8, 0);
    TEST_ASSERT_EQUAL_INT(3, idx);
}

// ---------------------------------------------------------------------------
// Test 2 – searchSequence: sequence absent
// ---------------------------------------------------------------------------
void test_searchSequence_not_found(void) {
    const uint8_t data[] = { 0x00, 0x01, 0x02, 0x03 };
    const uint8_t seq[]  = { 0xAB, 0xCD };
    int idx = SmlParser::searchSequence(seq, 2, data, 4, 0);
    TEST_ASSERT_EQUAL_INT(-1, idx);
}

// ---------------------------------------------------------------------------
// Test 3 – convertTo: positive big-endian value
// { 0x00, 0x01, 0x86, 0xA0 } == 100000
// ---------------------------------------------------------------------------
void test_convertTo_positive(void) {
    const uint8_t bytes[] = { 0x00, 0x01, 0x86, 0xA0 };
    int64_t result = SmlParser::convertTo(bytes, 4);
    TEST_ASSERT_EQUAL_INT64(100000LL, result);
}

// ---------------------------------------------------------------------------
// Test 4 – convertTo: negative value (sign extension)
// { 0xFF, 0xFF, 0xFF, 0xF6 } == -10
// ---------------------------------------------------------------------------
void test_convertTo_negative(void) {
    const uint8_t bytes[] = { 0xFF, 0xFF, 0xFF, 0xF6 };
    int64_t result = SmlParser::convertTo(bytes, 4);
    TEST_ASSERT_EQUAL_INT64(-10LL, result);
}

// ---------------------------------------------------------------------------
// Test 5 – addBytes: incomplete message produces no measurement
// ---------------------------------------------------------------------------
void test_addBytes_incomplete_no_measurement(void) {
    SmlParser parser;
    const int len = SmlParser::MIN_MESSAGE_BYTES - 1;
    std::vector<uint8_t> zeros(len, 0x00);
    parser.addBytes(zeros.data(), len);
    TEST_ASSERT_FALSE(parser.hasMeasurement());
}

// ---------------------------------------------------------------------------
// Test 6 – addBytes: crafted complete telegram parses correctly
//
// Telegram layout: SEQ_START + padding + valid SML_ListEntry TLVs for
// SEQ_CONSUMED/SEQ_PRODUCED/SEQ_POWER (see test_data.h for the field-by-field
// breakdown) + padding + SEQ_STOP, 299 bytes total (telegram 1 of
// TEST_STREAM_DATA):
//   consumedEnergy = 1000.0 Wh  (raw=10000, scaler=-1)
//   producedEnergy = 0.0    Wh  (raw=0,     scaler=-1)
//   currentPower   = 100.0  W   (raw=100,   scaler absent -> 10^0)
// ---------------------------------------------------------------------------
void test_addBytes_crafted_message(void) {
    static const int TLEN = 299;
    uint8_t msg[TLEN];
    memcpy(msg, TEST_STREAM_DATA, TLEN);  // telegram 1

    SmlParser parser;
    parser.addBytes(msg, TLEN);

    TEST_ASSERT_TRUE(parser.hasMeasurement());

    EhZMeasurement m = parser.getMeasurement();
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_DOUBLE(1000.0, m.consumedEnergy);
    TEST_ASSERT_EQUAL_DOUBLE(0.0,    m.producedEnergy);
    TEST_ASSERT_EQUAL_DOUBLE(100.0,  m.currentPower);
}

// ---------------------------------------------------------------------------
// Test 6b – addBytes: telegram without OBIS 16.7.0 (Leistung) is still valid
//
// Power is optional: a meter that never sends 16.7.0 must still produce a
// valid measurement, with currentPower defaulting to 0.0.
// ---------------------------------------------------------------------------
void test_addBytes_missing_power_still_valid(void) {
    static const int TLEN = 299;
    uint8_t msg[TLEN];
    memset(msg, 0, TLEN);

    const uint8_t start[] = { 0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01 };
    for (int i = 0; i < 8; i++) msg[i] = start[i];

    // SEQ_CONSUMED entry: status, valTime, unit, scaler=-1, value=10000 -> 1000.0 Wh
    const uint8_t c1[] = {
        0x77, 0x07, 0x01, 0x00, 0x01, 0x08, 0x00, 0xFF,
        0x01, 0x01, 0x62, 0x00, 0x52, 0xFF, 0x63, 0x27, 0x10, 0x01
    };
    for (size_t i = 0; i < sizeof(c1); i++) msg[90 + i] = c1[i];

    // SEQ_PRODUCED entry: status, valTime, unit, scaler=-1, value=0 -> 0.0 Wh
    const uint8_t p1[] = {
        0x77, 0x07, 0x01, 0x00, 0x02, 0x08, 0x00, 0xFF,
        0x01, 0x01, 0x62, 0x00, 0x52, 0xFF, 0x63, 0x00, 0x00, 0x01
    };
    for (size_t i = 0; i < sizeof(p1); i++) msg[150 + i] = p1[i];

    // No SEQ_POWER anywhere in the telegram.

    // SEQ_STOP  [295..298]
    msg[295] = 0x1B; msg[296] = 0x1B; msg[297] = 0x1B; msg[298] = 0x1B;

    SmlParser parser;
    parser.addBytes(msg, TLEN);

    TEST_ASSERT_TRUE(parser.hasMeasurement());

    EhZMeasurement m = parser.getMeasurement();
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_DOUBLE(1000.0, m.consumedEnergy);
    TEST_ASSERT_EQUAL_DOUBLE(0.0,    m.producedEnergy);
    TEST_ASSERT_EQUAL_DOUBLE(0.0,    m.currentPower);
}

// ---------------------------------------------------------------------------
// Test 7 – stream test (mirrors C# SmlParserTests.StreamTest)
//
// 1. Try to open "ComPortStream.udp" or "test/test_sml_parser/ComPortStream.udp"
// 2. If found: read entire file, feed in 6-byte chunks, collect measurements
// 3. If not found: use TEST_STREAM_DATA from test_data.h, feed in 6-byte chunks
// 4. Assert at least one measurement was collected
// ---------------------------------------------------------------------------
void test_stream_file(void) {
    std::vector<uint8_t> data;
    bool fileFound = false;

    const char* paths[] = {
        "ComPortStream.udp",
        "test/test_sml_parser/ComPortStream.udp"
    };

    for (int p = 0; p < 2 && !fileFound; p++) {
        FILE* f = fopen(paths[p], "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                data.resize((size_t)sz);
                size_t nread = fread(data.data(), 1, (size_t)sz, f);
                if (nread == (size_t)sz) {
                    fileFound = true;
                } else {
                    data.clear();
                }
            }
            fclose(f);
        }
    }

    if (!fileFound) {
        data.assign(TEST_STREAM_DATA, TEST_STREAM_DATA + TEST_STREAM_DATA_LEN);
    }

    SmlParser parser;
    int measurements = 0;
    const int CHUNK = 6;

    for (int offset = 0; offset < (int)data.size(); offset += CHUNK) {
        int remaining = (int)data.size() - offset;
        int toSend    = remaining < CHUNK ? remaining : CHUNK;
        parser.addBytes(data.data() + offset, toSend);
        if (parser.hasMeasurement()) {
            parser.getMeasurement();
            measurements++;
        }
    }

    TEST_ASSERT_GREATER_THAN_INT(0, measurements);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_searchSequence_found);
    RUN_TEST(test_searchSequence_not_found);
    RUN_TEST(test_convertTo_positive);
    RUN_TEST(test_convertTo_negative);
    RUN_TEST(test_addBytes_incomplete_no_measurement);
    RUN_TEST(test_addBytes_crafted_message);
    RUN_TEST(test_addBytes_missing_power_still_valid);
    RUN_TEST(test_stream_file);
    return UNITY_END();
}
