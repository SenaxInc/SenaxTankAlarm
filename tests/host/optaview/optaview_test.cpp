// Host tests for OptaView (OptaView/*.h): the Modbus master, the profile loader and codec, the
// device layer and the console, all run against a simulated SunSaver MPPT (sim_sunsaver.h).
//
//   make test ARDUINOJSON_DIR=<ArduinoJson v7.4.3>/src
//
// The code under test is compiled as C++14, the dialect of the Opta's compiler.

#include "test_support.h"

// ============================================================================
// Modbus master
// ============================================================================
static void testCrcAndFrames() {
  // The request and reply captured on the bench on 2026-07-03.
  const uint8_t request[] = {0x01, 0x04, 0x00, 0x08, 0x00, 0x01};
  CHECK_EQ(mbCrc16(request, sizeof(request)), 0x08B0);  // B0 08 on the wire
  uint8_t reply[] = {0x01, 0x04, 0x02, 0x11, 0x83, 0xF4, 0xC1};
  CHECK(mbCrcOk(reply, sizeof(reply)));
  CHECK(!mbCrcOk(reply, 3));
  reply[3] ^= 0x01;
  CHECK(!mbCrcOk(reply, sizeof(reply)));
  // Either byte of the CRC alone makes a frame bad.
  const uint8_t lowWrong[] = {0x01, 0x04, 0x02, 0x11, 0x83, 0xF5, 0xC1};
  const uint8_t highWrong[] = {0x01, 0x04, 0x02, 0x11, 0x83, 0xF4, 0xC0};
  CHECK(!mbCrcOk(lowWrong, sizeof(lowWrong)));
  CHECK(!mbCrcOk(highWrong, sizeof(highWrong)));
  uint8_t buffer[8] = {0x01, 0x04, 0x00, 0x08, 0x00, 0x01, 0, 0};
  CHECK_EQ(mbAppendCrc(buffer, 6), 8);
  CHECK_EQ(buffer[6], 0xB0);
  CHECK_EQ(buffer[7], 0x08);

  Bare b;
  uint16_t word = 0;
  MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
  CHECK_EQ(r.status, MB_OK);
  CHECK_EQ(word, 0x1183);
  CHECK(b.sim.requests.back() == bytes({0x01, 0x04, 0x00, 0x08, 0x00, 0x01, 0xB0, 0x08}));
  CHECK_EQ(b.master.lastRxLen(), 7);
  CHECK(memcmp(b.master.lastRx(), "\x01\x04\x02\x11\x83\xF4\xC1", 7) == 0);
  CHECK_EQ(b.master.frameLen(), 7);
  CHECK_EQ(r.noiseBytes, 0);

  // Function 03 reads the same table.
  word = 0;
  r = b.master.readRegisters(1, MB_FC_READ_HOLDING, 0x0008, 1, &word);
  CHECK_EQ(r.status, MB_OK);
  CHECK_EQ(word, 0x1183);
  CHECK_EQ(b.sim.requests.back()[1], 0x03);

  // A block: registers come back in order, high byte first.
  uint16_t block[5] = {0, 0, 0, 0, 0};
  r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 5, block);
  CHECK_EQ(r.status, MB_OK);
  for (uint16_t i = 0; i < 5; ++i) {
    CHECK_EQ(block[i], b.sim.regs[(uint16_t)(0x0008 + i)]);
  }

  // Function 06: address then value, and the acknowledgement must repeat them.
  r = b.master.writeRegister(1, 0xE022, 0x10C5);
  CHECK_EQ(r.status, MB_OK);
  CHECK(b.sim.requests.back() == SimSunSaver::frame(bytes({0x01, 0x06, 0xE0, 0x22, 0x10, 0xC5})));
  CHECK_EQ(b.sim.regs[0xE022], 0x10C5);

  // Function 05: FF00 switches on, 0000 off.
  r = b.master.writeCoil(1, 0x0001, true);
  CHECK_EQ(r.status, MB_OK);
  CHECK(b.sim.requests.back() == SimSunSaver::frame(bytes({0x01, 0x05, 0x00, 0x01, 0xFF, 0x00})));
  CHECK(b.sim.coils[0x0001]);
  r = b.master.writeCoil(1, 0x0001, false);
  CHECK_EQ(r.status, MB_OK);
  CHECK(b.sim.requests.back() == SimSunSaver::frame(bytes({0x01, 0x05, 0x00, 0x01, 0x00, 0x00})));
  CHECK(!b.sim.coils[0x0001]);

  // Function 16 is framed correctly; the SunSaver answers it with "illegal function".
  const uint16_t values[2] = {0x1111, 0x2222};
  r = b.master.writeRegisters(1, 0xE022, 2, values);
  CHECK(b.sim.requests.back() ==
        SimSunSaver::frame(bytes({0x01, 0x10, 0xE0, 0x22, 0x00, 0x02, 0x04, 0x11, 0x11, 0x22, 0x22})));
  CHECK_EQ(r.status, MB_EXCEPTION);
  CHECK_EQ(r.exception, 1);
  CHECK_EQ(b.sim.regs[0xE022], 0x10C5);

  // Requests that make no sense are never sent.
  const size_t before = b.sim.requests.size();
  CHECK_EQ(b.master.readRegisters(0, MB_FC_READ_INPUT, 8, 1, &word).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.readRegisters(248, MB_FC_READ_INPUT, 8, 1, &word).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 8, 0, &word).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 8, 126, &word).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0xFFFF, 2, &word).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 8, 1, nullptr).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.readRegisters(1, MB_FC_WRITE_REGISTER, 8, 1, &word).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.writeSingle(1, MB_FC_READ_INPUT, 8, 1).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.writeRegisters(1, 8, 124, values).status, MB_BAD_REQUEST);
  CHECK_EQ(b.master.rawRequest(nullptr, 4).status, MB_BAD_REQUEST);
  CHECK_EQ(b.sim.requests.size(), before);
  CHECK_EQ(b.master.lastTxLen(), 0);
}

static void testReplyValidation() {
  // Every way a reply can be wrong (review finding F-26): none of them is taken for data.
  struct Case {
    SimSunSaver::Fault fault;
    MbStatus expected;
    const char *name;
  };
  const Case cases[] = {
      {SimSunSaver::FAULT_BAD_CRC, MB_CRC, "bad CRC"},
      {SimSunSaver::FAULT_TRUNCATE, MB_SHORT, "cut short"},
      {SimSunSaver::FAULT_WRONG_SLAVE, MB_WRONG_SLAVE, "another slave"},
      {SimSunSaver::FAULT_WRONG_FUNCTION, MB_WRONG_FUNCTION, "another function"},
      {SimSunSaver::FAULT_ONE_REGISTER, MB_BAD_LENGTH, "one register for five, valid CRC"},
      {SimSunSaver::FAULT_EXTRA_REGISTER, MB_BAD_LENGTH, "six registers for five, valid CRC"},
      {SimSunSaver::FAULT_GARBAGE, MB_NOISE, "garbage"},
      {SimSunSaver::FAULT_NO_REPLY, MB_TIMEOUT, "nothing"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    Bare b;
    uint16_t words[5] = {0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA};
    b.sim.failNext(cases[i].fault);
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 5, words);
    CHECK_MSG(r.status == cases[i].expected, "%s: got status %d (%s)", cases[i].name, (int)r.status,
              mbStatusText(r.status));
    for (int w = 0; w < 5; ++w) {
      CHECK_MSG(words[w] == 0xAAAA, "%s: the output buffer was written", cases[i].name);
    }
    CHECK_EQ(b.master.frameLen(), cases[i].expected == MB_BAD_LENGTH ? b.master.lastRxLen() : 0);
    CHECK_EQ(b.master.stats.sent, 1);
    CHECK_EQ(b.master.stats.ok, 0);
    // The next request works again.
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 5, words).status, MB_OK);
    CHECK_EQ(words[0], 0x1183);
  }

  {  // An exception is reported with its code and is not data.
    Bare b;
    uint16_t word = 0xAAAA;
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0035, 1, &word);
    CHECK_EQ(r.status, MB_EXCEPTION);
    CHECK_EQ(r.exception, 2);
    CHECK_EQ(word, 0xAAAA);
    CHECK_EQ(b.master.stats.exceptions, 1);
    CHECK(!mbIsLinkFailure(r.status));
  }
  {  // The simulated controller has the specification's addresses and no others. A reserved one
     // in the middle of the EEPROM is refused like the gap after the RAM registers: alone, in a
     // block that touches it, and for a write. (A profile that maps a register there fails
     // testSpans, and its reads fail.)
    Bare b;
    const uint16_t reserved[] = {0xE00B, 0xE00C, 0xE018, 0xE019, 0xE020, 0xE021,
                                 0xE028, 0xE02F, 0xE039, 0xE03F, 0xE04E, 0xE050};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
      uint16_t word = 0xAAAA;
      const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, reserved[i], 1, &word);
      CHECK_MSG(r.status == MB_EXCEPTION && r.exception == 2, "0x%04X can be read", (unsigned)reserved[i]);
      const MbResult w = b.master.writeRegister(1, reserved[i], 5);
      CHECK_MSG(w.status == MB_EXCEPTION && w.exception == 2, "0x%04X can be written", (unsigned)reserved[i]);
      CHECK_EQ(b.sim.regs.count(reserved[i]), 0);
    }
    uint16_t words[4];
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0xE009, 4, words).status, MB_EXCEPTION);
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0xE009, 2, words).status, MB_OK);
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0xE00D, 4, words).status, MB_OK);
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0xE04F, 1, words).status, MB_OK);
  }
  {  // A write acknowledged with another value is not a success.
    Bare b;
    b.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    CHECK_EQ(b.master.writeRegister(1, 0xE022, 0x10C5).status, MB_ECHO_MISMATCH);
    CHECK_EQ(b.sim.regs[0xE022], 0x10C5);  // the controller stored it all the same
    b.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    CHECK_EQ(b.master.writeCoil(1, 0x0001, true).status, MB_ECHO_MISMATCH);
  }
  {  // The acknowledgement has to repeat all four bytes, the address as well as the value: an
     // acknowledgement for another register is not this write's.
    MbExpect e;
    e.slave = 1;
    e.function = MB_FC_WRITE_REGISTER;
    e.kind = MB_REPLY_ECHO;
    e.quantity = 0;
    const uint8_t sent[4] = {0xE0, 0x22, 0x10, 0xC5};
    memcpy(e.echo, sent, sizeof(sent));
    uint8_t code = 0;
    uint8_t ack[8] = {0x01, 0x06, 0xE0, 0x22, 0x10, 0xC5, 0, 0};
    CHECK_EQ(mbCheckReply(e, ack, sizeof(ack), &code), MB_OK);
    for (size_t i = 2; i < 6; ++i) {
      ack[i] ^= 0x01;
      CHECK_MSG(mbCheckReply(e, ack, sizeof(ack), &code) == MB_ECHO_MISMATCH,
                "byte %u of the acknowledgement is not compared", (unsigned)i);
      ack[i] ^= 0x01;
    }
    // On the wire: the write is lost, and what arrives acknowledges the register next to it.
    Bare b;
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    b.sim.inject(20, SimSunSaver::frame(bytes({0x01, 0x06, 0xE0, 0x23, 0x10, 0xC5})));
    CHECK_EQ(b.master.writeRegister(1, 0xE022, 0x10C5).status, MB_ECHO_MISMATCH);
  }
  {  // A reply to a read of bits holds as many bytes as the bits that were asked for need: one
     // byte too many or too few is not taken, valid CRC or not.
    Bare more;
    uint8_t bits[9] = {9, 9, 9, 9, 9, 9, 9, 9, 9};
    more.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    more.sim.inject(20, SimSunSaver::frame(bytes({0x01, 0x01, 0x02, 0xFF, 0xFF})));
    CHECK_EQ(more.master.readBits(1, MB_FC_READ_COILS, 0x0000, 8, bits).status, MB_BAD_LENGTH);
    Bare fewer;
    fewer.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    fewer.sim.inject(20, SimSunSaver::frame(bytes({0x01, 0x01, 0x01, 0xFF})));
    CHECK_EQ(fewer.master.readBits(1, MB_FC_READ_COILS, 0x0000, 9, bits).status, MB_BAD_LENGTH);
    for (size_t i = 0; i < sizeof(bits); ++i) {
      CHECK_EQ(bits[i], 9);
    }
  }
  {  // The identification that comes back has to be the kind that was asked for (read code 1).
    Bare b;
    MbDeviceId id;
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    b.sim.inject(20, SimSunSaver::frame(bytes(
                         {0x01, 0x2B, 0x0E, 0x02, 0x01, 0x00, 0x00, 0x01, 0x00, 0x03, 0x61, 0x62, 0x63})));
    CHECK_EQ(b.master.readDeviceId(1, id).status, MB_ECHO_MISMATCH);
    CHECK_EQ(id.count, 0);
  }
  {  // That fault has nothing to alter in a refusal, which is five bytes long and echoes no
     // value: the refusal goes out as it is, and the simulator does not reach past its end
     // (the sanitizer build watches for that). Each one still uses the fault up.
    Bare b;
    b.sim.refuseWrites = true;
    b.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    MbResult r = b.master.writeRegister(1, 0xE022, 0x10C5);
    CHECK_EQ(r.status, MB_EXCEPTION);
    CHECK_EQ(r.exception, 4);
    b.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    r = b.master.writeRegister(1, 0x0008, 1);  // not a setting
    CHECK_EQ(r.status, MB_EXCEPTION);
    CHECK_EQ(r.exception, 2);
    b.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    r = b.master.writeCoil(1, 0x0005, true);  // no such coil
    CHECK_EQ(r.status, MB_EXCEPTION);
    CHECK_EQ(r.exception, 2);
    b.sim.refuseWrites = false;
    CHECK_EQ(b.master.writeRegister(1, 0xE022, 0x10C5).status, MB_OK);
    // Nor in the reply to a read, which echoes nothing either: its data arrives as it is.
    uint16_t words[2] = {0, 0};
    b.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 2, words).status, MB_OK);
    CHECK_EQ(words[0], 0x1183);
    CHECK_EQ(words[1], b.sim.regs[0x0009]);
    uint8_t bits[1] = {9};
    b.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    CHECK_EQ(b.master.readBits(1, MB_FC_READ_COILS, 0x0000, 1, bits).status, MB_OK);
    CHECK_EQ(bits[0], 0);
  }
  {  // The Opta's DE-flip artifact: zero bytes in front of the reply are skipped and counted.
    for (uint8_t noise = 1; noise <= 3; ++noise) {
      Bare b;
      b.sim.leadingNoiseBytes = noise;
      uint16_t word = 0;
      const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
      CHECK_EQ(r.status, MB_OK);
      CHECK_EQ(r.noiseBytes, noise);
      CHECK_EQ(word, 0x1183);
      CHECK_EQ(b.master.lastRxLen(), 7u + noise);
      CHECK_EQ(b.master.frameLen(), 7);
    }
  }
  {  // Noise that looks like the start of a reply does not hide the real one behind it.
    Bare b;
    b.sim.inject(12, bytes({0x01, 0x04}));
    uint16_t word = 0;
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    CHECK_EQ(r.status, MB_OK);
    CHECK_EQ(r.noiseBytes, 2);
    CHECK_EQ(word, 0x1183);
  }
  {  // The request echoed back (some adapters do that) in front of the reply.
    Bare b;
    b.sim.inject(9, bytes({0x01, 0x04, 0x00, 0x08, 0x00, 0x01, 0xB0, 0x08}));
    uint16_t word = 0;
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    CHECK_EQ(r.status, MB_OK);
    CHECK_EQ(r.noiseBytes, 8);
    CHECK_EQ(word, 0x1183);
  }
  {  // Classification of buffers, without a transport.
    MbExpect e;
    e.slave = 1;
    e.function = 4;
    e.kind = MB_REPLY_REGISTERS;
    e.quantity = 1;
    memset(e.echo, 0, sizeof(e.echo));
    const uint8_t good[] = {0x01, 0x04, 0x02, 0x11, 0x83, 0xF4, 0xC1};
    MbLocation where = mbLocateReply(e, good, sizeof(good));
    CHECK(where.found);
    CHECK_EQ(where.offset, 0);
    CHECK_EQ(where.length, 7);
    where = mbLocateReply(e, good, sizeof(good) - 1);
    CHECK(!where.found);
    CHECK_EQ(mbReplyLength(e, good, 1), 0);
    CHECK_EQ(mbReplyLength(e, good, 2), 0);
    CHECK_EQ(mbReplyLength(e, good, 3), 7);
    const uint8_t other[] = {0x02, 0x04};
    CHECK(mbReplyLength(e, other, 2) < 0);
    const uint8_t exception[] = {0x01, 0x84, 0x02, 0xC2, 0xC1};
    CHECK_EQ(mbReplyLength(e, exception, 2), 5);
    CHECK(mbCrcOk(exception, sizeof(exception)));
    uint8_t code = 0;
    CHECK_EQ(mbCheckReply(e, exception, sizeof(exception), &code), MB_EXCEPTION);
    CHECK_EQ(code, 2);
    CHECK_EQ(mbClassifyFailure(e, good, 0), MB_TIMEOUT);
    CHECK_EQ(mbClassifyFailure(e, good, 4), MB_SHORT);
    const uint8_t noise[] = {0x00, 0xFF, 0x7E};
    CHECK_EQ(mbClassifyFailure(e, noise, sizeof(noise)), MB_NOISE);
  }
}

static void testTimeouts() {
  {  // Nothing answers: the wait is the timeout, measured across the 32-bit clock wrap.
    Bare b;
    b.sim.powered = false;
    uint16_t word = 0;
    CHECK(b.sim.nowMs() > 0xFFFFFF00UL);
    const uint64_t t0 = b.sim.elapsedMs();
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    const uint64_t took = b.sim.elapsedMs() - t0;
    CHECK(b.sim.nowMs() < 0x1000);  // the clock wrapped during the wait
    CHECK_EQ(r.status, MB_TIMEOUT);
    CHECK_MSG(took >= 1000 && took <= 1015, "took %llu ms", (unsigned long long)took);
    CHECK_MSG(r.elapsedMs >= 1000 && r.elapsedMs <= 1005, "elapsedMs %u", (unsigned)r.elapsedMs);
    CHECK_EQ(b.master.stats.timeouts, 1);
    CHECK(mbIsLinkFailure(r.status));
  }
  {  // A shorter timeout is honoured.
    Bare b;
    b.sim.powered = false;
    b.master.responseTimeoutMs = 250;
    uint16_t word = 0;
    const uint64_t t0 = b.sim.elapsedMs();
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t took = b.sim.elapsedMs() - t0;
    CHECK_MSG(took >= 250 && took <= 265, "took %llu ms", (unsigned long long)took);
  }
  {  // A reply that begins just before the timeout is allowed to finish.
    Bare b;
    b.sim.responseDelayMs = 990;
    uint16_t words[16];
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 16, words);
    CHECK_EQ(r.status, MB_OK);
    CHECK_MSG(r.elapsedMs > 1000 && r.elapsedMs < 1100, "elapsedMs %u", (unsigned)r.elapsedMs);
    CHECK_EQ(words[0], b.sim.regs[0x0008]);
  }
  {  // A reply that arrives after the master gave up is thrown away before the next request,
     // so it cannot be taken for the answer to that one.
    Bare b;
    b.sim.responseDelayMs = 1200;
    uint16_t word = 0xAAAA;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    b.sim.responseDelayMs = 20;
    b.sim.advance(500);  // the late reply to the first request is now waiting in the port
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word).status, MB_OK);
    CHECK_EQ(word, b.sim.regs[0x0009]);
    CHECK(word != b.sim.regs[0x0008]);
  }
  {  // A babbling bus fills the buffer; the master gives up instead of waiting forever.
    Bare b;
    b.sim.powered = false;
    b.sim.inject(5, std::vector<uint8_t>(600, 0xFF));
    uint16_t word = 0;
    const uint64_t t0 = b.sim.elapsedMs();
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    CHECK_EQ(r.status, MB_NOISE);
    CHECK(b.sim.elapsedMs() - t0 < 1500);
    CHECK_EQ(b.master.stats.otherErrors, 1);
  }
  {  // The bus is left quiet between a reply and the next request.
    Bare b;
    uint16_t word = 0;
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    const uint64_t replyEnd = b.sim.elapsedMs();
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word);
    CHECK(b.sim.requestTimes[1] >= replyEnd + b.master.interFrameGapMs);
    CHECK_EQ(b.master.interFrameGapMs, 5);
    CHECK(b.sim.requestTimes[1] < replyEnd + 10);  // and not for longer than that
  }
  {  // How long, follows from the baud rate: 3.5 characters of 11 bits, one millisecond more for
     // the millisecond clock, and never less than the 5 ms of 9600 baud.
    CHECK_EQ(mbInterFrameGapMs(300), 129);
    CHECK_EQ(mbInterFrameGapMs(600), 65);
    CHECK_EQ(mbInterFrameGapMs(1200), 33);
    CHECK_EQ(mbInterFrameGapMs(2400), 17);
    CHECK_EQ(mbInterFrameGapMs(4800), 9);
    CHECK_EQ(mbInterFrameGapMs(9600), 5);
    CHECK_EQ(mbInterFrameGapMs(19200), 5);
    CHECK_EQ(mbInterFrameGapMs(115200), 5);
    CHECK_EQ(mbInterFrameGapMs(1000000), 5);
    CHECK_EQ(mbInterFrameGapMs(1), 38501);
    CHECK_EQ(mbInterFrameGapMs(0), 38501);  // no division by zero
    // The silence that ends a frame of unknown length is 50 ms until 3.5 characters are longer.
    CHECK_EQ(mbQuietMs(115200), 50);
    CHECK_EQ(mbQuietMs(9600), 50);
    CHECK_EQ(mbQuietMs(1200), 50);
    CHECK_EQ(mbQuietMs(600), 65);
    CHECK_EQ(mbQuietMs(300), 129);
  }
  {  // The master takes both times when the link is opened through it, and keeps what it had
     // when the port refuses the settings.
    Bare b;
    CHECK_EQ(b.master.quietMs, 50);
    CHECK(b.master.configure(300, MB_8N2));
    CHECK_EQ(b.master.interFrameGapMs, 129);
    CHECK_EQ(b.master.quietMs, 129);
    const unsigned configures = b.sim.configures;
    CHECK(!b.master.configure(299, MB_8N2));
    CHECK_EQ(b.sim.configures, configures + 1);  // the port was asked, and said no
    CHECK_EQ(b.master.interFrameGapMs, 129);
    CHECK_EQ(b.master.quietMs, 129);
    CHECK(b.master.configure(1200, MB_8E1));
    CHECK_EQ(b.master.interFrameGapMs, 33);
    CHECK_EQ(b.master.quietMs, 50);
    CHECK(b.master.configure(9600, MB_8N2));
    CHECK_EQ(b.master.interFrameGapMs, 5);
    CHECK_EQ(b.master.quietMs, 50);
  }
  {  // At 300 baud the next request waits out 129 ms, not 5.
    Bare b;
    b.sim.deviceBaud = 300;
    CHECK(b.master.configure(300, MB_8N2));
    uint16_t word = 0;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_OK);
    const uint64_t replyEnd = b.sim.elapsedMs();
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word).status, MB_OK);
    CHECK(b.sim.requestTimes[1] >= replyEnd + 129);
    CHECK(b.sim.requestTimes[1] < replyEnd + 135);
  }
  {  // Counters.
    Bare b;
    uint16_t word = 0;
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    b.sim.failNext(SimSunSaver::FAULT_BAD_CRC);
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0036, 1, &word);
    CHECK_EQ(b.master.stats.sent, 3);
    CHECK_EQ(b.master.stats.ok, 1);
    CHECK_EQ(b.master.stats.crcErrors, 1);
    CHECK_EQ(b.master.stats.exceptions, 1);
    b.master.resetStats();
    CHECK_EQ(b.master.stats.sent, 0);
  }
}

// A read reply does not say which registers it holds. One that arrives after its request was
// given up would therefore pass for the answer to the next read of the same size. After an
// unanswered request, the next one to the same slave with the same function waits one more
// timeout, and what arrives in that time is dropped.
static void testLateReplies() {
  {  // The late reply arrives while the next request would have been waiting for its own.
    Bare b;
    b.sim.responseDelayMs = 1200;
    uint16_t word = 0xAAAA;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t gaveUp = b.sim.elapsedMs();
    b.sim.responseDelayMs = 20;  // the controller is quick again
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word);
    CHECK_EQ(r.status, MB_OK);
    CHECK_EQ(word, b.sim.regs[0x0009]);
    CHECK(word != b.sim.regs[0x0008]);
    CHECK_EQ(r.noiseBytes, 0);
    // The second request went out one timeout after the first was given up, not before.
    CHECK_MSG(b.sim.requestTimes[1] >= gaveUp + 1000 && b.sim.requestTimes[1] <= gaveUp + 1010, "sent %llu ms after",
              (unsigned long long)(b.sim.requestTimes[1] - gaveUp));
    CHECK_EQ(b.master.stats.lateArrivals, 1);
    // That is all: the next read does not wait.
    const uint64_t done = b.sim.elapsedMs();
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x000A, 1, &word).status, MB_OK);
    CHECK_EQ(word, b.sim.regs[0x000A]);
    CHECK(b.sim.requestTimes[2] <= done + 10);
    CHECK_EQ(b.master.stats.lateArrivals, 1);
  }
  {  // A controller that always answers later than the timeout: every read fails, and none is
     // answered with the value of the register before it.
    Bare b;
    b.sim.responseDelayMs = 1200;
    for (uint16_t addr = 0x0008; addr <= 0x000C; ++addr) {
      uint16_t word = 0xAAAA;
      const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, addr, 1, &word);
      CHECK_MSG(r.status == MB_TIMEOUT, "0x%04X: %s", (unsigned)addr, mbStatusText(r.status));
    }
    CHECK_EQ(b.master.stats.ok, 0);
    CHECK_EQ(b.master.stats.timeouts, 5);
    CHECK_EQ(b.master.stats.lateArrivals, 4);  // the four late replies that a following read waited out
  }
  {  // A late block reply does not spoil a read of another size either.
    Bare b;
    b.sim.responseDelayMs = 1200;
    uint16_t words[16];
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0xE000, 16, words).status, MB_TIMEOUT);
    b.sim.responseDelayMs = 20;
    uint16_t word = 0;
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    CHECK_MSG(r.status == MB_OK, "%s", mbStatusText(r.status));
    CHECK_EQ(word, b.sim.regs[0x0008]);
  }
  {  // The wait is one timeout, whatever the timeout is, and the clock may wrap during it.
    Bare b;
    b.sim.powered = false;
    b.master.responseTimeoutMs = 200;
    uint16_t word = 0;
    CHECK(b.sim.nowMs() > 0xFFFFFF00UL);
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t gaveUp = b.sim.elapsedMs();
    CHECK(b.sim.nowMs() > 0xFFFFFF00UL);  // given up before the wrap
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word).status, MB_TIMEOUT);
    CHECK(b.sim.nowMs() < 0x1000);        // and the wait crossed it
    CHECK_MSG(b.sim.requestTimes[1] >= gaveUp + 200 && b.sim.requestTimes[1] <= gaveUp + 210, "sent %llu ms after",
              (unsigned long long)(b.sim.requestTimes[1] - gaveUp));
    CHECK_EQ(b.master.stats.lateArrivals, 0);  // nothing arrived: this link is simply dead
  }
  {  // Another slave or another function does not wait: a late reply cannot be taken for theirs.
    Bare b;
    b.sim.powered = false;
    uint16_t word = 0;
    uint8_t bit = 0;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    uint64_t gaveUp = b.sim.elapsedMs();
    CHECK_EQ(b.master.readRegisters(2, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    CHECK(b.sim.requestTimes[1] <= gaveUp + 10);
    gaveUp = b.sim.elapsedMs();
    CHECK_EQ(b.master.readRegisters(2, MB_FC_READ_HOLDING, 0x0008, 1, &word).status, MB_TIMEOUT);
    CHECK(b.sim.requestTimes[2] <= gaveUp + 10);
    gaveUp = b.sim.elapsedMs();
    CHECK_EQ(b.master.readBits(2, MB_FC_READ_COILS, 0x0000, 1, &bit).status, MB_TIMEOUT);
    CHECK(b.sim.requestTimes[3] <= gaveUp + 10);
    gaveUp = b.sim.elapsedMs();
    CHECK_EQ(b.master.writeRegister(2, 0xE022, 1).status, MB_TIMEOUT);
    CHECK(b.sim.requestTimes[4] <= gaveUp + 10);
  }
  {  // A late reply for another function arrives in front of the real reply and is skipped as
     // noise, which is why such a request need not wait.
    Bare b;
    b.sim.responseDelayMs = 1200;
    uint16_t word = 0;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t gaveUp = b.sim.elapsedMs();
    b.sim.responseDelayMs = 20;
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_HOLDING, 0xE022, 1, &word);
    CHECK(b.sim.requestTimes[1] <= gaveUp + 10);
    CHECK_EQ(r.status, MB_OK);
    CHECK_EQ(word, b.sim.regs[0xE022]);
    CHECK_EQ(r.noiseBytes, 7);  // the late reply to the function 4 read
  }
  {  // Two unanswered requests of different kinds, the second given up while the reply to the
     // first could still arrive (the probe does this when it shortens the timeout). One record
     // cannot name both, so until both waits are over every request waits, whatever its kind.
    Bare b;
    uint16_t word = 0;
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t first = b.sim.elapsedMs();
    b.master.responseTimeoutMs = 300;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_HOLDING, 0xE022, 1, &word).status, MB_TIMEOUT);
    CHECK(b.sim.requestTimes[1] <= first + 10);  // another function: sent at once
    CHECK(b.sim.elapsedMs() < first + 400);      // and given up inside the first one's wait
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word).status, MB_OK);
    CHECK_EQ(word, b.sim.regs[0x0009]);
    CHECK_MSG(b.sim.requestTimes[2] >= first + 1000 && b.sim.requestTimes[2] <= first + 1010, "sent %llu ms after",
              (unsigned long long)(b.sim.requestTimes[2] - first));
    // A request to another slave waits as well while both are open.
    Bare c;
    c.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    CHECK_EQ(c.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t failed = c.sim.elapsedMs();
    c.master.responseTimeoutMs = 300;
    CHECK_EQ(c.master.readRegisters(1, MB_FC_READ_HOLDING, 0xE022, 1, &word).status, MB_TIMEOUT);
    CHECK_EQ(c.master.readRegisters(9, MB_FC_READ_INPUT, 0x0009, 1, &word).status, MB_TIMEOUT);
    CHECK(c.sim.requestTimes[2] >= failed + 1000);
    // Had the second been given up after the first one's wait was over, nothing would wait:
    // that is the normal case, and it keeps a scan of many slave addresses quick.
    Bare d;
    d.sim.powered = false;
    for (uint8_t slave = 1; slave <= 5; ++slave) {
      d.master.readRegisters(slave, MB_FC_READ_INPUT, 0x0008, 1, &word);
    }
    CHECK_MSG(d.sim.elapsedMs() < 5 * 1000 + 200, "took %llu ms", (unsigned long long)d.sim.elapsedMs());
  }
  {  // An exception is an answer: nothing more is coming, so nothing waits.
    Bare b;
    uint16_t word = 0;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0036, 1, &word).status, MB_EXCEPTION);
    const uint64_t done = b.sim.elapsedMs();
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_OK);
    CHECK(b.sim.requestTimes[1] <= done + 10);
  }
  {  // A late reply that is still coming in when the wait ends is allowed to finish first.
    Bare b;
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    uint16_t word = 0;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t gaveUp = b.sim.elapsedMs();
    const std::vector<uint8_t> late = SimSunSaver::frame(bytes({0x01, 0x04, 0x02, 0x11, 0x83}));
    b.sim.inject(997, late);  // its seven bytes arrive from 997 to 1003 ms: across the end of the wait
    const MbResult r = b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word);
    CHECK_EQ(r.status, MB_OK);
    CHECK_EQ(word, b.sim.regs[0x0009]);
    CHECK_EQ(r.noiseBytes, 0);
    CHECK_MSG(b.sim.requestTimes[1] >= gaveUp + 1003 + b.master.quietMs, "sent %llu ms after",
              (unsigned long long)(b.sim.requestTimes[1] - gaveUp));
  }
  {  // Noise that never stops does not hold the master for ever.
    Bare b;
    b.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    uint16_t word = 0;
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_TIMEOUT);
    const uint64_t gaveUp = b.sim.elapsedMs();
    b.sim.inject(900, std::vector<uint8_t>(5000, 0xFF));
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0009, 1, &word);
    CHECK_MSG(b.sim.requestTimes[1] >= gaveUp + 1000 && b.sim.requestTimes[1] <= gaveUp + 1000 + MB_GRACE_MS + 10,
              "sent %llu ms after", (unsigned long long)(b.sim.requestTimes[1] - gaveUp));
  }
  {  // Stray bytes between two answered requests are dropped as before, and are not counted as
     // late: only what arrives while a late reply is waited for is.
    Bare b;
    uint16_t word = 0;
    b.sim.inject(0, bytes({0x00, 0x55}));
    b.sim.advance(5);
    CHECK_EQ(b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word).status, MB_OK);
    CHECK_EQ(word, b.sim.regs[0x0008]);
    CHECK_EQ(b.master.stats.lateArrivals, 0);
    b.sim.responseDelayMs = 1200;
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    b.master.readRegisters(1, MB_FC_READ_INPUT, 0x0008, 1, &word);
    CHECK_EQ(b.master.stats.lateArrivals, 1);
    b.master.resetStats();
    CHECK_EQ(b.master.stats.lateArrivals, 0);
  }
}

