/**
 * OptaViewModbus.h
 *
 * Modbus RTU master for OptaView. No Arduino or mbed dependency: the serial link is reached
 * through the small MbTransport interface, so the same code runs on the Opta (RS-485 transport
 * in OptaView.ino) and in the host tests (simulated SunSaver in tests/host/optaview).
 *
 * A reply is accepted only when ALL of these hold (review findings F-26 / X-R03):
 *   - its CRC is valid,
 *   - it comes from the slave address that was polled,
 *   - it carries the requested function code (or that function's exception),
 *   - its length matches its own header AND the request (byte count == 2 x registers asked),
 *   - a write acknowledgement repeats the address and value (or quantity) that were sent.
 * An exception reply is reported as MB_EXCEPTION with its code. It is never treated as data
 * and never as "device found".
 *
 * Bytes that arrive before the reply (the Opta's DE-flip artifact showed up as a single 0x00
 * during the April 2026 bring-up) are skipped: the reply is located by slave address, function
 * and CRC, then checked as above. MbResult.noiseBytes says how many were skipped.
 *
 * One thing those checks cannot catch is a reply that arrives after its request was given up:
 * a read reply does not say which registers it holds, so it would pass for the answer to the
 * next read of the same size. After an unanswered request, the next request to the same slave
 * with the same function therefore waits one more timeout and drops what arrives (settleBus).
 * A reply that is later than two timeouts is beyond help: the timeout has to be longer than the
 * slowest reply.
 *
 * Written for C++14 (the Opta core's compiler) and uses no heap, no printf and no exceptions.
 */

#ifndef OPTAVIEW_MODBUS_H
#define OPTAVIEW_MODBUS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace optaview {

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------

enum MbFunction : uint8_t {
  MB_FC_READ_COILS = 0x01,
  MB_FC_READ_DISCRETE = 0x02,
  MB_FC_READ_HOLDING = 0x03,
  MB_FC_READ_INPUT = 0x04,
  MB_FC_WRITE_COIL = 0x05,
  MB_FC_WRITE_REGISTER = 0x06,
  MB_FC_WRITE_REGISTERS = 0x10,
  MB_FC_DEVICE_ID = 0x2B
};

// Serial frame formats the transport can be asked for.
enum MbFormat : uint8_t { MB_8N1 = 0, MB_8N2 = 1, MB_8E1 = 2, MB_8O1 = 3 };

inline const char *mbFormatName(uint8_t format) {
  switch (format) {
    case MB_8N1: return "8N1";
    case MB_8N2: return "8N2";
    case MB_8E1: return "8E1";
    case MB_8O1: return "8O1";
    default: return "?";
  }
}

enum MbStatus : uint8_t {
  MB_OK = 0,
  MB_TIMEOUT,         // nothing was received at all
  MB_CRC,             // a complete-looking reply failed its CRC
  MB_SHORT,           // a reply started and then stopped
  MB_NOISE,           // bytes arrived, but nothing that starts a reply
  MB_WRONG_SLAVE,     // a CRC-valid frame from another slave address
  MB_WRONG_FUNCTION,  // a CRC-valid frame for another function code
  MB_BAD_LENGTH,      // a CRC-valid reply whose size does not match the request
  MB_ECHO_MISMATCH,   // a write was acknowledged with a different address, value or quantity
  MB_EXCEPTION,       // the slave answered with a Modbus exception (see MbResult.exception)
  MB_BAD_REQUEST      // the request was invalid and was never sent
};

inline const char *mbStatusText(uint8_t status) {
  switch (status) {
    case MB_OK: return "ok";
    case MB_TIMEOUT: return "no reply";
    case MB_CRC: return "CRC error";
    case MB_SHORT: return "reply cut short";
    case MB_NOISE: return "noise on the line";
    case MB_WRONG_SLAVE: return "reply from another slave address";
    case MB_WRONG_FUNCTION: return "reply for another function";
    case MB_BAD_LENGTH: return "reply size does not match the request";
    case MB_ECHO_MISMATCH: return "write acknowledged with different data";
    case MB_EXCEPTION: return "exception";
    case MB_BAD_REQUEST: return "invalid request";
    default: return "?";
  }
}

