// A simulated SunSaver MPPT behind an MRC-1, for the OptaView host tests.
//
// It is the serial link (optaview::MbTransport) and the controller in one object: frames the
// master sends are answered the way the Morningstar specification describes, byte by byte and
// on a fake clock that only moves when the master waits. Each test can change how the controller
// behaves (no block reads, an older firmware, a slow reply) and can inject faults into the next
// replies (noise in front, a cut-off frame, a bad CRC, another slave's answer).
//
// The memory map here is written out by hand from the specification, not derived from
// profiles/sunsaver-mppt.json, so a wrong address in the profile shows up as an exception.

#ifndef OPTAVIEW_SIM_SUNSAVER_H
#define OPTAVIEW_SIM_SUNSAVER_H

#include <stdint.h>
#include <string.h>

#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "OptaViewModbus.h"

class SimSunSaver : public optaview::MbTransport {
 public:
  enum Fault {
    FAULT_NONE,
    FAULT_NO_REPLY,        // the request is lost
    FAULT_LOST_REPLY,      // the request is carried out, but its reply is lost
    FAULT_BAD_CRC,         // one bit flipped in the reply
    FAULT_TRUNCATE,        // the reply stops halfway
    FAULT_WRONG_SLAVE,     // a valid frame, from another slave address
    FAULT_WRONG_FUNCTION,  // a valid frame, for another function
    FAULT_ONE_REGISTER,    // a valid read reply holding a single register, whatever was asked
    FAULT_EXTRA_REGISTER,  // a valid read reply holding one register too many
    FAULT_WRONG_ECHO,      // a valid write acknowledgement for another value
    FAULT_GARBAGE          // bytes that are no frame at all
  };

  // --- the controller ---
  uint8_t slaveId = 1;
  uint32_t deviceBaud = 9600;
  uint8_t deviceFormat = optaview::MB_8N2;
  bool anyFormat = false;            // answers whatever the stop bits and parity
  bool readInput = true;             // function 04
  bool readHolding = true;           // function 03
  bool blocksAnswered = true;        // false: a read of more than one register gets no reply
  bool blocksRefused = false;        // true: a read of more than one register gets exception 3
  bool deviceIdSupported = true;
  bool resetAcknowledged = true;     // false: the controller restarts without answering
  bool resetWorks = true;            // false: the reset coil is acknowledged but nothing restarts
  uint32_t responseDelayMs = 20;
  // The controller answers one request at a time, in the order they came, as a Modbus RTU slave
  // does: a reply starts only once the one before it is out, however much quicker it is. False:
  // each reply comes at its own time, so a quick one overtakes a slow one, as a reply from
  // another device on the bus could.
  bool answersInOrder = true;
  uint32_t resetDownMs = 1500;       // silent for this long after a reset
  uint8_t leadingNoiseBytes = 0;     // 0x00 bytes in front of every reply (the DE-flip artifact)
  bool powered = true;               // false: nothing answers
  uint32_t portMaxBaud = 0;          // the serial port refuses a higher baud rate (0 = no limit)
  // How the controller encodes voltages: n = V * 32768 / numerator. Three figures, because the
  // specification gives 100 for everything but the regulation target (96.667), and a real
  // controller may differ from it in any of the three.
  double voltsNumerator = 100.0;       // the voltages it measures
  double settingsNumerator = 100.0;    // the voltage settings in its EEPROM
  double referenceNumerator = 96.667;  // its regulation target, Vb_ref
  uint16_t storeMask = 0xFFFF;       // FC06 stores (value & storeMask): a controller that alters what it is given
  uint16_t keepBits = 0;             // FC06 leaves these bits of the register as they were
  bool refuseWrites = false;         // FC06 gets exception 4

  // --- the log ---
  std::vector<std::vector<uint8_t> > requests;  // every frame the master sent
  std::vector<uint64_t> requestTimes;           // and when (elapsedMs)
  unsigned resets = 0;
  unsigned configures = 0;
  std::vector<uint16_t> triggered;              // action coils that were switched on

  // --- memory ---
  std::map<uint16_t, uint16_t> regs;
  std::map<uint16_t, bool> coils;

  // settingsNum <= 0 means "the same as the measured voltages".
  explicit SimSunSaver(double voltsNum = 100.0, double settingsNum = 0.0, double referenceNum = 96.667)
      : voltsNumerator(voltsNum),
        settingsNumerator(settingsNum > 0.0 ? settingsNum : voltsNum),
        referenceNumerator(referenceNum) {
    loadDefaults();
  }