struct RecordingTrace : MbTraceSink {
  int calls;
  size_t txLen;
  size_t rxLen;
  MbResult last;
  RecordingTrace() : calls(0), txLen(0), rxLen(0) { memset(&last, 0, sizeof(last)); }
  void onTransaction(const uint8_t *, size_t tx, const uint8_t *, size_t rx, const MbResult &result) override {
    ++calls;
    txLen = tx;
    rxLen = rx;
    last = result;
  }
};

static void testBitsDeviceIdAndRaw() {
  Bare b;
  uint8_t bits[3] = {9, 9, 9};
  CHECK_EQ(b.master.readBits(1, MB_FC_READ_COILS, 0x0000, 3, bits).status, MB_OK);
  CHECK(b.sim.requests.back() == SimSunSaver::frame(bytes({0x01, 0x01, 0x00, 0x00, 0x00, 0x03})));
  CHECK(bits[0] == 0 && bits[1] == 0 && bits[2] == 0);
  b.sim.coils[0x0001] = true;
  CHECK_EQ(b.master.readBits(1, MB_FC_READ_COILS, 0x0000, 3, bits).status, MB_OK);
  CHECK(bits[0] == 0 && bits[1] == 1 && bits[2] == 0);
  MbResult r = b.master.readBits(1, MB_FC_READ_COILS, 0x0005, 1, bits);
  CHECK_EQ(r.status, MB_EXCEPTION);
  CHECK_EQ(r.exception, 2);
  CHECK_EQ(b.master.readBits(1, MB_FC_READ_INPUT, 0, 1, bits).status, MB_BAD_REQUEST);

  MbDeviceId id;
  r = b.master.readDeviceId(1, id);
  CHECK_EQ(r.status, MB_OK);
  CHECK(b.sim.requests.back() == SimSunSaver::frame(bytes({0x01, 0x2B, 0x0E, 0x01, 0x00})));
  CHECK_EQ(id.count, 3);
  CHECK(strcmp(id.objects[0].text, "Morningstar Corp.") == 0);
  CHECK(strcmp(id.objects[1].text, "SS-MPPT") == 0);
  CHECK(strcmp(id.objects[2].text, "v01.01.01") == 0);
  CHECK_EQ(id.objects[2].id, 2);
  b.sim.deviceIdSupported = false;
  r = b.master.readDeviceId(1, id);
  CHECK_EQ(r.status, MB_EXCEPTION);
  CHECK_EQ(r.exception, 1);
  CHECK_EQ(id.count, 0);
  b.sim.deviceIdSupported = true;

  // A function with no known reply layout: the reply ends when the line goes quiet.
  RecordingTrace trace;
  b.master.trace = &trace;
  const uint8_t body[] = {0x01, 0x2B, 0x0E, 0x01, 0x00};
  r = b.master.rawRequest(body, sizeof(body));
  CHECK_EQ(r.status, MB_OK);
  CHECK_EQ(b.master.frameLen(), 49);
  CHECK_EQ(trace.calls, 1);
  CHECK_EQ(trace.txLen, 7);
  CHECK_EQ(trace.rxLen, 49);
  CHECK_EQ(trace.last.status, MB_OK);
  const uint8_t unknown[] = {0x01, 0x41, 0x00};
  r = b.master.rawRequest(unknown, sizeof(unknown));
  CHECK_EQ(r.status, MB_EXCEPTION);
  CHECK_EQ(r.exception, 1);
  b.sim.powered = false;
  r = b.master.rawRequest(unknown, sizeof(unknown));
  CHECK_EQ(r.status, MB_TIMEOUT);
  CHECK_EQ(trace.calls, 3);

  {  // A request with no data is four bytes long. The simulator must not read an address and a
     // value past its end (the sanitizer build watches this exactly sized buffer).
    Bare c;
    const std::vector<uint8_t> bare = SimSunSaver::frame(bytes({0x01, 0x11}));
    CHECK_EQ(bare.size(), 4);
    c.sim.send(bare.data(), bare.size());
    c.sim.advance(100);
    std::vector<uint8_t> reply;
    for (int byte = c.sim.read(); byte >= 0; byte = c.sim.read()) {
      reply.push_back((uint8_t)byte);
    }
    CHECK(reply == SimSunSaver::frame(bytes({0x01, 0x91, 0x01})));  // exception 1: illegal function
    const uint8_t noData[] = {0x01, 0x11};
    r = c.master.rawRequest(noData, sizeof(noData));
    CHECK_EQ(r.status, MB_EXCEPTION);
    CHECK_EQ(r.exception, 1);
    CHECK_EQ(c.sim.requests.back().size(), 4);
  }
}

// ============================================================================
// Text helpers
// ============================================================================
static std::string fixed(double value, uint8_t decimals) {
  char text[48];
  ovFormatFixed(value, decimals, text, sizeof(text));
  return std::string(text);
}

static void testText() {
  CHECK(fixed(13.68, 2) == "13.68");
  CHECK(fixed(0.125, 2) == "0.13");  // half rounds away from zero
  CHECK(fixed(-0.125, 2) == "-0.13");
  CHECK(fixed(-0.001, 2) == "0.00");  // never "-0.00"
  CHECK(fixed(-0.04, 2) == "-0.04");
  CHECK(fixed(2.5, 0) == "3");
  CHECK(fixed(1234.5678, 3) == "1234.568");
  CHECK(fixed(0.0, 1) == "0.0");
  CHECK(fixed(0.0305, 4) == "0.0305");
  CHECK(fixed(1.0, 9) == "1.000000");  // at most six decimals
  CHECK(fixed(1.0e19, 0) == "big");
  CHECK(fixed(-1.0e19, 2) == "-big");
  CHECK(fixed(std::numeric_limits<double>::quiet_NaN(), 2) == "nan");
  CHECK(fixed(7000.0, 1) == "7000.0");
  char small[4];
  CHECK_EQ(ovFormatFixed(123.456, 2, small, sizeof(small)), 3);
  CHECK(strcmp(small, "123") == 0);

  uint32_t u = 0;
  CHECK(ovParseU32("0", &u) && u == 0);
  CHECK(ovParseU32("4293", &u) && u == 4293);
  CHECK(ovParseU32("0x10C5", &u) && u == 4293);
  CHECK(ovParseU32("0X10c5", &u) && u == 4293);
  CHECK(ovParseU32("4294967295", &u) && u == 0xFFFFFFFFUL);
  CHECK(ovParseU32("0xFFFFFFFF", &u) && u == 0xFFFFFFFFUL);
  CHECK(!ovParseU32("4294967296", &u));
  CHECK(!ovParseU32("0x100000000", &u));
  CHECK(!ovParseU32("", &u));
  CHECK(!ovParseU32("0x", &u));
  CHECK(!ovParseU32("12a", &u));
  CHECK(!ovParseU32("-1", &u));
  CHECK(!ovParseU32(" 12", &u));
  CHECK(!ovParseU32("1.5", &u));
  CHECK(!ovParseU32(nullptr, &u));

  double d = 0.0;
  // A decimal parses to exactly the double the compiler makes of the same text. Adding up
  // tenths and hundredths would not: 0.1 * 3 is 0.30000000000000004, above a limit of 0.3.
  CHECK(ovParseDecimal("13.10", &d) && d == 13.10);
  CHECK(ovParseDecimal("13.25", &d) && d == 13.25);
  CHECK(ovParseDecimal("0.3", &d) && d == 0.3);
  CHECK(ovParseDecimal("0.7", &d) && d == 0.7);
  CHECK(ovParseDecimal("96.667", &d) && d == 96.667);
  CHECK(ovParseDecimal("0.000001", &d) && d == 0.000001);
  CHECK(ovParseDecimal("123456789.012345", &d) && d == 123456789.012345);
  CHECK(ovParseDecimal("999999999999999", &d) && d == 999999999999999.0);
  CHECK(ovParseDecimal("-5", &d) && d == -5.0);
  CHECK(ovParseDecimal("-0.3", &d) && d == -0.3);
  CHECK(ovParseDecimal("+2", &d) && d == 2.0);
  CHECK(ovParseDecimal(".5", &d) && d == 0.5);
  CHECK(ovParseDecimal("5.", &d) && d == 5.0);
  CHECK(ovParseDecimal("007.50", &d) && d == 7.5);
  CHECK(ovParseDecimal("0", &d) && d == 0.0);
  for (int hundredths = 0; hundredths <= 2000; ++hundredths) {
    char text[16];
    snprintf(text, sizeof(text), "%d.%02d", hundredths / 100, hundredths % 100);
    CHECK_MSG(ovParseDecimal(text, &d) && d == strtod(text, nullptr), "%s parsed inexactly", text);
  }
  CHECK(!ovParseDecimal(".", &d));
  CHECK(!ovParseDecimal("", &d));
  CHECK(!ovParseDecimal("1e3", &d));
  CHECK(!ovParseDecimal("13,1", &d));
  CHECK(!ovParseDecimal("--2", &d));
  CHECK(!ovParseDecimal("13.1V", &d));
  CHECK(!ovParseDecimal("1234567890123456", &d));
  CHECK(!ovParseDecimal(nullptr, &d));

  char hex[12];
  ovFormatHex(0x1183, 4, hex, sizeof(hex));
  CHECK(strcmp(hex, "1183") == 0);
  ovFormatHex(0xAB, 2, hex, sizeof(hex));
  CHECK(strcmp(hex, "AB") == 0);
  ovFormatHex(0x5, 0, hex, sizeof(hex));
  CHECK(strcmp(hex, "5") == 0);
  ovFormatHex(0x12345678UL, 9, hex, sizeof(hex));
  CHECK(strcmp(hex, "12345678") == 0);

  char tiny[8];
  OvText text(tiny, sizeof(tiny));
  text.add("hello world");
  CHECK(strcmp(tiny, "hello w") == 0);
  CHECK(text.overflowed());
  CHECK_EQ(text.length(), 7);
  OvText fits(tiny, sizeof(tiny));
  fits.addI(-12);
  fits.addChar(' ');
  fits.addHex(0xF, 2);
  CHECK(strcmp(tiny, "-12 0F") == 0);
  CHECK(!fits.overflowed());

  CHECK(ovEqualsIgnoreCase("EV_LVD", "ev_lvd"));
  CHECK(!ovEqualsIgnoreCase("EV_lvd", "EV_lvr"));
  CHECK(!ovEqualsIgnoreCase("EV_lvd", "EV_lvd2"));
  CHECK(!ovEqualsIgnoreCase(nullptr, "x"));

  char copy[4];
  CHECK_EQ(ovCopy(copy, sizeof(copy), "abcdef"), 3);
  CHECK(strcmp(copy, "abc") == 0);
  CHECK_EQ(ovCopy(copy, sizeof(copy), nullptr), 0);
  CHECK(copy[0] == '\0');

  // Text that does not fit is cut and ended inside the room it was given: with room for four
  // characters, the fifth and what follows it stay as they were.
  char room[8];
  memset(room, 'z', sizeof(room));
  CHECK_EQ(ovCopy(room, 4, "abcdef"), 3);
  CHECK(memcmp(room, "abc\0zzzz", 8) == 0);
  memset(room, 'z', sizeof(room));
  CHECK_EQ(ovFormatU64(123456, room, 4), 3);
  CHECK(memcmp(room, "123\0zzzz", 8) == 0);
  memset(room, 'z', sizeof(room));
  CHECK_EQ(ovFormatHex(0x12345678UL, 8, room, 4), 3);
  CHECK(memcmp(room, "123\0zzzz", 8) == 0);
  memset(room, 'z', sizeof(room));
  CHECK_EQ(ovFormatFixed(123.456, 2, room, 6), 5);  // the point fits, the second decimal does not
  CHECK(memcmp(room, "123.4\0zz", 8) == 0);
  memset(room, 'z', sizeof(room));
  CHECK_EQ(ovFormatFixed(-123.456, 2, room, 4), 3);
  CHECK(memcmp(room, "-12\0zzzz", 8) == 0);
  CHECK_EQ(ovFormatU64(7, room, 0), 0);
  CHECK_EQ(ovFormatHex(7, 2, room, 0), 0);
  CHECK_EQ(ovFormatFixed(7.0, 2, room, 0), 0);
  CHECK_EQ(ovCopy(room, 0, "abc"), 0);
  CHECK(memcmp(room, "-12\0zzzz", 8) == 0);  // no room at all: nothing is touched

  StringOut out;
  out.printI(-5);
  out.printChar(' ');
  out.printI(-2147483647 - 1);
  out.printChar(' ');
  out.printU(4294967295UL);
  out.printChar(' ');
  out.printPadded("ab", 5);
  out.printPadded("abcdef", 3);
  out.printChar('|');
  CHECK(out.text == "-5 -2147483648 4294967295 ab   abcdef |");
}

// ============================================================================
// Profile
// ============================================================================
// What ArduinoJson makes of one number, after the loader's repair.
static double throughJson(const char *text) {
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return ovJsonNumber(doc.as<JsonVariantConst>());
}

static void testExactDecimals() {
  // ArduinoJson reads a short decimal in single precision: 96.667 arrives as 96.66699982, and a
  // limit of 0.3 as 0.30000001, which a typed 0.3 would be "below". The loader gives back the
  // decimal that was written.
  {
    JsonDocument doc;
    CHECK(deserializeJson(doc, "96.667") == DeserializationError::Ok);
    CHECK(doc.as<double>() != 96.667);  // the reason for all this; fails if ArduinoJson changes
  }
  const char *const texts[] = {"96.667", "0.05",    "79.16",  "1.263",    "989.5",    "13.1",   "0.3",   "11.5",
                               "17.5",   "100",     "32768",  "65536",    "0.1",      "0.7",    "2.5",   "256",
                               "12345.6", "0.000123", "999999", "0.999999", "1.00001", "123456", "0.001", "4.4",
                               "15.35",  "0.000001", "0.0000015"};
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
    const double exact = strtod(texts[i], nullptr);
    const float single = (float)exact;
    CHECK_MSG(ovExactDecimal((double)single) == exact, "%s as a float did not come back", texts[i]);
    CHECK_MSG(ovExactDecimal(-(double)single) == -exact, "-%s as a float did not come back", texts[i]);
    // ArduinoJson builds the number from single-precision products, which can land a step or
    // two beside the nearest float.
    const float up = nextafterf(nextafterf(single, 1.0e30f), 1.0e30f);
    const float down = nextafterf(nextafterf(single, 0.0f), 0.0f);
    CHECK_MSG(ovExactDecimal((double)up) == exact, "%s two steps up did not come back", texts[i]);
    CHECK_MSG(ovExactDecimal((double)down) == exact, "%s two steps down did not come back", texts[i]);
    CHECK_MSG(throughJson(texts[i]) == exact, "%s through ArduinoJson did not come back", texts[i]);
    double typed = 0.0;
    CHECK(ovParseDecimal(texts[i], &typed));
    CHECK_MSG(typed == exact, "%s typed at the console is another number", texts[i]);
  }
  // Every voltage to the hundredth and every numerator to the thousandth, through ArduinoJson.
  for (int hundredths = 1; hundredths <= 3000; ++hundredths) {
    char text[16];
    snprintf(text, sizeof(text), "%d.%02d", hundredths / 100, hundredths % 100);
    CHECK_MSG(throughJson(text) == strtod(text, nullptr), "%s did not come back", text);
  }
  for (int thousandths = 90000; thousandths <= 101000; ++thousandths) {
    char text[16];
    snprintf(text, sizeof(text), "%d.%03d", thousandths / 1000, thousandths % 1000);
    CHECK_MSG(throughJson(text) == strtod(text, nullptr), "%s did not come back", text);
  }
  // Six significant digits anywhere in the range a profile uses.
  uint32_t seed = 12345;
  for (int i = 0; i < 20000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const unsigned digits = 100000u + (seed >> 8) % 900000u;  // 100000..999999
    const int decimals = (int)((seed >> 4) % 7u);             // 0..6 of them after the point
    char text[24];
    if (decimals == 6) {
      snprintf(text, sizeof(text), "0.%06u", digits);
    } else if (decimals == 0) {
      snprintf(text, sizeof(text), "%u", digits);
    } else {
      unsigned divisor = 1;
      for (int d = 0; d < decimals; ++d) {
        divisor *= 10u;
      }
      snprintf(text, sizeof(text), "%u.%0*u", digits / divisor, decimals, digits % divisor);
    }
    CHECK_MSG(throughJson(text) == strtod(text, nullptr), "%s did not come back", text);
  }

  // A number that needs double precision was read in double precision and is left alone.
  CHECK(ovExactDecimal(3.14159265358979) == 3.14159265358979);
  CHECK(throughJson("96.66700001") == 96.66700001);
  CHECK(throughJson("3.14159265358979") == strtod("3.14159265358979", nullptr) ||
        near(throughJson("3.14159265358979"), 3.14159265358979, 1e-14));
  CHECK(ovExactDecimal(0.0) == 0.0);
  CHECK(ovExactDecimal(1.0e-9) == 1.0e-9);
  CHECK(ovExactDecimal(4.0e9) == 4.0e9);
  CHECK(ovExactDecimal(std::numeric_limits<double>::infinity()) == std::numeric_limits<double>::infinity());
  const double nan = ovExactDecimal(std::numeric_limits<double>::quiet_NaN());
  CHECK(nan != nan);
  CHECK(throughJson("7") == 7.0);
  CHECK(throughJson("-13.25") == -13.25);
  CHECK(throughJson("1e3") == 1000.0);
  // Whatever goes in, the result is the same number to single precision.
  for (int i = 0; i < 20000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const float value = (float)(seed >> 8) / 16777216.0f * 2000.0f - 1000.0f;
    const double out = ovExactDecimal((double)value);
    const double magnitude = value < 0 ? -(double)value : (double)value;
    CHECK_MSG(near(out, (double)value, magnitude * 4.0e-7), "%.9g moved to %.17g", (double)value, out);
  }

  // The point of it: a limit written as 0.3 accepts a typed 0.3.
  const std::string json = replaced(kMiniProfile, "\"min\":1,\"max\":20", "\"min\":0.3,\"max\":13.1");
  OvProfile profile;
  char error[200];
  CHECK_MSG(profile.load(json.c_str(), json.size(), error, sizeof(error)), "%s", error);
  const OvRegister &a = *profile.findRegister("a");
  CHECK(a.minValue == 0.3);
  CHECK(a.maxValue == 13.1);
  uint16_t raw = 0;
  double typed = 0.0;
  CHECK(ovParseDecimal("0.3", &typed));
  CHECK_EQ(ovEncode(a, typed, &raw), OV_ENCODE_OK);
  CHECK(ovParseDecimal("13.10", &typed));
  CHECK_EQ(ovEncode(a, typed, &raw), OV_ENCODE_OK);
  CHECK(ovParseDecimal("0.29", &typed));
  CHECK_EQ(ovEncode(a, typed, &raw), OV_ENCODE_BELOW_MIN);
  CHECK(ovParseDecimal("13.11", &typed));
  CHECK_EQ(ovEncode(a, typed, &raw), OV_ENCODE_ABOVE_MAX);
}

static std::string formatted(const OvRegister &reg, uint16_t w0, uint16_t w1 = 0) {
  const uint16_t words[2] = {w0, w1};
  char text[240];
  ovFormatValue(reg, words, text, sizeof(text));
  return std::string(text);
}

static void testBuiltinProfile() {
  CHECK_EQ(gAllocLive, 0);
  {
    OvProfile profile;
    char error[200] = "x";
    CHECK(!profile.loaded());
    CHECK_MSG(profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error)), "%s", error);
    CHECK(profile.loaded());
    CHECK(error[0] == '\0');
    CHECK(gAllocLive > 0);
    CHECK(strcmp(profile.id(), "sunsaver-mppt") == 0);
    CHECK_EQ(profile.registerCount(), 94);
    CHECK_EQ(profile.coilCount(), 8);
    CHECK_EQ(profile.groupCount(), 13);
    CHECK_EQ(profile.liveGroupCount(), 2);
    CHECK_EQ(profile.checkCount(), 5);
    CHECK(profile.noteCount() >= 3);
    CHECK(profile.writeNoteCount() >= 2);
    CHECK(profile.wiringCount() >= 8);

    const OvModbusDefaults &modbus = profile.modbus();
    CHECK_EQ(modbus.slaveId, 1);
    CHECK_EQ(modbus.baud, 9600);
    CHECK_EQ(modbus.format, MB_8N2);
    CHECK_EQ(modbus.readFunction, 4);
    CHECK_EQ(modbus.maxBlock, 16);
    CHECK_EQ(modbus.responseTimeoutMs, 1000);
    CHECK(modbus.probeRegister != nullptr && modbus.probeRegister->addr == 0x0008);
    CHECK(modbus.resetCoil != nullptr && modbus.resetCoil->addr == 0x00FF);

    // In address order, without overlaps, and every register is usable as it stands.
    size_t bench = 0;
    for (size_t i = 0; i < profile.registerCount(); ++i) {
      const OvRegister &reg = profile.reg(i);
      if (i > 0) {
        const OvRegister &previous = profile.reg(i - 1);
        CHECK_MSG((uint32_t)previous.addr + previous.words <= reg.addr, "%s overlaps %s", previous.name, reg.name);
      }
      CHECK_MSG(reg.label[0] != '\0' && reg.group[0] != '\0', "%s", reg.name);
      CHECK_MSG(profile.findGroup(reg.group) != nullptr, "%s", reg.name);
      if (reg.type == OV_ENUM || reg.type == OV_BITS) {
        CHECK_MSG(reg.labels != nullptr, "%s has no labels", reg.name);
      }
      if (reg.writable) {
        // Only settings in EEPROM can be written, and never the two bus addresses.
        CHECK_MSG(reg.addr >= 0xE000 && reg.addr <= 0xE033, "%s is writable", reg.name);
        CHECK_MSG(reg.words == 1, "%s", reg.name);
      }
      if (reg.bench) {
        ++bench;
        CHECK_MSG(reg.addr >= 0x0008 && reg.addr <= 0x000C, "%s is marked bench", reg.name);
      }
      CHECK(profile.findRegister(reg.name) == &reg);
      CHECK(profile.findRegisterAt(reg.addr) == &reg);
    }
    CHECK_EQ(bench, 5);  // what the 2026 bench sessions actually read
    CHECK(!profile.findRegister("Emodbus_id")->writable);
    CHECK(!profile.findRegister("Emeter_id")->writable);
    CHECK(profile.findRegister("ev_LVD") == profile.findRegister("EV_lvd"));  // names ignore case
    CHECK(profile.findRegister("nope") == nullptr);
    CHECK(profile.findRegisterAt(0x0036) == nullptr);
    CHECK(profile.findRegisterAt(0x0015) == profile.findRegister("Ahc_r"));
    CHECK(profile.findRegisterAt(0x0016) == nullptr);  // its second word is not a register of its own
    CHECK(profile.findCoil("RESET") != nullptr);
    CHECK(profile.findGroup("load") != nullptr);

    // Every group that 'live' shows exists, and every group has registers.
    for (size_t i = 0; i < profile.liveGroupCount(); ++i) {
      CHECK(profile.findGroup(profile.liveGroup(i)) != nullptr);
    }
    for (size_t g = 0; g < profile.groupCount(); ++g) {
      size_t members = 0;
      for (size_t i = 0; i < profile.registerCount(); ++i) {
        if (strcmp(profile.reg(i).group, profile.group(g).name) == 0) {
          ++members;
        }
      }
      CHECK_MSG(members > 0, "group %s is empty", profile.group(g).name);
    }

    const OvScaleCheck &check = profile.scaleCheck();
    CHECK(check.present);
    CHECK_EQ(check.alternateCount, 1);
    // Exactly the number in the file, although ArduinoJson reads it in single precision.
    CHECK(check.alternates[0] == 96.667);
    CHECK(check.grid == 0.05);
    CHECK(profile.findScale("vref")->num == 96.667);
    CHECK(profile.findScale("i79")->num == 79.16);
    CHECK(profile.findScale("icomp")->num == 1.263);
    CHECK(profile.findScale("p989")->num == 989.5);
    CHECK(profile.findScale("vlive")->den == 32768.0);
    // Stored settings and live readings are tested apart, each on a scale of its own.
    CHECK(check.settingsScale == profile.findScale("vset"));
    CHECK(check.liveScale == profile.findScale("vlive"));
    CHECK(check.settingsScale != check.liveScale);
    CHECK(check.settingsScale->num == 100.0);
    CHECK(check.settingsFileNum == 100.0);
    CHECK(check.liveScale->num == 100.0);
    CHECK(check.liveFileNum == 100.0);
    CHECK_EQ(check.setpointCount, 14);
    for (uint8_t i = 0; i < check.setpointCount; ++i) {
      const OvRegister &setpoint = *check.setpoints[i];
      CHECK_MSG(setpoint.scale == check.settingsScale, "%s", setpoint.name);
      CHECK_MSG(setpoint.addr >= 0xE000 && setpoint.hasMin && setpoint.hasMax, "%s", setpoint.name);
    }
    CHECK(check.meter == profile.findRegister("adc_vb_f"));
    CHECK(check.battery == profile.findRegister("Vb_f"));
    CHECK(check.reference == profile.findRegister("Vb_ref"));
    CHECK(check.reference->scale == profile.findScale("vref"));
    CHECK(check.state == profile.findRegister("charge_state"));
    CHECK_EQ(check.regulatingCount, 3);
    // Every voltage setting 'set' can write is on the settings scale, and nothing that the
    // controller measures is: a multimeter must never decide how a setpoint is encoded.
    for (size_t i = 0; i < profile.registerCount(); ++i) {
      const OvRegister &reg = profile.reg(i);
      if (reg.scale == nullptr || strcmp(reg.scale->units, "V") != 0) {
        continue;
      }
      if (reg.writable) {
        CHECK_MSG(reg.scale == check.settingsScale, "%s is a writable voltage on scale %s", reg.name, reg.scale->name);
      }
      if (reg.addr < 0xE000) {
        CHECK_MSG(reg.scale != check.settingsScale, "%s is a reading on the settings scale", reg.name);
      }
    }

    // Loading again replaces the profile without leaking; clear() empties it.
    const long live = gAllocLive;
    CHECK(profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error)));
    CHECK_EQ(gAllocLive, live);
    profile.clear();
    CHECK(!profile.loaded());
    CHECK_EQ(profile.registerCount(), 0);
    CHECK_EQ(gAllocLive, 0);
    CHECK(profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error)));
  }
  CHECK_EQ(gAllocLive, 0);  // the destructor frees everything
}