inline const char *mbExceptionText(uint8_t code) {
  switch (code) {
    case 1: return "illegal function";
    case 2: return "illegal data address";
    case 3: return "illegal data value";
    case 4: return "device failure";
    case 5: return "acknowledge";
    case 6: return "device busy";
    case 8: return "memory parity error";
    default: return "unknown exception";
  }
}

// True for a failure of the link itself (worth a retry). False for MB_OK, for an exception
// (the slave answered, it just refused) and for a request that was never sent.
inline bool mbIsLinkFailure(uint8_t status) {
  return status != MB_OK && status != MB_EXCEPTION && status != MB_BAD_REQUEST;
}

static const size_t MB_MAX_FRAME = 256;     // Modbus RTU ADU limit
static const uint16_t MB_MAX_READ_REGISTERS = 125;
static const uint16_t MB_MAX_READ_BITS = 2000;
static const uint16_t MB_MAX_WRITE_REGISTERS = 123;
static const uint32_t MB_GRACE_MS = 400;    // extra time for a reply still arriving at the timeout

// ---------------------------------------------------------------------------------------------
// CRC-16 (Modbus): polynomial 0xA001, initial value 0xFFFF, low byte first on the wire
// ---------------------------------------------------------------------------------------------

inline uint16_t mbCrc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i];
    for (uint8_t shift = 0; shift < 8; ++shift) {
      if (crc & 0x0001) {
        crc = (uint16_t)((crc >> 1) ^ 0xA001);
      } else {
        crc = (uint16_t)(crc >> 1);
      }
    }
  }
  return crc;
}

inline bool mbCrcOk(const uint8_t *frame, size_t len) {
  if (len < 4) {
    return false;
  }
  const uint16_t onWire = (uint16_t)((uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8));
  return onWire == mbCrc16(frame, len - 2);
}

// Appends the CRC to frame[0..len) and returns the new length. The buffer needs 2 spare bytes.
inline size_t mbAppendCrc(uint8_t *frame, size_t len) {
  const uint16_t crc = mbCrc16(frame, len);
  frame[len] = (uint8_t)(crc & 0xFF);
  frame[len + 1] = (uint8_t)(crc >> 8);
  return len + 2;
}

// ---------------------------------------------------------------------------------------------
// What a reply must look like
// ---------------------------------------------------------------------------------------------

enum MbReplyKind : uint8_t {
  MB_REPLY_REGISTERS,  // FC03 / FC04: byte count + 2 bytes per register
  MB_REPLY_BITS,       // FC01 / FC02: byte count + packed bits
  MB_REPLY_ECHO,       // FC05 / FC06 / FC16: 4 bytes that repeat the request
  MB_REPLY_DEVICE_ID,  // FC 0x2B / 0x0E: object list
  MB_REPLY_UNKNOWN     // any other function: the frame ends when the line goes quiet
};

struct MbExpect {
  uint8_t slave;
  uint8_t function;
  MbReplyKind kind;
  uint16_t quantity;  // registers or bits asked for (REGISTERS / BITS)
  uint8_t echo[4];    // bytes 2..5 an ECHO reply must repeat; echo[0] = read code for DEVICE_ID
};

inline bool mbIsExceptionOf(const MbExpect &e, uint8_t functionByte) {
  return (e.function & 0x80) == 0 && functionByte == (uint8_t)(e.function | 0x80);
}