  uint16_t volts(double v) const { return (uint16_t)(v * 32768.0 / voltsNumerator + 0.5); }
  uint16_t setVolts(double v) const { return (uint16_t)(v * 32768.0 / settingsNumerator + 0.5); }
  uint16_t refVolts(double v) const { return (uint16_t)(v * 32768.0 / referenceNumerator + 0.5); }
  static uint16_t amps(double a) { return (uint16_t)(int16_t)(a * 32768.0 / 79.16 + (a < 0 ? -0.5 : 0.5)); }

  void setU32HiLo(uint16_t addr, uint32_t value) {
    regs[addr] = (uint16_t)(value >> 16);
    regs[(uint16_t)(addr + 1)] = (uint16_t)(value & 0xFFFF);
  }
  void setU32LoHi(uint16_t addr, uint32_t value) {
    regs[addr] = (uint16_t)(value & 0xFFFF);
    regs[(uint16_t)(addr + 1)] = (uint16_t)(value >> 16);
  }

  // A battery in float on a sunny bench, modelled on the 2026-07-03 capture. That capture holds
  // 0x0008-0x000C only; the float state and everything else here are this simulator's own.
  void loadDefaults() {
    regs.clear();
    coils.clear();
    // The addresses that exist, as the specification lists them. They are written out here and
    // not taken from the profile, so that a register the profile puts at a reserved address
    // (0xE00B, say) is refused by this controller as it would be by a real one, and fails its
    // test. Everything in between is reserved and answers exception 2.
    static const uint16_t kExisting[][2] = {
        {0x0008, 0x0034}, {0x0038, 0x003A},  // RAM: measurements, status, daily values
        {0xE000, 0xE00A},                    // EEPROM: charge settings, battery bank 1
        {0xE00D, 0xE017},                    //         a second set of the same settings
        {0xE01A, 0xE01F},                    //         temperature compensation, HVD/HVR, limits
        {0xE022, 0xE027},                    //         load: LVD/LVR, LHVD/LHVR, compensation, warning
        {0xE030, 0xE038},                    //         LED thresholds, MODBUS and meter ids, current limit
        {0xE040, 0xE04D},                    //         the stored totals
        {0xE04F, 0xE04F}};
    for (size_t i = 0; i < sizeof(kExisting) / sizeof(kExisting[0]); ++i) {
      for (uint32_t a = kExisting[i][0]; a <= kExisting[i][1]; ++a) regs[(uint16_t)a] = 0;
    }

    regs[0x0008] = volts(13.68);   // adc_vb_f
    regs[0x0009] = volts(17.45);   // adc_va_f
    regs[0x000A] = volts(13.65);   // adc_vl_f
    regs[0x000B] = amps(0.20);     // adc_ic_f
    regs[0x000C] = amps(0.14);     // adc_il_f
    regs[0x000D] = 31;             // T_hs
    regs[0x000E] = 25;             // T_batt
    regs[0x000F] = 25;             // T_amb
    regs[0x0010] = 0x0080;         // T_rts: not connected
    regs[0x0011] = 7;              // charge_state: FLOAT
    regs[0x0012] = 0;              // array_fault
    regs[0x0013] = volts(13.70);   // Vb_f
    regs[0x0014] = refVolts(13.70);  // Vb_ref
    setU32HiLo(0x0015, 1234);      // Ahc_r   123.4 Ah
    setU32HiLo(0x0017, 70000);     // Ahc_t   7000.0 Ah (needs both words)
    regs[0x0019] = 85;             // kWhc    8.5 kWh
    regs[0x001A] = 1;              // load_state: LOAD_ON
    regs[0x001B] = 0;              // load_fault
    regs[0x001C] = volts(11.50);   // V_lvd
    setU32HiLo(0x001D, 456);       // Ahl_r
    setU32HiLo(0x001F, 65536 + 9); // Ahl_t
    setU32HiLo(0x0021, 4321);      // hourmeter
    setU32HiLo(0x0023, 0);         // alarm
    regs[0x0025] = 0x000A;         // dip_switch: SW2 and SW4
    regs[0x0026] = 4;              // led_state
    regs[0x0027] = 181;            // Power_out
    regs[0x0028] = volts(17.20);   // Sweep_Vmp
    regs[0x0029] = 1325;           // Sweep_Pmax
    regs[0x002A] = volts(21.30);   // Sweep_Voc
    regs[0x002B] = volts(12.60);   // Vb_min_daily
    regs[0x002C] = volts(14.10);   // Vb_max_daily
    regs[0x002D] = 52;             // Ahc_daily
    regs[0x002E] = 31;             // Ahl_daily
    regs[0x0033] = volts(11.90);   // vb_min
    regs[0x0034] = volts(14.90);   // vb_max

    regs[0xE000] = setVolts(14.00);   // EV_reg
    regs[0xE001] = setVolts(13.70);   // EV_float
    regs[0xE002] = 9000;           // Et_float
    regs[0xE003] = 10800;          // Et_floatlb
    regs[0xE004] = setVolts(12.30);   // EV_floatlb_trip
    regs[0xE005] = setVolts(11.50);   // EV_float_cancel
    regs[0xE006] = 1800;           // Et_float_exit_cum
    regs[0xE007] = 0;              // EV_eq: no equalize in bank 1
    regs[0xE008] = 0;              // Et_eqcalendar
    regs[0xE009] = 10800;          // Et_eq_above
    regs[0xE00A] = 7200;           // Et_eq_reg
    regs[0xE00D] = setVolts(14.40);   // EV_reg2
    regs[0xE00E] = setVolts(13.70);   // EV_float2
    regs[0xE00F] = 9000;
    regs[0xE010] = 10800;
    regs[0xE011] = setVolts(12.30);
    regs[0xE012] = setVolts(11.50);
    regs[0xE013] = 1800;
    regs[0xE014] = setVolts(14.90);   // EV_eq2
    regs[0xE015] = 28;
    regs[0xE016] = 10800;
    regs[0xE017] = 7200;
    regs[0xE01A] = 20;             // EV_tempcomp: 0.0305 V/C
    regs[0xE01B] = setVolts(15.20);   // EV_hvd
    regs[0xE01C] = setVolts(14.00);   // EV_hvr
    regs[0xE01D] = setVolts(15.00);   // Evb_ref_lim
    regs[0xE01E] = 80;             // ETb_max
    regs[0xE01F] = 0x00E2;         // ETb_min: -30
    regs[0xE022] = setVolts(11.50);   // EV_lvd
    regs[0xE023] = setVolts(12.60);   // EV_lvr
    regs[0xE024] = 0;              // EV_lhvd: off
    regs[0xE025] = 0;              // EV_lhvr
    regs[0xE026] = 0;              // ER_icomp
    regs[0xE027] = 600;            // Et_lvd_warn: 60.0 s
    regs[0xE030] = setVolts(13.10);   // EV_soc_y2g
    regs[0xE031] = setVolts(12.80);   // EV_soc_g2y
    regs[0xE032] = setVolts(11.80);   // EV_soc_y2r0
    regs[0xE033] = setVolts(12.40);   // EV_soc_r2y
    regs[0xE034] = 1;              // Emodbus_id
    regs[0xE035] = 0x15;           // Emeter_id
    regs[0xE038] = amps(15.0);     // Eic_lim
    setU32LoHi(0xE040, 4321);      // Ehourmeter
    setU32LoHi(0xE042, 456);
    setU32LoHi(0xE044, 65536 + 9);
    setU32LoHi(0xE046, 1234);
    setU32LoHi(0xE048, 70000);     // EAhc_t: low word first
    regs[0xE04A] = 85;
    regs[0xE04B] = volts(10.90);   // EVb_min: a measurement kept in EEPROM
    regs[0xE04C] = volts(14.95);   // EVb_max
    regs[0xE04D] = volts(22.10);   // EVa_max
    regs[0xE04F] = 12;

    const uint16_t kCoils[] = {0x0000, 0x0001, 0x0002, 0x0010, 0x0011, 0x0012, 0x0016, 0x00FF};
    for (size_t i = 0; i < sizeof(kCoils) / sizeof(kCoils[0]); ++i) coils[kCoils[i]] = false;
  }