static void testProfileCodec() {
  OvProfile profile;
  char error[200];
  CHECK(profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error)));

  // Voltages at the specification's scale. 0x1183 is the battery reading of the bench capture.
  CHECK(formatted(*profile.findRegister("adc_vb_f"), 0x1183) == "13.68 V");
  // Currents are signed: 0xFFF0 is a small negative current, not 158 A (finding F-26).
  CHECK(formatted(*profile.findRegister("adc_ic_f"), 0xFFF0) == "-0.04 A");
  CHECK(formatted(*profile.findRegister("adc_ic_f"), 0x0053) == "0.20 A");
  CHECK(formatted(*profile.findRegister("Vb_ref"), 0x1224) == "13.70 V");  // the one x96.667 voltage
  // Temperatures are the low byte, signed.
  CHECK(formatted(*profile.findRegister("T_hs"), 0x0019) == "25 C");
  CHECK(formatted(*profile.findRegister("T_amb"), 0x00EC) == "-20 C");
  CHECK(formatted(*profile.findRegister("T_amb"), 0xFFEC) == "-20 C");
  CHECK(formatted(*profile.findRegister("T_rts"), 0x0080) == "not connected");
  CHECK(formatted(*profile.findRegister("T_rts"), 0x0019) == "25 C");
  // An unsigned byte is the low byte as well. (This profile has none.)
  OvRegister unsignedByte = *profile.findRegister("T_hs");
  unsignedByte.type = OV_U8;
  CHECK(formatted(unsignedByte, 0xFFEC) == "236 C");
  // Two-word counters: high word first in RAM, low word first in EEPROM.
  CHECK(formatted(*profile.findRegister("Ahc_t"), 0x0001, 0x1170) == "7000.0 Ah");
  CHECK(formatted(*profile.findRegister("EAhc_t"), 0x1170, 0x0001) == "7000.0 Ah");
  CHECK(formatted(*profile.findRegister("hourmeter"), 0x0000, 0x10E1) == "4321 h");
  // Enumerations and bit fields.
  CHECK(formatted(*profile.findRegister("charge_state"), 7) == "FLOAT (7)");
  CHECK(formatted(*profile.findRegister("charge_state"), 5) == "BULK (5)");
  CHECK(formatted(*profile.findRegister("charge_state"), 99) == "unknown (99)");
  CHECK(formatted(*profile.findRegister("led_state"), 7) == "unknown (7)");  // a gap in the list
  CHECK(formatted(*profile.findRegister("array_fault"), 0) == "none");
  CHECK(formatted(*profile.findRegister("array_fault"), 0x0020) == "EEPROM setting edit (reset required)");
  CHECK(formatted(*profile.findRegister("array_fault"), 0x0009) == "Overcurrent, Battery HVD");
  CHECK(formatted(*profile.findRegister("array_fault"), 0x8000) == "bit 15");
  CHECK(formatted(*profile.findRegister("load_fault"), 0x0040) == "EEPROM setting edit (reset required)");
  CHECK(formatted(*profile.findRegister("dip_switch"), 0x000A) == "SW2: custom LVD/LVR, SW4: MODBUS");
  CHECK(formatted(*profile.findRegister("alarm"), 0x0000, 0x0001) == "RTS open");  // bit 0 = low word
  CHECK(formatted(*profile.findRegister("alarm"), 0x0001, 0x0000) == "P12");       // bit 16
  // Other scales.
  CHECK(formatted(*profile.findRegister("kWhc"), 85) == "8.5 kWh");
  CHECK(formatted(*profile.findRegister("Et_lvd_warn"), 600) == "60.0 s");
  CHECK(formatted(*profile.findRegister("va_ref_fixed_pct"), 128) == "50.0 %");

  // Engineering value -> raw word, with the profile's limits.
  const OvRegister &lvd = *profile.findRegister("EV_lvd");
  uint16_t raw = 0;
  CHECK_EQ(ovEncode(lvd, 13.10, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 0x10C5);  // 4293
  CHECK_EQ(ovEncode(*profile.findRegister("EV_lvr"), 13.25, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 0x10F6);
  CHECK_EQ(ovEncode(lvd, 11.00, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 3604);
  CHECK_EQ(ovEncode(lvd, 8.99, &raw), OV_ENCODE_BELOW_MIN);
  CHECK_EQ(ovEncode(lvd, 15.01, &raw), OV_ENCODE_ABOVE_MAX);
  CHECK_EQ(ovEncode(lvd, 0.0, &raw), OV_ENCODE_BELOW_MIN);  // the LVD cannot be switched off
  CHECK_EQ(ovEncode(lvd, std::numeric_limits<double>::quiet_NaN(), &raw), OV_ENCODE_OUT_OF_RANGE);
  const OvRegister &equalize = *profile.findRegister("EV_eq");
  CHECK_EQ(ovEncode(equalize, 0.0, &raw), OV_ENCODE_OK);  // 0 switches equalize off
  CHECK_EQ(raw, 0);
  CHECK_EQ(ovEncode(equalize, 5.0, &raw), OV_ENCODE_BELOW_MIN);
  const OvRegister &absorbTime = *profile.findRegister("Et_float");
  CHECK_EQ(ovEncode(absorbTime, 900, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 900);
  CHECK_EQ(ovEncode(absorbTime, 70000, &raw), OV_ENCODE_OUT_OF_RANGE);
  CHECK_EQ(ovEncode(absorbTime, -1, &raw), OV_ENCODE_OUT_OF_RANGE);
  CHECK_EQ(ovEncode(*profile.findRegister("Emodbus_id"), 2, &raw), OV_ENCODE_NOT_WRITABLE);
  CHECK_EQ(ovEncode(*profile.findRegister("adc_vb_f"), 12.0, &raw), OV_ENCODE_NOT_WRITABLE);
  // This profile has no signed setting, but another may: a signed register takes negative
  // values and stops at its own two ends.
  OvRegister offset = absorbTime;
  offset.type = OV_I16;
  CHECK_EQ(ovEncode(offset, -5, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 0xFFFB);
  CHECK_EQ(ovEncode(offset, -32768, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 0x8000);
  CHECK_EQ(ovEncode(offset, 32767, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 0x7FFF);
  CHECK_EQ(ovEncode(offset, 32768, &raw), OV_ENCODE_OUT_OF_RANGE);
  CHECK_EQ(ovEncode(offset, -32769, &raw), OV_ENCODE_OUT_OF_RANGE);
  // Only one word can be written, whatever the register says about itself.
  OvRegister wide = absorbTime;
  wide.words = 2;
  CHECK_EQ(ovEncode(wide, 900, &raw), OV_ENCODE_NOT_WRITABLE);

  // With the other candidate scale the same voltages give the raw values worked out by hand
  // in the lithium review (13.10 V = 4441, 11.00 V = 3729).
  profile.findScale("vset")->num = 96.667;
  CHECK_EQ(ovEncode(lvd, 13.10, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 0x1159);
  CHECK_EQ(ovEncode(lvd, 11.00, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 3729);
  // The settings scale is not the scale of the readings: those have not moved...
  CHECK(formatted(*profile.findRegister("adc_vb_f"), 0x1183) == "13.68 V");
  profile.findScale("vlive")->num = 96.667;
  CHECK(formatted(*profile.findRegister("adc_vb_f"), 0x1183) == "13.23 V");  // 13.2250 at x96.667
  // ...and changing the readings' scale does not change how a setting is encoded.
  profile.findScale("vset")->num = 100.0;
  CHECK_EQ(ovEncode(lvd, 13.10, &raw), OV_ENCODE_OK);
  CHECK_EQ(raw, 0x10C5);
  profile.findScale("vlive")->num = 100.0;

  // Round trip: every value 'set' can produce reads back as the same text.
  for (int centivolts = 900; centivolts <= 1500; ++centivolts) {
    const double volts = centivolts / 100.0;
    CHECK_EQ(ovEncode(lvd, volts, &raw), OV_ENCODE_OK);
    const std::string shown = formatted(lvd, raw);
    CHECK_MSG(shown == fixed(volts, 2) + " V", "%.2f came back as %s", volts, shown.c_str());
  }
}

static void expectRejected(const std::string &json, const char *because) {
  OvProfile profile;
  char error[200] = "";
  const bool ok = profile.load(json.c_str(), json.size(), error, sizeof(error));
  CHECK_MSG(!ok, "accepted a profile that should fail with '%s'", because);
  CHECK_MSG(strstr(error, because) != nullptr, "expected an error with '%s', got '%s'", because, error);
  CHECK(!profile.loaded());
  CHECK_EQ(profile.registerCount(), 0);
  CHECK_EQ(profile.coilCount(), 0);
  CHECK_MSG(gAllocLive == 0, "%ld allocations left after '%s'", gAllocLive, because);
}

// A profile with two voltage scales and a scale check, to break one piece at a time.
static const char kScaleProfile[] =
    "{\"schema\":1,\"profile\":\"scales\",\"title\":\"Scales\",\"version\":\"1\","
    "\"scales\":{\"vl\":{\"num\":100,\"den\":32768,\"units\":\"V\",\"decimals\":2},"
    "\"vs\":{\"num\":100,\"den\":32768,\"units\":\"V\",\"decimals\":2},"
    "\"vr\":{\"num\":96.667,\"den\":32768,\"units\":\"V\",\"decimals\":2}},"
    "\"labels\":{\"st\":[\"OFF\",\"HOLD\"]},"
    "\"groups\":[{\"name\":\"g\",\"title\":\"G\"}],"
    "\"registers\":["
    "{\"addr\":8,\"name\":\"batt\",\"scale\":\"vl\",\"group\":\"g\"},"
    "{\"addr\":9,\"name\":\"slow\",\"scale\":\"vl\",\"group\":\"g\"},"
    "{\"addr\":10,\"name\":\"target\",\"scale\":\"vr\",\"group\":\"g\"},"
    "{\"addr\":11,\"name\":\"state\",\"type\":\"enum\",\"labels\":\"st\",\"group\":\"g\"},"
    "{\"addr\":12,\"name\":\"wide\",\"type\":\"u32\",\"scale\":\"vl\",\"group\":\"g\"},"
    "{\"addr\":14,\"name\":\"plain\",\"group\":\"g\"},"
    "{\"addr\":\"0xE000\",\"name\":\"sp1\",\"scale\":\"vs\",\"group\":\"g\",\"writable\":true,\"min\":9,\"max\":17},"
    "{\"addr\":\"0xE001\",\"name\":\"sp2\",\"scale\":\"vs\",\"group\":\"g\",\"writable\":true,\"min\":9,\"max\":17}],"
    "\"modbus\":{\"slaveId\":1,\"baud\":9600,\"format\":\"8N2\",\"readFunction\":4},"
    "\"scaleCheck\":{\"alternates\":[96.667],\"grid\":0.05,"
    "\"settings\":{\"scale\":\"vs\",\"setpoints\":[\"sp1\",\"sp2\"]},"
    "\"live\":{\"scale\":\"vl\",\"meter\":\"batt\",\"battery\":\"slow\",\"reference\":\"target\","
    "\"state\":\"state\",\"regulating\":[1]}}}";

static void testScaleCheckRejects() {
  const std::string good(kScaleProfile);
  {
    OvProfile profile;
    char error[200];
    CHECK_MSG(profile.load(good.c_str(), good.size(), error, sizeof(error)), "%s", error);
    const OvScaleCheck &check = profile.scaleCheck();
    CHECK(check.present);
    CHECK(check.settingsScale == profile.findScale("vs"));
    CHECK(check.liveScale == profile.findScale("vl"));
    CHECK_EQ(check.setpointCount, 2);
    CHECK(check.meter == profile.findRegister("batt"));
    CHECK(check.battery == profile.findRegister("slow"));
    CHECK(check.reference == profile.findRegister("target"));
    CHECK(check.state == profile.findRegister("state"));
    CHECK_EQ(check.regulatingCount, 1);
    CHECK(check.grid == 0.05);
  }
  {  // Either half may be left out; the other still works.
    OvProfile profile;
    char error[200];
    const std::string settingsOnly =
        replaced(good, ",\"live\":{\"scale\":\"vl\",\"meter\":\"batt\",\"battery\":\"slow\",\"reference\":\"target\","
                       "\"state\":\"state\",\"regulating\":[1]}",
                 "");
    CHECK_MSG(profile.load(settingsOnly.c_str(), settingsOnly.size(), error, sizeof(error)), "%s", error);
    CHECK(profile.scaleCheck().present);
    CHECK(profile.scaleCheck().settingsScale != nullptr);
    CHECK(profile.scaleCheck().liveScale == nullptr);
    CHECK(profile.scaleCheck().meter == nullptr);
    const std::string liveOnly = replaced(good, "\"settings\":{\"scale\":\"vs\",\"setpoints\":[\"sp1\",\"sp2\"]},", "");
    CHECK_MSG(profile.load(liveOnly.c_str(), liveOnly.size(), error, sizeof(error)), "%s", error);
    CHECK(profile.scaleCheck().present);
    CHECK(profile.scaleCheck().settingsScale == nullptr);
    CHECK_EQ(profile.scaleCheck().setpointCount, 0);
    CHECK(profile.scaleCheck().liveScale != nullptr);
    // With neither half there is nothing to check.
    const std::string neither = replaced(liveOnly, ",\"live\":{\"scale\":\"vl\",\"meter\":\"batt\",\"battery\":\"slow\","
                                                   "\"reference\":\"target\",\"state\":\"state\",\"regulating\":[1]}",
                                         "");
    CHECK_MSG(profile.load(neither.c_str(), neither.size(), error, sizeof(error)), "%s", error);
    CHECK(!profile.scaleCheck().present);
    // The grid has a default.
    const std::string noGrid = replaced(good, "\"grid\":0.05,", "");
    CHECK_MSG(profile.load(noGrid.c_str(), noGrid.size(), error, sizeof(error)), "%s", error);
    CHECK(profile.scaleCheck().grid == 0.05);
  }
  CHECK_EQ(gAllocLive, 0);

  expectRejected(replaced(good, "\"grid\":0.05", "\"grid\":0"), "scaleCheck.grid");
  expectRejected(replaced(good, "\"settings\":{\"scale\":\"vs\"", "\"settings\":{\"scale\":\"zz\""),
                 "scaleCheck.settings.scale names no scale");
  expectRejected(replaced(good, "\"settings\":{\"scale\":\"vs\",", "\"settings\":{"),
                 "scaleCheck.settings.scale names no scale");
  // A setpoint must exist, be on the settings scale and be one word.
  expectRejected(replaced(good, "[\"sp1\",\"sp2\"]", "[\"sp1\",\"nope\"]"), "scaleCheck.settings.setpoints");
  expectRejected(replaced(good, "[\"sp1\",\"sp2\"]", "[\"sp1\",\"batt\"]"), "scaleCheck.settings.setpoints");
  expectRejected(replaced(good, "[\"sp1\",\"sp2\"]", "[\"sp1\",\"plain\"]"), "scaleCheck.settings.setpoints");
  expectRejected(replaced(good, "[\"sp1\",\"sp2\"]", "[\"sp1\",7]"), "scaleCheck.settings.setpoints");
  expectRejected(replaced(good, "\"live\":{\"scale\":\"vl\"", "\"live\":{\"scale\":\"zz\""),
                 "scaleCheck.live.scale names no scale");
  // One scale for both halves would let a multimeter decide how settings are written.
  expectRejected(replaced(replaced(good, "\"live\":{\"scale\":\"vl\"", "\"live\":{\"scale\":\"vs\""),
                          "\"meter\":\"batt\",\"battery\":\"slow\",", ""),
                 "must be two scales");
  expectRejected(replaced(good, "\"meter\":\"batt\"", "\"meter\":\"nope\""), "scaleCheck.live.meter names no register");
  expectRejected(replaced(good, "\"battery\":\"slow\"", "\"battery\":\"nope\""),
                 "scaleCheck.live.battery names no register");
  expectRejected(replaced(good, "\"reference\":\"target\"", "\"reference\":\"nope\""),
                 "scaleCheck.live.reference names no register");
  expectRejected(replaced(good, "\"state\":\"state\"", "\"state\":\"nope\""), "scaleCheck.live.state names no register");
  expectRejected(replaced(good, "\"meter\":\"batt\"", "\"meter\":\"sp1\""), "scaleCheck.live.meter must be");
  expectRejected(replaced(good, "\"meter\":\"batt\"", "\"meter\":\"wide\""), "scaleCheck.live.meter must be");
  expectRejected(replaced(good, "\"battery\":\"slow\"", "\"battery\":\"target\""), "scaleCheck.live.battery must be");
  // The target needs a scale, and not the one under test: on that one the battery and the
  // target would agree at any numerator.
  expectRejected(replaced(good, "\"reference\":\"target\"", "\"reference\":\"plain\""),
                 "scaleCheck.live.reference must be");
  expectRejected(replaced(good, "\"reference\":\"target\"", "\"reference\":\"batt\""),
                 "scaleCheck.live.reference must be");
  expectRejected(replaced(good, "\"state\":\"state\"", "\"state\":\"wide\""), "scaleCheck.live.state must be");
  // No setting may be written with the scale that the multimeter decides, setpoint or not.
  expectRejected(replaced(replaced(good, "[\"sp1\",\"sp2\"]", "[\"sp1\"]"),
                          "\"name\":\"sp2\",\"scale\":\"vs\"", "\"name\":\"sp2\",\"scale\":\"vl\""),
                 "a writable register cannot use scaleCheck.live.scale: sp2");
}

static void testProfileRejects() {
  const std::string mini(kMiniProfile);
  {
    OvProfile profile;
    char error[200];
    CHECK_MSG(profile.load(mini.c_str(), mini.size(), error, sizeof(error)), "%s", error);
    CHECK_EQ(profile.registerCount(), 5);
    CHECK_EQ(profile.groupCount(), 2);  // "h" was not declared and is added
    CHECK_EQ(profile.findRegister("b")->addr, 0xE011);
    CHECK_EQ(profile.findRegister("c")->words, 2);
    CHECK(profile.findRegister("e")->loFirst);
    CHECK(profile.findRegister("a")->writable);
    CHECK(!profile.findRegister("a")->bench);
    CHECK(!profile.scaleCheck().present);
    CHECK_EQ(profile.modbus().maxBlock, 3);
    CHECK_EQ(profile.findRegister("a")->type, OV_U16);
    CHECK_EQ(profile.findRegister("b")->type, OV_ENUM);
    CHECK_EQ(profile.findRegister("c")->type, OV_U32);
  }
  {  // Each type name gives its own type, and of the one-word types only the two plain 16-bit
     // numbers may be writable.
    const struct {
      const char *json;
      uint8_t type;
      bool mayBeWritable;
    } kinds[] = {{"\"type\":\"u16\"", OV_U16, true},
                 {"\"type\":\"i16\"", OV_I16, true},
                 {"\"type\":\"u8\"", OV_U8, false},
                 {"\"type\":\"i8\"", OV_I8, false},
                 {"\"type\":\"enum\",\"labels\":\"st\"", OV_ENUM, false},
                 {"\"type\":\"bits\",\"labels\":\"st\"", OV_BITS, false}};
    const char *const last =
        "{\"addr\":\"0xE015\",\"name\":\"e\",\"type\":\"u32\",\"order\":\"lo-hi\",\"group\":\"h\"}";
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
      const std::string entry = std::string(last) + ",{\"addr\":\"0xE020\",\"name\":\"t\"," + kinds[i].json;
      const std::string typed = replaced(mini, last, (entry + "}").c_str());
      const std::string writable = replaced(mini, last, (entry + ",\"writable\":true}").c_str());
      {
        OvProfile profile;
        char error[200];
        CHECK_MSG(profile.load(typed.c_str(), typed.size(), error, sizeof(error)), "%s: %s", kinds[i].json, error);
        CHECK_MSG(profile.findRegister("t")->type == kinds[i].type, "%s", kinds[i].json);
        CHECK_EQ(profile.findRegister("t")->words, 1);
        CHECK(!profile.findRegister("t")->writable);  // unless the profile says so
        if (kinds[i].mayBeWritable) {
          CHECK_MSG(profile.load(writable.c_str(), writable.size(), error, sizeof(error)), "%s: %s", kinds[i].json,
                    error);
          CHECK(profile.findRegister("t")->writable);
        }
      }
      if (!kinds[i].mayBeWritable) {
        expectRejected(writable, "only one-word u16 and i16 registers can be writable");
      }
    }
  }
  {  // The registers may stand in any order in the file. They are kept by address, which the
     // block reads rely on, and everything that names one still finds the right one.
    const char *const first = "{\"addr\":\"0xE010\",\"name\":\"a\",\"scale\":\"v\",\"group\":\"g\",\"writable\":true,"
                              "\"min\":1,\"max\":20},";
    const char *const last =
        "{\"addr\":\"0xE015\",\"name\":\"e\",\"type\":\"u32\",\"order\":\"lo-hi\",\"group\":\"h\"}";
    std::string moved(first);
    moved.erase(moved.size() - 1);  // without its comma
    const std::string shuffled = replaced(replaced(mini, first, ""), last, (std::string(last) + "," + moved).c_str());
    {
      OvProfile profile;
      char error[200];
      CHECK_MSG(profile.load(shuffled.c_str(), shuffled.size(), error, sizeof(error)), "%s", error);
      CHECK_EQ(profile.registerCount(), 5);
      for (size_t i = 0; i + 1 < profile.registerCount(); ++i) {
        CHECK_MSG(profile.reg(i).addr < profile.reg(i + 1).addr, "%s stands before %s", profile.reg(i).name,
                  profile.reg(i + 1).name);
      }
      CHECK(strcmp(profile.reg(0).name, "a") == 0);
      CHECK(profile.findRegister("a")->writable && profile.findRegister("a")->hasMax);
      CHECK(profile.modbus().probeRegister == profile.findRegister("a"));
      CHECK(profile.check(0).below == profile.findRegister("a"));
      CHECK(profile.check(0).above == profile.findRegister("d"));
    }
    // An overlap is found wherever the two entries stand in the file.
    expectRejected(replaced(shuffled, "\"addr\":\"0xE010\"", "\"addr\":\"0xE013\""), "share an address");
  }
  {  // What the profile says about the link is what OptaView starts with.
    std::string own = replaced(mini, "\"slaveId\":1", "\"slaveId\":5");
    own = replaced(own, "\"baud\":9600", "\"baud\":19200");
    own = replaced(own, "\"format\":\"8N2\"", "\"format\":\"8E1\"");
    own = replaced(own, "\"readFunction\":4", "\"readFunction\":3");
    own = replaced(own, "\"maxBlock\":3", "\"maxBlock\":2,\"responseTimeoutMs\":250,\"resetWaitMs\":400");
    OvProfile profile;
    char error[200];
    CHECK_MSG(profile.load(own.c_str(), own.size(), error, sizeof(error)), "%s", error);
    const OvModbusDefaults &link = profile.modbus();
    CHECK_EQ(link.slaveId, 5);
    CHECK_EQ(link.baud, 19200u);
    CHECK_EQ(link.format, MB_8E1);
    CHECK_EQ(link.readFunction, 3);
    CHECK_EQ(link.maxBlock, 2);
    CHECK_EQ(link.responseTimeoutMs, 250);
    CHECK_EQ(link.resetWaitMs, 400);
    // And a controller that answers at those settings only is read at once.
    Rig rig(100.0, own.c_str());
    rig.sim.slaveId = 5;
    rig.sim.deviceBaud = 19200;
    rig.sim.deviceFormat = MB_8E1;
    rig.sim.readInput = false;  // function 03 only
    rig.sim.regs[0xE010] = rig.sim.setVolts(12.00);
    CHECK_HAS(rig.run("status"), "slave 5, 19200 8E1, read function 3, timeout 250 ms");
    CHECK_HAS(rig.run("get a"), "12.00 V");
    CHECK_EQ(rig.sim.requests.back()[0], 5);
    CHECK_EQ(rig.sim.requests.back()[1], 3);
    CHECK_EQ(rig.master.responseTimeoutMs, 250);
  }
  CHECK_EQ(gAllocLive, 0);

  expectRejected("", "empty");
  expectRejected("{", "not valid JSON");
  expectRejected("[1,2]", "JSON object");
  expectRejected(replaced(mini, "\"schema\":1", "\"schema\":2"), "schema");
  expectRejected(replaced(mini, "\"profile\":\"mini\",", ""), "no \"profile\" name");
  expectRejected("{\"schema\":1,\"profile\":\"x\",\"registers\":[]}", "no registers");
  expectRejected(replaced(mini, "\"name\":\"a\"", "\"name\":\"9a\""), "needs a \"name\"");
  expectRejected(replaced(mini, "\"name\":\"a\"", "\"name\":\"a b\""), "needs a \"name\"");
  expectRejected(replaced(mini, "\"name\":\"b\"", "\"name\":\"A\""), "share a name");
  expectRejected(replaced(mini, "\"addr\":57361", "\"addr\":\"0xE010\""), "share an address");
  expectRejected(replaced(mini, "\"addr\":\"0xE012\"", "\"addr\":\"0xE013\""), "share an address");  // the u32 runs into d
  expectRejected(replaced(mini, "\"addr\":\"0xE012\"", "\"addr\":\"0xFFFF\""), "runs past");
  expectRejected(replaced(mini, "\"addr\":57361", "\"addr\":70000"), "bad or missing \"addr\"");
  expectRejected(replaced(mini, "\"addr\":57361", "\"addr\":\"0xZZ\""), "bad or missing \"addr\"");
  expectRejected(replaced(mini, "\"addr\":57361,", ""), "bad or missing \"addr\"");
  expectRejected(replaced(mini, "\"addr\":\"0xFF\"", "\"addr\":65536"), "bad or missing \"addr\" on coil");
  expectRejected(replaced(mini, "\"addr\":\"0xFF\"", "\"addr\":\"0x100FF\""), "bad or missing \"addr\" on coil");
  expectRejected(replaced(mini, "\"addr\":\"0xFF\",", ""), "bad or missing \"addr\" on coil");
  expectRejected(replaced(mini, "\"scale\":\"v\"", "\"scale\":\"zz\""), "unknown \"scale\"");
  expectRejected(replaced(mini, "\"labels\":\"st\"", "\"labels\":\"zz\""), "unknown \"labels\"");
  expectRejected(replaced(mini, "\"type\":\"enum\"", "\"type\":\"u64\""), "unknown \"type\"");
  expectRejected(replaced(mini, "\"type\":\"enum\"", "\"type\":\"enum\",\"scale\":\"v\""), "cannot have a scale");
  expectRejected(replaced(mini, "\"type\":\"u32\",\"group\":\"g\"", "\"type\":\"u32\",\"group\":\"g\",\"writable\":true"),
                 "only one-word");
  expectRejected(replaced(mini, "\"min\":1,\"max\":20", "\"min\":21,\"max\":20"), "\"min\" is above \"max\"");
  expectRejected(replaced(mini, "\"order\":\"lo-hi\"", "\"order\":\"mid\""), "\"order\"");
  expectRejected(replaced(mini, "\"name\":\"d\"", "\"name\":\"d\",\"verified\":\"maybe\""), "\"verified\"");
  expectRejected(replaced(mini, "\"name\":\"d\"", "\"name\":\"d\",\"sentinel\":{\"raw\":1}"), "sentinel");
  expectRejected(replaced(mini, "\"name\":\"d\"", "\"name\":\"d\",\"type\":\"bits\",\"labels\":\"st\",\"words\":3"),
                 "\"words\"");
  expectRejected(replaced(mini, "\"den\":32768", "\"den\":0"), "above zero");
  expectRejected(replaced(mini, "\"num\":100,", ""), "needs \"num\" and \"den\"");
  expectRejected(replaced(mini, "[\"A\",\"B\"]", "[]"), "label set");
  expectRejected(replaced(mini, "{\"name\":\"g\",\"title\":\"G\"}", "{\"title\":\"G\"}"), "a group needs");
  expectRejected(replaced(mini, "\"slaveId\":1", "\"slaveId\":0"), "slaveId");
  expectRejected(replaced(mini, "\"slaveId\":1", "\"slaveId\":248"), "slaveId");
  expectRejected(replaced(mini, "\"readFunction\":4", "\"readFunction\":6"), "readFunction");
  expectRejected(replaced(mini, "\"maxBlock\":3", "\"maxBlock\":0"), "maxBlock");
  expectRejected(replaced(mini, "\"maxBlock\":3", "\"maxBlock\":126"), "maxBlock");
  expectRejected(replaced(mini, "\"baud\":9600", "\"baud\":1"), "baud");
  expectRejected(replaced(mini, "\"format\":\"8N2\"", "\"format\":\"7E1\""), "format");
  expectRejected(replaced(mini, "\"probeRegister\":\"a\"", "\"probeRegister\":\"zz\""), "probeRegister");
  expectRejected(replaced(mini, "\"resetCoil\":\"reset\"", "\"resetCoil\":\"zz\""), "resetCoil");
  expectRejected(replaced(mini, "\"liveGroups\":[\"g\"]", "\"liveGroups\":[\"zz\"]"), "liveGroups");
  expectRejected(replaced(mini, "\"below\":\"a\"", "\"below\":\"b\""), "a check needs");  // b is an enum
  expectRejected(replaced(mini, "\"below\":\"a\"", "\"below\":\"zz\""), "a check needs");
  expectRejected(replaced(mini, "\"coils\":[{\"addr\":\"0xFF\",\"name\":\"reset\",\"trigger\":true}]",
                          "\"coils\":[{\"addr\":1,\"name\":\"x\"},{\"addr\":2,\"name\":\"X\"}]"),
                 "two coils share a name");
  testScaleCheckRejects();

  // A truncated file (a download cut short) is refused cleanly wherever the cut falls.
  const std::string full(OV_BUILTIN_PROFILE);
  size_t cuts = 0;
  for (size_t length = 0; length < full.size(); length += (length + 400 > full.size() ? 1 : 89)) {
    OvProfile profile;
    char error[200];
    CHECK_MSG(!profile.load(full.c_str(), length, error, sizeof(error)), "accepted the first %zu bytes", length);
    CHECK(error[0] != '\0');
    ++cuts;
  }
  CHECK(cuts > 500);
  CHECK_EQ(gAllocLive, 0);
}

static void testProfileAllocationFailures() {
  // Count the allocations of a normal load, then fail each one in turn.
  gAllocCalls = 0;
  gAllocFailAt = -1;
  long total = 0;
  {
    OvProfile profile;
    char error[200];
    CHECK(profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error)));
    total = gAllocCalls;
  }
  CHECK(total >= 15);
  for (long failAt = 0; failAt < total; ++failAt) {
    OvProfile profile;
    char error[200] = "";
    gAllocCalls = 0;
    gAllocFailAt = failAt;
    const bool ok = profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error));
    gAllocFailAt = -1;
    CHECK_MSG(!ok, "load survived a failed allocation %ld", failAt);
    CHECK_MSG(strstr(error, "out of memory") != nullptr, "allocation %ld: '%s'", failAt, error);
    CHECK(!profile.loaded());
    CHECK_MSG(gAllocLive == 0, "allocation %ld: %ld left", failAt, gAllocLive);
    // The same object loads fine afterwards.
    CHECK(profile.load(OV_BUILTIN_PROFILE, strlen(OV_BUILTIN_PROFILE), error, sizeof(error)));
    CHECK_EQ(profile.registerCount(), 94);
  }
  CHECK_EQ(gAllocLive, 0);
}

// ============================================================================
// Device layer
// ============================================================================
static void testSpans() {
  {
    Rig rig;
    // Every register is in exactly one span, in order; spans never cross a gap, split a
    // register or exceed the profile's block size.
    size_t cursor = 0;
    size_t next = 0;
    size_t spans = 0;
    OvSpan span;
    while (rig.device.nextSpan(nullptr, &cursor, &span)) {
      ++spans;
      CHECK_EQ(span.first, next);
      CHECK(span.count >= 1);
      CHECK(span.words <= 16);
      uint32_t addr = span.addr;
      for (size_t k = 0; k < span.count; ++k) {
        const OvRegister &reg = rig.profile.reg(span.first + k);
        CHECK_MSG(reg.addr == addr, "%s is not next to its neighbour", reg.name);
        addr += reg.words;
      }
      CHECK_EQ(addr - span.addr, span.words);
      // Every address of the span exists in the simulated controller: no reserved gaps are read.
      for (uint32_t a = span.addr; a < addr; ++a) {
        CHECK_MSG(rig.sim.regs.count((uint16_t)a) == 1, "0x%04X is not a SunSaver register", (unsigned)a);
      }
      next = span.first + span.count;
    }
    CHECK_EQ(next, rig.profile.registerCount());
    CHECK(spans >= 10 && spans <= 30);

    // The 'live' group is one request; 'status' is three runs.
    cursor = 0;
    CHECK(rig.device.nextSpan(rig.profile.findGroup("live")->name, &cursor, &span));
    CHECK_EQ(span.addr, 0x0008);
    CHECK_EQ(span.words, 9);
    CHECK(!rig.device.nextSpan(rig.profile.findGroup("live")->name, &cursor, &span));
    cursor = 0;
    const char *status = rig.profile.findGroup("status")->name;
    CHECK(rig.device.nextSpan(status, &cursor, &span) && span.addr == 0x0011 && span.words == 4);
    CHECK(rig.device.nextSpan(status, &cursor, &span) && span.addr == 0x001A && span.words == 3);
    CHECK(rig.device.nextSpan(status, &cursor, &span) && span.addr == 0x0023 && span.words == 4);
    CHECK(!rig.device.nextSpan(status, &cursor, &span));
    // The stored totals: five two-word counters and four words, then one register after a gap.
    cursor = 0;
    const char *totals = rig.profile.findGroup("totals")->name;
    CHECK(rig.device.nextSpan(totals, &cursor, &span) && span.addr == 0xE040 && span.words == 14);
    CHECK(rig.device.nextSpan(totals, &cursor, &span) && span.addr == 0xE04F && span.words == 1);
  }
  {
    // A block size of 3: a two-word register is kept whole.
    Rig rig(100.0, kMiniProfile);
    size_t cursor = 0;
    OvSpan span;
    CHECK(rig.device.nextSpan(nullptr, &cursor, &span) && span.addr == 0xE010 && span.words == 2 && span.count == 2);
    CHECK(rig.device.nextSpan(nullptr, &cursor, &span) && span.addr == 0xE012 && span.words == 3 && span.count == 2);
    CHECK(rig.device.nextSpan(nullptr, &cursor, &span) && span.addr == 0xE015 && span.words == 2 && span.count == 1);
    CHECK(!rig.device.nextSpan(nullptr, &cursor, &span));

    // And a two-word register is read correctly whatever the block mode.
    rig.sim.setU32HiLo(0xE012, 0x00012345UL);
    rig.sim.setU32LoHi(0xE015, 0x00067890UL);
    for (uint8_t mode = OV_BLOCKS_AUTO; mode <= OV_BLOCKS_OFF; ++mode) {
      rig.device.setBlockMode(mode);
      rig.device.beginBatch();
      uint16_t words[2] = {0, 0};
      OvWordStatus status;
      CHECK(rig.device.readRegister(*rig.profile.findRegister("c"), words, &status));
      CHECK_EQ(ovRawValue(*rig.profile.findRegister("c"), words), 0x00012345UL);
      CHECK(rig.device.readRegister(*rig.profile.findRegister("e"), words, &status));
      CHECK_EQ(ovRawValue(*rig.profile.findRegister("e"), words), 0x00067890UL);
    }
  }
}

static void testReadsAndFallback() {
  {  // Blocks work: 'live' takes four requests.
    Rig rig;
    const std::string out = rig.run("live");
    CHECK_EQ(rig.sim.requests.size(), 4);
    CHECK_HAS(lineWith(out, "adc_vb_f"), "13.68 V");
    CHECK_HAS(lineWith(out, "adc_vb_f"), "[0x1183]");
    CHECK_HAS(lineWith(out, "charge_state"), "FLOAT (7)");
    CHECK_HAS(lineWith(out, "dip_switch"), "SW2: custom LVD/LVR, SW4: MODBUS");
    CHECK_HAS(lineWith(out, "T_rts"), "not connected");
    CHECK_HAS(lineWith(out, "alarm"), "[0x0000 0x0000]");
    CHECK(!rig.device.blocksFailed());
    CHECK_HASNT(out, "block reads do not work");
  }
  {  // The MRC-1 case the TankAlarm client works around: no reply to a block, singles are fine.
    Rig rig;
    rig.sim.blocksAnswered = false;
    const std::string out = rig.run("live");
    CHECK(rig.device.blocksFailed());
    // The block, its first register alone, the block once more, then word by word.
    CHECK_EQ(rig.sim.requests.size(), 3u + 8u + 11u);
    CHECK_EQ(rig.sim.requests[0][5], 9);
    CHECK_EQ(rig.sim.requests[1][5], 1);
    CHECK_EQ(rig.sim.requests[2][5], 9);
    for (size_t i = 3; i < rig.sim.requests.size(); ++i) {
      CHECK_EQ(rig.sim.requests[i][5], 1);
    }
    CHECK(!rig.device.linkDown());
    CHECK_HAS(lineWith(out, "adc_vb_f"), "13.68 V");
    CHECK_HAS(lineWith(out, "led_state"), "FLOAT (slow green blink) (4)");
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_HAS(out, "block reads do not work on this link");
    // Told once, remembered for the session.
    const size_t before = rig.sim.requests.size();
    const std::string again = rig.run("live");
    CHECK_EQ(rig.sim.requests.size() - before, 20);
    CHECK_HASNT(again, "block reads do not work");
    CHECK_HAS(rig.run("status"), "gave up on blocks");
    // 'blocks auto' tries again.
    rig.sim.blocksAnswered = true;
    CHECK_HAS(rig.run("blocks auto"), "Block reads: auto");
    const size_t before2 = rig.sim.requests.size();
    rig.run("live");
    CHECK_EQ(rig.sim.requests.size() - before2, 4);
  }
  {  // A controller that refuses blocks with an exception.
    Rig rig;
    rig.sim.blocksRefused = true;
    const std::string out = rig.run("live");
    CHECK(rig.device.blocksFailed());
    CHECK_HAS(lineWith(out, "adc_il_f"), "0.14 A");
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_EQ(rig.sim.requests.size(), 1u + 9u + 11u);
  }
  {  // 'blocks off' never asks for more than one register.
    Rig rig;
    CHECK_HAS(rig.run("blocks off"), "Block reads: off");
    rig.run("live");
    CHECK_EQ(rig.sim.requests.size(), 20);
    for (size_t i = 0; i < rig.sim.requests.size(); ++i) {
      CHECK_EQ(rig.sim.requests[i][5], 1);
    }
    CHECK_HAS(rig.run("blocks"), "Block reads: off");
    CHECK_HAS(rig.run("blocks sideways"), "Usage: blocks");
  }
  {  // 'blocks on' with a controller that refuses blocks: each register of a refused block is
     // asked alone, so everything is still read. Blocks stay on, because that was asked for, and
     // nothing is said about having given them up.
    Rig rig;
    rig.sim.blocksRefused = true;
    rig.run("blocks on");
    const std::string out = rig.run("dump live");
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_HAS(lineWith(out, "adc_vb_f"), "13.68 V");
    CHECK_EQ(rig.sim.requests.size(), 1u + 9u);
    CHECK(!rig.device.blocksFailed());
    CHECK_HASNT(out, "block reads do not work");
    CHECK_HAS(rig.run("blocks"), "Block reads: on\r\n");
    CHECK_HASNT(rig.run("status"), "gave up on blocks");
  }
  {  // A long read is cut into blocks of the size the profile allows: 16 for the SunSaver.
    Rig rig;
    const std::string out = rig.run("get 0x0008 40");
    CHECK_HAS(out, "0x002F  ");
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_EQ(rig.sim.requests.size(), 3);
    const uint16_t starts[] = {0x0008, 0x0018, 0x0028};
    const uint16_t counts[] = {16, 16, 8};
    for (size_t i = 0; i < 3 && i < rig.sim.requests.size(); ++i) {
      CHECK_EQ((rig.sim.requests[i][2] << 8) | rig.sim.requests[i][3], starts[i]);
      CHECK_EQ((rig.sim.requests[i][4] << 8) | rig.sim.requests[i][5], counts[i]);
    }
  }
  {  // A two-word register is read when both of its words were. With one of them missing it is
     // shown as not read, never as a number made of the half that came.
    Rig rig;
    rig.sim.regs.erase(0x0018);  // the low word of Ahc_t
    uint16_t words[2] = {0, 0};
    OvWordStatus status;
    CHECK(!rig.device.readRegister(*rig.profile.findRegister("Ahc_t"), words, &status));
    CHECK_EQ(status.status, MB_EXCEPTION);
    CHECK_EQ(status.exception, 2);
    CHECK_HAS(lineWith(rig.run("get Ahc_t"), " Ahc_t "), "-- exception 2 (illegal data address)");
    const std::string dump = rig.run("dump counters");
    CHECK_HAS(lineWith(dump, " Ahc_t "), "-- exception 2 (illegal data address)");
    CHECK_HAS(lineWith(dump, " Ahc_r "), "123.4 Ah");  // its neighbours are read as before
    CHECK_HAS(lineWith(dump, " kWhc "), "8.5 kWh");
    const std::string span = rig.run("get 0x0017 2");
    CHECK_HAS(span, "0x0017  0x0001");
    CHECK_HAS(span, "0x0018  -- exception 2");
    CHECK_HASNT(span, "Ahc_t");
  }
  {  // 'blocks on' does not fall back: a lost block is reported as lost.
    Rig rig;
    rig.sim.blocksAnswered = false;
    rig.run("blocks on");
    const std::string out = rig.run("dump live");
    CHECK(!rig.device.blocksFailed());
    CHECK_EQ(countOf(out, "-- no reply"), 9);
  }
  {  // 'blocks on' repeats a block like any other read: one lost reply goes unnoticed, and it is
     // the block that is asked for again.
    Rig rig;
    rig.run("blocks on");
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    const std::string out = rig.run("dump live");
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_HAS(lineWith(out, "adc_vb_f"), "13.68 V");
    CHECK_EQ(rig.sim.requests.size(), 2);
    CHECK_EQ(rig.sim.requests[0][5], 9);
    CHECK(rig.sim.requests[1] == rig.sim.requests[0]);
    // As often as 'retries' says, and no more often.
    Rig twice;
    twice.run("blocks on");
    twice.device.retries = 2;
    twice.sim.blocksAnswered = false;
    CHECK_EQ(countOf(twice.run("dump live"), "-- no reply"), 9);
    CHECK_EQ(twice.sim.requests.size(), 3);
    Rig once;
    once.run("blocks on");
    once.device.retries = 0;
    once.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    CHECK_EQ(countOf(once.run("dump live"), "-- no reply"), 9);
    CHECK_EQ(once.sim.requests.size(), 1);
  }
  {  // An older controller without registers 0x0038-0x003A: the block is refused, each register
     // is asked alone, and only the missing ones are reported. Blocks stay in use.
    Rig rig;
    rig.sim.makeOldFirmware();
    const std::string out = rig.run("dump mppt");
    CHECK_HAS(lineWith(out, "Sweep_Voc"), "21.30 V");
    CHECK_HAS(lineWith(out, "va_ref_fixed "), "-- exception 2 (illegal data address)");
    CHECK_EQ(countOf(out, "-- exception 2"), 3);
    CHECK(!rig.device.blocksFailed());
    CHECK(!rig.device.linkDown());
    CHECK_HASNT(out, "not answering");
  }
  {  // One lost reply is retried and nobody notices.
    Rig rig;
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    const std::string out = rig.run("get adc_vb_f");
    CHECK_HAS(out, "13.68 V");
    CHECK_EQ(rig.master.stats.timeouts, 1);
    CHECK_EQ(rig.master.stats.ok, 1);
  }
  {  // The same controller, and the reply to the first register that is asked alone is lost as
     // well. That is the third request in a row without an answer only if the block was repeated
     // first: the single read comes second, is repeated, and the batch goes on.
    Rig rig;
    rig.sim.blocksAnswered = false;
    rig.sim.failNext(SimSunSaver::FAULT_NONE);      // the block: silence anyway
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY);  // its first register alone
    const std::string out = rig.run("live");
    CHECK(!rig.device.linkDown());
    CHECK(rig.device.blocksFailed());
    CHECK_HASNT(out, "The controller is not answering");
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_HAS(lineWith(out, "adc_vb_f"), "13.68 V");
    CHECK_HAS(lineWith(out, "led_state"), "FLOAT (slow green blink) (4)");
    CHECK_EQ(rig.sim.requests.size(), 4u + 8u + 11u);  // block, single, single again, block, the rest
    CHECK_EQ(rig.sim.requests[0][5], 9);
    CHECK_EQ(rig.sim.requests[1][5], 1);
    CHECK_EQ(rig.sim.requests[2][5], 1);
    CHECK_EQ(rig.sim.requests[3][5], 9);
    CHECK_HAS(out, "block reads do not work on this link");
  }
  {  // One lost reply to a block does not end block reads: the register alone answers, the block
     // is asked again and answers too.
    Rig rig;
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    const std::string out = rig.run("live");
    CHECK(!rig.device.blocksFailed());
    CHECK(!rig.device.linkDown());
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_HAS(lineWith(out, "adc_vb_f"), "13.68 V");
    CHECK_EQ(rig.sim.requests.size(), 4u + 2u);
    CHECK_EQ(rig.sim.requests[0][5], 9);
    CHECK_EQ(rig.sim.requests[1][5], 1);
    CHECK_EQ(rig.sim.requests[2][5], 9);
    CHECK_HASNT(out, "block reads do not work");
    // The same for a reply that arrives damaged, and for the second block of the command.
    Rig other;
    other.sim.failNext(SimSunSaver::FAULT_NONE);
    other.sim.failNext(SimSunSaver::FAULT_BAD_CRC);
    const std::string second = other.run("live");
    CHECK(!other.device.blocksFailed());
    CHECK_EQ(countOf(second, "--"), 0);
    CHECK_EQ(other.sim.requests.size(), 4u + 2u);
  }
  {  // The register that is asked alone after a silent block may answer with a refusal, as on the
     // older controller, whose block 0x0038-0x003A starts with a register it does not have. A
     // refusal is an answer: the controller is there, so the block gets its second try. That one
     // is refused as well, and the registers are asked one by one.
    Rig rig;
    rig.sim.makeOldFirmware();
    rig.sim.failNext(SimSunSaver::FAULT_NONE);      // the block 0x0027-0x002A
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY);  // the block 0x0038-0x003A
    const std::string out = rig.run("dump mppt");
    CHECK_HAS(lineWith(out, "Sweep_Voc"), "21.30 V");
    CHECK_EQ(countOf(out, "-- exception 2"), 3);
    CHECK_HASNT(out, "no reply");
    CHECK(!rig.device.blocksFailed());
    CHECK(!rig.device.linkDown());
    CHECK_EQ(rig.sim.requests.size(), 7);
    CHECK_EQ(rig.sim.requests[1][5], 3);  // the block: no reply
    CHECK_EQ(rig.sim.requests[2][5], 1);  // its first register alone: refused
    CHECK_EQ(rig.sim.requests[2][3], 0x38);
    CHECK(rig.sim.requests[3] == rig.sim.requests[1]);  // the block once more: refused
    for (size_t i = 4; i < 7; ++i) {
      CHECK_EQ(rig.sim.requests[i][5], 1);
      CHECK_EQ(rig.sim.requests[i][3], 0x38 + (i - 4));
    }
    // On a link that is silent on every block, that second try is what shows it: the controller
    // has answered in between, so it is blocks that do not work, and none is asked for again.
    Rig silent;
    silent.sim.makeOldFirmware();
    silent.sim.blocksAnswered = false;
    uint16_t words[3] = {0xAAAA, 0xAAAA, 0xAAAA};
    OvWordStatus status[3];
    silent.device.beginBatch();
    silent.device.readSpan(0x0038, 3, words, status);
    CHECK(silent.device.blocksFailed());
    CHECK(!silent.device.linkDown());
    CHECK_EQ(silent.sim.requests.size(), 5);
    CHECK_EQ(silent.sim.requests[0][5], 3);
    CHECK_EQ(silent.sim.requests[1][5], 1);
    CHECK(silent.sim.requests[2] == silent.sim.requests[0]);
    CHECK_EQ(silent.sim.requests[3][5], 1);
    CHECK_EQ(silent.sim.requests[4][5], 1);
    for (int i = 0; i < 3; ++i) {
      CHECK_EQ(status[i].status, MB_EXCEPTION);
      CHECK_EQ(status[i].exception, 2);
      CHECK_EQ(words[i], 0);
    }
  }
  {  // Without repeats (retries 0) one silent block and one register that answers settle it, as
     // they always did.
    Rig rig;
    rig.device.retries = 0;
    rig.sim.blocksAnswered = false;
    const std::string out = rig.run("live");
    CHECK(rig.device.blocksFailed());
    CHECK_EQ(rig.sim.requests.size(), 2u + 8u + 11u);
    CHECK_EQ(countOf(out, "--"), 0);
  }
  {  // More repeats do not hide the controller either: three silent blocks in a row used to end
     // the batch before a single register was asked.
    Rig rig;
    rig.device.retries = 3;
    rig.sim.blocksAnswered = false;
    const std::string out = rig.run("live");
    CHECK(!rig.device.linkDown());
    CHECK(rig.device.blocksFailed());
    CHECK_EQ(countOf(out, "--"), 0);
    CHECK_EQ(rig.sim.requests.size(), 3u + 8u + 11u);
  }
  {  // A dead link ends the command after three failed requests instead of 20 timeouts.
    Rig rig;
    rig.sim.powered = false;
    const uint64_t t0 = rig.sim.elapsedMs();
    const std::string out = rig.run("live");
    CHECK_EQ(rig.sim.requests.size(), 3);  // the block, one register alone, and its repeat
    CHECK_EQ(rig.sim.requests[0][5], 9);
    CHECK_EQ(rig.sim.requests[1][5], 1);
    CHECK_EQ(rig.sim.requests[2][5], 1);
    // Three timeouts, and before each repeat one more for a reply that might only be late.
    const uint64_t took = rig.sim.elapsedMs() - t0;
    CHECK_MSG(took >= 5000 && took < 5200, "took %llu ms", (unsigned long long)took);
    CHECK_HASNT(out, "something arrived");  // nothing did
    CHECK_HAS(lineWith(out, "adc_vb_f"), "-- no reply");
    CHECK_HAS(lineWith(out, "adc_va_f"), "-- not read");
    CHECK_HAS(out, "The controller is not answering");
    CHECK_HASNT(out, "Status");  // the second group was not attempted
    CHECK(!rig.device.blocksFailed());  // nothing was learned about blocks
    CHECK_HASNT(out, "voltage scale");
    // The next command starts afresh.
    rig.sim.powered = true;
    CHECK_HAS(rig.run("get adc_vb_f"), "13.68 V");
  }
  {  // The same with single-register reads.
    Rig rig;
    rig.sim.powered = false;
    rig.run("blocks off");
    const std::string out = rig.run("dump");
    CHECK_EQ(rig.sim.requests.size(), 3);
    CHECK_HAS(out, "The controller is not answering");
  }
  {  // A controller that answers later than the timeout allows. No register is shown with the
     // value of the one before it, and the user is told that replies are arriving late.
    Rig rig;
    rig.run("blocks off");
    rig.sim.responseDelayMs = 1200;
    const std::string out = rig.run("dump live");
    CHECK_HAS(lineWith(out, "adc_vb_f"), "-- no reply");
    CHECK_HASNT(out, " V ");  // not one voltage was shown
    CHECK_HASNT(out, " A ");
    CHECK_EQ(rig.master.stats.ok, 0);
    CHECK_HAS(out, "Note: something arrived after a request had been given up, and was dropped. If that was\r\n"
                   "      its reply, replies are slower than the timeout: 'timeout <ms>' sets it (now 1000 ms).");
    CHECK_EQ(countOf(out, "Note: something arrived"), 1);
    CHECK(out.find("The controller is not answering") < out.find("Note: something arrived"));
    CHECK_HAS(rig.run("status"),
              "          Something arrived after a request had been given up, 2 times: late replies?");
    // With enough time the same controller reads correctly.
    rig.sim.advance(3000);
    CHECK_HAS(rig.run("timeout 2000"), "Reply timeout: 2000 ms");
    const std::string again = rig.run("dump live");
    CHECK_HAS(lineWith(again, "adc_vb_f"), "13.68 V");
    CHECK_HAS(lineWith(again, "adc_va_f"), "17.45 V");
    CHECK_HAS(lineWith(again, "adc_ic_f"), "0.20 A");
    CHECK_HASNT(again, "something arrived");
  }
  {  // Every command says so, not only the register reads, and each says it once. 'coils'
     // repeats its requests too, and 'watch' reads until a key is pressed.
    Rig rig;
    rig.sim.responseDelayMs = 1200;
    const std::string coils = rig.run("coils");
    CHECK_HAS(coils, "-- no reply");
    CHECK_EQ(countOf(coils, "Note: something arrived after a request had been given up"), 1);
    CHECK_HAS(coils, "'timeout <ms>' sets it (now 1000 ms).");
    rig.sim.advance(3000);
    rig.host.pressKeyAfter(5500);  // in the pause after the first pass
    const std::string watched = rig.run("watch 1");
    CHECK_HAS(watched, "(no answer; trying again)");
    CHECK_EQ(countOf(watched, "Note: something arrived after a request had been given up"), 1);
    CHECK(watched.find("Stopped.") < watched.find("Note: something arrived"));
    // A command that sends nothing has nothing to say about it.
    rig.sim.advance(3000);
    CHECK_HASNT(rig.run("status"), "Note: something arrived");
    CHECK_HASNT(rig.run("help"), "something arrived");
  }
  {  // CRC errors are a link failure too, and are counted as such.
    Rig rig;
    rig.sim.failNext(SimSunSaver::FAULT_BAD_CRC, 4);
    const std::string out = rig.run("get adc_vb_f");
    CHECK_HAS(out, "-- CRC error");
    CHECK_EQ(rig.master.stats.crcErrors, 2);
  }
}

struct CountingListener : OvProbeListener {
  int attempts;
  CountingListener() : attempts(0) {}
  void onProbeAttempt(const OvLink &, const MbResult &) override { ++attempts; }
};

// Notes, for every attempt of a probe, the baud rate it was made at and the pause between
// frames that the master kept at that moment.
struct GapListener : OvProbeListener {
  ModbusMaster &master;
  std::vector<std::pair<uint32_t, uint16_t> > seen;
  std::vector<uint16_t> quiet;
  explicit GapListener(ModbusMaster &m) : master(m) {}
  void onProbeAttempt(const OvLink &tried, const MbResult &) override {
    seen.push_back(std::make_pair(tried.baud, master.interFrameGapMs));
    quiet.push_back(master.quietMs);
  }
};

static void testProbe() {
  {  // The usual case: found at the first attempt, well inside the plan's 5 second target.
    Rig rig;
    const uint64_t t0 = rig.sim.elapsedMs();
    const std::string out = rig.run("probe");
    CHECK_HAS(out, " (0x0008). Press a key to stop.\r\n");  // where there are keys, it says how to stop
    CHECK_HAS(out, "Found it: slave 1, 9600 8N2, read function 4.");
    CHECK_HAS(out, "Battery voltage reads 13.68 V.");
    CHECK_HAS(out, "vendor \"Morningstar Corp.\", product \"SS-MPPT\", version \"v01.01.01\"");
    CHECK_EQ(rig.sent(0x04), 1);
    CHECK(rig.sim.elapsedMs() - t0 < 500);
  }
  {  // The controller is set to 19200 8N1.
    Rig rig;
    rig.sim.deviceBaud = 19200;
    rig.sim.deviceFormat = MB_8N1;
    const uint64_t t0 = rig.sim.elapsedMs();
    const std::string out = rig.run("probe");
    CHECK_HAS(out, "Found it: slave 1, 19200 8N1, read function 4.");
    CHECK_MSG(rig.sim.elapsedMs() - t0 < 5000, "took %llu ms", (unsigned long long)(rig.sim.elapsedMs() - t0));
    CHECK_EQ(rig.device.link.baud, 19200);
    CHECK_EQ(rig.device.link.format, MB_8N1);
    CHECK_HAS(out, "9600 8N2, slave 1, function 4: no reply");
    CHECK_HAS(rig.run("live"), "13.68 V");  // the settings found are in use
    CHECK_HAS(rig.run("status"), "19200 8N1");
  }
  {  // The controller only answers function 03.
    Rig rig;
    rig.sim.readInput = false;
    const std::string out = rig.run("probe");
    CHECK_HAS(out, "function 4: exception 1 (illegal function)");
    CHECK_HAS(out, "Found it: slave 1, 9600 8N2, read function 3.");
    CHECK_EQ(rig.device.link.readFunction, 3);
    rig.run("live");
    CHECK_EQ(rig.sim.requests.back()[1], 0x03);
  }
  {  // An exception is an answer, but it is not a find (finding F-26).
    Rig rig;
    rig.sim.regs.erase(0x0008);
    const std::string out = rig.run("probe");
    CHECK_HASNT(out, "Found it");
    CHECK_HAS(out, "A controller answers at slave 1, 9600 8N2, but it refuses to read that register");
    CHECK_HAS(out, "exception 2 (illegal data address)");
    CHECK_EQ(rig.sent(0x04) + rig.sent(0x03), 2);  // no point sweeping baud rates after that
    CHECK_EQ(rig.sent(0x2B), 0);
  }
  {  // Nothing there: the sweep ends in bounded time, says so, shows the wiring checklist and
     // leaves the link settings as they were.
    Rig rig;
    rig.sim.powered = false;
    rig.run("link 19200 8N1");
    rig.run("slave 7");
    const uint64_t t0 = rig.sim.elapsedMs();
    const std::string out = rig.run("probe");
    const uint64_t took = rig.sim.elapsedMs() - t0;
    CHECK_HAS(out, "Nothing answered.");
    CHECK_HAS(out, "MRC-1 power switch is ON");
    CHECK_HAS(out, "Opta A    to  MRC-1 terminal B");
    CHECK_HASNT(out, "Found it");
    CHECK_MSG(took < 12000, "took %llu ms", (unsigned long long)took);
    CHECK_EQ(rig.device.link.baud, 19200);
    CHECK_EQ(rig.device.link.format, MB_8N1);
    CHECK_EQ(rig.device.link.slave, 7);
    CHECK_HAS(out, "19200 8N1, slave 7, function 4: no reply");  // what the user had set comes first
    CHECK_HAS(out, "9600 8N2, slave 1, function 3: no reply");   // and the profile's defaults are covered
  }
  {  // Bytes but never a frame: say so, because the cure is different.
    Rig rig;
    rig.sim.failNext(SimSunSaver::FAULT_GARBAGE, 100);
    const std::string out = rig.run("probe");
    CHECK_HAS(out, "Bytes arrived, but never a valid reply");
    CHECK_HAS(out, "noise on the line");
    CHECK_HASNT(out, "Nothing answered");
  }
  {  // A key press stops the sweep.
    Rig rig;
    rig.sim.powered = false;
    rig.host.pressKeyAfter(1500);
    const std::string out = rig.run("probe");
    CHECK_HAS(out, "Stopped.");
    CHECK_EQ(rig.host.discards, 1);
    CHECK(rig.sim.requests.size() <= 3);
    CHECK_EQ(rig.device.link.baud, 9600);
  }
  {  // The device layer alone, with a listener.
    Rig rig;
    rig.sim.slaveId = 9;
    CountingListener listener;
    const OvProbeOutcome outcome = rig.device.probe(&listener);
    CHECK(!outcome.found && !outcome.refused && !outcome.sawBytes && !outcome.aborted);
    CHECK_EQ(outcome.attempts, listener.attempts);
    CHECK_EQ(outcome.attempts, 12);  // 2 baud rates x 3 formats x 2 read functions
    CHECK_EQ(rig.master.responseTimeoutMs, 1000);  // the sweep's shorter timeout is put back
    // 'scan' finds a controller at another address.
    const std::string out = rig.run("scan 1 12");
    CHECK_HAS(out, "Asking slaves 1 to 12 at 9600 8N2. Press a key to stop.\r\n");
    CHECK_HAS(out, "slave 9: answers");
    CHECK_HAS(out, "1 slave answered.");
    CHECK_EQ(rig.master.responseTimeoutMs, 1000);
    CHECK_HAS(rig.run("slave 9"), "slave 9, 9600 8N2");
    CHECK_HAS(rig.run("get adc_vb_f"), "13.68 V");
  }
  {  // The pause between frames belongs to the baud rate, also while a probe tries one rate
     // after the other, and afterwards it is that of the link that stays in use.
    Rig rig;
    rig.sim.deviceBaud = 19200;
    rig.sim.deviceFormat = MB_8N1;
    rig.sim.portMaxBaud = 115200;
    CHECK_HAS(rig.run("link 300 8N2"), "300 8N2");
    CHECK_EQ(rig.master.interFrameGapMs, 129);
    CHECK_EQ(rig.master.quietMs, 129);
    GapListener listener(rig.master);
    rig.device.beginBatch();
    const OvProbeOutcome found = rig.device.probe(&listener);
    CHECK(found.found);
    CHECK_EQ(found.link.baud, 19200);
    CHECK(listener.seen.size() > 12);  // 300 baud, then 9600, then 19200
    bool at300 = false;
    bool at9600 = false;
    for (size_t i = 0; i < listener.seen.size(); ++i) {
      CHECK_EQ(listener.seen[i].second, mbInterFrameGapMs(listener.seen[i].first));
      CHECK_EQ(listener.quiet[i], mbQuietMs(listener.seen[i].first));
      at300 = at300 || listener.seen[i].first == 300;
      at9600 = at9600 || listener.seen[i].first == 9600;
    }
    CHECK(at300);
    CHECK(at9600);
    CHECK_EQ(rig.master.interFrameGapMs, 5);
    CHECK_EQ(rig.master.quietMs, 50);

    // Nothing found: the link goes back to what it was, and so do the pauses.
    Rig lost;
    lost.sim.powered = false;
    lost.run("link 600 8N2");
    CHECK_EQ(lost.master.interFrameGapMs, 65);
    lost.device.beginBatch();
    CHECK(!lost.device.probe(nullptr).found);
    CHECK_EQ(lost.device.link.baud, 600);
    CHECK_EQ(lost.master.interFrameGapMs, 65);
    CHECK_EQ(lost.master.quietMs, 65);
  }
  {  // 'link' sets them with the baud rate. A rate that the serial port refuses leaves the link
     // and the pauses as they were.
    Rig rig;
    rig.sim.portMaxBaud = 115200;
    rig.sim.deviceBaud = 300;
    CHECK_EQ(rig.master.interFrameGapMs, 5);
    CHECK_HAS(rig.run("link 300"), "300 8N2");
    CHECK_EQ(rig.master.interFrameGapMs, 129);
    CHECK_HAS(rig.run("get adc_vb_f"), "13.68 V");
    const uint64_t replyEnd = rig.sim.elapsedMs();
    const size_t sent = rig.sim.requests.size();
    CHECK_HAS(rig.run("get adc_va_f"), "17.45 V");
    CHECK(rig.sim.requestTimes[sent] >= replyEnd + 129);
    const std::string refused = rig.run("link 230400");
    CHECK_HAS(refused, "The serial port cannot do that. The link is unchanged.");
    CHECK_HAS(refused, "300 8N2");
    CHECK_EQ(rig.device.link.baud, 300);
    CHECK_EQ(rig.master.interFrameGapMs, 129);
    CHECK_EQ(rig.master.quietMs, 129);
    CHECK_HAS(rig.run("get adc_vb_f"), "13.68 V");  // the port is open again with the old settings
    rig.sim.deviceBaud = 9600;
    CHECK_HAS(rig.run("link 9600"), "9600 8N2");
    CHECK_EQ(rig.master.interFrameGapMs, 5);
    CHECK_EQ(rig.master.quietMs, 50);
  }
  {
    Rig rig;
    CHECK_HAS(rig.run("scan 0"), "Slave addresses run from 1 to 247");
    CHECK_HAS(rig.run("scan 5 2"), "Slave addresses run from 1 to 247");
    CHECK_HAS(rig.run("scan 20 22"), "No slave answered.");
    rig.host.pressKeyAfter(700);
    const std::string out = rig.run("scan 2 200");
    CHECK_HAS(out, "Stopped.");
    CHECK(rig.sim.requests.size() < 10);
  }
}

// ============================================================================
// Console
// ============================================================================
// Types `keys` into an editor. Returns the completed lines, each followed by '|'.
static std::string typeInto(OvLineEditor &editor, StringOut &echo, const std::string &keys) {
  std::string lines;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (editor.feed((unsigned char)keys[i], echo)) {
      lines += editor.line();
      lines += '|';
    }
  }
  return lines;
}