// Length of the reply that starts at p, judged from its own header.
//   > 0  full frame length, CRC included
//     0  cannot tell yet, more bytes are needed
//   < 0  p cannot be the start of the reply
inline int mbReplyLength(const MbExpect &e, const uint8_t *p, size_t available) {
  if (available < 1 || p[0] != e.slave) {
    return -1;
  }
  if (available < 2) {
    return 0;
  }
  if (mbIsExceptionOf(e, p[1])) {
    return 5;
  }
  if (p[1] != e.function) {
    return -1;
  }
  switch (e.kind) {
    case MB_REPLY_REGISTERS:
    case MB_REPLY_BITS: {
      if (available < 3) {
        return 0;
      }
      const size_t total = (size_t)3 + p[2] + 2;
      return total > MB_MAX_FRAME ? -1 : (int)total;
    }
    case MB_REPLY_ECHO:
      return 8;
    case MB_REPLY_DEVICE_ID: {
      // slave, 0x2B, 0x0E, read code, conformity, more follows, next object, object count,
      // then per object: id, length, bytes
      if (available < 3) {
        return 0;
      }
      if (p[2] != 0x0E) {
        return -1;
      }
      if (available < 8) {
        return 0;
      }
      size_t pos = 8;
      for (uint8_t i = 0; i < p[7]; ++i) {
        if (available < pos + 2) {
          return 0;
        }
        pos += (size_t)2 + p[pos + 1];
        if (pos + 2 > MB_MAX_FRAME) {
          return -1;
        }
      }
      return (int)(pos + 2);
    }
    case MB_REPLY_UNKNOWN:
    default:
      return 0;
  }
}

struct MbLocation {
  bool found;
  size_t offset;
  size_t length;
};

// Looks for a complete, CRC-valid reply to `e` anywhere in buf.
inline MbLocation mbLocateReply(const MbExpect &e, const uint8_t *buf, size_t len) {
  MbLocation where = {false, 0, 0};
  for (size_t offset = 0; offset < len; ++offset) {
    const int need = mbReplyLength(e, buf + offset, len - offset);
    if (need <= 0 || offset + (size_t)need > len) {
      continue;
    }
    if (mbCrcOk(buf + offset, (size_t)need)) {
      where.found = true;
      where.offset = offset;
      where.length = (size_t)need;
      return where;
    }
  }
  return where;
}

// For a function with no known reply layout: the frame is whatever runs from a matching
// slave + function pair to the end of the buffer, if its CRC is valid.
inline MbLocation mbLocateUnknownReply(const MbExpect &e, const uint8_t *buf, size_t len) {
  MbLocation where = {false, 0, 0};
  for (size_t offset = 0; offset + 4 <= len; ++offset) {
    if (buf[offset] != e.slave) {
      continue;
    }
    if (buf[offset + 1] != e.function && !mbIsExceptionOf(e, buf[offset + 1])) {
      continue;
    }
    if (mbCrcOk(buf + offset, len - offset)) {
      where.found = true;
      where.offset = offset;
      where.length = len - offset;
      return where;
    }
  }
  return where;
}

// Checks a located, CRC-valid frame against the request.
inline MbStatus mbCheckReply(const MbExpect &e, const uint8_t *frame, size_t len, uint8_t *exception) {
  if (mbIsExceptionOf(e, frame[1])) {
    *exception = frame[2];
    return MB_EXCEPTION;
  }
  switch (e.kind) {
    case MB_REPLY_REGISTERS:
      return (frame[2] == e.quantity * 2u && len == (size_t)5 + e.quantity * 2u) ? MB_OK : MB_BAD_LENGTH;
    case MB_REPLY_BITS:
      return (frame[2] == (e.quantity + 7u) / 8u) ? MB_OK : MB_BAD_LENGTH;
    case MB_REPLY_ECHO:
      return memcmp(frame + 2, e.echo, 4) == 0 ? MB_OK : MB_ECHO_MISMATCH;
    case MB_REPLY_DEVICE_ID:
      return frame[3] == e.echo[0] ? MB_OK : MB_ECHO_MISMATCH;
    case MB_REPLY_UNKNOWN:
    default:
      return MB_OK;
  }
}