  // Controller firmware before v08: registers 0x0038 to 0x003A do not exist.
  void makeOldFirmware() {
    regs.erase(0x0038);
    regs.erase(0x0039);
    regs.erase(0x003A);
    regs.erase(0xE036);
    regs.erase(0xE037);
  }

  // --- faults for the next replies, one per request ---
  void failNext(Fault fault, unsigned times = 1) {
    for (unsigned i = 0; i < times; ++i) faults_.push_back(fault);
  }

  // Bytes that appear on the bus by themselves, as from another device: the first can be read
  // `afterMs` from now, and the others follow one every millisecond, or back to back where one
  // character takes longer than that (1.15 ms at 9600 baud 8N2). Where they are on the wire at
  // the same time as a reply, or as other injected bytes, both are spoiled (see putOnBus).
  void inject(uint32_t afterMs, const std::vector<uint8_t> &bytes) {
    const uint64_t charUs = charMicros();
    const uint64_t stepUs = charUs > 1000 ? charUs : 1000;
    for (size_t i = 0; i < bytes.size(); ++i) {
      const uint64_t dueUs = (now_ + afterMs) * 1000 + (uint64_t)i * stepUs;
      putOnBus(dueUs > charUs ? dueUs - charUs : 0, dueUs, bytes[i]);
    }
  }