static void testLineEditor() {
  {  // Nothing is typed until a character that counts arrives, and again after each Enter.
    OvLineEditor editor;
    StringOut echo;
    CHECK(editor.empty());
    CHECK(typeInto(editor, echo, "\x01\x1B\b") == "" && editor.empty());
    CHECK(typeInto(editor, echo, "l") == "" && !editor.empty());
    CHECK(typeInto(editor, echo, "\b") == "" && editor.empty());
    CHECK(typeInto(editor, echo, "li") == "" && !editor.empty());
    CHECK(typeInto(editor, echo, "ve\r") == "live|" && editor.empty());
    CHECK(typeInto(editor, echo, "\n") == "" && editor.empty());
    CHECK(typeInto(editor, echo, "x") == "" && !editor.empty());
    editor.clear();
    CHECK(editor.empty());
  }
  {  // Lines end with LF, CR or CR LF, and each Enter is one line.
    OvLineEditor editor;
    StringOut echo;
    CHECK(typeInto(editor, echo, "live\n") == "live|");
    CHECK(echo.text == "live\r\n");
    CHECK(typeInto(editor, echo, "probe\r") == "probe|");
    CHECK(editor.expectsLineFeed());
    CHECK(typeInto(editor, echo, "\n") == "");  // the other half of that Enter
    CHECK(!editor.expectsLineFeed());
    CHECK(typeInto(editor, echo, "a\r\nb\r\nc\n\nd\r\re\n") == "a|b|c||d||e|");
    CHECK(typeInto(editor, echo, "\n") == "|");  // an LF that follows an LF is an empty line
  }
  {  // Backspace and delete rub out; at the start of the line they do nothing.
    OvLineEditor editor;
    StringOut echo;
    CHECK(typeInto(editor, echo, "\b\x7F" "sett\b EV_lvx\x7F" "d\n") == "set EV_lvd|");
    CHECK(echo.text == "sett\b \b EV_lvx\b \bd\r\n");
  }
  {  // Control characters, escape sequences' first byte and bytes above ASCII are dropped.
    OvLineEditor editor;
    StringOut echo;
    CHECK(typeInto(editor, echo, std::string("l\x01i\tv\x1B") + "e\xC3\xA9\n") == "live|");
    CHECK(echo.text == "live\r\n");
    CHECK(!editor.feed(-1, echo));  // "nothing to read" from a serial port
    CHECK(!editor.feed(0, echo));
    CHECK(!editor.feed(256 + 'a', echo));
    CHECK(typeInto(editor, echo, "\n") == "|");
  }
  {  // A line longer than the console takes keeps its start; the echo shows where it stopped.
    OvLineEditor editor;
    StringOut echo;
    const std::string lines = typeInto(editor, echo, std::string(500, 'x') + "\n");
    CHECK_EQ(lines.size(), OV_LINE_MAX);  // OV_LINE_MAX - 1 characters and the '|'
    CHECK_EQ(echo.text.size(), OV_LINE_MAX - 1 + 2);
    CHECK(typeInto(editor, echo, "ok\n") == "ok|");
  }
  {  // clear() forgets a half-typed line and a pending line feed.
    OvLineEditor editor;
    StringOut echo;
    typeInto(editor, echo, "half a comm");
    editor.clear();
    CHECK(typeInto(editor, echo, "id\r") == "id|");
    CHECK(editor.expectsLineFeed());
    editor.clear();
    CHECK(!editor.expectsLineFeed());
    CHECK(typeInto(editor, echo, "\n") == "|");
    typeInto(editor, echo, "id\r");
    editor.lineFeedTaken();  // the sketch took the LF while a command ran
    CHECK(typeInto(editor, echo, "\n") == "|");
  }
  {  // Through the console: what a terminal session looks like, CR LF line ends included.
    Rig rig;
    OvLineEditor editor;
    const std::string keys = "set EV_lvd 13.10\r\n\r\nyes\r\n";
    for (size_t i = 0; i < keys.size(); ++i) {
      if (editor.feed((unsigned char)keys[i], rig.out)) {
        rig.console.handleLine(editor.line());
        rig.console.prompt();
      }
    }
    gTranscript += rig.out.text;
    // The empty line between the question and the yes did not cancel it.
    CHECK_HAS(rig.out.text, "Type yes to write it. Anything else cancels.\r\nyes/no> \r\nyes/no> yes\r\n");
    CHECK_HAS(rig.out.text, "Written and read back: EV_lvd = 13.10 V [0x10C5]");
    CHECK_EQ(rig.sim.regs[0xE022], 0x10C5);
  }
}

