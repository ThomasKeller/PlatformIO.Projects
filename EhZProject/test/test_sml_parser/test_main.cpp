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
// Telegram layout (299 bytes, same structure as test_data.h telegram 1):
//   consumedEnergy1 = 1000.0 Wh  (raw 10000000, offset=10, numBytes=5)
//   producedEnergy1 = 0.0    Wh
//   consumedEnergy2 = 200.0  Wh  (raw  2000000, offset= 7, numBytes=5)
//   producedEnergy2 = 0.0    Wh
//   currentPower    = 100.0  W   (raw     1000, offset= 7, numBytes=4)
// ---------------------------------------------------------------------------
void test_addBytes_crafted_message(void) {
    // Build the telegram byte-by-byte matching test_data.h telegram 1.
    static const int TLEN = 299;
    uint8_t msg[TLEN];
    memset(msg, 0, TLEN);

    // SEQ_START  [0..7]
    const uint8_t start[] = { 0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01 };
    for (int i = 0; i < 8; i++) msg[i] = start[i];

    // SEQ_CONSUMED1  [58..65], skip 10, value at [76..80]
    const uint8_t c1[] = { 0x77, 0x07, 0x01, 0x00, 0x01, 0x08, 0x00, 0xFF };
    for (int i = 0; i < 8; i++) msg[58 + i] = c1[i];
    msg[76] = 0x00; msg[77] = 0x00; msg[78] = 0x98; msg[79] = 0x96; msg[80] = 0x80;

    // SEQ_PRODUCED1  [81..88], skip 10, value at [99..103] = all zero (0.0)
    const uint8_t p1[] = { 0x77, 0x07, 0x01, 0x00, 0x02, 0x08, 0x00, 0xFF };
    for (int i = 0; i < 8; i++) msg[81 + i] = p1[i];

    // SEQ_CONSUMED2  [104..111], skip 7, value at [119..123]
    const uint8_t c2[] = { 0x77, 0x07, 0x01, 0x00, 0x01, 0x08, 0x01, 0xFF };
    for (int i = 0; i < 8; i++) msg[104 + i] = c2[i];
    msg[119] = 0x00; msg[120] = 0x00; msg[121] = 0x1E; msg[122] = 0x84; msg[123] = 0x80;

    // SEQ_PRODUCED2  [124..131], skip 7, value at [139..143] = all zero (0.0)
    const uint8_t p2[] = { 0x77, 0x07, 0x01, 0x00, 0x02, 0x08, 0x01, 0xFF };
    for (int i = 0; i < 8; i++) msg[124 + i] = p2[i];

    // SEQ_POWER  [144..151], skip 7, value at [159..162]
    const uint8_t pw[] = { 0x77, 0x07, 0x01, 0x00, 0x10, 0x07, 0x00, 0xFF };
    for (int i = 0; i < 8; i++) msg[144 + i] = pw[i];
    msg[159] = 0x00; msg[160] = 0x00; msg[161] = 0x03; msg[162] = 0xE8;

    // SEQ_STOP  [295..298]
    msg[295] = 0x1B; msg[296] = 0x1B; msg[297] = 0x1B; msg[298] = 0x1B;

    SmlParser parser;
    parser.addBytes(msg, TLEN);

    TEST_ASSERT_TRUE(parser.hasMeasurement());

    EhZMeasurement m = parser.getMeasurement();
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_DOUBLE(1000.0, m.consumedEnergy1);
    TEST_ASSERT_EQUAL_DOUBLE(0.0,    m.producedEnergy1);
    TEST_ASSERT_EQUAL_DOUBLE(200.0,  m.consumedEnergy2);
    TEST_ASSERT_EQUAL_DOUBLE(0.0,    m.producedEnergy2);
    TEST_ASSERT_EQUAL_DOUBLE(100.0,  m.currentPower);
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
    RUN_TEST(test_stream_file);
    return UNITY_END();
}