  // The battery is at `v` volts: both of its filtered readings say so, and the lowest and highest
  // voltage of the day and since the last reset take it in.
  void setBattery(double v) {
    const uint16_t raw = volts(v);
    regs[0x0008] = raw;  // adc_vb_f
    regs[0x0013] = raw;  // Vb_f: the slow filter, settled
    const uint16_t lows[] = {0x002B, 0x0033};   // Vb_min_daily, vb_min
    const uint16_t highs[] = {0x002C, 0x0034};  // Vb_max_daily, vb_max
    for (size_t i = 0; i < 2; ++i) {
      if (raw < regs[lows[i]]) regs[lows[i]] = raw;
      if (raw > regs[highs[i]]) regs[highs[i]] = raw;
    }
  }

  uint64_t elapsedMs() const { return now_; }
  size_t incomingCount() const { return incoming_.size(); }  // bytes not read yet, due or on their way
  void advance(uint32_t ms) { now_ += ms; }

  static std::vector<uint8_t> frame(std::vector<uint8_t> body) {
    const uint16_t crc = optaview::mbCrc16(body.data(), body.size());
    body.push_back((uint8_t)(crc & 0xFF));
    body.push_back((uint8_t)(crc >> 8));
    return body;
  }

  // --- optaview::MbTransport ---
  bool configure(uint32_t baud, uint8_t format) override {
    ++configures;
    if (format > optaview::MB_8O1 || baud < 300) return false;
    if (portMaxBaud != 0 && baud > portMaxBaud) return false;
    linkBaud_ = baud;
    linkFormat_ = format;
    incoming_.clear();
    now_ += 40;  // RS485.end(), begin() and the settling delays
    return true;
  }