static void testConsoleBasics() {
  Rig rig;
  rig.out.text.clear();
  rig.console.begin();
  CHECK_HAS(rig.out.text, "OptaView 0.3.0");
  CHECK_HAS(rig.out.text, "Morningstar SunSaver MPPT");
  CHECK_HAS(rig.out.text, "slave 1, 9600 8N2, read function 4");
  rig.out.text.clear();
  rig.console.prompt();
  CHECK(rig.out.text == "optaview> ");

  CHECK(rig.run("") == "");
  CHECK(rig.run("   \t  ") == "");
  CHECK_HAS(rig.run("frobnicate"), "Unknown command 'frobnicate'");
  // An overlong line is cut, not overrun, and only the start of it is repeated back.
  CHECK_HAS(rig.run(std::string(400, 'x')), "Unknown command 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx...'. 'help'");
  CHECK_HAS(rig.run("yes"), "Nothing is waiting for a yes or a no.");
  CHECK_HAS(rig.run("NO"), "Nothing is waiting for a yes or a no.");
  std::string many = "raw";
  for (int i = 0; i < 60; ++i) {
    many += " 1";
  }
  rig.run(many);  // more words than the console keeps

  // Every command the help text names is a command.
  const std::string help = rig.run("help");
  const char *const commands[] = {"probe",  "scan",  "id",     "wires",    "live",  "watch",  "dump",
                                  "get",    "coils", "snapshot", "scalecheck", "set", "coil",  "reset",
                                  "status", "link",  "slave",  "fc",       "timeout", "blocks", "scale",
                                  "profile", "trace", "sniff", "raw",      "expert"};
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    CHECK_MSG(has(help, commands[i]), "help does not mention %s", commands[i]);
    rig.host.pressKeyAfter(0);  // so that 'watch' and 'sniff' return at once
    const std::string out = rig.run(commands[i]);
    CHECK_MSG(!has(out, "Unknown command"), "%s is not a command", commands[i]);
    if (rig.console.awaitingConfirmation()) {
      rig.run("no");
    }
  }
  CHECK_HAS(rig.run("HELP"), "Finding the controller");  // commands ignore case
  CHECK_HAS(rig.run("?"), "Finding the controller");

  // 'scalecheck' ran in that loop against a controller at the profile's scales.
  const std::string settled = rig.run("status");
  CHECK_HAS(settled, "Voltage   stored settings: scale vset = x100/32768, confirmed by 'scalecheck'");
  CHECK_HAS(settled, "          live readings:   scale vlive = x100/32768, confirmed by 'scalecheck'");
  CHECK_HASNT(settled, "'scalecheck' tests");

  Rig fresh;
  const std::string status = fresh.run("status");
  CHECK_HAS(status, "94 registers, 8 coils");
  CHECK_HAS(status, "slave 1, 9600 8N2, read function 4, timeout 1000 ms");
  CHECK_HAS(status, "blocks auto: up to 16 registers in a request");
  CHECK_HAS(status, "Voltage   stored settings: scale vset = x100/32768, not confirmed on this controller");
  CHECK_HAS(status, "          live readings:   scale vlive = x100/32768, not confirmed on this controller");
  CHECK_HAS(status, "          'scalecheck' tests what is not confirmed.");

  CHECK_HAS(rig.run("link"), "Link: slave 1, 9600 8N2, read function 4");
  CHECK_HAS(rig.run("link 19200"), "19200 8N2");
  CHECK_HAS(rig.run("link 9600 8e1"), "9600 8E1");
  CHECK_HAS(rig.run("link fast"), "Usage: link");
  CHECK_HAS(rig.run("link 19200 9X9"), "The format is one of");
  CHECK_HAS(rig.run("link"), "9600 8E1");  // a line that is refused changes nothing, the baud rate neither
  CHECK_HAS(rig.run("link 299"), "Usage: link");
  CHECK_HAS(rig.run("link 1000001"), "Usage: link");
  CHECK_HAS(rig.run("link"), "9600 8E1");
  CHECK_HAS(rig.run("link 300 8E1"), "300 8E1");
  CHECK_HAS(rig.run("link 9600 8N2"), "9600 8N2");
  CHECK_HAS(rig.run("slave 0"), "1 to 247");
  CHECK_HAS(rig.run("slave 248"), "1 to 247");
  CHECK_HAS(rig.run("slave 256"), "1 to 247");
  CHECK_HAS(rig.run("slave 257"), "1 to 247");  // not address 1 by the back door
  CHECK_HAS(rig.run("link"), "slave 1,");
  CHECK_HAS(rig.run("slave 247"), "slave 247,");
  CHECK_HAS(rig.run("slave 1"), "slave 1,");
  CHECK_HAS(rig.run("fc 5"), "function 3 or 4");
  CHECK_HAS(rig.run("fc 3"), "read function 3");
  CHECK_HAS(rig.run("fc 4"), "read function 4");
  CHECK_HAS(rig.run("timeout 10"), "50 to 10000");
  CHECK_HAS(rig.run("timeout 49"), "50 to 10000");
  CHECK_HAS(rig.run("timeout 10001"), "50 to 10000");
  CHECK_HAS(rig.run("timeout 65636"), "50 to 10000");  // 100 ms, were it cut to 16 bits
  CHECK_HAS(rig.run("timeout 50"), "Reply timeout: 50 ms");
  CHECK_HAS(rig.run("timeout 10000"), "Reply timeout: 10000 ms");
  CHECK_EQ(rig.master.responseTimeoutMs, 10000);
  CHECK_HAS(rig.run("timeout 300"), "Reply timeout: 300 ms");
  CHECK_EQ(rig.master.responseTimeoutMs, 300);
  CHECK_HAS(rig.run("timeout"), "300 ms");

  const std::string profile = rig.run("profile");
  CHECK_HAS(profile, "Morningstar SunSaver MPPT MODBUS Specification v11");
  CHECK_HAS(profile, "Before writing settings:");
  const std::string groups = rig.run("groups");
  CHECK_HAS(groups, "live        Meters (9 registers)");
  CHECK_HAS(groups, "comm        Communication (EEPROM) (2 registers)");
  CHECK_HAS(rig.run("wires"), "DIP switch 4 selects MODBUS");
  CHECK_HAS(rig.run("id"), "product \"SS-MPPT\"");
  rig.sim.deviceIdSupported = false;
  CHECK_HAS(rig.run("id"), "Identification: not available (exception 1 (illegal function)).");
}

static void testConsoleReads() {
  Rig rig;
  const std::string dump = rig.run("dump");
  // Every register of the profile is listed once, with a value.
  for (size_t i = 0; i < rig.profile.registerCount(); ++i) {
    const std::string needle = std::string(" ") + rig.profile.reg(i).name + " ";
    CHECK_MSG(countOf(dump, needle.c_str()) == 1, "%s is listed %zu times", rig.profile.reg(i).name,
              countOf(dump, needle.c_str()));
  }
  CHECK_EQ(countOf(dump, "--"), 0);
  CHECK_HAS(lineWith(dump, " EV_lvd "), "0xE022 W~ EV_lvd");
  CHECK_HAS(lineWith(dump, " EV_lvd "), "11.50 V");
  CHECK_HAS(lineWith(dump, " adc_vb_f "), "0x0008    adc_vb_f");  // confirmed on the bench: no ~
  CHECK_HAS(lineWith(dump, " Ahc_t "), "7000.0 Ah");
  CHECK_HAS(lineWith(dump, " Ahc_t "), "[0x0001 0x1170]");
  CHECK_HAS(lineWith(dump, " EAhc_t "), "7000.0 Ah");
  CHECK_HAS(lineWith(dump, " EAhc_t "), "[0x1170 0x0001]");
  CHECK_HAS(lineWith(dump, " ETb_min "), "-30 C");
  CHECK_HAS(lineWith(dump, " EV_tempcomp "), "0.0305 V/C");
  CHECK_HAS(lineWith(dump, " Eic_lim "), "15.00 A");
  CHECK_HAS(dump, "Load and LVD settings (EEPROM)");
  CHECK_HAS(dump, "W = 'set' can change it.");
  // Reading everything took a block per run of registers, not a request per register.
  CHECK(rig.sim.requests.size() < 25);
  // The reminder about the unconfirmed scales appears once per session.
  CHECK_HAS(dump, "Note: voltages are shown with scales that are not confirmed on this controller\r\n"
                  "      (live readings x100/32768, stored settings x100/32768). 'scalecheck' tests them.");
  CHECK_HASNT(rig.run("dump"), "Note: voltages are shown");
  CHECK_HASNT(rig.run("live"), "Note: voltages are shown");
  {  // It names the scales whose values the command has just shown, each one once.
    Rig other;
    CHECK_HASNT(other.run("get charge_state"), "Note: voltages are shown");
    CHECK_HASNT(other.run("get Vb_ref"), "Note: voltages are shown");  // the target's scale is not one of the two
    const std::string setting = other.run("get EV_lvd");
    CHECK_HAS(setting, "'set' accepts from 9.00 V to 15.00 V.\r\n"
                       "Note: voltages are shown with a scale that is not confirmed on this controller\r\n"
                       "      (stored settings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(other.run("get EV_lvr"), "Note: voltages are shown");
    CHECK_HAS(other.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(other.run("dump"), "Note: voltages are shown");
    // Nothing was shown when nothing answered.
    Rig silent;
    silent.sim.powered = false;
    CHECK_HASNT(silent.run("live"), "Note: voltages are shown");
    silent.sim.powered = true;
    CHECK_HAS(silent.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");
    // A command that lost the link on the way says that and nothing more, and what it had
    // shown until then does not count for the next command.
    Rig lost;
    lost.sim.failNext(SimSunSaver::FAULT_NONE);
    lost.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 4);
    const std::string cut = lost.run("live");
    CHECK_HAS(cut, " adc_vb_f ");
    CHECK_HAS(cut, "The controller is not answering");
    CHECK_HASNT(cut, "Note: voltages are shown");
    CHECK_HAS(lost.run("dump load"), "      (stored settings x100/32768). 'scalecheck' tests it.");
    // 'watch' shows the same voltages as 'live' and ends with the same reminder.
    Rig watching;
    watching.host.pressKeyAfter(600);  // during the pause after the first pass
    const std::string watched = watching.run("watch");
    CHECK_EQ(countOf(watched, " adc_vb_f "), 1);
    CHECK_HAS(watched, "Stopped.\r\nNote: voltages are shown with a scale that is not confirmed on this controller\r\n"
                       "      (live readings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(watching.run("live"), "Note: voltages are shown");
    // Stopped before it showed anything: nothing to remind of.
    Rig early;
    early.host.pressKeyAfter(0);
    CHECK_HASNT(early.run("watch"), "Note: voltages are shown");
    CHECK_HAS(early.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");
  }

  const std::string load = rig.run("dump LOAD");
  CHECK_HAS(load, "EV_lvr");
  CHECK_HASNT(load, "EV_reg");
  CHECK_HAS(rig.run("dump nothing"), "There is no group called 'nothing'");

  const std::string get = rig.run("get ev_lvd");
  CHECK_HAS(get, "0xE022 W~ EV_lvd");
  CHECK_HAS(get, "'set' accepts from 9.00 V to 15.00 V.");
  CHECK_HAS(rig.run("get EV_eq"), "from 10.00 V to 17.00 V, or 0 to switch it off");
  CHECK_HAS(rig.run("get Et_float"), "anything the register can hold");
  CHECK_HAS(rig.run("get Emodbus_id"), "a wrong value makes the controller unreachable");
  CHECK_HAS(rig.run("get nope"), "There is no register called 'nope'");
  CHECK_HAS(rig.run("get"), "Usage: get");

  const std::string raw = rig.run("get 0x0008 5");
  CHECK_HAS(raw, "0x0008  0x1183  4483   adc_vb_f = 13.68 V");
  CHECK_HAS(raw, "0x000C  0x003A  58     adc_il_f = 0.14 A");
  const std::string two = rig.run("get 0x0017 2");
  CHECK_HAS(two, "Ahc_t = 7000.0 Ah");
  CHECK_HASNT(rig.run("get 0x0017"), "Ahc_t");  // half a two-word register is not decoded
  // A register is named on the line of its first word, and only there.
  const std::string pair = rig.run("get 0x0015 4");
  CHECK_HAS(lineWith(pair, "0x0015  "), "Ahc_r = 123.4 Ah");
  CHECK_HAS(lineWith(pair, "0x0017  "), "Ahc_t = 7000.0 Ah");
  CHECK_EQ(countOf(pair, " = "), 2);
  const std::string hole = rig.run("get 0x0034 4");  // 0x0035-0x0037 do not exist
  CHECK_HAS(hole, "0x0034  0x1312");
  CHECK_EQ(countOf(hole, "-- exception 2 (illegal data address)"), 3);
  CHECK_HAS(rig.run("get 8 126"), "The count runs from 1 to 125");
  CHECK_HAS(rig.run("get 0xFFFF 2"), "Addresses run from 0 to 0xFFFF");
  CHECK_HAS(rig.run("get 0x10000"), "Addresses run from 0 to 0xFFFF");

  // A key press stops a long listing.
  rig.host.pressKeyAfter(60);
  const std::string stopped = rig.run("dump");
  CHECK_HAS(stopped, "Stopped.");
  CHECK_HASNT(stopped, "Stored totals");

  const std::string coils = rig.run("coils");
  CHECK_HAS(lineWith(coils, "load_disconnect"), "off");
  CHECK_HAS(lineWith(coils, "clear_kwh"), "(action)");
  CHECK_HAS(coils, "have not been tried on our bench");
  rig.sim.coils[0x0001] = true;
  CHECK_HAS(lineWith(rig.run("coils"), "load_disconnect"), "on ");
  rig.sim.coils.erase(0x0002);
  const size_t asked = rig.sim.requests.size();
  CHECK_HAS(lineWith(rig.run("coils"), "charge_disconnect"), "-- exception 2");
  size_t again = 0;
  for (size_t i = asked; i < rig.sim.requests.size(); ++i) {
    again += rig.sim.requests[i][1] == 0x01 && rig.sim.requests[i][3] == 0x02 ? 1 : 0;
  }
  CHECK_EQ(again, 1);  // a refusal is an answer: the coil is not asked for a second time
}

static void testConsoleSet() {
  {
    Rig rig;
    // The whole flow for the setting this project is after: LVD 13.10 V, LVR 13.25 V.
    const std::string ask = rig.run("set EV_lvr 13.25");
    CHECK_HAS(ask, "EV_lvr: Low voltage reconnect");
    CHECK_HAS(lineWith(ask, "  now  "), "12.60 V");
    CHECK_HAS(lineWith(ask, "  now  "), "[0x1021]");
    CHECK_HAS(lineWith(ask, "  new  "), "13.25 V");
    CHECK_HAS(lineWith(ask, "  new  "), "[0x10F6]");
    CHECK_HAS(ask, "  Careful: the scale of stored settings (vset, x100/32768) is not confirmed on this controller.");
    CHECK_HAS(ask, "    If 'now' is not the value you expect, stop here and run 'scalecheck'.");
    CHECK_HAS(ask, "Before you write:");
    CHECK_HAS(ask, "'EEPROM setting edit' fault");
    CHECK_HAS(ask, "Type yes to write it.");
    CHECK(rig.console.awaitingConfirmation());
    CHECK_EQ(rig.sent(0x06), 0);  // nothing is written before the answer
    CHECK_EQ(rig.sim.regs[0xE023], 0x1021);
    rig.out.text.clear();
    rig.console.prompt();
    CHECK(rig.out.text == "yes/no> ");

    const std::string done = rig.run("yes");
    CHECK_HAS(done, "Written and read back: EV_lvr = 13.25 V [0x10F6]");
    CHECK_HAS(done, "'reset' when all your changes are in");
    CHECK(!rig.console.awaitingConfirmation());
    CHECK_EQ(rig.sent(0x06), 1);
    CHECK_EQ(rig.sim.regs[0xE023], 0x10F6);
    CHECK(rig.sim.requests[rig.sim.requests.size() - 2] ==
          SimSunSaver::frame(bytes({0x01, 0x06, 0xE0, 0x23, 0x10, 0xF6})));

    const std::string second = rig.run("set ev_lvd 13.10");
    CHECK_HAS(lineWith(second, "  new  "), "[0x10C5]");
    CHECK_HASNT(second, "Careful: Low voltage reconnect");  // 13.10 is below the new LVR
    CHECK_HASNT(second, "Before you write:");               // the notes were shown once
    CHECK_HAS(second, "'profile' repeats the notes on writing");
    CHECK_HAS(rig.run("YES"), "Written and read back: EV_lvd = 13.10 V [0x10C5]");
    CHECK_EQ(rig.sim.regs[0xE022], 0x10C5);
    CHECK_HAS(rig.run("status"), "Settings were changed in this session");
    // The controller now shows its "EEPROM changed" fault.
    CHECK_HAS(lineWith(rig.run("live"), "array_fault"), "EEPROM setting edit (reset required)");
  }
  {
    Rig rig;
    // Anything but yes cancels, and nothing is sent.
    rig.run("set EV_lvd 13.10");
    CHECK_HAS(rig.run("no"), "Cancelled. Nothing was sent.");
    CHECK(!rig.console.awaitingConfirmation());
    // An empty line is not an answer (a terminal's stray line end must not decide anything):
    // the question stays open.
    rig.run("set EV_lvd 13.10");
    CHECK(rig.run("") == "");
    CHECK(rig.run("   ") == "");
    CHECK(rig.console.awaitingConfirmation());
    CHECK_HAS(rig.run("yes please"), "Cancelled");
    CHECK(!rig.console.awaitingConfirmation());
    // The sketch drops an open question when the terminal reconnects.
    rig.run("set EV_lvd 13.10");
    CHECK(rig.console.cancelPending());
    CHECK(!rig.console.awaitingConfirmation());
    CHECK(!rig.console.cancelPending());
    CHECK_HAS(rig.run("yes"), "Nothing is waiting for a yes or a no.");
    rig.run("set EV_lvd 13.10");
    CHECK_HAS(rig.run("live"), "Cancelled");  // another command is not a yes, and is not run either
    CHECK_EQ(rig.sent(0x06), 0);
    CHECK_EQ(rig.sim.regs[0xE022], 0x0EB8);

    // Raising the LVD above the LVR is allowed (the LVR comes next) but pointed out.
    const std::string ask = rig.run("set EV_lvd 13.10");
    CHECK_HAS(ask, "Careful: Low voltage reconnect (EV_lvr) must be above low voltage disconnect (EV_lvd).");
    CHECK_HAS(ask, "After this write, EV_lvd = 13.10 V and EV_lvr = 12.60 V.");
    rig.run("no");

    // The other way round, lowering the LVR to the LVD or below it: what counts is the value
    // that is about to be written, not the one the register still holds.
    static const char kLvrAbove[] =
        "Careful: Low voltage reconnect (EV_lvr) must be above low voltage disconnect (EV_lvd).";
    const std::string lowered = rig.run("set EV_lvr 11.00");
    CHECK_HAS(lowered, kLvrAbove);
    CHECK_HAS(lowered, "After this write, EV_lvd = 11.50 V and EV_lvr = 11.00 V.");
    rig.run("no");
    // Equal is not above.
    const std::string equal = rig.run("set EV_lvr 11.50");
    CHECK_HAS(equal, kLvrAbove);
    CHECK_HAS(equal, "After this write, EV_lvd = 11.50 V and EV_lvr = 11.50 V.");
    rig.run("no");
    CHECK_HASNT(rig.run("set EV_lvr 11.55"), "Careful: Low voltage reconnect");
    rig.run("no");
    // A register that stands in three pairs has all three looked at.
    const std::string hvd = rig.run("set EV_hvd 13.90");
    CHECK_HAS(hvd, "(EV_hvr) must be below high voltage disconnect (EV_hvd).");
    CHECK_HAS(hvd, "After this write, EV_hvr = 14.00 V and EV_hvd = 13.90 V.");
    CHECK_HAS(hvd, "After this write, EV_reg = 14.00 V and EV_hvd = 13.90 V.");
    CHECK_HAS(hvd, "After this write, EV_reg2 = 14.40 V and EV_hvd = 13.90 V.");
    CHECK_EQ(countOf(hvd, "After this write, "), 3);
    rig.run("no");

    // Refusals. None of them reaches the bus as a write.
    CHECK_HAS(rig.run("set"), "Usage: set");
    CHECK_HAS(rig.run("set EV_lvd"), "Usage: set");
    CHECK_HAS(rig.run("set nope 1"), "There is no register called 'nope'");
    CHECK_HAS(rig.run("set EV_lvd thirteen"), "'thirteen' is not a number");
    CHECK_HAS(rig.run("set EV_lvd 13.1V"), "is not a number");
    CHECK_HAS(rig.run("set EV_lvd 8.5"), "That is below what this profile allows for EV_lvd: from 9.00 V to 15.00 V.");
    CHECK_HAS(rig.run("set EV_lvd 131"), "That is above what this profile allows for EV_lvd");
    CHECK_HAS(rig.run("set EV_lvd 0"), "That is below");
    CHECK_HAS(rig.run("set EV_lvd raw 70000"), "A raw value is a whole number from 0 to 65535");
    CHECK_HAS(rig.run("set EV_lvd raw"), "A raw value is a whole number");
    CHECK_HAS(rig.run("set EV_lvd raw 100"), "That is below");  // 0.31 V: the limits apply to raw values too
    CHECK_HAS(rig.run("set Et_float 70000"), "That value does not fit the register.");
    const std::string readOnly = rig.run("set Emodbus_id 2");
    CHECK_HAS(readOnly, "Emodbus_id is read-only in this profile.");
    CHECK_HAS(readOnly, "a wrong value makes the controller unreachable");
    CHECK_HAS(rig.run("set adc_vb_f 12"), "adc_vb_f is read-only in this profile.");
    CHECK_HAS(rig.run("set charge_state 5"), "read-only");
    CHECK(!rig.console.awaitingConfirmation());
    CHECK_EQ(rig.sent(0x06), 0);

    // Writing what is already there is not a write.
    CHECK_HAS(rig.run("set EV_lvd 11.5"), "It already holds that value. Nothing to write.");
    CHECK_HAS(rig.run("set EV_eq 0"), "It already holds that value.");
    CHECK(!rig.console.awaitingConfirmation());

    // A raw value, and a register named by its address.
    const std::string rawAsk = rig.run("set EV_lvd raw 0x10C5");
    CHECK_HAS(lineWith(rawAsk, "  new  "), "13.10 V");
    CHECK_HAS(rig.run("yes"), "Written and read back: EV_lvd = 13.10 V [0x10C5]");
    const std::string byAddress = rig.run("set 0xE023 13.25");
    CHECK_HAS(byAddress, "EV_lvr: Low voltage reconnect");
    CHECK_HAS(rig.run("yes"), "EV_lvr = 13.25 V");
    CHECK_HAS(rig.run("set 0xE0FF 1"), "There is no register called");
    // An address is taken as it was typed. One past the end of the map is no register and must
    // not wrap round to one (0x1E022 would land on EV_lvd), and the second word of a two-word
    // register is not a register either.
    CHECK_HAS(rig.run("set 0x1E022 12.00"), "There is no register called '0x1E022'");
    CHECK_HAS(rig.run("set 122914 12.00"), "There is no register called '122914'");
    CHECK_HAS(rig.run("set 0x0016 5"), "There is no register called '0x0016'");
    CHECK(!rig.console.awaitingConfirmation());
    // 0 is accepted where it switches a feature off.
    rig.run("set EV_float 0");
    CHECK_HAS(rig.run("yes"), "EV_float = 0.00 V [0x0000]");
    // A time, with no scale.
    rig.run("set Et_float 900");
    CHECK_HAS(rig.run("yes"), "Et_float = 900 s [0x0384]");
    CHECK_EQ(rig.sim.regs[0xE002], 900);
  }
  {  // The current value cannot be read: do not write blind.
    Rig rig;
    rig.sim.powered = false;
    const std::string out = rig.run("set EV_lvd 13.10");
    CHECK_HAS(out, "Could not read EV_lvd first (no reply), so nothing was written.");
    CHECK(!rig.console.awaitingConfirmation());
    CHECK_EQ(rig.sent(0x06), 0);
  }
  // What is said after a write that could not be confirmed, and what it leaves standing. A
  // missing acknowledgement is not a refusal, and "failed" is only said of what is known to have
  // failed.
  static const char kChanged[] = "Settings were changed in this session. 'reset' makes the controller use them.";
  static const char kMaybe[] =
      "A write got no answer in this session, so a setting may have been changed. If one was,\r\n"
      "'reset' makes the controller use it.";
  static const char kUsesIt[] = "The controller uses it after a restart: 'reset' when all your changes are in.";
  static const char kUsesWhatItHolds[] =
      "The controller uses what it holds after a restart: 'reset' when all your changes are in.";
  static const char kIfWritten[] =
      "If it was written, the controller uses it after a restart: 'reset' when all your changes are in.";
  {  // The write is lost on the way: the read-back shows the old value.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller did not acknowledge the write (no reply), and EV_lvd still reads 11.50 V [0x0EB8]: "
                   "it was not written.\r\n");
    CHECK_HASNT(out, "restart");
    CHECK_HASNT(out, "failed");
    CHECK_EQ(rig.sent(0x06), 1);  // a write is never repeated on its own
    CHECK_HASNT(rig.run("status"), "Settings were changed");
    CHECK_HASNT(rig.run("status"), "may have been changed");
    CHECK(!rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
  }
  {  // The acknowledgement is lost, but the write went through: the read-back shows that.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller did not acknowledge the write (no reply), but it took effect: EV_lvd = 13.10 V");
    CHECK_HAS(out, kUsesIt);
    CHECK_EQ(rig.sent(0x06), 1);
    CHECK_HAS(rig.run("status"), kChanged);
  }
  {  // The controller acknowledges, then holds something else. It holds a changed setting, so the
     // restart is due all the same.
    Rig rig;
    rig.sim.storeMask = 0xFFF0;
    rig.run("set EV_lvd 13.10");
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller acknowledged the write, but EV_lvd reads 13.09 V [0x10C0], not what was written.");
    CHECK_HAS(out, kUsesWhatItHolds);
    CHECK_HASNT(out, kUsesIt);
    CHECK(rig.device.resetNeeded());
    CHECK_HAS(rig.run("status"), kChanged);
  }
  {  // The controller acknowledges and still reads what it held: said as such, and an acknowledged
     // write counts.
    Rig rig;
    rig.sim.storeMask = 0x0000;
    rig.sim.regs[0xE022] = 0;  // what a mask of 0 stores, so that it "still reads" this
    rig.run("set EV_lvd 13.10");
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller acknowledged the write, but EV_lvd still reads 0.00 V [0x0000], not what was written.");
    CHECK_HAS(out, kUsesWhatItHolds);
    CHECK(rig.device.resetNeeded());
  }
  {  // The controller acknowledges, and then the read-back fails: nothing confirms the value, but
     // the controller said that it wrote.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_NONE);
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller acknowledged the write.\r\n"
                   "Reading it back failed (no reply).\r\n"
                   "'get EV_lvd' shows what it holds now.\r\n");
    CHECK_HAS(out, kUsesWhatItHolds);
    CHECK_HASNT(out, "failed too");
    CHECK_HASNT(out, "not known");
    CHECK(rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
    CHECK_HAS(rig.run("status"), kChanged);
    CHECK_HAS(rig.run("get EV_lvd"), "13.10 V");
  }
  {  // The controller refuses the write.
    Rig rig;
    rig.sim.refuseWrites = true;
    rig.run("set EV_lvd 13.10");
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller refused the write (exception 4 (device failure)), and EV_lvd still reads 11.50 V "
                   "[0x0EB8].\r\n");
    CHECK_HASNT(out, "restart");
    CHECK(!rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
    // Refused, and the register cannot be read afterwards either: a refusal is still a refusal.
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_NONE);
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    const std::string blind = rig.run("yes");
    CHECK_HAS(blind, "The controller refused the write (exception 4 (device failure)).\r\n"
                     "Reading it back failed too (no reply).\r\n"
                     "'get EV_lvd' shows what it holds now.\r\n");
    CHECK_HASNT(blind, "not known");
    CHECK_HASNT(blind, "restart");
    CHECK(!rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
  }
  {  // Write acknowledged with other data.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_WRONG_ECHO);
    const std::string out = rig.run("yes");
    // The simulator did store the value; only its acknowledgement was wrong.
    CHECK_HAS(out, "did not acknowledge the write (write acknowledged with different data), but it took effect");
  }
  {  // No acknowledgement, and the register reads something that is neither the old value nor
     // the new one: the controller took the write its own way, and its reply was lost.
    Rig rig;
    rig.sim.storeMask = 0xFFF0;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller did not acknowledge the write (no reply), and EV_lvd reads 13.09 V [0x10C0], "
                   "which is neither what it held nor what was sent.\r\n");
    CHECK_HAS(out, kUsesWhatItHolds);
    CHECK(rig.device.resetNeeded());
  }
  {  // The link dies after the question: say what is and is not known. Nothing came back, so
     // nothing says that the write failed.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.powered = false;
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller did not acknowledge the write (no reply).\r\n"
                   "Reading it back failed too (no reply): whether it was written is not known.\r\n"
                   "'get EV_lvd' shows what it holds now.\r\n");
    CHECK_HAS(out, kIfWritten);
    CHECK_HASNT(out, "The write failed");
    CHECK(!rig.device.resetNeeded());
    CHECK(rig.device.resetMaybeNeeded());
    const std::string status = rig.run("status");
    CHECK_HAS(status, kMaybe);
    CHECK_HASNT(status, kChanged);
    // The link comes back and the register is read: it holds what it held, so nothing is due.
    rig.sim.powered = true;
    CHECK_HAS(rig.run("get EV_lvd"), "11.50 V");
    CHECK(!rig.device.resetMaybeNeeded());
    CHECK(!rig.device.resetNeeded());
    CHECK_HASNT(rig.run("status"), "may have been changed");
    CHECK_HASNT(rig.run("status"), kChanged);
  }
  {  // The same silence, but the controller did store the value: only its acknowledgement and
     // the answers to the read-back were lost. The next read of the register shows it, and then
     // the restart is known to be due.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "whether it was written is not known.");
    CHECK_EQ(rig.sim.regs[0xE022], 0x10C5);
    CHECK(rig.device.resetMaybeNeeded());
    CHECK(!rig.device.resetNeeded());
    CHECK_HAS(rig.run("status"), kMaybe);
    // A read of other registers settles nothing.
    rig.run("get EV_lvr");
    CHECK(rig.device.resetMaybeNeeded());
    // A block that holds the register does.
    rig.run("dump load");
    CHECK(rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
    const std::string status = rig.run("status");
    CHECK_HAS(status, kChanged);
    CHECK_HASNT(status, "may have been changed");
    // The restart ends it.
    rig.run("reset anyway");
    const std::string restarted = rig.run("yes");
    CHECK_HAS(restarted, "Restarting the controller. Press a key to stop waiting.\r\n");
    CHECK_HAS(restarted, "The controller answers again");
    CHECK(!rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
  }
  {  // Setting it again is the other way to find out: 'set' reads the register first.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    rig.run("yes");
    CHECK(rig.device.resetMaybeNeeded());
    CHECK_HAS(rig.run("set EV_lvd 13.10"), "It already holds that value. Nothing to write.");
    CHECK(rig.device.resetNeeded());
    CHECK_HAS(rig.run("status"), kChanged);
  }
  {  // Asking to change the other register of a pair reads this one for the comparison, and that
     // settles it just the same.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
    rig.run("yes");
    CHECK(rig.device.resetMaybeNeeded());
    rig.run("set EV_lvr 13.25");
    CHECK(!rig.device.resetMaybeNeeded());
    rig.run("no");
  }
  {  // A restart makes the controller use whatever it holds, so it ends the doubt as well.
    Rig rig;
    rig.run("set EV_lvr 13.25");
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
    rig.run("yes");
    CHECK(rig.device.resetMaybeNeeded());
    rig.run("reset");
    CHECK_HAS(rig.run("yes"), "The controller answers again");
    CHECK(!rig.device.resetMaybeNeeded());
    CHECK_HASNT(rig.run("status"), "may have been changed");
  }
  {  // With other link settings the register of another controller may be read: that settles
     // nothing, and the doubt stays until a restart.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
    rig.run("yes");
    CHECK(rig.device.resetMaybeNeeded());
    rig.run("slave 2");
    rig.sim.slaveId = 2;
    CHECK_HAS(rig.run("get EV_lvd"), "11.50 V");
    CHECK(rig.device.resetMaybeNeeded());
    rig.run("slave 1");
    rig.sim.slaveId = 1;
    CHECK_HAS(rig.run("get EV_lvd"), "11.50 V");
    CHECK(rig.device.resetMaybeNeeded());
    CHECK_HAS(rig.run("status"), kMaybe);
  }
  {  // The same holds below the console, for a caller that reads before it has called
     // settleLink(): what the register reads on another slave address, at another baud rate or
     // in another format says nothing about the write. Back on the settings the write went out
     // on, a read settles it.
    for (int which = 0; which < 3; ++which) {
      Rig rig;
      rig.run("set EV_lvd 13.10");
      rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
      rig.run("yes");
      CHECK(rig.device.resetMaybeNeeded());
      const OvLink written = rig.device.link;
      if (which == 0) {
        rig.device.link.slave = 2;
        rig.sim.slaveId = 2;
      } else if (which == 1) {
        rig.device.link.baud = 19200;
        rig.sim.deviceBaud = 19200;
      } else {
        rig.device.link.format = MB_8N1;
        rig.sim.deviceFormat = MB_8N1;
      }
      CHECK(rig.device.applyLink());
      uint16_t word = 0;
      OvWordStatus status;
      rig.device.beginBatch();
      rig.device.readSpan(0xE022, 1, &word, &status);
      CHECK_EQ(status.status, MB_OK);
      CHECK_MSG(rig.device.resetMaybeNeeded(), "a read on other link settings (case %d) settled the write", which);
      rig.device.link = written;
      rig.sim.slaveId = 1;
      rig.sim.deviceBaud = 9600;
      rig.sim.deviceFormat = MB_8N2;
      CHECK(rig.device.applyLink());
      rig.device.readSpan(0xE022, 1, &word, &status);
      CHECK_EQ(status.status, MB_OK);
      CHECK(!rig.device.resetMaybeNeeded());
      CHECK(!rig.device.resetNeeded());
    }
  }
  {  // Two writes in doubt to the same register are one register to read, and a read settles
     // them. The console cannot get there, because 'set' reads the register before it asks; a
     // caller of the device can.
    Rig rig;
    const OvRegister *lvd = rig.profile.findRegister("EV_lvd");
    CHECK(lvd != nullptr);
    const uint16_t held = rig.sim.regs[0xE022];
    uint16_t word = 0;
    OvWordStatus status;
    for (int i = 0; i < 2; ++i) {
      rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);  // the write, the read-back and its repeat
      CHECK_EQ(rig.device.writeRegister(*lvd, 0x10C5, held).effect, OV_WRITE_UNKNOWN);
      CHECK(rig.device.resetMaybeNeeded());
    }
    CHECK_EQ(rig.sim.regs[0xE022], held);
    rig.device.beginBatch();
    rig.device.readSpan(0xE022, 1, &word, &status);
    CHECK_EQ(word, held);
    CHECK(!rig.device.resetMaybeNeeded());  // neither was taken: nothing is due
    CHECK(!rig.device.resetNeeded());
    // What counts is what the register held before the first of the two. Here the first was
    // taken and only its acknowledgement was lost; the caller found that out by other means and
    // gives the new value as what the register holds before its second write. The register
    // then reads as before the second write, and a restart is due all the same.
    Rig taken;
    lvd = taken.profile.findRegister("EV_lvd");
    taken.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    taken.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    CHECK_EQ(taken.device.writeRegister(*lvd, 0x10C5, held).effect, OV_WRITE_UNKNOWN);
    CHECK_EQ(taken.sim.regs[0xE022], 0x10C5);
    taken.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
    CHECK_EQ(taken.device.writeRegister(*lvd, 0x10D0, 0x10C5).effect, OV_WRITE_UNKNOWN);
    CHECK(taken.device.resetMaybeNeeded());
    CHECK(!taken.device.resetNeeded());
    taken.device.beginBatch();
    taken.device.readSpan(0xE022, 1, &word, &status);
    CHECK_EQ(word, 0x10C5);
    CHECK(taken.device.resetNeeded());
    CHECK(!taken.device.resetMaybeNeeded());
  }
  {  // Two writes in doubt, to two registers. Reading the second settles the second; the first
     // stays in doubt until a restart, because only one register is remembered.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
    rig.run("yes");
    rig.run("set Et_float 900");  // asks for no other register, so EV_lvd is not read on the way
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
    CHECK_HAS(rig.run("yes"), "whether it was written is not known.");
    CHECK(rig.device.resetMaybeNeeded());
    CHECK_HAS(rig.run("get Et_float"), "9000 s");
    CHECK(rig.device.resetMaybeNeeded());
    CHECK_HAS(rig.run("get EV_lvd"), "11.50 V");
    CHECK(rig.device.resetMaybeNeeded());
    CHECK(!rig.device.resetNeeded());
    // A write that is known to have been taken says more than the doubt.
    rig.run("set EV_lvr 13.25");
    CHECK_HAS(rig.run("yes"), "Written and read back");
    CHECK(rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
    const std::string status = rig.run("status");
    CHECK_HAS(status, kChanged);
    CHECK_HASNT(status, "may have been changed");
  }
  {  // A profile without a reset command has nothing to say about restarts.
    const std::string noReset = replaced(kMiniProfile, ",\"resetCoil\":\"reset\"", "");
    Rig rig(100.0, noReset.c_str());
    rig.sim.regs[0xE010] = rig.sim.setVolts(12.00);
    rig.run("set a 13.00");
    rig.sim.powered = false;
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "whether it was written is not known.");
    CHECK_HASNT(out, "restart");
  }
}

static void testConsoleCoilsAndReset() {
  {  // A controller at another slave address. Every request goes there: the reads, a write and
     // its read-back, a switch, and the restart with the questions that follow it. Nothing
     // answers at address 1 here, so a request sent there would be a request lost.
    Rig rig;
    rig.sim.slaveId = 9;
    CHECK_HAS(rig.run("slave 9"), "slave 9,");
    CHECK_HAS(lineWith(rig.run("live"), " adc_vb_f "), "13.68 V");
    rig.run("set EV_lvr 13.25");
    CHECK_HAS(rig.run("yes"), "Written and read back: EV_lvr = 13.25 V");
    rig.run("set EV_lvd 13.10");
    CHECK_HAS(rig.run("yes"), "Written and read back: EV_lvd = 13.10 V");
    CHECK_HAS(lineWith(rig.run("coils"), "load_disconnect"), "off");
    rig.run("coil load_disconnect on");
    CHECK_HAS(rig.run("yes"), "Sent and acknowledged. load_disconnect now reads on.");
    CHECK(rig.sim.coils[0x0001]);
    CHECK_HAS(rig.run("reset"), "Type yes to restart it.");
    const std::string restarted = rig.run("yes");
    CHECK_HAS(restarted, "Reset acknowledged.");
    CHECK_HAS(restarted, "The controller answers again after 3.0 s.");
    CHECK_HAS(lineWith(restarted, "V_lvd"), "13.10 V");
    CHECK_EQ(rig.sim.resets, 1);
    CHECK(rig.sim.requests.size() > 20);
    for (size_t i = 0; i < rig.sim.requests.size(); ++i) {
      CHECK_MSG(rig.sim.requests[i][0] == 9, "request %zu went to slave %u", i, (unsigned)rig.sim.requests[i][0]);
    }
  }
  {
    Rig rig;
    CHECK_HAS(rig.run("coil"), "Usage: coil");
    CHECK_HAS(rig.run("coil load_disconnect"), "Usage: coil");
    CHECK_HAS(rig.run("coil load_disconnect maybe"), "Usage: coil");
    CHECK_HAS(rig.run("coil nope on"), "There is no coil called 'nope'");
    CHECK_HAS(rig.run("coil reset on"), "Use 'reset' for that");
    CHECK(!rig.console.awaitingConfirmation());

    const std::string ask = rig.run("coil load_disconnect on");
    CHECK_HAS(ask, "Switch it on (coil 0x0001)?");
    CHECK_EQ(rig.sent(0x05), 0);
    CHECK_HAS(rig.run("yes"), "Sent and acknowledged. load_disconnect now reads on.");
    CHECK(rig.sim.coils[0x0001]);
    rig.run("coil load_disconnect off");
    CHECK_HAS(rig.run("yes"), "load_disconnect now reads off.");
    CHECK(!rig.sim.coils[0x0001]);
    rig.run("coil clear_kwh on");
    CHECK_HAS(rig.run("yes"), "Sent and acknowledged.");
    CHECK_EQ(rig.sim.triggered.size(), 1);
    CHECK_EQ(rig.sim.triggered[0], 0x0012);
    rig.run("coil equalize on");
    CHECK_HAS(rig.run("n"), "Cancelled");
    CHECK(!rig.sim.coils[0x0000]);
    rig.sim.coils.erase(0x0002);
    rig.run("coil charge_disconnect on");
    CHECK_HAS(rig.run("yes"), "The controller refused it: exception 2 (illegal data address).");
  }
  {  // The acknowledgement is lost, but the controller has switched. That is not a refusal: the
     // coil is read back, and what it reads is what is said.
    Rig rig;
    rig.run("coil load_disconnect on");
    rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    const std::string out = rig.run("yes");
    CHECK(rig.sim.coils[0x0001]);
    CHECK_HAS(out, "Sent, but the controller did not acknowledge it (no reply).\r\n"
                   "load_disconnect now reads on: it was switched.\r\n");
    CHECK_HASNT(out, "refused");
    CHECK_HASNT(out, "not known");
    CHECK_EQ(rig.sent(0x05), 1);  // a command is never repeated on its own
    // Switching it off again, with an acknowledgement that arrives damaged.
    rig.run("coil load_disconnect off");
    rig.sim.failNext(SimSunSaver::FAULT_BAD_CRC);
    const std::string off = rig.run("yes");
    CHECK(!rig.sim.coils[0x0001]);
    CHECK_HAS(off, "Sent, but the controller did not acknowledge it (CRC error).\r\n"
                   "load_disconnect now reads off: it was switched.\r\n");
  }
  {  // The command itself is lost: the coil reads as before.
    Rig rig;
    rig.run("coil load_disconnect on");
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY);
    const std::string out = rig.run("yes");
    CHECK(!rig.sim.coils[0x0001]);
    CHECK_HAS(out, "Sent, but the controller did not acknowledge it (no reply).\r\n"
                   "load_disconnect now reads off: it was not switched.\r\n");
    CHECK_HASNT(out, "refused");
  }
  {  // Nothing answers any more: say that it is not known.
    Rig rig;
    rig.run("coil load_disconnect on");
    rig.sim.powered = false;
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "Sent, but the controller did not acknowledge it (no reply).\r\n"
                   "It is not known whether it was switched: 'coils' shows the coil states.\r\n");
    CHECK_HASNT(out, "refused");
    CHECK_HASNT(out, "now reads");
  }
  {  // An action coil reads off whatever it did, so there is nothing to read back.
    Rig rig;
    rig.run("coil clear_kwh on");
    rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    const std::string out = rig.run("yes");
    CHECK_EQ(rig.sim.triggered.size(), 1);  // it was carried out
    CHECK_HAS(out, "Sent, but the controller did not acknowledge it (no reply).\r\n"
                   "It is not known whether the controller carried it out.\r\n");
    CHECK_EQ(rig.sent(0x01), 0);
    CHECK_EQ(rig.sent(0x05), 1);
  }
  {  // An acknowledged switch whose read-back fails is still acknowledged.
    Rig rig;
    rig.run("coil load_disconnect on");
    rig.sim.failNext(SimSunSaver::FAULT_NONE);
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "Sent and acknowledged.\r\n");
    CHECK_HASNT(out, "now reads");
    CHECK_HASNT(out, "not known");
  }
  {  // set, then reset: the fault clears and the new LVD is in effect.
    Rig rig;
    rig.run("set EV_lvr 13.25");
    rig.run("yes");
    rig.run("set EV_lvd 13.10");
    rig.run("yes");
    const std::string ask = rig.run("reset");
    CHECK_HAS(ask, "This restarts the controller (coil 0x00FF).");
    CHECK_HASNT(ask, "Problem:");
    CHECK_EQ(rig.sim.resets, 0);
    const uint64_t t0 = rig.sim.elapsedMs();
    const std::string out = rig.run("yes");
    CHECK_EQ(rig.sim.resets, 1);
    CHECK_HAS(out, "Reset acknowledged.");
    CHECK_HAS(out, "The controller answers again after 3.0 s.");
    CHECK(rig.sim.elapsedMs() - t0 >= 3000);
    CHECK_HAS(lineWith(out, "array_fault"), "none");
    CHECK_HAS(lineWith(out, "V_lvd"), "13.10 V");
    CHECK_HASNT(rig.run("status"), "Settings were changed");
  }
  {  // The pair check stands in the way of a reset with LVD above LVR.
    Rig rig;
    rig.run("set EV_lvd 13.10");
    rig.run("yes");
    const std::string refused = rig.run("reset");
    CHECK_HAS(refused, "Problem: Low voltage reconnect (EV_lvr) must be above low voltage disconnect (EV_lvd).");
    CHECK_HAS(refused, "Now EV_lvd = 13.10 V and EV_lvr = 12.60 V.");
    CHECK_HAS(refused, "Put that right first, or use 'reset anyway'.");
    CHECK(!rig.console.awaitingConfirmation());
    CHECK_EQ(rig.sim.resets, 0);
    CHECK_HAS(rig.run("reset anyway"), "Type yes to restart it.");
    rig.run("no");
    CHECK_EQ(rig.sim.resets, 0);
    // Settings that are switched off (0) are not compared: this controller's load high voltage
    // pair is off, both registers of it.
    rig.run("set EV_lvr 13.25");
    rig.run("yes");
    CHECK_HASNT(rig.run("reset"), "Problem:");
    rig.run("no");
    // One of a pair being off is enough. With no high voltage disconnect there is nothing for
    // the reconnect voltage or the absorption voltages to stay below, before the write or
    // before the restart.
    const std::string off = rig.run("set EV_hvd 0");
    CHECK_HAS(lineWith(off, "  new  "), "[0x0000]");
    CHECK_HASNT(off, "After this write, ");
    CHECK_HAS(rig.run("yes"), "Written and read back: EV_hvd = 0.00 V [0x0000]");
    const std::string restart = rig.run("reset");
    CHECK_HASNT(restart, "Problem:");
    CHECK_HAS(restart, "Type yes to restart it.");
    rig.run("no");
  }
  {  // A pair that cannot be checked stands in the way like one that is out of order, whichever
     // of its two registers does not read.
    Rig rig;
    rig.sim.regs.erase(0xE023);  // EV_lvr, the upper one of its pair
    const std::string upper = rig.run("reset");
    CHECK_HAS(upper, "  Could not check EV_lvd against EV_lvr: a register did not read.");
    CHECK_EQ(countOf(upper, "Could not check"), 1);
    CHECK_HAS(upper, "Put that right first, or use 'reset anyway'.");
    CHECK(!rig.console.awaitingConfirmation());
    rig.sim.regs[0xE023] = rig.sim.setVolts(12.60);
    rig.sim.regs.erase(0xE01C);  // EV_hvr, the lower one of its pair
    const std::string lower = rig.run("reset");
    CHECK_HAS(lower, "  Could not check EV_hvr against EV_hvd: a register did not read.");
    CHECK_EQ(countOf(lower, "Could not check"), 1);
    CHECK_HAS(lower, "Put that right first, or use 'reset anyway'.");
    CHECK(!rig.console.awaitingConfirmation());
    // Only the word 'anyway' overrides that.
    CHECK_HAS(rig.run("reset now"), "Put that right first, or use 'reset anyway'.");
    CHECK(!rig.console.awaitingConfirmation());
    CHECK_HAS(rig.run("reset anyway"), "Type yes to restart it.");
    rig.run("no");
    CHECK_EQ(rig.sim.resets, 0);
  }
  {  // The controller restarts without answering the command (the specification allows it).
    Rig rig;
    rig.sim.resetAcknowledged = false;
    rig.run("reset");
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "No acknowledgement (no reply).");
    CHECK_HAS(out, "The controller answers again after");
    CHECK_EQ(rig.sim.resets, 1);
    CHECK_EQ(rig.sent(0x05), 1);  // the reset command is not repeated
  }
  {  // It takes longer than expected to come back.
    Rig rig;
    rig.sim.resetDownMs = 7000;
    rig.run("reset");
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller answers again after");
    CHECK_HASNT(out, "after 3.0 s");
  }
  {  // It never comes back.
    Rig rig;
    rig.sim.resetDownMs = 3600000;
    rig.run("reset");
    const uint64_t t0 = rig.sim.elapsedMs();
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller has not answered for 20 s.");
    CHECK(rig.sim.elapsedMs() - t0 < 23000);
    CHECK_HASNT(out, "Meters");
  }
  {  // A key press ends the wait.
    Rig rig;
    rig.sim.resetDownMs = 3600000;
    rig.run("reset");
    rig.host.pressKeyAfter(5000);
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "Stopped waiting.");
  }
  {  // The controller refuses the reset coil.
    Rig rig;
    rig.sim.coils.erase(0x00FF);
    rig.run("reset");
    const std::string out = rig.run("yes");
    CHECK_HAS(out, "The controller refused the reset command: exception 2 (illegal data address).");
  }
  {  // The command is acknowledged but nothing restarts: the fault that is still there says so.
    Rig rig;
    rig.sim.resetWorks = false;
    rig.run("set EV_lvr 13.25");
    rig.run("yes");
    rig.run("reset");
    const std::string out = rig.run("yes");
    CHECK_HAS(lineWith(out, "array_fault"), "EEPROM setting edit (reset required)");
    CHECK_HAS(out, "If a fault above still asks for a reset, the controller did not restart");
  }
  {  // No link: say so instead of asking for a yes.
    Rig rig;
    rig.sim.powered = false;
    const std::string out = rig.run("reset");
    CHECK_HAS(out, "The controller is not answering");
    CHECK(!rig.console.awaitingConfirmation());
  }
  {  // A profile without a reset coil.
    const std::string noReset = replaced(kMiniProfile, ",\"resetCoil\":\"reset\"", "");
    Rig rig(100.0, noReset.c_str());
    CHECK_HAS(rig.run("reset"), "This profile has no reset command.");
  }
}