// Names the failure when no acceptable reply was found in buf.
inline MbStatus mbClassifyFailure(const MbExpect &e, const uint8_t *buf, size_t len) {
  if (len == 0) {
    return MB_TIMEOUT;
  }
  if (mbCrcOk(buf, len)) {  // a whole valid frame, but not the reply that was asked for
    if (buf[0] != e.slave) {
      return MB_WRONG_SLAVE;
    }
    if (buf[1] != e.function && !mbIsExceptionOf(e, buf[1])) {
      return MB_WRONG_FUNCTION;
    }
    return MB_BAD_LENGTH;
  }
  bool started = false;
  bool complete = false;
  for (size_t offset = 0; offset + 2 <= len; ++offset) {
    if (buf[offset] != e.slave) {
      continue;
    }
    if (buf[offset + 1] != e.function && !mbIsExceptionOf(e, buf[offset + 1])) {
      continue;
    }
    started = true;
    const int need = mbReplyLength(e, buf + offset, len - offset);
    if (need > 0 && offset + (size_t)need <= len) {
      complete = true;
    }
  }
  if (complete) {
    return MB_CRC;
  }
  return started ? MB_SHORT : MB_NOISE;
}

// ---------------------------------------------------------------------------------------------
// Transport and results
// ---------------------------------------------------------------------------------------------

// The half-duplex serial link. OptaView.ino implements it over ArduinoRS485 with the TX
// bracket that survived the 2026 SunSaver bring-up; the tests implement it with a simulator.
class MbTransport {
 public:
  virtual ~MbTransport() {}
  // (Re)opens the link. Returns false when the baud rate or format is not supported.
  virtual bool configure(uint32_t baud, uint8_t format) = 0;
  // Sends one frame. Returns once the last stop bit has left and the receiver is listening.
  virtual void send(const uint8_t *data, size_t len) = 0;
  // Next received byte, or -1 when none is waiting.
  virtual int read() = 0;
  // Monotonic milliseconds; may wrap like millis().
  virtual uint32_t nowMs() = 0;
  // Called while the master waits. Time must advance across calls.
  virtual void idle() {}
};

struct MbResult {
  MbStatus status;
  uint8_t exception;    // exception code when status == MB_EXCEPTION
  uint8_t noiseBytes;   // bytes skipped in front of the reply
  uint16_t elapsedMs;   // from the end of the request to the reply (or to giving up)
};

struct MbStats {
  uint32_t sent;
  uint32_t ok;
  uint32_t timeouts;
  uint32_t crcErrors;
  uint32_t exceptions;
  uint32_t otherErrors;
  // Waits after an unanswered request during which something did arrive: late replies, most likely.
  uint32_t lateArrivals;
};

struct MbDeviceIdObject {
  uint8_t id;
  char text[40];
};

struct MbDeviceId {
  uint8_t count;
  bool moreFollows;
  MbDeviceIdObject objects[6];
};

// Receives every transaction after it finishes (the console's "trace on").
class MbTraceSink {
 public:
  virtual ~MbTraceSink() {}
  virtual void onTransaction(const uint8_t *tx, size_t txLen, const uint8_t *rx, size_t rxLen,
                             const MbResult &result) = 0;
};

// ---------------------------------------------------------------------------------------------
// The master
// ---------------------------------------------------------------------------------------------

class ModbusMaster {
 public:
  explicit ModbusMaster(MbTransport &transport)
      : responseTimeoutMs(1000),
        quietMs(50),
        interFrameGapMs(5),
        trace(nullptr),
        transport_(transport),
        lastTxLen_(0),
        rxLen_(0),
        frameOffset_(0),
        frameLen_(0),
        lastActivityMs_(0),
        haveActivity_(false),
        lateOpen_(false),
        lateAny_(false),
        lateSlave_(0),
        lateFunction_(0),
        lateSinceMs_(0),
        lateWindowMs_(0) {
    memset(&stats, 0, sizeof(stats));
    memset(lastTx_, 0, sizeof(lastTx_));
    memset(rx_, 0, sizeof(rx_));
  }