  void send(const uint8_t *data, size_t len) override {
    requests.push_back(std::vector<uint8_t>(data, data + len));
    requestTimes.push_back(now_);
    now_ += wireMs(len);  // the request goes out at the port's baud rate
    if (!powered || now_ < downUntil_) return;
    if (linkBaud_ != deviceBaud) return;
    if (!anyFormat && linkFormat_ != deviceFormat) return;
    if (len < 4 || !optaview::mbCrcOk(data, len)) return;
    if (data[0] != slaveId) return;

    Fault fault = FAULT_NONE;
    if (!faults_.empty()) {
      fault = faults_.front();
      faults_.pop_front();
    }
    if (fault == FAULT_NO_REPLY) return;

    std::vector<uint8_t> reply;
    const bool answer = respond(data, len, &reply);
    if (!answer || fault == FAULT_LOST_REPLY) return;

    switch (fault) {
      case FAULT_BAD_CRC:
        reply[reply.size() / 2] ^= 0x10;
        break;
      case FAULT_TRUNCATE:
        reply.resize(reply.size() / 2);
        break;
      case FAULT_WRONG_SLAVE:
        reply[0] = (uint8_t)(slaveId + 1);
        reply = frame(std::vector<uint8_t>(reply.begin(), reply.end() - 2));
        break;
      case FAULT_WRONG_FUNCTION:
        reply[1] = (uint8_t)(reply[1] == 0x03 ? 0x04 : 0x03);
        reply = frame(std::vector<uint8_t>(reply.begin(), reply.end() - 2));
        break;
      case FAULT_ONE_REGISTER: {
        std::vector<uint8_t> body;
        body.push_back(data[0]);
        body.push_back(data[1]);
        body.push_back(2);
        body.push_back(0x12);
        body.push_back(0x34);
        reply = frame(body);
        break;
      }
      case FAULT_EXTRA_REGISTER: {
        std::vector<uint8_t> body(reply.begin(), reply.end() - 2);
        body[2] = (uint8_t)(body[2] + 2);
        body.push_back(0xAB);
        body.push_back(0xCD);
        reply = frame(body);
        break;
      }
      case FAULT_WRONG_ECHO:
        // Only the eight-byte acknowledgement of a write (function 5 or 6) echoes a value. A
        // refusal is five bytes long and goes out as it is.
        if (reply.size() == 8 && (reply[1] == 0x05 || reply[1] == 0x06)) {
          reply[5] ^= 0x01;
          reply = frame(std::vector<uint8_t>(reply.begin(), reply.end() - 2));
        }
        break;
      case FAULT_GARBAGE:
        reply.assign(6, 0x5A);
        break;
      default:
        break;
    }

    // The reply starts responseDelayMs after the request, or once the reply before it is out
    // (see answersInOrder). Its bytes go out back to back, one character time each at the baud
    // rate and format of the link (1.15 ms at 9600 baud 8N2, 96 microseconds at 115200, 36.7 ms
    // at 300), and each can be read once its last bit is in.
    const uint64_t charUs = charMicros();
    uint64_t atUs = (now_ + responseDelayMs) * 1000;
    if (answersInOrder && busyUntilUs_ > atUs) atUs = busyUntilUs_;
    std::vector<uint8_t> wire((size_t)leadingNoiseBytes, (uint8_t)0x00);
    wire.insert(wire.end(), reply.begin(), reply.end());
    for (size_t i = 0; i < wire.size(); ++i) {
      putOnBus(atUs + (uint64_t)i * charUs, atUs + (uint64_t)(i + 1) * charUs, wire[i]);
    }
    const uint64_t endUs = atUs + (uint64_t)wire.size() * charUs;
    if (endUs > busyUntilUs_) busyUntilUs_ = endUs;
  }

  // Puts one byte on the bus: it is on the wire from startUs, and can be read from dueUs, when
  // its last bit is in. The bus keeps the bytes in the order they are due, each behind those due
  // no later. Two transmitters on one RS-485 pair at once garble each other, so a byte that
  // shares any time on the wire with another is spoiled, and so is the other. One sender's own
  // bytes follow one another and never share it. The master's requests are not on this wire:
  // the real port does not listen while it sends (see OptaView.ino), and the echo that some
  // adapters give back is the request itself, heard as it goes out.
  void putOnBus(uint64_t startUs, uint64_t dueUs, uint8_t value) {
    OnWire byte;
    byte.startUs = startUs;
    byte.dueUs = dueUs;
    byte.value = value;
    byte.garbled = false;
    std::deque<OnWire>::iterator at = incoming_.end();
    while (at != incoming_.begin() && (at - 1)->dueUs > dueUs) --at;
    // In front of it: the bytes that are not yet in when it starts.
    for (std::deque<OnWire>::iterator it = at; it != incoming_.begin() && (it - 1)->dueUs > startUs; --it) {
      (it - 1)->garbled = true;
      byte.garbled = true;
    }
    // Behind it: the bytes that start before it is in.
    for (std::deque<OnWire>::iterator it = at; it != incoming_.end(); ++it) {
      if (it->startUs < dueUs) {
        it->garbled = true;
        byte.garbled = true;
      }
    }
    incoming_.insert(at, byte);
  }

  // One character on the wire at the link's baud rate and format, in microseconds.
  uint64_t charMicros() const {
    return optaview::mbCharMicros(linkBaud_ == 0 ? 9600 : linkBaud_, linkFormat_);
  }

  // How long `bytes` characters take on the wire, rounded up to the millisecond clock.
  uint64_t wireMs(size_t bytes) const { return ((uint64_t)bytes * charMicros() + 999) / 1000; }

  // A spoiled byte arrives with its bits inverted: whatever the receiver makes of two senders at
  // once, it is not what either of them sent.
  int read() override {
    if (incoming_.empty() || incoming_.front().dueUs > now_ * 1000) return -1;
    const OnWire byte = incoming_.front();
    incoming_.pop_front();
    return byte.garbled ? (uint8_t)~byte.value : byte.value;
  }

