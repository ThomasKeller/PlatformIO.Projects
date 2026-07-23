#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <math.h>
#include "EhZMeasurement.h"

// Header-only, spec-generic SML parser.
// Feed raw bytes from the EHZ meter via addBytes(); poll hasMeasurement()
// and call getMeasurement() once a complete SML message has been decoded.
//
// Rather than assuming fixed byte offsets for each OBIS register (which only
// hold for the one meter telegram they were reverse-engineered from), this
// parser walks the generic SML TLV (Type-Length-Value) encoding: after
// locating an OBIS objName, it steps through the surrounding SML_ListEntry
// [objName, status, valTime, unit, scaler, value, valueSignature], skipping
// the fields it doesn't need and decoding `scaler` + `value` dynamically.
// That makes it work with any SML-conformant meter, not just the one the
// offsets were originally sniffed from.
class SmlParser {
public:
    static const int MIN_MESSAGE_BYTES = 299;

    // OBIS sequences: TL(list, 7 elements) + TL(octet-string, len 6) + 6-byte OBIS code.
    static const uint8_t SEQ_START[8];
    static const uint8_t SEQ_STOP[4];
    static const uint8_t SEQ_CONSUMED[8];   // OBIS 1.8.0
    static const uint8_t SEQ_PRODUCED[8];   // OBIS 2.8.0
    static const uint8_t SEQ_POWER[8];      // OBIS 16.7.0 (optional)

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

    // Diagnostics: how many complete SML telegrams (start+stop framing
    // found) have been seen, how many failed the CRC16 check (dropped/
    // corrupted bytes - e.g. from SoftwareSerial buffer overruns under WiFi
    // load), and how many of the CRC-valid ones actually decoded (consumed
    // AND produced found). A gap between "found" and "valid" while bytes are
    // clearly arriving points at a framing/OBIS mismatch or a noisy link.
    unsigned long telegramsFound() const { return _telegramsFound; }
    unsigned long telegramsCrcFailed() const { return _telegramsCrcFailed; }
    unsigned long telegramsValid() const { return _telegramsValid; }

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
    // Supports up to 8 bytes; always sign-extends based on the MSB, so that
    // negative current-power readings are decoded correctly.
    static int64_t convertTo(const uint8_t* values, int len) {
        return convertToImpl(values, len, /*signExtend=*/true);
    }

    // Finds the SML_ListEntry for `seq` (objName) starting from startIndex,
    // then generically decodes its `scaler` and `value` fields and returns
    // value * 10^scaler. Returns false if the objName isn't found or the
    // entry is malformed/truncated.
    static bool searchAndParse(const uint8_t* data, int dataLen,
                               int startIndex,
                               const uint8_t* seq, int seqLen,
                               double& outValue) {
        int pos = searchSequence(seq, seqLen, data, dataLen, startIndex);
        if (pos < 0) return false;
        pos += seqLen;  // now positioned right after objName, at `status`

        if (!skipElement(data, dataLen, pos)) return false;  // status
        if (!skipElement(data, dataLen, pos)) return false;  // valTime
        if (!skipElement(data, dataLen, pos)) return false;  // unit

        int64_t scalerRaw = 0;
        bool scalerPresent = false;
        if (!readNumeric(data, dataLen, pos, scalerRaw, scalerPresent)) return false;

        int64_t valueRaw = 0;
        bool valuePresent = false;
        if (!readNumeric(data, dataLen, pos, valueRaw, valuePresent)) return false;
        if (!valuePresent) return false;  // value must be present

        double scale = pow(10.0, scalerPresent ? (double)scalerRaw : 0.0);
        outValue = (double)valueRaw * scale;
        return true;
    }

private:
    // SML TL type nibbles (bits 6-4 of the first TL byte).
    static const uint8_t TYPE_OCTET_STRING = 0x00;
    static const uint8_t TYPE_BOOLEAN      = 0x40;
    static const uint8_t TYPE_INTEGER      = 0x50;
    static const uint8_t TYPE_UNSIGNED     = 0x60;
    static const uint8_t TYPE_LIST         = 0x70;

    std::vector<uint8_t> _buf;
    EhZMeasurement       _meas;
    bool                 _ready;
    unsigned long        _telegramsFound = 0;
    unsigned long        _telegramsCrcFailed = 0;
    unsigned long        _telegramsValid = 0;