  // How long to wait for a reply to start and finish. A reply that comes later than that is
  // dropped if it arrives within one more timeout (see settleBus). Later still, nothing can tell
  // it from the answer to the next request, so this must be longer than the slowest reply.
  uint16_t responseTimeoutMs;
  uint16_t quietMs;            // silence that ends a reply of unknown length
  uint16_t interFrameGapMs;    // minimum bus silence before a request (3.5 characters is 4 ms at 9600)
  MbStats stats;
  MbTraceSink *trace;

  MbTransport &transport() { return transport_; }

  // FC03 / FC04. out receives `quantity` registers.
  MbResult readRegisters(uint8_t slave, uint8_t function, uint16_t address, uint16_t quantity,
                         uint16_t *out) {
    if (!validSlave(slave) || (function != MB_FC_READ_HOLDING && function != MB_FC_READ_INPUT) ||
        quantity < 1 || quantity > MB_MAX_READ_REGISTERS || out == nullptr ||
        (uint32_t)address + quantity > 0x10000UL) {
      return badRequest();
    }
    uint8_t tx[8];
    buildSimple(tx, slave, function, address, quantity);
    MbExpect e = makeExpect(slave, function, MB_REPLY_REGISTERS, quantity);
    MbResult r = transact(tx, sizeof(tx), e);
    if (r.status == MB_OK) {
      const uint8_t *f = frame();
      for (uint16_t i = 0; i < quantity; ++i) {
        out[i] = (uint16_t)(((uint16_t)f[3 + 2 * i] << 8) | f[4 + 2 * i]);
      }
    }
    return r;
  }

  // FC01 / FC02. out receives one byte (0 or 1) per bit.
  MbResult readBits(uint8_t slave, uint8_t function, uint16_t address, uint16_t quantity, uint8_t *out) {
    if (!validSlave(slave) || (function != MB_FC_READ_COILS && function != MB_FC_READ_DISCRETE) ||
        quantity < 1 || quantity > MB_MAX_READ_BITS || out == nullptr ||
        (uint32_t)address + quantity > 0x10000UL) {
      return badRequest();
    }
    uint8_t tx[8];
    buildSimple(tx, slave, function, address, quantity);
    MbExpect e = makeExpect(slave, function, MB_REPLY_BITS, quantity);
    MbResult r = transact(tx, sizeof(tx), e);
    if (r.status == MB_OK) {
      const uint8_t *f = frame();
      for (uint16_t i = 0; i < quantity; ++i) {
        out[i] = (uint8_t)((f[3 + i / 8] >> (i % 8)) & 0x01);
      }
    }
    return r;
  }

  // FC06. The reply must repeat the address and the value.
  MbResult writeRegister(uint8_t slave, uint16_t address, uint16_t value) {
    return writeSingle(slave, MB_FC_WRITE_REGISTER, address, value);
  }

  // FC05. The reply must repeat the address and the value (0xFF00 = on, 0x0000 = off).
  MbResult writeCoil(uint8_t slave, uint16_t address, bool on) {
    return writeSingle(slave, MB_FC_WRITE_COIL, address, on ? 0xFF00 : 0x0000);
  }

  // FC05 or FC06 with the 16-bit value field given as-is.
  MbResult writeSingle(uint8_t slave, uint8_t function, uint16_t address, uint16_t value) {
    if (!validSlave(slave) || (function != MB_FC_WRITE_COIL && function != MB_FC_WRITE_REGISTER)) {
      return badRequest();
    }
    uint8_t tx[8];
    buildSimple(tx, slave, function, address, value);
    MbExpect e = makeExpect(slave, function, MB_REPLY_ECHO, 0);
    memcpy(e.echo, tx + 2, 4);
    return transact(tx, sizeof(tx), e);
  }