static void testConsoleRawAndExpert() {
  Rig rig;
  const std::string read = rig.run("raw 1 4 8 2");
  CHECK_HAS(read, "> 01 04 00 08 00 02 F0 09");
  CHECK_HAS(read, "< 01 04 04 11 83");
  CHECK_HAS(read, "0x0008  0x1183  4483");
  CHECK_HAS(rig.run("raw 1 3 0xE022 1"), "0xE022  0x0EB8  3768");
  CHECK_HAS(rig.run("raw 1 1 0 3"), "0x0001  off");
  CHECK_HAS(rig.run("raw 1 4 0x35 1"), "(exception 2 (illegal data address)");
  CHECK_HAS(rig.run("raw"), "Usage: raw");
  CHECK_HAS(rig.run("raw 1 4 8"), "Usage: raw");
  CHECK_HAS(rig.run("raw 0 4 8 1"), "Usage: raw");
  CHECK_HAS(rig.run("raw 248 4 8 1"), "Usage: raw");
  CHECK_HAS(rig.run("raw 257 4 8 1"), "Usage: raw");      // not slave 1 by the back door
  CHECK_HAS(rig.run("raw 1 0 8 1"), "Usage: raw");
  CHECK_HAS(rig.run("raw 1 128 8 1"), "Usage: raw");      // from 0x80 on these are exception replies
  CHECK_HAS(rig.run("raw 1 4 0x10008 1"), "Usage: raw");  // an address does not wrap round
  CHECK_HAS(rig.run("raw 1 4 8 65537"), "Usage: raw");    // nor does a count
  CHECK_HAS(rig.run("raw 1 4 8 0"), "The count runs from 1 to 125");
  CHECK_HAS(rig.run("raw 1 1 0 0"), "The count runs from 1 to 64");
  CHECK_HAS(rig.run("raw 1 4 8 126"), "The count runs from 1 to 125");
  CHECK_HAS(rig.run("raw 1 1 0 65"), "The count runs from 1 to 64");

  // Writing to an arbitrary address is bench-only: it needs expert mode and a yes.
  const size_t before = rig.sim.requests.size();
  CHECK_HAS(rig.run("raw 1 6 0xE022 0x10C5"), "'expert on' unlocks the rest");
  CHECK_HAS(rig.run("raw 1 5 1 0xFF00"), "'expert on' unlocks the rest");
  CHECK_HAS(rig.run("raw 1 16 0xE022 1"), "'expert on' unlocks the rest");
  CHECK_HAS(rig.run("raw 1 0x2B 0E 01 00"), "'expert on' unlocks the rest");
  CHECK_EQ(rig.sim.requests.size(), before);
  CHECK(!rig.console.awaitingConfirmation());

  CHECK_HAS(rig.run("expert on"), "Expert mode is on");
  CHECK_HAS(rig.run("status"), "expert on");
  const std::string ask = rig.run("raw 1 6 0xE022 0x10C5");
  CHECK_HAS(ask, "Write 0x10C5 to address 0xE022 on slave 1 with function 6?");
  CHECK_EQ(rig.sim.requests.size(), before);
  CHECK_HAS(rig.run("nope"), "Cancelled");
  CHECK_EQ(rig.sim.regs[0xE022], 0x0EB8);
  // In expert mode too, neither the address nor the value of a write wraps round into range.
  CHECK_HAS(rig.run("raw 1 6 0x1E022 0x10C5"), "Usage: raw");
  CHECK_HAS(rig.run("raw 1 6 0xE022 0x110C5"), "Usage: raw");
  CHECK_HAS(rig.run("raw 257 6 0xE022 0x10C5"), "Usage: raw");
  CHECK(!rig.console.awaitingConfirmation());
  CHECK_EQ(rig.sim.requests.size(), before);
  rig.run("raw 1 6 0xE022 0x10C5");
  const std::string done = rig.run("yes");
  CHECK_HAS(done, "> 01 06 E0 22 10 C5");
  CHECK_HAS(done, "Acknowledged.");
  CHECK_EQ(rig.sim.regs[0xE022], 0x10C5);
  rig.run("raw 1 5 1 0xFF00");
  CHECK_HAS(rig.run("yes"), "Acknowledged.");
  CHECK(rig.sim.coils[0x0001]);
  // The SunSaver has no function 16: its exception is shown as such.
  rig.run("raw 1 16 0xE022 1");
  CHECK_HAS(rig.run("yes"), "Refused: exception 1 (illegal function).");
  CHECK_HASNT(rig.run("status"), "may have been changed");
  {  // A raw write whose acknowledgement does not arrive is not called refused: the controller
     // may have carried it out. For a register that leaves a restart that may be due.
    Rig lost;
    lost.run("expert on");
    lost.run("raw 1 6 0xE022 0x10C5");
    lost.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    const std::string out = lost.run("yes");
    CHECK_EQ(lost.sim.regs[0xE022], 0x10C5);
    CHECK_HAS(out, "Not acknowledged: no reply.\r\n"
                   "It is not known whether the controller carried it out.\r\n");
    CHECK_HASNT(out, "Refused");
    CHECK_HASNT(out, "Not accepted");
    CHECK(lost.device.resetMaybeNeeded());
    CHECK(!lost.device.resetNeeded());
    CHECK_HAS(lost.run("status"), "A write got no answer in this session, so a setting may have been changed.");
    // OptaView does not know which register a raw request was for, so no read settles it.
    lost.run("get EV_lvd");
    CHECK(lost.device.resetMaybeNeeded());
    lost.run("reset anyway");
    CHECK_HAS(lost.run("yes"), "The controller answers again");
    CHECK(!lost.device.resetMaybeNeeded());
  }
  {  // A coil switches at once: no restart is due for it, acknowledged or not.
    Rig coil;
    coil.run("expert on");
    coil.run("raw 1 5 1 0xFF00");
    CHECK_HAS(coil.run("yes"), "Acknowledged.");
    CHECK(!coil.device.resetNeeded());
    coil.run("raw 1 5 1 0x0000");
    coil.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    const std::string out = coil.run("yes");
    CHECK_HAS(out, "Not acknowledged: no reply.");
    CHECK(!coil.sim.coils[0x0001]);
    CHECK(!coil.device.resetMaybeNeeded());
    CHECK(!coil.device.resetNeeded());
    // A register does call for one.
    coil.run("raw 1 6 0xE022 0x10C5");
    CHECK_HAS(coil.run("yes"), "Acknowledged.");
    CHECK(coil.device.resetNeeded());
  }
  // Any other function goes out as typed, and only after a yes: OptaView cannot tell whether
  // it reads or writes (function 15 switches coils).
  size_t sentSoFar = rig.sim.requests.size();
  const std::string askId = rig.run("raw 1 0x2B 0E 01 00");
  CHECK_HAS(askId, "Send function 43 to slave 1 with the bytes 0E 01 00?");
  CHECK_HAS(askId, "it may change the controller, and nothing is checked.");
  CHECK_HAS(askId, "Type yes to send it. Anything else cancels.");
  CHECK(rig.console.awaitingConfirmation());
  CHECK_EQ(rig.sim.requests.size(), sentSoFar);
  const std::string id = rig.run("yes");
  CHECK_HAS(id, "> 01 2B 0E 01 00 70 77");
  CHECK_HAS(id, "4D 6F 72 6E 69 6E 67 73 74 61 72");  // "Morningstar"
  CHECK_EQ(rig.sim.requests.size(), sentSoFar + 1);
  CHECK(!rig.console.awaitingConfirmation());

  rig.sim.coils[0x0001] = false;
  sentSoFar = rig.sim.requests.size();
  const std::string askCoils = rig.run("raw 1 15 00 01 00 01 01 01");  // write multiple coils
  CHECK_HAS(askCoils, "Send function 15 to slave 1 with the bytes 00 01 00 01 01 01?");
  CHECK_EQ(rig.sim.requests.size(), sentSoFar);
  CHECK_HAS(rig.run("no"), "Cancelled. Nothing was sent.");
  CHECK_EQ(rig.sim.requests.size(), sentSoFar);
  rig.run("raw 1 15 00 01 00 01 01 01");
  CHECK_HAS(rig.run("live"), "Cancelled. Nothing was sent.");  // another command is not a yes
  CHECK_EQ(rig.sim.requests.size(), sentSoFar);
  rig.run("raw 1 15 00 01 00 01 01 01");
  const std::string coilsSent = rig.run("yes");
  CHECK_HAS(coilsSent, "> 01 0F 00 01 00 01 01 01 D2 97");
  CHECK_HAS(coilsSent, "(exception 1 (illegal function)");
  CHECK_EQ(rig.sim.requests.size(), sentSoFar + 1);
  CHECK(!rig.sim.coils[0x0001]);

  // A function with no data at all.
  const std::string askBare = rig.run("raw 1 17");
  CHECK_HAS(askBare, "Send function 17 to slave 1 with no data?");
  CHECK_HAS(rig.run("yes"), "> 01 11 C0 2C");
  CHECK_HAS(rig.run("raw 1 0x2B 0E 1G"), "'1G' is not a hex byte");
  CHECK(!rig.console.awaitingConfirmation());
  CHECK_HAS(rig.run("raw"), "Everything but functions 1 to 4 asks for 'yes' before it is sent.");
  CHECK_HAS(rig.run("expert off"), "Expert mode is off.");
  CHECK_HAS(rig.run("raw 1 6 0xE022 1"), "'expert on' unlocks the rest");
  CHECK_HAS(rig.run("expert sideways"), "Usage: expert");
}