  // The clock starts just below the 32-bit wrap, so every test crosses it.
  uint32_t nowMs() override { return (uint32_t)(0xFFFFFF00ULL + now_); }

  // Something that never returns keeps waiting on this clock. No test needs a simulated day, so
  // the clock refuses to go further: the test fails with a message (Rig::run catches this)
  // instead of holding CI until its own time limit.
  void idle() override {
    if (now_ >= kRunawayMs) {
      throw std::runtime_error("a simulated day has passed and it has not returned");
    }
    now_ += 1;
  }

 private:
  static const uint64_t kRunawayMs = 24ULL * 3600ULL * 1000ULL;
  uint64_t now_ = 0;
  uint64_t downUntil_ = 0;
  uint32_t linkBaud_ = 0;
  uint8_t linkFormat_ = 0;
  std::deque<Fault> faults_;
  struct OnWire {
    uint64_t startUs;  // its start bit goes out (microseconds on the clock of elapsedMs)
    uint64_t dueUs;    // its last bit is in: from then it can be read
    uint8_t value;     // what its sender sent
    bool garbled;      // another byte was on the wire at the same time
  };
  std::deque<OnWire> incoming_;  // in the order they are due
  uint64_t busyUntilUs_ = 0;     // when the last byte of the controller's last reply is in

  static std::vector<uint8_t> exception(const uint8_t *request, uint8_t code) {
    std::vector<uint8_t> body;
    body.push_back(request[0]);
    body.push_back((uint8_t)(request[1] | 0x80));
    body.push_back(code);
    return frame(body);
  }

  uint32_t u32HiLo(uint16_t addr) { return ((uint32_t)regs[addr] << 16) | regs[(uint16_t)(addr + 1)]; }

  // What an action coil does, as the specification names it. The stored totals at 0xE040 on
  // change only when the running totals are stored (force_eeprom_update): when a real
  // controller stores them by itself is not known, and this one never does.
  void act(uint16_t coil) {
    if (coil == 0x0010) {         // clear_ah_resettable
      setU32HiLo(0x0015, 0);      // Ahc_r
      setU32HiLo(0x001D, 0);      // Ahl_r
    } else if (coil == 0x0011) {  // clear_ah_total
      setU32HiLo(0x0017, 0);      // Ahc_t
      setU32HiLo(0x001F, 0);      // Ahl_t
    } else if (coil == 0x0012) {  // clear_kwh
      regs[0x0019] = 0;           // kWhc
    } else if (coil == 0x0016) {  // force_eeprom_update: the running totals, low word first
      setU32LoHi(0xE040, u32HiLo(0x0021));  // Ehourmeter
      setU32LoHi(0xE042, u32HiLo(0x001D));  // EAhl_r
      setU32LoHi(0xE044, u32HiLo(0x001F));  // EAhl_t
      setU32LoHi(0xE046, u32HiLo(0x0015));  // EAhc_r
      setU32LoHi(0xE048, u32HiLo(0x0017));  // EAhc_t
      regs[0xE04A] = regs[0x0019];          // EkWhc
    }
  }

  void restart() {
    ++resets;
    regs[0x0012] = (uint16_t)(regs[0x0012] & ~(1u << 5));  // array_fault: EEPROM setting edit
    regs[0x001B] = (uint16_t)(regs[0x001B] & ~(1u << 6));  // load_fault: EEPROM setting edit
    // The LVD in effect is taken from the EEPROM setting at start-up, in measurement units.
    regs[0x001C] = volts((double)regs[0xE022] * settingsNumerator / 32768.0);
    downUntil_ = now_ + resetDownMs;
  }