  // FC16. The reply must repeat the address and the quantity. The SunSaver MPPT specification
  // does not list this function; it is here for other devices.
  MbResult writeRegisters(uint8_t slave, uint16_t address, uint16_t quantity, const uint16_t *values) {
    if (!validSlave(slave) || quantity < 1 || quantity > MB_MAX_WRITE_REGISTERS || values == nullptr ||
        (uint32_t)address + quantity > 0x10000UL) {
      return badRequest();
    }
    uint8_t tx[MB_MAX_FRAME];
    tx[0] = slave;
    tx[1] = MB_FC_WRITE_REGISTERS;
    tx[2] = (uint8_t)(address >> 8);
    tx[3] = (uint8_t)(address & 0xFF);
    tx[4] = (uint8_t)(quantity >> 8);
    tx[5] = (uint8_t)(quantity & 0xFF);
    tx[6] = (uint8_t)(quantity * 2);
    for (uint16_t i = 0; i < quantity; ++i) {
      tx[7 + 2 * i] = (uint8_t)(values[i] >> 8);
      tx[8 + 2 * i] = (uint8_t)(values[i] & 0xFF);
    }
    const size_t len = mbAppendCrc(tx, (size_t)7 + quantity * 2u);
    MbExpect e = makeExpect(slave, MB_FC_WRITE_REGISTERS, MB_REPLY_ECHO, 0);
    memcpy(e.echo, tx + 2, 4);
    return transact(tx, len, e);
  }

  // FC 0x2B / MEI 0x0E, read code 0x01 (basic identification, stream access) from object 0.
  MbResult readDeviceId(uint8_t slave, MbDeviceId &out) {
    memset(&out, 0, sizeof(out));
    if (!validSlave(slave)) {
      return badRequest();
    }
    uint8_t tx[7];
    tx[0] = slave;
    tx[1] = MB_FC_DEVICE_ID;
    tx[2] = 0x0E;
    tx[3] = 0x01;
    tx[4] = 0x00;
    mbAppendCrc(tx, 5);
    MbExpect e = makeExpect(slave, MB_FC_DEVICE_ID, MB_REPLY_DEVICE_ID, 0);
    e.echo[0] = 0x01;
    MbResult r = transact(tx, sizeof(tx), e);
    if (r.status == MB_OK) {
      const uint8_t *f = frame();
      out.moreFollows = f[5] != 0;
      size_t pos = 8;
      const size_t capacity = sizeof(out.objects) / sizeof(out.objects[0]);
      for (uint8_t i = 0; i < f[7]; ++i) {
        const uint8_t id = f[pos];
        const uint8_t n = f[pos + 1];
        if (out.count < capacity) {
          MbDeviceIdObject &object = out.objects[out.count++];
          object.id = id;
          size_t copied = 0;
          for (; copied < n && copied < sizeof(object.text) - 1; ++copied) {
            const uint8_t c = f[pos + 2 + copied];
            object.text[copied] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
          }
          object.text[copied] = '\0';
        }
        pos += (size_t)2 + n;
      }
    }
    return r;
  }

  // Any other request. body = slave, function, data (no CRC), 2..252 bytes. The reply is taken
  // as complete when the line has been quiet for quietMs, and only its CRC, slave address and
  // function code can be checked.
  MbResult rawRequest(const uint8_t *body, size_t bodyLen) {
    if (body == nullptr || bodyLen < 2 || bodyLen > MB_MAX_FRAME - 2 || !validSlave(body[0])) {
      return badRequest();
    }
    uint8_t tx[MB_MAX_FRAME];
    memcpy(tx, body, bodyLen);
    const size_t len = mbAppendCrc(tx, bodyLen);
    MbExpect e = makeExpect(body[0], body[1], MB_REPLY_UNKNOWN, 0);
    return transact(tx, len, e);
  }

  // The request and everything received during the last transaction (noise included).
  const uint8_t *lastTx() const { return lastTx_; }
  size_t lastTxLen() const { return lastTxLen_; }
  const uint8_t *lastRx() const { return rx_; }
  size_t lastRxLen() const { return rxLen_; }
  // The accepted reply frame inside lastRx(), or length 0 when there was none.
  const uint8_t *frame() const { return rx_ + frameOffset_; }
  size_t frameLen() const { return frameLen_; }

