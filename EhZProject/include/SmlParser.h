#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "EhZMeasurement.h"

// Header-only SML parser ported from SmlParser.cs.
// Feed raw bytes from the EHZ meter via addBytes(); poll hasMeasurement()
// and call getMeasurement() once a complete SML message has been decoded.
class SmlParser {
public:
    static const int MIN_MESSAGE_BYTES = 299;

    // OBIS sequences
    static const uint8_t SEQ_START[8];
    static const uint8_t SEQ_STOP[4];
    static const uint8_t SEQ_CONSUMED1[8];   // OBIS 1.8.0
    static const uint8_t SEQ_PRODUCED1[8];   // OBIS 2.8.0
    static const uint8_t SEQ_CONSUMED2[8];   // OBIS 1.8.1
    static const uint8_t SEQ_PRODUCED2[8];   // OBIS 2.8.1
    static const uint8_t SEQ_POWER[8];       // OBIS 16.7.0

    SmlParser() : _ready(false) {
        _buf.reserve(512);
    }

    // Append incoming bytes to the internal buffer and attempt to parse.
    void addBytes(const uint8_t* bytes, int len) {
        for (int i = 0; i < len; i++) {
            _buf.push_back(bytes[i]);
        }
        _tryParse();
    }

    // Returns true when a fully parsed measurement is available.
    bool hasMeasurement() const { return _ready; }

    // Returns the last parsed measurement and resets the ready flag.
    EhZMeasurement getMeasurement() {
        _ready = false;
        return _meas;
    }

    // ----------------------------------------------------------------
    // Static helpers (also used internally)
    // ----------------------------------------------------------------

    // Scan data[startIndex..dataLen-1] for seq.
    // Returns the index of the first byte of the match, or -1 if not found.
    static int searchSequence(const uint8_t* seq, int seqLen,
                              const uint8_t* data, int dataLen,
                              int startIndex) {
        if (seqLen <= 0 || dataLen <= 0) return -1;
        for (int i = startIndex; i <= dataLen - seqLen; i++) {
            bool match = true;
            for (int j = 0; j < seqLen; j++) {
                if (data[i + j] != seq[j]) { match = false; break; }
            }
            if (match) return i;
        }
        return -1;
    }

    // Big-endian byte array to signed 64-bit integer (matches ConvertTo in C#).
    // Supports up to 8 bytes; handles sign extension for values shorter than
    // 8 bytes so that negative current-power readings are decoded correctly.
    static int64_t convertTo(const uint8_t* values, int len) {
        int64_t result = 0;
        for (int i = 0; i < len; i++) {
            result = (result << 8) | values[i];
        }
        // Sign-extend if the most-significant bit of the first byte is set.
        if (len > 0 && len < 8 && (values[0] & 0x80)) {
            result |= (-1LL) << (len * 8);
        }
        return result;
    }

    // Find seq starting from startIndex, skip (offset) bytes after the
    // sequence end, read numBytes, divide by factor → outValue.
    // Returns false if the sequence was not found or there are not enough bytes.
    static bool searchAndParse(const uint8_t* data, int dataLen,
                               int startIndex,
                               const uint8_t* seq, int seqLen,
                               int offset, int numBytes,
                               double factor, double& outValue) {
        int pos = searchSequence(seq, seqLen, data, dataLen, startIndex);
        if (pos < 0) return false;

        int valueStart = pos + seqLen + offset;
        if (valueStart + numBytes > dataLen) return false;

        int64_t raw = convertTo(data + valueStart, numBytes);
        outValue = raw / factor;
        return true;
    }

private:
    std::vector<uint8_t> _buf;
    EhZMeasurement       _meas;
    bool                 _ready;

    void _tryParse() {
        int dataLen = (int)_buf.size();
        if (dataLen < MIN_MESSAGE_BYTES) return;

        const uint8_t* data = _buf.data();

        // Locate start sequence
        int startPos = searchSequence(SEQ_START, sizeof(SEQ_START),
                                      data, dataLen, 0);
        if (startPos < 0) {
            // Discard everything except the last 7 bytes (partial start seq)
            if (dataLen > 7) {
                _buf.erase(_buf.begin(), _buf.begin() + (dataLen - 7));
            }
            return;
        }

        // Locate stop sequence after the start
        int stopPos = searchSequence(SEQ_STOP, sizeof(SEQ_STOP),
                                     data, dataLen,
                                     startPos + (int)sizeof(SEQ_START));
        if (stopPos < 0) return;  // message not yet complete

        int msgEnd = stopPos + (int)sizeof(SEQ_STOP);

        // We have a complete message: data[startPos..msgEnd-1]
        EhZMeasurement m;
        bool ok = true;

        ok &= searchAndParse(data, msgEnd, startPos,
                             SEQ_CONSUMED1, sizeof(SEQ_CONSUMED1),
                             10, 5, 10000.0, m.consumedEnergy1);
        ok &= searchAndParse(data, msgEnd, startPos,
                             SEQ_PRODUCED1, sizeof(SEQ_PRODUCED1),
                             10, 5, 10000.0, m.producedEnergy1);
        ok &= searchAndParse(data, msgEnd, startPos,
                             SEQ_CONSUMED2, sizeof(SEQ_CONSUMED2),
                             7, 5, 10000.0, m.consumedEnergy2);
        ok &= searchAndParse(data, msgEnd, startPos,
                             SEQ_PRODUCED2, sizeof(SEQ_PRODUCED2),
                             7, 5, 10000.0, m.producedEnergy2);
        ok &= searchAndParse(data, msgEnd, startPos,
                             SEQ_POWER, sizeof(SEQ_POWER),
                             7, 4, 10.0,   m.currentPower);

        if (ok) {
            m.valid = true;
            _meas   = m;
            _ready  = true;
        }

        // Consume the bytes up to the end of this message
        _buf.erase(_buf.begin(), _buf.begin() + msgEnd);
    }
};

// Static member definitions require C++17 inline variables.
// The build_flags in platformio.ini set -std=gnu++17.
inline constexpr uint8_t SmlParser::SEQ_START[8]    = { 0x1B, 0x1B, 0x1B, 0x1B,
                                                         0x01, 0x01, 0x01, 0x01 };
inline constexpr uint8_t SmlParser::SEQ_STOP[4]     = { 0x1B, 0x1B, 0x1B, 0x1B };
inline constexpr uint8_t SmlParser::SEQ_CONSUMED1[8]= { 0x77, 0x07, 0x01, 0x00,
                                                         0x01, 0x08, 0x00, 0xFF };
inline constexpr uint8_t SmlParser::SEQ_PRODUCED1[8]= { 0x77, 0x07, 0x01, 0x00,
                                                         0x02, 0x08, 0x00, 0xFF };
inline constexpr uint8_t SmlParser::SEQ_CONSUMED2[8]= { 0x77, 0x07, 0x01, 0x00,
                                                         0x01, 0x08, 0x01, 0xFF };
inline constexpr uint8_t SmlParser::SEQ_PRODUCED2[8]= { 0x77, 0x07, 0x01, 0x00,
                                                         0x02, 0x08, 0x01, 0xFF };
inline constexpr uint8_t SmlParser::SEQ_POWER[8]    = { 0x77, 0x07, 0x01, 0x00,
                                                         0x10, 0x07, 0x00, 0xFF };