  // Builds the reply to a request addressed to this controller. False = it stays silent.
  bool respond(const uint8_t *request, size_t len, std::vector<uint8_t> *reply) {
    const uint8_t function = request[1];
    // Only the 8-byte requests carry an address and a value. A shorter frame (function 0x2B,
    // or a function with no data at all) must not be read past its end.
    const bool hasFields = len == 8;
    const uint16_t addr = hasFields ? (uint16_t)((request[2] << 8) | request[3]) : 0;
    const uint16_t value = hasFields ? (uint16_t)((request[4] << 8) | request[5]) : 0;

    if (function == 0x03 || function == 0x04) {
      if (len != 8) return false;
      if ((function == 0x03 && !readHolding) || (function == 0x04 && !readInput)) {
        *reply = exception(request, 1);
        return true;
      }
      if (value < 1 || value > 125) {
        *reply = exception(request, 3);
        return true;
      }
      if (value > 1 && !blocksAnswered) return false;
      if (value > 1 && blocksRefused) {
        *reply = exception(request, 3);
        return true;
      }
      std::vector<uint8_t> body;
      body.push_back(request[0]);
      body.push_back(function);
      body.push_back((uint8_t)(value * 2));
      for (uint16_t i = 0; i < value; ++i) {
        const std::map<uint16_t, uint16_t>::const_iterator it = regs.find((uint16_t)(addr + i));
        if (it == regs.end()) {
          *reply = exception(request, 2);
          return true;
        }
        body.push_back((uint8_t)(it->second >> 8));
        body.push_back((uint8_t)(it->second & 0xFF));
      }
      *reply = frame(body);
      return true;
    }

    if (function == 0x01 || function == 0x02) {
      if (len != 8) return false;
      if (value < 1 || value > 2000) {
        *reply = exception(request, 3);
        return true;
      }
      std::vector<uint8_t> body;
      body.push_back(request[0]);
      body.push_back(function);
      body.push_back((uint8_t)((value + 7) / 8));
      body.resize(3 + (value + 7) / 8, 0);
      for (uint16_t i = 0; i < value; ++i) {
        const std::map<uint16_t, bool>::const_iterator it = coils.find((uint16_t)(addr + i));
        if (it == coils.end()) {
          *reply = exception(request, 2);
          return true;
        }
        if (it->second) body[3 + i / 8] = (uint8_t)(body[3 + i / 8] | (1u << (i % 8)));
      }
      *reply = frame(body);
      return true;
    }

    if (function == 0x05) {
      if (len != 8) return false;
      if (coils.find(addr) == coils.end()) {
        *reply = exception(request, 2);
        return true;
      }
      if (value != 0xFF00 && value != 0x0000) {
        *reply = exception(request, 3);
        return true;
      }
      reply->assign(request, request + len);  // the acknowledgement repeats the request
      if (addr == 0x00FF) {
        if (value == 0xFF00 && resetWorks) {
          restart();
          return resetAcknowledged;
        }
        return true;
      }
      if (addr >= 0x0010) {  // action coils do their thing and read back off
        if (value == 0xFF00) {
          triggered.push_back(addr);
          act(addr);
        }
        return true;
      }
      coils[addr] = value == 0xFF00;
      return true;
    }

    if (function == 0x06) {
      if (len != 8) return false;
      if (addr < 0xE000 || regs.find(addr) == regs.end()) {
        *reply = exception(request, 2);
        return true;
      }
      if (refuseWrites) {
        *reply = exception(request, 4);
        return true;
      }
      regs[addr] = (uint16_t)((regs[addr] & keepBits) | (value & storeMask & ~keepBits));
      regs[0x0012] = (uint16_t)(regs[0x0012] | (1u << 5));  // "EEPROM setting edit" until a reset
      regs[0x001B] = (uint16_t)(regs[0x001B] | (1u << 6));
      reply->assign(request, request + len);
      return true;
    }

    if (function == 0x2B && len == 7 && request[2] == 0x0E && deviceIdSupported) {
      const char *const kObjects[] = {"Morningstar Corp.", "SS-MPPT", "v01.01.01"};
      std::vector<uint8_t> body;
      body.push_back(request[0]);
      body.push_back(0x2B);
      body.push_back(0x0E);
      body.push_back(request[3]);  // read code
      body.push_back(0x01);        // conformity level
      body.push_back(0x00);        // more follows
      body.push_back(0x00);        // next object
      body.push_back(3);           // objects
      for (uint8_t i = 0; i < 3; ++i) {
        body.push_back(i);
        body.push_back((uint8_t)strlen(kObjects[i]));
        for (const char *p = kObjects[i]; *p != '\0'; ++p) body.push_back((uint8_t)*p);
      }
      *reply = frame(body);
      return true;
    }

    *reply = exception(request, 1);  // function 16 included: the SunSaver does not have it
    return true;
  }
};

#endif  // OPTAVIEW_SIM_SUNSAVER_H