  void resetStats() { memset(&stats, 0, sizeof(stats)); }

 private:
  MbTransport &transport_;
  uint8_t lastTx_[MB_MAX_FRAME];
  size_t lastTxLen_;
  uint8_t rx_[MB_MAX_FRAME + 8];
  size_t rxLen_;
  size_t frameOffset_;
  size_t frameLen_;
  uint32_t lastActivityMs_;
  bool haveActivity_;
  // The last request that went unanswered, while its reply could still arrive (see settleBus).
  bool lateOpen_;
  bool lateAny_;          // more than one such request, to different slaves or functions
  uint8_t lateSlave_;
  uint8_t lateFunction_;
  uint32_t lateSinceMs_;  // when the master gave up on it
  uint32_t lateWindowMs_; // how long after that its reply is still expected

  static bool validSlave(uint8_t slave) { return slave >= 1 && slave <= 247; }

  static void buildSimple(uint8_t *tx, uint8_t slave, uint8_t function, uint16_t address, uint16_t value) {
    tx[0] = slave;
    tx[1] = function;
    tx[2] = (uint8_t)(address >> 8);
    tx[3] = (uint8_t)(address & 0xFF);
    tx[4] = (uint8_t)(value >> 8);
    tx[5] = (uint8_t)(value & 0xFF);
    mbAppendCrc(tx, 6);
  }

  static MbExpect makeExpect(uint8_t slave, uint8_t function, MbReplyKind kind, uint16_t quantity) {
    MbExpect e;
    e.slave = slave;
    e.function = function;
    e.kind = kind;
    e.quantity = quantity;
    memset(e.echo, 0, sizeof(e.echo));
    return e;
  }

  MbResult badRequest() {
    MbResult r = {MB_BAD_REQUEST, 0, 0, 0};
    lastTxLen_ = 0;
    rxLen_ = 0;
    frameOffset_ = 0;
    frameLen_ = 0;
    return r;
  }

  // Gets the bus ready for a request to e.slave with e.function.
  //
  // A read reply carries no address, so a reply that arrives after its request was given up
  // would pass for the answer to the next read of the same size, and spoil one of another size.
  // After a request that went unanswered, the next request to the same slave with the same
  // function therefore waits until one more timeout has passed since then, and everything that
  // arrives in that time is dropped. Requests to other slaves or with other functions do not
  // wait: a late reply cannot be mistaken for theirs.
  void settleBus(const MbExpect &e) {
    if (lateOpen_) {
      const uint32_t began = transport_.nowMs();
      const uint32_t since = (uint32_t)(began - lateSinceMs_);
      if (since >= lateWindowMs_) {
        lateOpen_ = false;
      } else if (lateAny_ || (e.slave == lateSlave_ && e.function == lateFunction_)) {
        const uint32_t guardMs = lateWindowMs_ - since;
        uint32_t lastDrop = began;
        bool dropped = false;
        for (;;) {
          bool got = false;
          while (transport_.read() >= 0) {
            got = true;
          }
          const uint32_t now = transport_.nowMs();
          if (got) {
            dropped = true;
            lastDrop = now;
          }
          const uint32_t waited = (uint32_t)(now - began);
          // Done when the window is over, unless a late frame is still coming in. Noise that
          // never stops must not hold the master for ever.
          const bool arriving = dropped && (uint32_t)(now - lastDrop) < quietMs;
          if (waited >= guardMs && (!arriving || waited >= guardMs + MB_GRACE_MS)) {
            break;
          }
          transport_.idle();
        }
        lateOpen_ = false;
        if (dropped) {
          ++stats.lateArrivals;
        }
      }
    }

    // Leave the bus quiet for a moment after the previous frame, then drop whatever is waiting:
    // a stale byte must not be taken for the start of the answer to this request.
    if (haveActivity_) {
      while ((uint32_t)(transport_.nowMs() - lastActivityMs_) < interFrameGapMs) {
        transport_.idle();
      }
    }
    while (transport_.read() >= 0) {
    }
  }