    // CRC16/X-25 (poly 0x8408 reflected, init 0xFFFF, final XOR 0xFFFF),
    // as used by SML's end-of-transmission trailer. Verified against real
    // capture data: this exact variant, with the trailer's 2 CRC bytes
    // little-endian, matches the meter's own checksum.
    static uint16_t crc16X25(const uint8_t* data, int len) {
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < len; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++) {
                if (crc & 0x0001) crc = (uint16_t)((crc >> 1) ^ 0x8408);
                else crc = (uint16_t)(crc >> 1);
            }
        }
        return (uint16_t)(crc ^ 0xFFFF);
    }

    // Shared implementation behind convertTo()/readNumeric(): only sign-extends
    // when the field is actually a signed SML Integer, so a large Unsigned
    // counter with its MSB set (e.g. a big cumulative Wh reading) isn't
    // misread as negative.
    static int64_t convertToImpl(const uint8_t* values, int len, bool signExtend) {
        int64_t result = 0;
        for (int i = 0; i < len; i++) {
            result = (result << 8) | values[i];
        }
        if (signExtend && len > 0 && len < 8 && (values[0] & 0x80)) {
            result |= (-1LL) << (len * 8);
        }
        return result;
    }

    // Reads one SML TL (type/length) header at data[pos], handling the
    // multi-byte length extension (bit 7 = "more length bytes follow").
    // `length` is the SML-spec length: for lists, the number of child
    // elements; for everything else, the total byte count *including* the
    // TL header itself. `tlBytes` is how many bytes the header itself took.
    static bool readTL(const uint8_t* data, int dataLen, int pos,
                       uint8_t& type, int& length, int& tlBytes) {
        if (pos < 0 || pos >= dataLen) return false;
        int i = pos;
        uint8_t first = data[i];
        type = first & 0x70;
        length = first & 0x0F;
        bool more = (first & 0x80) != 0;
        i++;
        while (more) {
            if (i >= dataLen) return false;
            uint8_t b = data[i];
            length = (length << 4) | (b & 0x0F);
            more = (b & 0x80) != 0;
            i++;
        }
        tlBytes = i - pos;
        return true;
    }

    // Advances `pos` past one full SML element, recursing into lists.
    // Used for fields whose content we don't need (status, valTime, unit).
    static bool skipElement(const uint8_t* data, int dataLen, int& pos) {
        uint8_t type; int length, tlBytes;
        if (!readTL(data, dataLen, pos, type, length, tlBytes)) return false;
        pos += tlBytes;
        if (type == TYPE_LIST) {
            for (int i = 0; i < length; i++) {
                if (!skipElement(data, dataLen, pos)) return false;
            }
        } else {
            int dataBytes = length - tlBytes;
            if (dataBytes < 0 || pos + dataBytes > dataLen) return false;
            pos += dataBytes;
        }
        return true;
    }

    // Reads a numeric (Integer/Unsigned) TLV at `pos` and advances it.
    // SML encodes an absent optional field as a zero-length octet string
    // (single TL byte 0x01); that case sets isPresent=false, outValue=0.
    static bool readNumeric(const uint8_t* data, int dataLen, int& pos,
                            int64_t& outValue, bool& isPresent) {
        uint8_t type; int length, tlBytes;
        if (!readTL(data, dataLen, pos, type, length, tlBytes)) return false;
        int dataBytes = length - tlBytes;
        if (dataBytes < 0 || pos + tlBytes + dataBytes > dataLen) return false;

        if (type == TYPE_OCTET_STRING && dataBytes == 0) {
            isPresent = false;
            outValue = 0;
            pos += tlBytes;
            return true;
        }

        // Only Integer/Unsigned actually encode a number - anything else
        // here (list, boolean, non-empty octet string) means malformed or
        // unexpected input from the meter; reject rather than misinterpret
        // arbitrary bytes as a value. Cap at 8 bytes: convertToImpl()
        // accumulates into an int64_t, and shifting one by more than 8
        // bytes' worth is undefined behavior.
        if ((type != TYPE_INTEGER && type != TYPE_UNSIGNED) || dataBytes > 8) {
            return false;
        }

        isPresent = true;
        outValue = convertToImpl(data + pos + tlBytes, dataBytes, type == TYPE_INTEGER);
        pos += tlBytes + dataBytes;
        return true;
    }

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

        // The stop escape is followed by a fixed 4-byte trailer:
        // 0x1A, a fill-byte count, then a little-endian CRC16 covering
        // everything from SEQ_START through the fill-byte count (inclusive).
        // Wait for all of it to arrive before deciding anything.
        int trailerStart = stopPos + (int)sizeof(SEQ_STOP);
        int msgEnd = trailerStart + 4;
        if (msgEnd > dataLen) return;  // trailer not fully arrived yet

        _telegramsFound++;

        uint16_t embeddedCrc = (uint16_t)(data[trailerStart + 2] | (data[trailerStart + 3] << 8));
        uint16_t computedCrc = crc16X25(data + startPos, (trailerStart + 2) - startPos);

        if (computedCrc != embeddedCrc) {
            // Corrupted telegram (e.g. bytes dropped under WiFi/CPU load) -
            // discard rather than risk decoding garbage as a real reading.
            _telegramsCrcFailed++;
            _buf.erase(_buf.begin(), _buf.begin() + msgEnd);
            return;
        }

        // We have a complete, CRC-valid message: data[startPos..trailerStart-1]
        EhZMeasurement m;
        bool ok = true;

        ok &= searchAndParse(data, trailerStart, startPos,
                             SEQ_CONSUMED, sizeof(SEQ_CONSUMED), m.consumedEnergy);
        ok &= searchAndParse(data, trailerStart, startPos,
                             SEQ_PRODUCED, sizeof(SEQ_PRODUCED), m.producedEnergy);

        // Power is optional: not every meter sends OBIS 16.7.0. Its absence
        // must not invalidate the whole measurement; currentPower simply
        // stays at its default (0.0) if not found.
        searchAndParse(data, trailerStart, startPos,
                       SEQ_POWER, sizeof(SEQ_POWER), m.currentPower);

        if (ok) {
            m.valid = true;
            _meas   = m;
            _ready  = true;
            _telegramsValid++;
        }

        // Consume the bytes up to the end of this message
        _buf.erase(_buf.begin(), _buf.begin() + msgEnd);
    }
};

const uint8_t SmlParser::SEQ_START[8]    = { 0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01 };
const uint8_t SmlParser::SEQ_STOP[4]     = { 0x1B, 0x1B, 0x1B, 0x1B };
const uint8_t SmlParser::SEQ_CONSUMED[8] = { 0x77, 0x07, 0x01, 0x00, 0x01, 0x08, 0x00, 0xFF };
const uint8_t SmlParser::SEQ_PRODUCED[8] = { 0x77, 0x07, 0x01, 0x00, 0x02, 0x08, 0x00, 0xFF };
const uint8_t SmlParser::SEQ_POWER[8]    = { 0x77, 0x07, 0x01, 0x00, 0x10, 0x07, 0x00, 0xFF };