// The voltage scale check. The four cases below are the four ways a real controller may turn out
// to encode voltages; in each one both verdicts have to come out right, and neither may be
// taken from the other scale's evidence.
static void testScaleCheck() {
  {  // A. The specification as written: readings and settings x100, the target x96.667.
    Rig rig(100.0);
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "Stored settings (scale vset, now x100/32768). Candidates: x100, x96.667.");
    CHECK_HAS(out, "  1. Setpoints: 13 in use (14 of 14 read)");
    CHECK_HAS(out, "at x100, 13 of 13 are multiples of 0.05 V");
    CHECK_HAS(out, "at x96.667, 1 of 13 is a multiple of 0.05 V");
    CHECK_HAS(out, "Live readings (scale vlive, now x100/32768). Candidates: x100, x96.667.");
    CHECK_HAS(out, "the controller is holding the battery at its target (Vb_ref = 13.70 V).");
    CHECK_HAS(out, "Vb_f reads 13.70 V at x100, 13.24 V at x96.667");
    CHECK_HAS(out, "  3. Multimeter: no reading given.");
    CHECK_HAS(out, "Verdict\r\n"
                   "  Stored settings: x100, which is what OptaView uses now.\r\n"
                   "  Live readings:   x100, which is what OptaView uses now.\r\n");
    CHECK_HASNT(out, "The two differ");
    CHECK_HASNT(out, "That holds only if");
    // Settled: the reminders stop.
    CHECK_HASNT(rig.run("live"), "Note: voltages are shown");
    CHECK_HASNT(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");
    CHECK_EQ(countOf(rig.run("status"), "confirmed by 'scalecheck'"), 2);

    const std::string metered = rig.run("scalecheck 13.68");
    CHECK_HAS(metered, "13.68 V measured, which with the register value works out to x99.9");
    CHECK_HAS(metered, "adc_vb_f reads 13.68 V at x100, 13.23 V at x96.667");
    CHECK_HAS(metered, "  Live readings:   x100, which is what OptaView uses now.");
    CHECK_HASNT(metered, "The regulation test disagrees");
    CHECK_HAS(rig.run("scalecheck twelve"), "Usage: scalecheck");
    CHECK_HAS(rig.run("scalecheck -3"), "Usage: scalecheck");
    CHECK_HAS(rig.run("scalecheck 0"), "Usage: scalecheck");
  }
  {  // B. Everything x96.667, as the TankAlarm client assumes.
    Rig rig(96.667);
    const std::string first = rig.run("live");
    CHECK_HAS(lineWith(first, "adc_vb_f"), "14.15 V");  // the specification's scale reads 3.4 % high here
    CHECK_HAS(first, "(live readings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(rig.run("live"), "Note: voltages are shown");  // once is enough
    CHECK_HAS(rig.run("dump load"), "(stored settings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(rig.run("dump load"), "Note: voltages are shown");
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "at x96.667, 13 of 13 are multiples of 0.05 V");
    CHECK_HAS(out, "  Stored settings: x96.667, not the x100 that OptaView uses now.\r\n"
                   "     'scale vset 96.667' switches it until OptaView restarts. Correct the profile file too.");
    // The battery matches the target at x96.667, but that alone cannot overturn the scale in
    // use: case C looks exactly the same from here.
    CHECK_HAS(out, "Vb_f reads 14.17 V at x100, 13.70 V at x96.667\r\n     -> x96.667\r\n");
    CHECK_HAS(out, "(That holds only if the target's own scale, vref, is right. A multimeter reading tells.)");
    CHECK_HAS(out, "  Live readings:   cannot tell yet. A multimeter reading settles it: scalecheck <volts>");
    // Nothing is settled: the reminders stay.
    const std::string warned = rig.run("set EV_lvd 13.10");
    CHECK_HAS(warned, "is not confirmed");
    CHECK_HAS(lineWith(warned, "  new  "), "[0x10C5]");  // what x100 makes of 13.10 V: wrong for this controller
    rig.run("no");
    // The check found the settings' scale in use to be wrong, so the reminder about it is back.
    CHECK_HAS(rig.run("dump load"), "(stored settings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(rig.run("dump load"), "Note: voltages are shown");
    CHECK_HASNT(rig.run("live"), "Note: voltages are shown");  // nothing new was found about the readings

    const std::string metered = rig.run("scalecheck 13.68");
    CHECK_HAS(metered, "works out to x96.67");
    CHECK_HAS(metered, "  Live readings:   x96.667, not the x100 that OptaView uses now.\r\n"
                       "     'scale vlive 96.667' switches it until OptaView restarts. Correct the profile file too.");
    CHECK_HASNT(metered, "The two differ");  // both x96.667
    CHECK_HASNT(metered, "The regulation test disagrees");

    const std::string wrong = rig.run("status");
    CHECK_HAS(wrong, "stored settings: scale vset = x100/32768, but 'scalecheck' found x96.667 on this controller");
    CHECK_HAS(wrong, "live readings:   scale vlive = x100/32768, but 'scalecheck' found x96.667 on this controller");
    // Each scale is switched on its own. Switching to what the check found confirms it.
    const std::string switched = rig.run("scale vset 96.667");
    CHECK_HAS(switched, "vset is now x96.667/32768 until OptaView restarts.");
    CHECK_HASNT(switched, "has not confirmed");
    CHECK_HAS(lineWith(rig.run("live"), "adc_vb_f"), "14.15 V");  // the readings have not moved
    const std::string ask = rig.run("set EV_lvd 13.10");
    CHECK_HAS(lineWith(ask, "  now  "), "11.50 V");
    CHECK_HAS(lineWith(ask, "  new  "), "[0x1159]");  // encoded the way this controller stores it
    CHECK_HASNT(ask, "is not confirmed");
    rig.run("yes");
    CHECK_EQ(rig.sim.regs[0xE022], 0x1159);
    const std::string half = rig.run("status");
    CHECK_HAS(half, "stored settings: scale vset = x96.667/32768, confirmed by 'scalecheck'");
    CHECK_HAS(half, "live readings:   scale vlive = x100/32768, but 'scalecheck' found x96.667 on this controller");
    CHECK_HAS(rig.run("scale vlive 96.667"), "vlive is now x96.667/32768 until OptaView restarts.");
    CHECK_HAS(lineWith(rig.run("live"), "adc_vb_f"), "13.68 V");
    // Checking again with both switched agrees with itself.
    const std::string again = rig.run("scalecheck 13.68");
    CHECK_HAS(again, "Stored settings (scale vset, now x96.667/32768). Candidates: x96.667, x100.");
    // Confirmed, but only for this session: the profile file is where it has to be put right.
    CHECK_HAS(again, "  Stored settings: x96.667, which is what OptaView uses now.\r\n"
                     "     The profile file still has x100: correct it there, or OptaView starts with that again.\r\n"
                     "  Live readings:   x96.667, which is what OptaView uses now.\r\n"
                     "     The profile file still has x100: correct it there, or OptaView starts with that again.\r\n");
    CHECK_HASNT(again, "That holds only if");
  }
  {  // C. Everything x100, the target too: the specification's 96.667 for Vb_ref is the mistake.
    Rig rig(100.0, OV_BUILTIN_PROFILE, 100.0, 100.0);
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "  Stored settings: x100, which is what OptaView uses now.");
    CHECK_HAS(out, "the controller is holding the battery at its target (Vb_ref = 13.24 V).");
    CHECK_HAS(out, "Vb_f reads 13.70 V at x100, 13.24 V at x96.667\r\n     -> x96.667\r\n");
    // The readings are right as they are. The regulation test must not talk OptaView out of it.
    CHECK_HAS(out, "  Live readings:   cannot tell yet.");
    CHECK_HASNT(out, "'scale vlive");
    const std::string metered = rig.run("scalecheck 13.68");
    CHECK_HAS(metered, "  Live readings:   x100, which is what OptaView uses now.\r\n"
                       "     The regulation test disagrees. If the multimeter is right, the target's own scale (vref)\r\n"
                       "     is the one that is off: 'scale vref 100' would put Vb_ref at the battery voltage.");
    CHECK_HASNT(rig.run("live"), "Note: voltages are shown");  // both settled
    // The battery a little off its target still names the known figure, not 100.4.
    rig.sim.regs[0x0013] = rig.sim.volts(13.76);
    CHECK_HAS(rig.run("scalecheck 13.68"), "'scale vref 100' would put Vb_ref at the battery voltage.");
    rig.sim.regs[0x0013] = rig.sim.volts(13.70);
    CHECK_HAS(rig.run("scale vref 100"), "vref is now x100/32768");
    const std::string fixed = rig.run("scalecheck 13.68");
    CHECK_HAS(fixed, "(Vb_ref = 13.70 V)");
    CHECK_HASNT(fixed, "The regulation test disagrees");
    CHECK_HASNT(fixed, "That holds only if");
  }
  {  // D. Readings x96.667 and settings x100: one figure for what the controller measures,
     // another for what it is told. The case a single voltage scale gets wrong.
    Rig rig(96.667, OV_BUILTIN_PROFILE, 100.0, 96.667);
    CHECK_HAS(lineWith(rig.run("live"), "adc_vb_f"), "14.15 V");
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "at x100, 13 of 13 are multiples of 0.05 V");
    CHECK_HAS(out, "  Stored settings: x100, which is what OptaView uses now.");
    CHECK_HAS(out, "  Live readings:   cannot tell yet.");
    const std::string metered = rig.run("scalecheck 13.68");
    CHECK_HAS(metered, "works out to x96.67");
    CHECK_HAS(metered, "  Stored settings: x100, which is what OptaView uses now.\r\n"
                       "  Live readings:   x96.667, not the x100 that OptaView uses now.\r\n"
                       "     'scale vlive 96.667' switches it until OptaView restarts. Correct the profile file too.\r\n"
                       "  The two differ on this controller. Each has its own scale in the profile, so both can be right.");
    CHECK_HASNT(metered, "'scale vset");  // the multimeter says nothing about the settings
    CHECK_HASNT(metered, "The regulation test disagrees");
    CHECK_HAS(rig.run("scale vlive 96.667"), "vlive is now x96.667/32768");
    const std::string live = rig.run("live");
    CHECK_HAS(lineWith(live, "adc_vb_f"), "13.68 V");
    CHECK_HAS(lineWith(live, " V_lvd "), "11.50 V");
    // The setting is still written at x100, whatever the multimeter said about the readings.
    const std::string ask = rig.run("set EV_lvd 13.10");
    CHECK_HAS(lineWith(ask, "  now  "), "11.50 V");
    CHECK_HAS(lineWith(ask, "  new  "), "[0x10C5]");
    CHECK_HASNT(ask, "is not confirmed");
    rig.run("yes");
    CHECK_EQ(rig.sim.regs[0xE022], 0x10C5);
    rig.run("set EV_lvr 13.25");
    rig.run("yes");
    CHECK_HAS(rig.run("reset"), "Type yes");
    rig.run("yes");
    // After the restart the controller acts on 13.10 V, and the reading of it says so.
    CHECK_HAS(lineWith(rig.run("live"), " V_lvd "), "13.10 V");
    const std::string again = rig.run("scalecheck 13.68");
    CHECK_HAS(again, "  Stored settings: x100, which is what OptaView uses now.\r\n"
                     "  Live readings:   x96.667, which is what OptaView uses now.\r\n"
                     "     The profile file still has x100: correct it there, or OptaView starts with that again.\r\n"
                     "  The two differ on this controller.");
  }
  {  // At night the regulation test does not apply. The setpoints still settle the settings;
     // the readings wait for a multimeter.
    Rig rig(100.0);
    rig.sim.regs[0x0011] = 3;
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "the controller is in NIGHT (3), not holding the battery at its target");
    CHECK_HAS(out, "     -> cannot tell now; try again in absorption or float");
    CHECK_HAS(out, "  Stored settings: x100, which is what OptaView uses now.");
    CHECK_HAS(out, "  Live readings:   cannot tell yet. A multimeter reading settles it: scalecheck <volts>");
    const std::string status = rig.run("status");
    CHECK_HAS(status, "stored settings: scale vset = x100/32768, confirmed by 'scalecheck'");
    CHECK_HAS(status, "live readings:   scale vlive = x100/32768, not confirmed on this controller");
    CHECK_HAS(status, "'scalecheck' tests what is not confirmed.");
    CHECK_HASNT(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");
    // The reminder now names the one scale that is still open.
    CHECK_HAS(rig.run("live"), "Note: voltages are shown with a scale that is not confirmed on this controller\r\n"
                               "      (live readings x100/32768). 'scalecheck' tests it.");
  }
  {  // Odd setpoints and night: nothing to go on, and the console says so instead of guessing.
    Rig rig(100.0);
    rig.sim.regs[0x0011] = 3;
    for (uint16_t a = 0xE000; a <= 0xE033; ++a) {
      const OvRegister *reg = rig.profile.findRegisterAt(a);
      if (reg != nullptr && reg->scale == rig.profile.scaleCheck().settingsScale) {
        rig.sim.regs[a] = (uint16_t)(rig.sim.regs[a] == 0 ? 0 : rig.sim.regs[a] + 5);  // + 0.015 V
      }
    }
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "     -> too few round setpoints to tell");
    CHECK_HAS(out, "  Stored settings: cannot tell from these setpoints.\r\n"
                   "     If you know what one of them was set to, 'get <name>' shows whether this scale reads it right.");
    CHECK_HAS(out, "  Live readings:   cannot tell yet.");
    CHECK_HAS(rig.run("dump"), "(live readings x100/32768, stored settings x100/32768). 'scalecheck' tests them.");
    // A multimeter settles the readings, and only the readings.
    const std::string metered = rig.run("scalecheck 13.70");
    CHECK_HAS(metered, "  Stored settings: cannot tell from these setpoints.");
    CHECK_HAS(metered, "  Live readings:   x100, which is what OptaView uses now.");
    CHECK_HAS(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");
    CHECK_HAS(rig.run("scalecheck 13.22"), "  Live readings:   x96.667, not the x100 that OptaView uses now.");
    // A reading that fits no candidate decides nothing, and says what it would mean.
    const std::string off = rig.run("scalecheck 12.00");
    CHECK_HAS(off, "     -> no candidate is within 1.2 % of the multimeter");
    CHECK_HAS(off, "  Live readings:   the multimeter fits none of the candidates, so this stays open.\r\n"
                   "     That takes back what an earlier check found (x96.667).\r\n"
                   "     If the reading is right, the scale is x87.71: 'scale vlive 87.71' uses that.");
  }
  {  // A multimeter reading that fits nothing is not overruled by the regulation test.
    Rig rig(100.0);
    const std::string out = rig.run("scalecheck 12.00");
    CHECK_HAS(out, "Vb_f reads 13.70 V at x100, 13.24 V at x96.667\r\n     -> x100\r\n");
    CHECK_HAS(out, "  Live readings:   the multimeter fits none of the candidates, so this stays open.");
    CHECK_HAS(rig.run("status"), "live readings:   scale vlive = x100/32768, not confirmed on this controller");
    // A verdict against the scale in use takes back an earlier confirmation.
    rig.run("scalecheck 13.68");
    CHECK_EQ(countOf(rig.run("status"), "confirmed by 'scalecheck'"), 2);
    rig.run("scalecheck 13.22");
    CHECK_HAS(rig.run("status"),
              "live readings:   scale vlive = x100/32768, but 'scalecheck' found x96.667 on this controller");
    // A battery register that reads 0 cannot be compared with anything.
    rig.sim.regs[0x0008] = 0;
    const std::string dead = rig.run("scalecheck 13.68");
    CHECK_HAS(dead, "  3. Multimeter: adc_vb_f reads 0, so there is nothing to compare the reading with.");
    CHECK_HAS(dead, "  Live readings:   x100, which is what OptaView uses now.");  // regulation still confirms
    rig.sim.regs[0x0013] = 0;
    const std::string flat = rig.run("scalecheck");
    CHECK_HAS(flat, "  2. Regulation: the battery or the target register reads 0.\r\n     -> cannot tell\r\n");
    CHECK_HAS(flat, "  Live readings:   cannot tell yet.");
  }
  {  // Blank or damaged EEPROM cells are not settings that somebody chose. 0xFFFF would look
     // perfectly round at x100 (199.997 V) and must not vote.
    Rig rig(96.667);
    const uint16_t blanked[] = {0xE000, 0xE001, 0xE00D, 0xE00E, 0xE014, 0xE01B, 0xE01C, 0xE030};
    for (size_t i = 0; i < sizeof(blanked) / sizeof(blanked[0]); ++i) {
      rig.sim.regs[blanked[i]] = 0xFFFF;
    }
    // A cell with a few stray bits in it is no setting either. These two would be 0.003 V and
    // 0.05 V, round under every candidate, far below anything the profile allows.
    rig.sim.regs[0xE031] = 0x0001;
    rig.sim.regs[0xE032] = 0x0010;
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "  1. Setpoints: 3 in use (14 of 14 read)");
    CHECK_HAS(out, "     -> too few round setpoints to tell");
    CHECK_HAS(out, "  Stored settings: cannot tell from these setpoints.");
    // With one more real setpoint there are enough again, and they say what they said before.
    rig.sim.regs[0xE000] = rig.sim.setVolts(14.00);
    const std::string enough = rig.run("scalecheck");
    CHECK_HAS(enough, "  1. Setpoints: 4 in use (14 of 14 read)");
    CHECK_HAS(enough, "at x96.667, 4 of 4 are multiples of 0.05 V");
    CHECK_HAS(enough, "  Stored settings: x96.667, not the x100 that OptaView uses now.");
  }
  {  // Setpoints that cannot be read are left out, not counted as zero.
    Rig rig(100.0);
    rig.sim.regs.erase(0xE030);
    rig.sim.regs.erase(0xE031);
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "  1. Setpoints: 11 in use (12 of 14 read)");
    CHECK_HAS(out, "  Stored settings: x100, which is what OptaView uses now.");
    // The regulation registers missing: that test says so and the rest goes on.
    rig.sim.regs.erase(0x0014);
    const std::string blind = rig.run("scalecheck 13.68");
    CHECK_HAS(blind, "  2. Regulation: the registers could not be read.");
    CHECK_HAS(blind, "  Live readings:   x100, which is what OptaView uses now.");
    rig.sim.regs.erase(0x0008);
    CHECK_HAS(rig.run("scalecheck 13.68"), "  3. Multimeter: the register could not be read.");
  }
  {  // A profile of one's own may name no alternates and give a setpoint no limits. Then there is
     // one candidate, which any match confirms, and nothing but its value says whether a setpoint
     // is in use. A register that reads 0 is still not a measurement and not a setting.
    const std::string own = replaced(replaced(std::string(kScaleProfile), "\"alternates\":[96.667],", ""),
                                     ",\"min\":9,\"max\":17}]", "}]");  // sp2 has no limits now
    Rig rig(100.0, own.c_str());
    rig.sim.regs[0xE000] = rig.sim.setVolts(14.00);  // sp1
    rig.sim.regs[0xE001] = 0;                        // sp2: switched off
    rig.sim.regs[11] = 1;                            // state: HOLD, the one that regulates
    rig.sim.regs[9] = 0;                             // slow: the battery reads nothing
    rig.sim.regs[10] = 30;                           // target: 0.09 V
    const std::string dead = rig.run("scalecheck");
    CHECK_HAS(dead, "Stored settings (scale vs, now x100/32768). Candidates: x100.\r\n");
    CHECK_HAS(dead, "  1. Setpoints: 1 in use (2 of 2 read)");
    CHECK_HAS(dead, "at x100, 1 of 1 is a multiple of 0.05 V");
    CHECK_HAS(dead, "Live readings (scale vl, now x100/32768). Candidates: x100.\r\n");
    CHECK_HAS(dead, "  2. Regulation: the battery or the target register reads 0.\r\n     -> cannot tell\r\n");
    CHECK_HAS(dead, "  Live readings:   cannot tell yet.");
    // The same with a battery reading and no target.
    rig.sim.regs[9] = 30;
    rig.sim.regs[10] = 0;
    const std::string aimless = rig.run("scalecheck");
    CHECK_HAS(aimless, "  2. Regulation: the battery or the target register reads 0.\r\n     -> cannot tell\r\n");
    CHECK_HAS(aimless, "  Live readings:   cannot tell yet.");
    CHECK_HAS(rig.run("status"), "not confirmed on this controller");
    // With both there, the one candidate is confirmed by a battery at its target.
    rig.sim.regs[9] = rig.sim.volts(13.70);
    rig.sim.regs[10] = rig.sim.refVolts(13.70);
    const std::string held = rig.run("scalecheck");
    CHECK_HAS(held, "the controller is holding the battery at its target (target = 13.70 V).");
    CHECK_HAS(held, "slow reads 13.70 V at x100\r\n     -> x100\r\n");
    CHECK_HAS(held, "  Live readings:   x100, which is what OptaView uses now.");
  }
  {  // No link.
    Rig rig;
    rig.sim.powered = false;
    const std::string out = rig.run("scalecheck");
    CHECK_HAS(out, "The controller is not answering");
    CHECK_HASNT(out, "Verdict");
    CHECK_HAS(rig.run("status"), "not confirmed on this controller");
  }
  {  // The judging functions on their own.
    CHECK(near(ovGridDistance(14.0015, 0.05), 0.0015, 1e-9));
    CHECK(near(ovGridDistance(13.98, 0.05), 0.02, 1e-9));
    CHECK(near(ovGridDistance(0.0, 0.05), 0.0, 1e-12));

    OvScaleCandidates list;
    memset(&list, 0, sizeof(list));
    ovAddCandidate(&list, 100.0);
    ovAddCandidate(&list, 96.667);
    CHECK_EQ(list.count, 2);
    ovAddCandidate(&list, 96.667);
    ovAddCandidate(&list, 100.0004);
    CHECK_EQ(list.count, 2);  // already listed
    ovAddCandidate(&list, 0.0);
    ovAddCandidate(&list, -5.0);
    CHECK_EQ(list.count, 2);
    ovAddCandidate(&list, 98.0);
    CHECK_EQ(list.count, 3);
    for (int i = 0; i < 10; ++i) {
      ovAddCandidate(&list, 50.0 + i);
    }
    CHECK_EQ(list.count, OV_SCALE_CANDIDATES);  // the list is full, not overrun

    OvSettingsScaleReport settings;
    memset(&settings, 0, sizeof(settings));
    ovAddCandidate(&settings.candidates, 100.0);
    ovAddCandidate(&settings.candidates, 96.667);
    const uint16_t three[] = {4588, 4489, 3768};
    ovJudgeSetpoints(&settings, three, 3, 32768.0, 0.05);
    CHECK_EQ(settings.fits[0], 3);
    CHECK_EQ(settings.verdict, -1);  // three setpoints are too few to call it
    // Truncated instead of rounded by whoever programmed them: still on the grid.
    const uint16_t truncated[] = {4587, 4489, 3768, 4128, 4292};
    ovJudgeSetpoints(&settings, truncated, 5, 32768.0, 0.05);
    CHECK_EQ(settings.fits[0], 5);
    CHECK_EQ(settings.verdict, 0);
    // Values on a 0.01 V grid only: no pattern, no verdict.
    const uint16_t odd[] = {4594, 4496, 3775, 4135, 4300, 4391};
    ovJudgeSetpoints(&settings, odd, 6, 32768.0, 0.05);
    CHECK_EQ(settings.verdict, -1);
    // Most of the setpoints have to be round under the winner: six of ten are enough, five of
    // ten are not. The others here sit 0.02 V off a round value and are round under neither.
    const uint16_t sixOfTen[] = {4588, 4489, 3768, 4129, 4293, 4719,  // 14.00 13.70 11.50 12.60 13.10 14.40
                                 4496, 3775, 4299, 4725};             // 13.72 11.52 13.12 14.42
    ovJudgeSetpoints(&settings, sixOfTen, 10, 32768.0, 0.05);
    CHECK_EQ(settings.fits[0], 6);
    CHECK_EQ(settings.fits[1], 0);
    CHECK_EQ(settings.verdict, 0);
    const uint16_t fiveOfTen[] = {4588, 4489, 3768, 4129, 4293, 4037,  // the sixth is 12.32 now
                                  4496, 3775, 4299, 4725};
    ovJudgeSetpoints(&settings, fiveOfTen, 10, 32768.0, 0.05);
    CHECK_EQ(settings.fits[0], 5);
    CHECK_EQ(settings.fits[1], 0);
    CHECK_EQ(settings.verdict, -1);
    // And the winner needs twice the fits of any other candidate. 12.00, 13.50 and 15.00 V are
    // round at x96.667 as well (11.60, 13.05 and 14.50 V). With three of six setpoints like that,
    // x100 is ahead by two to one. With a fourth it is six against four: too close to call.
    const uint16_t twoToOne[] = {4588, 4489, 3768, 3932, 4424, 4915};
    ovJudgeSetpoints(&settings, twoToOne, 6, 32768.0, 0.05);
    CHECK_EQ(settings.fits[0], 6);
    CHECK_EQ(settings.fits[1], 3);
    CHECK_EQ(settings.verdict, 0);
    const uint16_t tooClose[] = {4588, 4489, 3441, 3932, 4424, 4915};  // 10.50 V is 10.15 V at x96.667
    ovJudgeSetpoints(&settings, tooClose, 6, 32768.0, 0.05);
    CHECK_EQ(settings.fits[0], 6);
    CHECK_EQ(settings.fits[1], 4);
    CHECK_EQ(settings.verdict, -1);
    // None at all.
    ovJudgeSetpoints(&settings, odd, 0, 32768.0, 0.05);
    CHECK_EQ(settings.setpointsUsed, 0);
    CHECK_EQ(settings.verdict, -1);

    // The multimeter decides; the regulation test may only confirm.
    OvLiveScaleReport live;
    memset(&live, 0, sizeof(live));
    ovAddCandidate(&live.candidates, 100.0);
    ovAddCandidate(&live.candidates, 96.667);
    live.regulationVerdict = 0;
    live.meterVerdict = -1;
    ovJudgeLive(&live);
    CHECK_EQ(live.verdict, 0);
    CHECK(!live.conflict);
    live.regulationVerdict = 1;
    ovJudgeLive(&live);
    CHECK_EQ(live.verdict, -1);  // it cannot overturn the scale in use
    live.regulationVerdict = -1;
    ovJudgeLive(&live);
    CHECK_EQ(live.verdict, -1);
    live.meterRaw = 4483;
    live.meterGivenVolts = 13.23;
    ovJudgeMeter(&live, 32768.0);
    CHECK(live.meterCompared);
    CHECK_EQ(live.meterMatches, 1);
    CHECK_EQ(live.meterVerdict, 1);
    CHECK(near(live.impliedNumerator, 96.70, 0.01));
    live.regulationVerdict = 0;
    ovJudgeLive(&live);
    CHECK_EQ(live.verdict, 1);  // the multimeter, against the regulation test
    CHECK(live.conflict);
    live.regulationVerdict = 1;
    ovJudgeLive(&live);
    CHECK_EQ(live.verdict, 1);
    CHECK(!live.conflict);
    // Within 1.2 % counts; 1.3 % does not.
    live.meterGivenVolts = 13.68 * 1.011;
    ovJudgeMeter(&live, 32768.0);
    CHECK_EQ(live.meterVerdict, 0);
    live.meterGivenVolts = 13.68 * 1.013;
    ovJudgeMeter(&live, 32768.0);
    CHECK_EQ(live.meterVerdict, -1);
    CHECK_EQ(live.meterMatches, 0);
    live.regulationVerdict = 0;
    ovJudgeLive(&live);
    CHECK_EQ(live.verdict, -1);  // a reading that fits nothing keeps the question open
    CHECK(!live.conflict);
    // Two candidates closer together than the multimeter can separate: no verdict.
    ovAddCandidate(&live.candidates, 99.5);
    live.meterGivenVolts = 13.65;
    ovJudgeMeter(&live, 32768.0);
    CHECK_EQ(live.meterMatches, 2);
    CHECK_EQ(live.meterVerdict, -1);
    // No reading, or a register at 0: no comparison.
    live.meterRaw = 0;
    ovJudgeMeter(&live, 32768.0);
    CHECK(!live.meterCompared);
    live.meterRaw = 4483;
    live.meterGivenVolts = 0.0;
    ovJudgeMeter(&live, 32768.0);
    CHECK(!live.meterCompared);

    // Regulation: exactly one candidate has to put the battery within 0.15 V of the target.
    memset(&live, 0, sizeof(live));
    ovAddCandidate(&live.candidates, 100.0);
    ovAddCandidate(&live.candidates, 96.667);
    live.batteryRaw = 4489;  // 13.70 V at x100
    live.referenceVolts = 13.70;
    ovJudgeRegulation(&live, 32768.0);
    CHECK_EQ(live.regulationVerdict, 0);
    CHECK_EQ(live.regulationMatches, 1);
    live.referenceVolts = 13.24;
    ovJudgeRegulation(&live, 32768.0);
    CHECK_EQ(live.regulationVerdict, 1);
    live.referenceVolts = 13.47;  // between the two: the battery is not at its target
    ovJudgeRegulation(&live, 32768.0);
    CHECK_EQ(live.regulationVerdict, -1);
    CHECK_EQ(live.regulationMatches, 0);
    ovAddCandidate(&live.candidates, 99.5);
    live.referenceVolts = 13.67;
    ovJudgeRegulation(&live, 32768.0);
    CHECK_EQ(live.regulationMatches, 2);
    CHECK_EQ(live.regulationVerdict, -1);
  }
  {  // The device's report, without the console.
    Rig rig(96.667, OV_BUILTIN_PROFILE, 100.0, 96.667);  // case D
    OvScaleReport report;
    CHECK(rig.device.scaleCheck(13.68, &report));
    CHECK(report.settings.tested);
    CHECK_EQ(report.settings.candidates.count, 2);
    CHECK(report.settings.candidates.num[0] == 100.0);
    CHECK(report.settings.candidates.num[1] == 96.667);
    CHECK_EQ(report.settings.setpointsRead, 14);
    CHECK_EQ(report.settings.setpointsUsed, 13);
    CHECK_EQ(report.settings.fits[0], 13);
    CHECK_EQ(report.settings.verdict, 0);
    CHECK(report.live.tested);
    CHECK(report.live.regulationRead);
    CHECK(report.live.regulating);
    CHECK_EQ(report.live.stateRaw, 7);
    CHECK_EQ(report.live.regulationVerdict, 1);
    CHECK(report.live.meterGiven && report.live.meterRead && report.live.meterCompared);
    CHECK_EQ(report.live.meterVerdict, 1);
    CHECK_EQ(report.live.verdict, 1);
    CHECK(!report.live.conflict);
    // An override becomes candidate 0; the file's own number stays in the list.
    rig.profile.findScale("vlive")->num = 98.5;
    CHECK(rig.device.scaleCheck(0.0, &report));
    CHECK_EQ(report.live.candidates.count, 3);
    CHECK(report.live.candidates.num[0] == 98.5);
    CHECK(report.live.candidates.num[1] == 100.0);
    CHECK(report.live.candidates.num[2] == 96.667);
    CHECK_EQ(report.settings.candidates.count, 2);  // the other scale's list is its own
    CHECK(!report.live.meterGiven);
    CHECK_EQ(report.live.verdict, -1);
  }
  {
    Rig rig;
    const std::string scales = rig.run("scale");
    CHECK_HAS(scales, "vlive     x100/32768 V");
    CHECK_HAS(scales, "vset      x100/32768 V");
    CHECK_HAS(scales, "vref      x96.667/32768 V");
    CHECK_HAS(scales, "i79       x79.16/32768 A");
    CHECK_HAS(rig.run("scale nope 1"), "There is no scale called 'nope'");
    CHECK_HAS(rig.run("scale vlive"), "Usage: scale <name> <x>");
    CHECK_HAS(rig.run("scale vlive 0"), "Usage: scale <name> <x>");
    CHECK_HAS(rig.run("scale vlive -96.667"), "Usage: scale <name> <x>");
    CHECK_HAS(rig.run("scale vlive lots"), "Usage: scale <name> <x>");
    CHECK_HAS(rig.run("scale i79 80"), "i79 is now x80/32768");
    CHECK_HAS(rig.run("scale VREF 100"), "vref is now x100/32768");
    // Changing other scales settles neither of the two that are checked.
    CHECK_HAS(rig.run("dump"), "(live readings x100/32768, stored settings x100/32768). 'scalecheck' tests them.");
  }
  {  // Only 'scalecheck' confirms a scale. One that was set by hand stays unconfirmed, and a
     // confirmed one that is changed by hand afterwards is unconfirmed again.
    Rig rig;
    CHECK_HAS(rig.run("scale vlive 96.667"),
              "vlive is now x96.667/32768 until OptaView restarts. The profile file is unchanged.\r\n"
              "'scalecheck' has not confirmed that figure on this controller.");
    const std::string status = rig.run("status");
    CHECK_HAS(status, "stored settings: scale vset = x100/32768, not confirmed on this controller");
    CHECK_HAS(status, "live readings:   scale vlive = x96.667/32768, not confirmed on this controller");
    CHECK_HAS(rig.run("live"), "      (live readings x96.667/32768). 'scalecheck' tests it.");
    CHECK_HAS(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");
    // This controller follows the specification, and the check says so against the hand-set scale.
    const std::string out = rig.run("scalecheck 13.68");
    CHECK_HAS(out, "Live readings (scale vlive, now x96.667/32768). Candidates: x96.667, x100.");
    CHECK_HAS(out, "  Stored settings: x100, which is what OptaView uses now.\r\n"
                   "  Live readings:   x100, not the x96.667 that OptaView uses now.\r\n"
                   "     'scale vlive 100' switches it back to the profile's figure.");
    CHECK_HASNT(out, "Correct the profile file");  // the file is right; the hand-set figure was not
    CHECK_HAS(rig.run("live"), "      (live readings x96.667/32768). 'scalecheck' tests it.");  // found wrong: said again
    CHECK_HASNT(rig.run("scale vlive 100"), "has not confirmed");
    CHECK_EQ(countOf(rig.run("status"), "confirmed by 'scalecheck'"), 2);
    CHECK_HASNT(rig.run("dump"), "Note: voltages are shown");
    CHECK_HASNT(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");

    CHECK_HAS(rig.run("scale vset 96.667"), "'scalecheck' has not confirmed that figure on this controller.");
    const std::string ask = rig.run("set EV_lvd 13.10");
    CHECK_HAS(ask, "  Careful: the scale of stored settings (vset, x96.667/32768) is not confirmed on this controller.");
    rig.run("no");
    CHECK_HAS(rig.run("status"),
              "stored settings: scale vset = x96.667/32768, but 'scalecheck' found x100 on this controller");
    CHECK_HAS(rig.run("dump load"), "      (stored settings x96.667/32768). 'scalecheck' tests it.");
    CHECK_HASNT(rig.run("scale vset 100"), "has not confirmed");  // back to what the check found
    CHECK_HASNT(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");
    CHECK_HASNT(rig.run("status"), "'scalecheck' tests");
  }
  {  // A multimeter reading that fits no candidate contradicts an earlier verdict too: the live
     // scale is open again, while the stored settings keep the verdict they got on their own.
    Rig rig;
    CHECK_HAS(rig.run("live"), "Note: voltages are shown");  // the reminder has been given once
    rig.run("scalecheck 13.68");
    CHECK_EQ(countOf(rig.run("status"), "confirmed by 'scalecheck'"), 2);
    CHECK_HASNT(rig.run("live"), "Note: voltages are shown");
    const std::string off = rig.run("scalecheck 12.00");
    CHECK_HAS(off, "  Stored settings: x100, which is what OptaView uses now.\r\n"
                   "  Live readings:   the multimeter fits none of the candidates, so this stays open.\r\n"
                   "     That takes back what an earlier check found (x100).\r\n"
                   "     If the reading is right, the scale is x87.71: 'scale vlive 87.71' uses that.");
    const std::string status = rig.run("status");
    CHECK_HAS(status, "stored settings: scale vset = x100/32768, confirmed by 'scalecheck'");
    CHECK_HAS(status, "live readings:   scale vlive = x100/32768, not confirmed on this controller");
    CHECK_HAS(rig.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");  // the reminder is back
    CHECK_HASNT(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");
    // Taking the reading at its word is then a matter for the next check.
    rig.run("scale vlive 87.71");
    CHECK_HAS(rig.run("scalecheck 12.00"), "  Live readings:   x87.71, which is what OptaView uses now.");
    // With no earlier verdict there is nothing to take back. The reminder comes back all the
    // same: the reading has just contradicted every candidate, the scale in use included.
    Rig fresh;
    CHECK_HAS(fresh.run("live"), "Note: voltages are shown");
    CHECK_HASNT(fresh.run("live"), "Note: voltages are shown");
    CHECK_HASNT(fresh.run("scalecheck 12.00"), "takes back");
    CHECK_HAS(fresh.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(fresh.run("live"), "Note: voltages are shown");
    // A check that merely cannot tell leaves an earlier verdict alone.
    Rig night;
    night.run("scalecheck 13.68");
    night.sim.regs[0x0011] = 3;
    CHECK_HAS(night.run("scalecheck"), "  Live readings:   cannot tell yet.");
    CHECK_EQ(countOf(night.run("status"), "confirmed by 'scalecheck'"), 2);
  }
  {  // What 'scalecheck' found belongs to the controller it talked to. Another slave address or
     // other serial settings may reach another controller, so both scales are unconfirmed again.
    Rig rig;
    rig.run("scalecheck 13.68");
    CHECK_EQ(countOf(rig.run("status"), "confirmed by 'scalecheck'"), 2);
    CHECK_HAS(rig.run("slave 2"),
              "Link: slave 2, 9600 8N2, read function 4\r\n"
              "Note: with other link settings this may be another controller, so what 'scalecheck'\r\n"
              "      found no longer counts. The voltage scales are unconfirmed again.");
    const std::string status = rig.run("status");
    CHECK_HAS(status, "stored settings: scale vset = x100/32768, not confirmed on this controller");
    CHECK_HAS(status, "live readings:   scale vlive = x100/32768, not confirmed on this controller");
    CHECK_HASNT(rig.run("slave 1"), "Note:");  // nothing had been found at slave 2
    CHECK_HAS(rig.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");
    CHECK_HAS(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");

    // The same address again, the read function, the timeout and the block mode change nothing.
    rig.run("scalecheck 13.68");
    CHECK_HASNT(rig.run("slave 1"), "Note:");
    rig.run("fc 3");
    rig.run("timeout 800");
    rig.run("blocks off");
    CHECK_HASNT(rig.run("link 100"), "unconfirmed");  // refused: the link is unchanged
    CHECK_HASNT(rig.run("link 9600 8N2"), "unconfirmed");
    CHECK_EQ(countOf(rig.run("status"), "confirmed by 'scalecheck'"), 2);
    CHECK_HASNT(rig.run("set EV_lvd 13.10"), "is not confirmed");
    rig.run("no");

    // Other serial settings.
    CHECK_HAS(rig.run("link 9600 8N1"), "The voltage scales are unconfirmed again.");
    CHECK_EQ(countOf(rig.run("status"), "confirmed by 'scalecheck'"), 0);
    CHECK_HASNT(rig.run("link 19200 8N1"), "Note:");  // said once; nothing is left to take back

    // 'probe' that ends at other settings.
    Rig probing;
    probing.run("scalecheck 13.68");
    probing.sim.deviceBaud = 19200;
    const std::string found = probing.run("probe");
    CHECK_HAS(found, "Found it: slave 1, 19200 8N2, read function 4.");
    CHECK_HAS(found, "These settings are now in use. Try 'live'.\r\n"
                     "Note: with other link settings this may be another controller");
    CHECK_EQ(countOf(probing.run("status"), "confirmed by 'scalecheck'"), 0);
    // 'probe' that finds the same settings, or nothing at all, leaves the verdicts alone.
    Rig same;
    same.run("scalecheck 13.68");
    CHECK_HASNT(same.run("probe"), "unconfirmed");
    same.sim.powered = false;
    CHECK_HASNT(same.run("probe"), "unconfirmed");
    same.sim.powered = true;
    CHECK_EQ(countOf(same.run("status"), "confirmed by 'scalecheck'"), 2);

    // The reminder after reads is given once for each controller.
    Rig twice;
    CHECK_HAS(twice.run("live"), "Note: voltages are shown");
    CHECK_HASNT(twice.run("live"), "Note: voltages are shown");
    CHECK_HASNT(twice.run("slave 2"), "Note:");
    twice.run("slave 1");
    CHECK_HAS(twice.run("live"), "Note: voltages are shown");
    // The stored settings have a reminder of their own, and the same holds for it. It waits for
    // a command that shows a stored setting: 'live' shows none.
    Rig stored;
    CHECK_HAS(stored.run("dump load"), "      (stored settings x100/32768). 'scalecheck' tests it.");
    CHECK_HASNT(stored.run("dump load"), "Note: voltages are shown");
    stored.run("slave 2");
    stored.run("slave 1");
    CHECK_HAS(stored.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");
    CHECK_HAS(stored.run("dump load"), "      (stored settings x100/32768). 'scalecheck' tests it.");
  }
  {  // The device counts every change to what was found, a finding that is taken back included,
     // and each scale by itself. A console that gave its reminder at an earlier count gives it again.
    Rig rig;
    const uint32_t settingsAt = rig.device.settingsScaleEpoch();
    const uint32_t liveAt = rig.device.liveScaleEpoch();
    rig.device.setSettingsScaleFound(0.0);
    CHECK(rig.device.settingsScaleEpoch() != settingsAt);
    CHECK(rig.device.liveScaleEpoch() == liveAt);
    rig.device.setLiveScaleFound(0.0);
    CHECK(rig.device.liveScaleEpoch() != liveAt);
    CHECK(rig.device.settingsScaleOpen() && rig.device.liveScaleOpen());
  }
  {  // A profile without a scale check.
    Rig rig(100.0, kMiniProfile);
    CHECK_HAS(rig.run("scalecheck"), "This profile has no voltage scale check.");
    CHECK_HASNT(rig.run("dump"), "Note: voltages are shown");
    CHECK_HASNT(rig.run("status"), "Voltage ");
    CHECK_HAS(rig.run("live"), "a ");
    CHECK_HAS(rig.run("wires"), "This profile has no wiring notes.");
  }
}

static void testSnapshot() {
  {  // Text in a snapshot is a JSON string whatever it holds. Names and labels come from the
     // profile, which may carry a quote, a backslash or a control character.
    static const char kAwkward[] = "5\" panel \\ C:\\x\ttab\nline\x01\x1F end";
    StringOut out;
    ovPrintJsonString(out, kAwkward);
    CHECK(out.text == "\"5\\\" panel \\\\ C:\\\\x\\u0009tab\\u000Aline\\u0001\\u001F end\"");
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, out.text);
    CHECK_MSG(!error, "not a JSON string: %s", error.c_str());
    CHECK(strcmp(doc.as<const char *>() != nullptr ? doc.as<const char *>() : "", kAwkward) == 0);
    StringOut plain;
    ovPrintJsonString(plain, "");
    CHECK(plain.text == "\"\"");
  }
  {
    Rig rig;
    const std::string out = rig.run("snapshot");
    const size_t begin = out.find("--- snapshot begins ---");
    const size_t end = out.find("--- snapshot ends ---");
    CHECK(begin != std::string::npos && end != std::string::npos && end > begin);
    const std::string json = out.substr(begin + strlen("--- snapshot begins ---"), end - begin - strlen("--- snapshot begins ---"));
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json);
    CHECK_MSG(!error, "the snapshot is not valid JSON: %s", error.c_str());
    CHECK(strcmp(doc["profile"] | "", "sunsaver-mppt") == 0);
    CHECK(strcmp(doc["optaview"] | "", OV_VERSION) == 0);
    CHECK_EQ(doc["link"]["slave"] | 0, 1);
    CHECK_EQ(doc["link"]["baud"] | 0, 9600);
    CHECK(strcmp(doc["link"]["format"] | "", "8N2") == 0);
    CHECK_EQ(doc["missing"] | -1, 0);
    JsonArrayConst registers = doc["registers"].as<JsonArrayConst>();
    CHECK_EQ(registers.size(), 94);
    size_t index = 0;
    for (JsonVariantConst item : registers) {
      const OvRegister &reg = rig.profile.reg(index++);
      uint32_t addr = 0;
      CHECK(ovParseU32(item["addr"] | "", &addr));
      CHECK_EQ(addr, reg.addr);
      CHECK(strcmp(item["name"] | "", reg.name) == 0);
      JsonArrayConst raw = item["raw"].as<JsonArrayConst>();
      CHECK_EQ(raw.size(), reg.words);
      for (uint8_t w = 0; w < reg.words; ++w) {
        CHECK_MSG((raw[w] | -1) == rig.sim.regs[(uint16_t)(reg.addr + w)], "%s word %u", reg.name, (unsigned)w);
      }
      CHECK((item["text"] | "")[0] != '\0');
    }
  }
  {  // With the trace on, its lines stay out of the JSON, and it carries on afterwards.
    Rig rig;
    rig.run("trace on");
    const std::string out = rig.run("snapshot");
    const size_t begin = out.find("--- snapshot begins ---") + strlen("--- snapshot begins ---");
    const size_t end = out.find("--- snapshot ends ---");
    const std::string json = out.substr(begin, end - begin);
    CHECK_HASNT(json, "> 01 04");
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json);
    CHECK_MSG(!error, "with the trace on the snapshot is not valid JSON: %s", error.c_str());
    CHECK_EQ(doc["registers"].as<JsonArrayConst>().size(), 94);
    CHECK_EQ(doc["missing"] | -1, 0);
    CHECK_HAS(out.substr(end), "(The trace was paused for the snapshot, so that it stays valid JSON.)");
    CHECK_HAS(rig.run("get adc_vb_f"), "> 01 04 00 08 00 01 B0 08");
    CHECK_HAS(rig.run("status"), "trace on");
    // Without the trace there is nothing to say about it.
    rig.run("trace off");
    CHECK_HASNT(rig.run("snapshot"), "trace");
    CHECK_HASNT(rig.run("get adc_vb_f"), "> 01 04");
  }
  {  // Registers that do not read are listed with the reason, and the JSON stays valid.
    Rig rig;
    rig.sim.makeOldFirmware();
    const std::string out = rig.run("snapshot");
    CHECK_HAS(out, "\"missing\": 5");
    CHECK_HAS(out, "{\"addr\": \"0x0039\", \"name\": \"va_ref_fixed\", \"error\": \"exception 2 (illegal data address)\"}");
    CHECK_HAS(out, "5 registers could not be read");
    const size_t begin = out.find('{');
    const size_t end = out.find("--- snapshot ends ---");
    JsonDocument doc;
    CHECK(!deserializeJson(doc, out.substr(begin, end - begin)));
    CHECK_EQ(doc["registers"].as<JsonArrayConst>().size(), 94);
  }
  {  // No link: still well-formed, and everything is counted as missing.
    Rig rig;
    rig.sim.powered = false;
    const std::string out = rig.run("snapshot");
    CHECK_HAS(out, "\"missing\": 94");
    CHECK_HAS(out, "\"error\": \"not read\"");
    const size_t begin = out.find('{');
    const size_t end = out.find("--- snapshot ends ---");
    JsonDocument doc;
    CHECK(!deserializeJson(doc, out.substr(begin, end - begin)));
    CHECK_HAS(out, "The controller is not answering");
  }
  {  // Stopped by a key: well-formed, with the rest counted as missing.
    Rig rig;
    rig.host.pressKeyAfter(100);
    const std::string out = rig.run("snapshot");
    const size_t begin = out.find('{');
    const size_t end = out.find("--- snapshot ends ---");
    JsonDocument doc;
    CHECK(!deserializeJson(doc, out.substr(begin, end - begin)));
    const long missing = doc["missing"] | -1;
    const size_t listed = doc["registers"].as<JsonArrayConst>().size();
    CHECK(missing > 0 && listed > 0);
    CHECK_EQ(missing + (long)listed, 94);
    CHECK_HAS(out, "Stopped.");
  }
}

static void testWatchSniffTrace() {
  {
    Rig rig;
    rig.host.pressKeyAfter(2600);
    const uint64_t t0 = rig.sim.elapsedMs();
    const std::string out = rig.run("watch 1");
    const uint64_t took = rig.sim.elapsedMs() - t0;
    CHECK_HAS(out, "Press a key to stop.");
    CHECK_EQ(countOf(out, " adc_vb_f "), 3);  // at 0, 1 and 2 seconds or so
    CHECK_HAS(out, "Stopped.");
    CHECK_MSG(took >= 2600 && took < 2700, "took %llu ms", (unsigned long long)took);
    CHECK_EQ(rig.host.discards, 1);
    CHECK_HAS(rig.run("watch 0"), "Usage: watch");
    CHECK_HAS(rig.run("watch soon"), "Usage: watch");
  }
  {  // 'watch' keeps going while the link is down: that is when it is most useful.
    Rig rig;
    rig.sim.powered = false;
    rig.host.pressKeyAfter(11000);
    const std::string out = rig.run("watch 1");
    CHECK(countOf(out, "(no answer; trying again)") >= 2);
    CHECK_HAS(out, "Stopped.");
  }
  {
    Rig rig;
    rig.sim.inject(40, bytes({0x01, 0x04, 0x00, 0x08, 0x00, 0x01, 0xB0, 0x08}));
    rig.sim.inject(120, bytes({0x55, 0xAA, 0x55}));
    const size_t before = rig.sim.requests.size();
    const std::string out = rig.run("sniff 300");
    CHECK_HAS(out, "Listening for 300 ms without sending. Press a key to stop.\r\n");
    CHECK_HAS(out, "01 04 00 08 00 01 B0 08  (a complete Modbus frame)");
    CHECK_HAS(out, "ms  55 AA 55\r\n");
    CHECK_HAS(out, "Heard 11 bytes.");
    CHECK_EQ(rig.sim.requests.size(), before);  // it never transmits
    CHECK_HAS(rig.run("sniff 200"), "Heard 0 bytes.");
    CHECK_HAS(rig.run("sniff 5"), "Usage: sniff");
    rig.sim.inject(10, std::vector<uint8_t>(150, 0x33));  // longer than one line: printed in pieces
    const std::string flood = rig.run("sniff 400");
    CHECK_HAS(flood, "Heard 150 bytes.");
    CHECK_EQ(countOf(flood, " ms  33 "), 3);
    rig.host.pressKeyAfter(50);
    const uint64_t t0 = rig.sim.elapsedMs();
    CHECK_HAS(rig.run("sniff 60000"), "Stopped.");
    CHECK(rig.sim.elapsedMs() - t0 < 200);
  }
  {
    Rig rig;
    CHECK_HAS(rig.run("trace on"), "Trace is on");
    const std::string out = rig.run("get adc_vb_f");
    CHECK_HAS(out, "    > 01 04 00 08 00 01 B0 08\r\n");
    CHECK_HAS(out, "    < 01 04 02 11 83 F4 C1  (ok, ");
    // 'raw' does not print the exchange twice while the trace is on.
    CHECK_EQ(countOf(rig.run("raw 1 4 8 1"), "> 01 04 00 08 00 01 B0 08"), 1);
    rig.sim.powered = false;
    CHECK_HAS(rig.run("get adc_vb_f"), "    < (nothing)  (no reply, 1000 ms)");
    rig.sim.powered = true;
    CHECK_HAS(rig.run("trace off"), "Trace is off.");
    CHECK_HASNT(rig.run("get adc_vb_f"), "> 01 04");
    CHECK_HAS(rig.run("trace maybe"), "Usage: trace");
  }
}

// ============================================================================
// Two front ends on one controller (the USB console and the web page's console)
// ============================================================================

// A second console on a rig's device, the way the web page has one.
struct OtherConsole {
  StringOut out;
  FakeHost host;
  OvConsole console;
  explicit OtherConsole(Rig &rig) : console(out, host, rig.device) { host.sim = &rig.sim; }
  std::string run(const std::string &line) {
    out.text.clear();
    console.handleLine(line.c_str());
    gTranscript += out.text;
    return out.text;
  }
};

struct PingExtra : OvConsoleExtra {
  unsigned calls = 0;
  bool command(int argc, char **argv, OvOut &out) override {
    if (!ovEqualsIgnoreCase(argv[0], "ping")) {
      return false;
    }
    ++calls;
    out.print("pong ");
    out.printU((uint32_t)argc);
    out.println();
    return true;
  }
  void help(OvOut &out) override {
    out.println("Extras");
    out.println("  ping                    answers pong");
  }
  void status(OvOut &out) override { out.println("Ping      ready"); }
};

struct PongExtra : OvConsoleExtra {
  unsigned calls = 0;
  bool command(int, char **argv, OvOut &out) override {
    if (!ovEqualsIgnoreCase(argv[0], "pong")) {
      return false;
    }
    ++calls;
    out.println("ping");
    return true;
  }
  void help(OvOut &out) override { out.println("  pong                    answers ping"); }
  void status(OvOut &out) override { out.println("Pong      ready"); }
};

static void testTwoConsoles() {
  static const char kStale[] = "Not sent: the controller or the link was changed from elsewhere after this was asked.";
  {
    // A "yes" is only good for what the question showed. Once the other front end has written,
    // restarted the controller, changed the link or changed a scale, nothing is sent on it.
    Rig rig;
    OtherConsole web(rig);
    rig.run("set EV_lvd 12.00");
    CHECK(rig.console.awaitingConfirmation());
    web.run("set EV_lvr 13.00");
    CHECK_HAS(web.run("yes"), "Written and read back: EV_lvr = 13.00 V");
    const size_t writes = rig.sent(0x06);
    const std::string late = rig.run("yes");
    CHECK_HAS(late, kStale);
    CHECK_HAS(late, "Give the command again to see how things are now.");
    CHECK_EQ(rig.sent(0x06), writes);
    CHECK_EQ(rig.sim.regs[0xE022], 0x0EB8);
    CHECK(!rig.console.awaitingConfirmation());
    // Asked again, it goes through.
    rig.run("set EV_lvd 12.00");
    CHECK_HAS(rig.run("yes"), "Written and read back: EV_lvd = 12.00 V");

    // The link: even when it is put back, the question was about what was there before.
    rig.run("set EV_lvd 11.50");
    web.run("slave 2");
    web.run("slave 1");
    CHECK_HAS(rig.run("yes"), kStale);
    CHECK_EQ(rig.sim.regs[0xE022], rig.sim.setVolts(12.00));

    // A scale: the value in the question was worked out with the old figure.
    rig.run("set EV_lvd 11.50");
    web.run("scale vset 96.667");
    CHECK_HAS(rig.run("yes"), kStale);
    web.run("scale vset 100");

    // A restart from the other side.
    rig.run("set EV_lvd 11.50");
    web.run("reset");
    CHECK_HAS(web.run("yes"), "The controller answers again");
    CHECK_HAS(rig.run("yes"), kStale);

    // A coil switched from the other side.
    rig.run("set EV_lvd 11.50");
    web.run("coil load_disconnect on");
    CHECK_HAS(web.run("yes"), "Sent and acknowledged.");
    CHECK_HAS(rig.run("yes"), kStale);

    // A raw request from the other side goes to the controller past everything OptaView keeps
    // track of. A raw write has changed the very register the question was about.
    web.run("expert on");
    rig.run("set EV_lvd 11.50");
    web.run("raw 1 6 0xE022 0x1000");
    CHECK_HAS(web.run("yes"), "Acknowledged.");
    CHECK_HAS(rig.run("yes"), kStale);
    CHECK_EQ(rig.sim.regs[0xE022], 0x1000);
    // And what a function that OptaView does not know has done, nobody can tell.
    rig.run("set EV_lvd 11.50");
    web.run("raw 1 0x2B 0E 01 00");
    CHECK_HAS(web.run("yes"), "4D 6F 72 6E 69 6E 67 73 74 61 72");
    CHECK_HAS(rig.run("yes"), kStale);
    CHECK_EQ(rig.sim.regs[0xE022], 0x1000);
    web.run("expert off");

    // It works both ways round.
    web.run("coil load_disconnect off");
    rig.run("set EV_lvd 11.50");
    CHECK_HAS(rig.run("yes"), "Written and read back");
    CHECK_HAS(web.run("yes"), kStale);
    CHECK(rig.sim.coils[0x0001]);

    // Reading changes nothing, so it voids nothing. Neither do the settings that only say how
    // to ask: the timeout, the read function, block reads.
    rig.run("set EV_lvd 12.10");
    web.run("live");
    web.run("dump load");
    web.run("status");
    web.run("timeout 800");
    web.run("fc 3");
    web.run("fc 4");
    web.run("blocks off");
    web.run("scalecheck");
    CHECK_HAS(rig.run("yes"), "Written and read back: EV_lvd = 12.10 V");

    // A question that the other side cancelled, or never answered, voids nothing either.
    rig.run("set EV_lvd 12.20");
    web.run("set EV_lvr 13.10");
    web.run("no");
    web.run("reset");
    CHECK(web.console.cancelPending());
    CHECK_HAS(rig.run("yes"), "Written and read back: EV_lvd = 12.20 V");
  }
  {
    // What 'scalecheck' finds belongs to the controller, so both consoles know it.
    Rig rig;
    OtherConsole web(rig);
    CHECK_HAS(web.run("set EV_lvd 12.00"), "Careful: the scale of stored settings");
    web.run("no");
    CHECK_HAS(rig.run("scalecheck 13.68"), "Verdict");
    CHECK(rig.device.settingsScaleConfirmed());
    CHECK(rig.device.liveScaleConfirmed());
    const std::string status = web.run("status");
    CHECK_HAS(lineWith(status, "stored settings"), "confirmed by 'scalecheck'");
    CHECK_HAS(lineWith(status, "live readings"), "confirmed by 'scalecheck'");
    CHECK_HASNT(web.run("set EV_lvd 12.00"), "Careful: the scale of stored settings");
    web.run("no");
    CHECK_HASNT(web.run("live"), "not confirmed on this controller");

    // Another slave address may be another controller: the findings go, for both.
    const std::string moved = web.run("slave 2");
    CHECK_HAS(moved, "what 'scalecheck'");
    CHECK_HAS(moved, "The voltage scales are unconfirmed again.");
    CHECK(!rig.device.settingsScaleConfirmed());
    CHECK(!rig.device.liveScaleConfirmed());
    CHECK_HAS(lineWith(rig.run("status"), "stored settings"), "not confirmed on this controller");
    // The console that did not change the link has nothing to announce: it is told when it asks.
    CHECK_HASNT(rig.run("slave 1"), "scalecheck");
  }
  {
    // The reminder about an unconfirmed scale is given once on each console, and again on both
    // when something has happened that calls for it.
    static const char kNote[] = "Note: voltages are shown with a scale that is not confirmed on this controller";
    Rig rig;
    OtherConsole web(rig);
    CHECK_HAS(rig.run("live"), kNote);
    CHECK_HASNT(rig.run("live"), kNote);
    CHECK_HAS(web.run("live"), kNote);
    CHECK_HASNT(web.run("live"), kNote);
    web.run("scale vlive 96.667");
    CHECK_HAS(rig.run("live"), kNote);
    CHECK_HAS(web.run("live"), kNote);
    CHECK_HASNT(rig.run("live"), kNote);
    // A check that finds the scale in use wrong brings it back as well.
    CHECK_HAS(rig.run("scalecheck 13.68"), "not the x96.667 that OptaView uses now");
    CHECK_HAS(web.run("live"), kNote);
    CHECK_HAS(rig.run("live"), kNote);
    // Switched to what was found, it is confirmed: nothing more to say on either.
    web.run("scale vlive 100");
    CHECK_HASNT(rig.run("live"), kNote);
    CHECK_HASNT(web.run("live"), kNote);
  }
  {
    // The stored settings have a count of their own, and it follows the same rule.
    static const char kNote[] = "Note: voltages are shown with a scale that is not confirmed on this controller";
    Rig rig;
    OtherConsole web(rig);
    CHECK_HAS(rig.run("dump load"), kNote);
    CHECK_HASNT(rig.run("dump load"), kNote);
    CHECK_HAS(web.run("get EV_lvd"), kNote);
    CHECK_HASNT(web.run("get EV_lvd"), kNote);
    web.run("scale vset 96.667");
    CHECK_HAS(rig.run("dump load"), "      (stored settings x96.667/32768). 'scalecheck' tests it.");
    CHECK_HAS(web.run("get EV_lvd"), kNote);
    CHECK_HASNT(rig.run("dump load"), kNote);
    // None of that used up the reminder about live readings, which nobody has been given yet.
    CHECK_HAS(rig.run("live"), "      (live readings x100/32768). 'scalecheck' tests it.");
  }
  {
    // Settings written from one side wait for a restart on both, until either side restarts.
    Rig rig;
    OtherConsole web(rig);
    static const char kWaiting[] = "Settings were changed in this session. 'reset' makes the controller use them.";
    CHECK_HASNT(rig.run("status"), kWaiting);
    CHECK(!rig.device.resetNeeded());
    web.run("set EV_lvr 13.00");
    web.run("yes");
    CHECK(rig.device.resetNeeded());
    CHECK_HAS(rig.run("status"), kWaiting);
    CHECK_HAS(web.run("status"), kWaiting);
    rig.run("reset");
    CHECK_HAS(rig.run("yes"), "The controller answers again");
    CHECK(!rig.device.resetNeeded());
    CHECK_HASNT(web.run("status"), kWaiting);
    CHECK_HASNT(rig.run("status"), kWaiting);

    // A write that did not take is nothing to restart for.
    rig.sim.refuseWrites = true;
    web.run("set EV_lvr 13.20");
    CHECK_HAS(web.run("yes"), "The controller refused the write");
    CHECK(!rig.device.resetNeeded());
    CHECK(!rig.device.resetMaybeNeeded());
    rig.sim.refuseWrites = false;
    // A write that got no answer at all is in doubt on both, until either side reads the
    // register again.
    static const char kMaybe[] = "A write got no answer in this session, so a setting may have been changed.";
    web.run("set EV_lvr 13.10");
    rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    CHECK_HAS(web.run("yes"), "whether it was written is not known.");
    CHECK_HAS(rig.run("status"), kMaybe);
    CHECK_HAS(web.run("status"), kMaybe);
    CHECK_HASNT(rig.run("status"), kWaiting);
    CHECK_HAS(rig.run("get EV_lvr"), "13.10 V");
    CHECK_HAS(web.run("status"), kWaiting);
    CHECK_HASNT(web.run("status"), kMaybe);
    rig.run("reset");
    CHECK_HAS(rig.run("yes"), "The controller answers again");
    CHECK(!rig.device.resetNeeded());
    // A restart that the controller did not come back from leaves the note standing.
    web.run("set EV_lvr 13.20");
    web.run("yes");
    CHECK(rig.device.resetNeeded());
    rig.sim.powered = false;
    rig.run("reset anyway");
    CHECK(!rig.console.awaitingConfirmation());  // the checks cannot be read, and that ends it
    rig.sim.powered = true;
    CHECK(rig.device.resetNeeded());
  }
  {
    // Every console counts for itself: an open question, expert mode and the trace of one are
    // not the other's.
    Rig rig;
    OtherConsole web(rig);
    rig.run("set EV_lvd 12.00");
    CHECK_HAS(web.run("yes"), "Nothing is waiting for a yes or a no.");
    CHECK(rig.console.awaitingConfirmation());
    rig.run("no");
    rig.run("expert on");
    CHECK_HAS(web.run("raw 1 6 0xE022 0x1000"), "Only functions 1 to 4 are open here.");
    CHECK_HAS(lineWith(web.run("status"), "Modes"), "expert off, trace off");
    CHECK_HAS(lineWith(rig.run("status"), "Modes"), "expert on, trace off");
  }
  {
    // newVisitor(): what the last visitor left behind is gone.
    Rig rig;
    OtherConsole web(rig);
    CHECK_HAS(web.run("set EV_lvd 12.00"), "Before you write:");
    web.run("no");
    web.run("expert on");
    web.run("trace on");
    CHECK(rig.master.trace == &web.console);
    CHECK(web.console.traceOn());
    CHECK_HAS(lineWith(web.run("status"), "Modes"), "expert on, trace on");
    CHECK_HASNT(web.run("set EV_lvd 12.00"), "Before you write:");
    CHECK(web.console.awaitingConfirmation());
    web.console.newVisitor();
    CHECK(!web.console.awaitingConfirmation());
    CHECK(!web.console.traceOn());
    CHECK(rig.master.trace == nullptr);
    CHECK_HAS(web.run("yes"), "Nothing is waiting for a yes or a no.");
    CHECK_HAS(web.run("raw 1 6 0xE022 0x1000"), "Only functions 1 to 4 are open here.");
    CHECK_HAS(web.run("set EV_lvd 12.00"), "Before you write:");  // the notes on writing, once more
    web.run("no");
    CHECK_HAS(web.run("live"), "not confirmed on this controller");
    web.console.newVisitor();
    CHECK_HAS(web.run("live"), "not confirmed on this controller");  // and the reminder
    CHECK_EQ(rig.sent(0x06), 0);

    // The trace of the other console is not this one's to switch off.
    rig.run("trace on");
    CHECK(rig.master.trace == &rig.console);
    web.console.newVisitor();
    CHECK(rig.master.trace == &rig.console);
    rig.run("trace off");

    // Everything that is said once is said once to each visitor: the reminder about each of the
    // two scales, and that block reads were given up on this link.
    Rig mrc;
    OtherConsole page(mrc);
    mrc.sim.blocksAnswered = false;
    const std::string first = page.run("dump");
    CHECK_HAS(first, "(live readings x100/32768, stored settings x100/32768). 'scalecheck' tests them.");
    CHECK_HAS(first, "Note: block reads do not work on this link");
    const std::string second = page.run("dump");
    CHECK_HASNT(second, "Note: voltages are shown");
    CHECK_HASNT(second, "Note: block reads do not work");
    page.console.newVisitor();
    const std::string third = page.run("dump");
    CHECK_HAS(third, "(live readings x100/32768, stored settings x100/32768). 'scalecheck' tests them.");
    CHECK_HAS(third, "Note: block reads do not work on this link");
    CHECK(mrc.device.blocksFailed());  // the device still knows; only the telling starts again
  }
  {
    // Commands from outside the console.
    Rig rig;
    PingExtra ping;
    CHECK_HAS(rig.run("ping"), "Unknown command 'ping'");
    CHECK(!rig.console.addExtra(nullptr));
    CHECK(rig.console.addExtra(&ping));
    CHECK(rig.run("ping a b") == "pong 3\r\n");
    CHECK(rig.run("PING") == "pong 1\r\n");
    CHECK_EQ(ping.calls, 2);
    CHECK_HAS(rig.run("pong"), "Unknown command 'pong'");
    const std::string help = rig.run("help");
    CHECK_HAS(help, "  ping                    answers pong");
    CHECK(help.find("  expert on|off") < help.find("Extras"));
    CHECK(help.find("Extras") < help.find("In the lists, W marks"));
    const std::string status = rig.run("status");
    CHECK_HAS(status, "Ping      ready");
    CHECK(status.find("Modes ") < status.find("Ping "));
    // A command of the console itself is never handed on.
    CHECK_HAS(rig.run("live"), "adc_vb_f");
    CHECK_EQ(ping.calls, 2);
    // While a question is open, an extra's command cancels like any other line.
    rig.run("set EV_lvd 12.00");
    CHECK_HAS(rig.run("ping"), "Cancelled. Nothing was sent.");
    CHECK_EQ(ping.calls, 2);
    // A second one, as in the sketch (the network and the web page): each is asked in the
    // order it was added until one takes the command, and both show in 'help' and 'status'.
    PongExtra pong;
    CHECK(rig.console.addExtra(&pong));
    CHECK(rig.run("pong") == "ping\r\n");
    CHECK(rig.run("ping") == "pong 1\r\n");
    CHECK_EQ(pong.calls, 1);
    CHECK_EQ(ping.calls, 3);
    const std::string helpBoth = rig.run("help");
    CHECK_HAS(helpBoth, "  ping                    answers pong");
    CHECK_HAS(helpBoth, "  pong                    answers ping");
    CHECK(helpBoth.find("answers pong") < helpBoth.find("answers ping"));
    const std::string statusBoth = rig.run("status");
    CHECK_HAS(statusBoth, "Ping      ready\r\nPong      ready\r\n");
    // There is room for four.
    PingExtra more[OV_MAX_EXTRAS];
    for (uint8_t i = 0; i + 2 < OV_MAX_EXTRAS; ++i) {
      CHECK(rig.console.addExtra(&more[i]));
    }
    CHECK(!rig.console.addExtra(&more[OV_MAX_EXTRAS - 1]));
  }
}

static void testTranscript() {
  // The console only ever prints plain ASCII, and ends every line with CR LF.
  CHECK(gTranscript.size() > 50000);
  size_t bad = 0;
  size_t longest = 0;
  size_t lineStart = 0;
  for (size_t i = 0; i < gTranscript.size(); ++i) {
    const unsigned char c = (unsigned char)gTranscript[i];
    if (c == '\n') {
      if (i == 0 || gTranscript[i - 1] != '\r') {
        ++bad;
      }
      // Hex dumps and bit lists may run long; ordinary text stays within a terminal's width.
      const std::string line = gTranscript.substr(lineStart, i - lineStart);
      if (line.size() > longest && !has(line, " < ") && !has(line, " > ") && !has(line, " ms  ") &&
          !has(line, "\"") && !has(line, "[0x")) {
        longest = line.size();
      }
      lineStart = i + 1;
    } else if (c == '\r') {
      if (i + 1 >= gTranscript.size() || gTranscript[i + 1] != '\n') {
        ++bad;
      }
    } else if (c < 0x20 || c > 0x7E) {
      ++bad;
    }
  }
  CHECK_EQ(bad, 0);
  CHECK_MSG(longest <= 106, "the longest plain line is %zu characters", longest);

  // OPTAVIEW_TRANSCRIPT=<file> keeps everything the console printed, to read what a session
  // looks like without a controller on the bench.
  const char *path = getenv("OPTAVIEW_TRANSCRIPT");
  if (path != nullptr && path[0] != '\0') {
    FILE *file = fopen(path, "wb");
    if (file != nullptr) {
      fwrite(gTranscript.data(), 1, gTranscript.size(), file);
      fclose(file);
    }
  }
}

int main() {
  startTestTimeLimit();
  testCrcAndFrames();
  testReplyValidation();
  testTimeouts();
  testLateReplies();
  testBitsDeviceIdAndRaw();
  testText();
  testExactDecimals();
  testBuiltinProfile();
  testProfileCodec();
  testProfileRejects();
  testProfileAllocationFailures();
  testSpans();
  testReadsAndFallback();
  testProbe();
  testLineEditor();
  testConsoleBasics();
  testConsoleReads();
  testConsoleSet();
  testConsoleCoilsAndReset();
  testConsoleRawAndExpert();
  testScaleCheck();
  testSnapshot();
  testWatchSniffTrace();
  testTwoConsoles();
  testTranscript();
  CHECK_EQ(gAllocLive, 0);

  if (gFailures != 0) {
    fprintf(stderr, "optaview: %ld of %ld checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("optaview: all %ld checks passed\n", gChecks);
  return 0;
}