  // A request got no proper reply. Its reply may still arrive, for one more timeout from now.
  void noteUnanswered(const MbExpect &e, uint32_t now) {
    uint32_t window = responseTimeoutMs;
    bool any = false;
    if (lateOpen_) {
      const uint32_t since = (uint32_t)(now - lateSinceMs_);
      if (since < lateWindowMs_) {
        // The reply to an earlier request can still arrive as well. One record cannot name
        // both, so until both windows are over every request waits.
        any = lateAny_ || e.slave != lateSlave_ || e.function != lateFunction_;
        if (lateWindowMs_ - since > window) {
          window = lateWindowMs_ - since;
        }
      }
    }
    lateOpen_ = true;
    lateAny_ = any;
    lateSlave_ = e.slave;
    lateFunction_ = e.function;
    lateSinceMs_ = now;
    lateWindowMs_ = window;
  }

  MbResult transact(const uint8_t *tx, size_t txLen, const MbExpect &e) {
    MbResult r = {MB_TIMEOUT, 0, 0, 0};

    settleBus(e);

    memcpy(lastTx_, tx, txLen);
    lastTxLen_ = txLen;
    rxLen_ = 0;
    frameOffset_ = 0;
    frameLen_ = 0;

    transport_.send(tx, txLen);
    ++stats.sent;

    const uint32_t start = transport_.nowMs();
    uint32_t lastByte = start;
    MbLocation where = {false, 0, 0};
    for (;;) {
      bool got = false;
      int b;
      while (rxLen_ < sizeof(rx_) && (b = transport_.read()) >= 0) {
        rx_[rxLen_++] = (uint8_t)b;
        got = true;
      }
      const uint32_t now = transport_.nowMs();
      if (got) {
        lastByte = now;
        if (e.kind != MB_REPLY_UNKNOWN) {
          where = mbLocateReply(e, rx_, rxLen_);
          if (where.found) {
            break;
          }
        }
      }
      const uint32_t elapsed = (uint32_t)(now - start);
      const uint32_t quiet = (uint32_t)(now - lastByte);
      if (rxLen_ == 0) {
        if (elapsed >= responseTimeoutMs) {
          break;
        }
      } else if (rxLen_ >= sizeof(rx_)) {
        break;
      } else if (quiet >= quietMs && (e.kind == MB_REPLY_UNKNOWN || elapsed >= responseTimeoutMs)) {
        break;
      } else if (elapsed >= (uint32_t)responseTimeoutMs + MB_GRACE_MS) {
        break;
      }
      transport_.idle();
    }

    const uint32_t end = transport_.nowMs();
    lastActivityMs_ = end;
    haveActivity_ = true;
    const uint32_t took = (uint32_t)(end - start);
    r.elapsedMs = took > 0xFFFF ? 0xFFFF : (uint16_t)took;

    if (!where.found && e.kind == MB_REPLY_UNKNOWN) {
      where = mbLocateUnknownReply(e, rx_, rxLen_);
    }
    if (where.found) {
      frameOffset_ = where.offset;
      frameLen_ = where.length;
      r.noiseBytes = where.offset > 255 ? 255 : (uint8_t)where.offset;
      r.status = mbCheckReply(e, rx_ + where.offset, where.length, &r.exception);
    } else {
      r.status = mbClassifyFailure(e, rx_, rxLen_);
    }

    if (mbIsLinkFailure(r.status)) {
      noteUnanswered(e, end);
    }

    switch (r.status) {
      case MB_OK: ++stats.ok; break;
      case MB_TIMEOUT: ++stats.timeouts; break;
      case MB_CRC: ++stats.crcErrors; break;
      case MB_EXCEPTION: ++stats.exceptions; break;
      default: ++stats.otherErrors; break;
    }
    if (trace != nullptr) {
      trace->onTransaction(lastTx_, lastTxLen_, rx_, rxLen_, r);
    }
    return r;
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_MODBUS_H
