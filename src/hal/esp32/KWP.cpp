#include "KWP.h"
#include <Arduino.h>
#include <HardwareSerial.h>

#define DEBUG_LEVEL 1

// The ported code prints per-byte/per-bit debug throughout the hot read path.
// At 115200 baud that blocking I/O roughly triples measuring-block latency, so
// route this file's bare `Serial` (NOT Serial2, the K-line) to a null sink. The
// useful connect trace is kept via KWP::dbg -> /kwpdbg.
struct KwpNullSerial {
  template <class... A> void print(A...)   {}
  template <class... A> void println(A...) {}
  template <class... A> void write(A...)   {}
};
static KwpNullSerial kwpNull;
#define Serial kwpNull

//HardwareSerial Serial2(2);

KWP::KWP(uint8_t receivePin, uint8_t transmitPin){
  _OBD_RX_PIN = receivePin;
  _OBD_TX_PIN = transmitPin;

  pinMode(transmitPin, OUTPUT);
  digitalWrite(transmitPin, HIGH);

  //obd = new NewSoftwareSerial(receivePin, transmitPin, false); // RX, TX, inverse logic

  #ifdef DEBUG_LEVEL
    Serial.println(F("KWP created"));
  #endif
}

KWP::~KWP(){
  Serial2.end(); 
  //obd = NULL;
}

bool KWP::connect(uint8_t addr, int baudrate) {
  char db[80];
  snprintf(db, sizeof(db), "connect a=%02X b=%d rx=%d tx=%d\n", addr, baudrate, _OBD_RX_PIN, _OBD_TX_PIN);
  dbg = db;
  blockCounter = 0;
  magicOk = false;                 // cleared until the 0x55 01 8A sync proves the baud
  pinMode(_OBD_TX_PIN, OUTPUT);
  digitalWrite(_OBD_TX_PIN, HIGH); // hold the K-line idle-high for the ISO 9141 W5
  delay(300);                      // bus-idle time (>=300ms) so the ECU sees a clean
                                   // start bit. An aborted handshake leaves an EDC15
                                   // mis-synced/deaf; a guaranteed idle gap before the
                                   // 5-baud address is the standard "won't init" fix.
  KWP5BaudInit(addr);
  dbg += "5baud sent, waiting reply...\n";
  Serial2.begin(baudrate, SERIAL_8N1, _OBD_RX_PIN, _OBD_TX_PIN);
  char s[3];
  int size = 3;
  if (!KWPReceiveBlock(s, 3, size)) { dbg += "no reply (KWPReceiveBlock timeout)\n"; return false; }
  snprintf(db, sizeof(db), "got magic %02X %02X %02X\n", (uint8_t)s[0], (uint8_t)s[1], (uint8_t)s[2]);
  dbg += db;
  if (    (((uint8_t)s[0]) != 0x55)
     ||   (((uint8_t)s[1]) != 0x01)
     ||   (((uint8_t)s[2]) != 0x8A)   ){
    dbg += "ERROR: invalid magic (want 55 01 8A)\n";
    disconnect();
    errorData++;
    return false;
  }
  connected = true;
  currAddr = addr;
  magicOk = true;                  // baud confirmed by the sync; keep it even if the
                                   // connect-block read below fails on a noisy K-line
  dbg += "magic OK, reading connect blocks...\n";
  if (!readConnectBlocks()) { dbg += "readConnectBlocks failed\n"; return false; }
  dbg += "CONNECTED\n";
  return true;
}

uint8_t KWP::getCurrAddr() {
  return currAddr;
}

// K-line loopback self-test. On a correctly-wired half-duplex K-line, a byte we
// transmit appears back on RX (the line reflects it). No echo => TX and RX are
// not both on the K-line (bad pins / transceiver / wiring).
String KWP::probe(int rxPin, int txPin) {
  connected = false;
  uint8_t rx = rxPin < 0 ? _OBD_RX_PIN : (uint8_t)rxPin;
  uint8_t tx = txPin < 0 ? _OBD_TX_PIN : (uint8_t)txPin;
  Serial2.begin(10400, SERIAL_8N1, rx, tx);
  delay(20);
  while (Serial2.available()) Serial2.read();          // drain
  Serial2.write(0xAA);
  Serial2.flush();
  unsigned long t = millis();
  uint8_t got[8]; int n = 0;
  while (millis() - t < 250 && n < 8) { if (Serial2.available()) got[n++] = (uint8_t)Serial2.read(); }
  char db[64];
  snprintf(db, sizeof(db), "probe rx=%d tx=%d sent=AA got=%d:", rx, tx, n);
  String s = db;
  for (int i = 0; i < n; i++) { char b[6]; snprintf(b, sizeof(b), " %02X", got[i]); s += b; }
  if (n == 0)                    s += "\nNO ECHO -> TX/RX not on the K-line (pins/transceiver)";
  else if (got[0] == 0xAA)       s += "\nECHO OK -> K-line wired; issue is init/baud/polarity";
  else                           s += "\nGARBLED echo -> wrong baud/inversion";
  Serial2.end();
  dbg = s;
  return s;
}

// ISO 14230-2 fast-init probe. See the header for intent. We try the ECU's own
// physical address first, then the ISO 14230-4 functional address 0x33, since
// VAG KWP2000 ECUs vary on which they answer. Everything is reported into the
// returned String (and dbg) — the caller only reads, it never commits to a
// KWP2000 session here.
String KWP::fastInitProbe(uint8_t addr) {
  connected = false;
  magicOk = false;
  String out = "fast-init probe a=";
  { char b[8]; snprintf(b, sizeof(b), "%02X\n", addr); out += b; }

  uint8_t targets[2] = { addr, 0x33 };
  for (int t = 0; t < 2; ++t) {
    uint8_t tgt = targets[t];
    // --- fast-init wake pulse, bit-banged on TX (line idle high, 25ms low, 25ms high) ---
    Serial2.end();
    pinMode(_OBD_TX_PIN, OUTPUT);
    digitalWrite(_OBD_TX_PIN, HIGH);
    delay(300);                                    // ensure the bus has been idle (> W5)
    digitalWrite(_OBD_TX_PIN, LOW);   delay(25);   // TiniL = 25ms
    digitalWrite(_OBD_TX_PIN, HIGH);  delay(25);   // TiniH = 25ms
    // --- StartCommunication (SID 0x81) at 10400 8N1: Fmt Tgt Src SID CS ---
    Serial2.begin(10400, SERIAL_8N1, _OBD_RX_PIN, _OBD_TX_PIN);
    while (Serial2.available()) Serial2.read();    // drain
    uint8_t req[4] = { 0xC1, tgt, 0xF1, 0x81 };
    uint8_t cs = 0; for (int i = 0; i < 4; ++i) cs += req[i];
    for (int i = 0; i < 4; ++i) obdWrite(req[i]); // obdWrite drops each byte's half-duplex echo
    obdWrite(cs);
    // --- capture the reply (initial 400ms window, extended 60ms per byte) ---
    uint8_t rx[16]; int n = 0;
    unsigned long deadline = millis() + 400;
    while ((long)(millis() - deadline) < 0 && n < 16) {
      if (Serial2.available()) { rx[n++] = (uint8_t)Serial2.read(); deadline = millis() + 60; }
    }
    { char b[24]; snprintf(b, sizeof(b), "tgt %02X -> %d:", tgt, n); out += b; }
    for (int i = 0; i < n; ++i) { char b[6]; snprintf(b, sizeof(b), " %02X", rx[i]); out += b; }
    if (n >= 6 && rx[0] == 0x83 && rx[3] == 0xC1) {  // positive StartComm response
      char b[48]; snprintf(b, sizeof(b), "  <- KWP2000 OK (KB %02X %02X)\n", rx[4], rx[5]); out += b;
      // Exercise the live path so one drive tells us if it's buildable: start a
      // diagnostic session, then read one data record (raw). A 50/61 reply
      // confirms sessions/reads work and shows the data format for this EDC15.
      uint8_t sds[5] = { 0x82, tgt, 0xF1, 0x10, 0x89 };   // StartDiagnosticSession
      out += kwp2000Frame(sds, 5, "  StartDiagSession(10 89) ->");
      uint8_t rd[5]  = { 0x82, tgt, 0xF1, 0x21, 0x01 };   // ReadDataByLocalIdentifier 0x01
      out += kwp2000Frame(rd, 5, "  ReadLocalId(21 01) ->");
      out += "RESULT: fast-init SUPPORTED -> KWP2000 path is viable\n";
      Serial2.end();
      dbg = out;
      return out;
    }
    out += (n == 0 ? "  (no reply)\n" : "  (unexpected)\n");
    Serial2.end();
  }
  out += "RESULT: no fast-init response -> ECU is KWP1281-only (keep 5-baud)\n";
  dbg = out;
  return out;
}

// Send one raw KWP2000 (ISO 14230) frame (checksum appended) and capture the
// reply bytes. gap = insert the P3 inter-request pause first. Returns byte count.
int KWP::kwp2000Send(const uint8_t* body, int n, uint8_t* rx, int maxRx, bool gap) {
  if (gap) delay(60);                          // ISO 14230 P3: gap before the next request
  drainRx();
  uint8_t cs = 0; for (int i = 0; i < n; ++i) cs += body[i];
  for (int i = 0; i < n; ++i) obdWrite(body[i]);   // obdWrite drops each byte's echo
  obdWrite(cs);
  int m = 0;
  unsigned long deadline = millis() + 300;
  while ((long)(millis() - deadline) < 0 && m < maxRx) {
    if (Serial2.available()) { rx[m++] = (uint8_t)Serial2.read(); deadline = millis() + 50; }
  }
  return m;
}

String KWP::kwp2000Frame(const uint8_t* body, int n, const char* label) {
  uint8_t rx[24];
  int m = kwp2000Send(body, n, rx, 24, true);
  String out = label;
  if (m == 0) out += " (no reply)";
  for (int i = 0; i < m; ++i) { char b[6]; snprintf(b, sizeof(b), " %02X", rx[i]); out += b; }
  out += "\n";
  return out;
}

// Establish a KWP2000 session over ISO 14230 fast-init. On success the session is
// live and readBlock() will use the 0x21 service. Returns false (leaving the line
// idle) if the ECU doesn't answer fast-init or the validation read fails, so the
// caller can fall back to KWP1281.
bool KWP::connectKwp2000(uint8_t addr) {
  char db[64]; snprintf(db, sizeof(db), "kwp2000 fast-init a=%02X\n", addr); dbg = db;
  connected = false; kwp2000_ = false; obd2_ = false; magicOk = false;
  fastInitWake();
  uint8_t rx[32];
  uint8_t sc[4] = { 0xC1, addr, 0xF1, 0x81 };          // StartCommunication
  int m = kwp2000Send(sc, 4, rx, 32, false);
  if (m < 6 || rx[0] != 0x83 || rx[3] != 0xC1) { dbg += "no fast-init reply\n"; Serial2.end(); return false; }
  dbg += "startComm OK\n";
  uint8_t sds[5] = { 0x82, addr, 0xF1, 0x10, 0x89 };   // StartDiagnosticSession (best-effort)
  kwp2000Send(sds, 5, rx, 32, true);
  // Validate the whole path with a real group read; only then declare success.
  kwp2000_ = true; connected = true; currAddr = addr;
  SENSOR probe[16];
  if (readBlock2000(addr, 1, 16, probe) <= 0) { dbg += "validation read failed\n"; disconnect(); return false; }
  magicOk = true;
  dbg += "KWP2000 CONNECTED\n";
  return true;
}

// KWP2000 measuring-block read (service 0x21 ReadDataByLocalIdentifier). The reply
// is <fmt> F1 <src> 61 <group> [k a b][k a b]... <cs> — the same measuring-block
// triples as KWP1281, so parseGroup() handles the scaling/labels.
int KWP::readBlock2000(uint8_t addr, int group, int maxSensors, SENSOR res[]) {
  uint8_t req[5] = { 0x82, addr, 0xF1, 0x21, (uint8_t)group };
  uint8_t rx[80];
  int m = 0;
  for (int attempt = 0; attempt <= kMaxSendRetries; ++attempt) {   // resend on a noise glitch
    m = kwp2000Send(req, 5, rx, 80, true);
    if (m >= 6) break;
    drainRx();
  }
  int p = -1;
  for (int i = 3; i < m - 1; ++i) { if (rx[i] == 0x61) { p = i; break; } }  // positive response SID
  if (p < 0) { disconnect(); errorData++; return 0; }              // 0x7F negative / no reply
  int di = p + 1;
  if (di < m && rx[di] == (uint8_t)group) di++;                    // skip the echoed record id
  int nbytes = (m - 1) - di; if (nbytes < 0) nbytes = 0;           // exclude the trailing checksum
  return parseGroup(addr, group, &rx[di], nbytes / 3, maxSensors, res);
}

// Shared ISO 14230 fast-init wake: idle-high, 25ms low, 25ms high, then open the
// UART at 10400 8N1 ready for StartCommunication.
void KWP::fastInitWake() {
  Serial2.end();
  pinMode(_OBD_TX_PIN, OUTPUT);
  digitalWrite(_OBD_TX_PIN, HIGH); delay(300);
  digitalWrite(_OBD_TX_PIN, LOW);  delay(25);
  digitalWrite(_OBD_TX_PIN, HIGH); delay(25);
  Serial2.begin(10400, SERIAL_8N1, _OBD_RX_PIN, _OBD_TX_PIN);
  drainRx();
}

// Generic OBD-II session (ISO 14230-4) over fast-init at the functional address
// 0x33. Validated with a live RPM read. The safety net when the ECU speaks
// KWP2000 but not the VAG measuring blocks.
bool KWP::connectObd2() {
  dbg = "obd2 fast-init a=33\n";
  connected = false; kwp2000_ = false; obd2_ = false; magicOk = false;
  fastInitWake();
  uint8_t rx[32];
  uint8_t sc[4] = { 0xC1, 0x33, 0xF1, 0x81 };          // StartCommunication (functional)
  int m = kwp2000Send(sc, 4, rx, 32, false);
  if (m < 6 || rx[0] != 0x83 || rx[3] != 0xC1) { dbg += "no fast-init reply\n"; Serial2.end(); return false; }
  dbg += "obd2 startComm OK\n";
  obd2_ = true; connected = true; currAddr = 0x33;
  SENSOR probe;                                          // validate with an RPM PID read
  if (!obd2ReadPid(0x0C, probe)) { dbg += "obd2 PID read failed\n"; disconnect(); return false; }
  magicOk = true; dbg += "OBD2 CONNECTED\n";
  return true;
}

// Read one OBD-II mode-01 PID and scale it (standard SAE J1979 formulas).
bool KWP::obd2ReadPid(uint8_t pid, SENSOR& out) {
  uint8_t req[5] = { 0x82, 0x33, 0xF1, 0x01, pid };
  uint8_t rx[32]; int m = 0;
  for (int a = 0; a <= kMaxSendRetries; ++a) { m = kwp2000Send(req, 5, rx, 32, true); if (m >= 6) break; drainRx(); }
  int p = -1;
  for (int i = 3; i < m - 1; ++i) { if (rx[i] == 0x41 && i + 1 < m && rx[i + 1] == pid) { p = i; break; } }
  if (p < 0) return false;                               // 0x41 = positive mode-01 response
  int A = (p + 2 < m) ? rx[p + 2] : 0;
  int B = (p + 3 < m) ? rx[p + 3] : 0;
  float v = 0; String u, d;
  switch (pid) {
    case 0x0C: v = ((A * 256) + B) / 4.0f;  u = F("rpm");  d = F("Engine Speed"); break;
    case 0x05: v = A - 40;                  u = F("C");    d = F("Coolant");      break;
    case 0x0D: v = A;                       u = F("km/h"); d = F("Speed");        break;
    case 0x10: v = ((A * 256) + B) / 100.0f;u = F("g/s");  d = F("MAF");          break;
    case 0x0B: v = A;                       u = F("kPa");  d = F("MAP");          break;
    case 0x0F: v = A - 40;                  u = F("C");    d = F("Intake Air");   break;
    case 0x11: v = A * 100.0f / 255;        u = F("%");    d = F("Throttle");     break;
    case 0x04: v = A * 100.0f / 255;        u = F("%");    d = F("Load");         break;
    default: return false;
  }
  char buf[16]; dtostrf(v, 4, 2, buf);
  out.type = 0; out.a = A; out.b = B; out.desc = d; out.value = String(buf); out.units = u;
  return true;
}

// OBD-II "group" read: map a group number to a set of standard PIDs so the same
// group-based UI works. group 1 = overview, group 2 = boost/air.
int KWP::readBlockObd2(int group, int maxSensors, SENSOR res[]) {
  static const uint8_t g1[4] = { 0x0C, 0x05, 0x0D, 0x10 };   // RPM, Coolant, Speed, MAF
  static const uint8_t g2[4] = { 0x0B, 0x0F, 0x11, 0x04 };   // MAP, IAT, Throttle, Load
  const uint8_t* pids = (group == 2) ? g2 : g1;
  int j = 0;
  for (int i = 0; i < 4 && j < maxSensors; ++i) {
    SENSOR s;
    if (obd2ReadPid(pids[i], s)) res[j++] = s;
  }
  if (j == 0) { disconnect(); errorData++; }               // nothing answered -> session gone
  return j;
}

// KWP1281: request stored fault codes (title 0x07). The ECU replies with one or
// more 0xFC blocks (3 bytes per DTC: code hi, code lo, elaboration); we ACK
// (0x09) after each until the ECU sends a plain 0x09 (no more).
int KWP::readFaultCodes(uint16_t codes[], uint8_t info[], int maxCodes) {
  char s[8];
  sprintf(s, "\x03%c\x07\x03", blockCounter);   // len=3, title 0x07
  if (!KWPSendBlock(s, 4)) return -1;
  int n = 0;
  while (true) {
    int size = 0;
    char r[64];
    if (!KWPReceiveBlock(r, 64, size)) return n;
    uint8_t title = (uint8_t)r[2];
    if (title == 0x09) break;                    // acknowledge / no more faults
    if (title != 0xFC) break;                    // unexpected
    int nd = (size - 4) / 3;                      // data = indices 3..size-2
    for (int i = 0; i < nd && n < maxCodes; ++i) {
      codes[n] = (uint16_t)(((uint8_t)r[3 + i * 3] << 8) | (uint8_t)r[3 + i * 3 + 1]);
      info[n]  = (uint8_t)r[3 + i * 3 + 2];
      ++n;
    }
    if (!KWPSendAckBlock()) return n;             // ask for the next block
  }
  return n;
}

// KWP1281: clear stored fault codes (title 0x05); ECU replies with ACK 0x09.
bool KWP::clearFaultCodes() {
  char s[8];
  sprintf(s, "\x03%c\x05\x03", blockCounter);
  if (!KWPSendBlock(s, 4)) return false;
  int size = 0;
  char r[64];
  if (!KWPReceiveBlock(r, 64, size)) return false;
  return (uint8_t)r[2] == 0x09;
}

void KWP::disconnect() {
  connected = false;
  kwp2000_ = false;
  obd2_ = false;
  Serial2.end();
}

int KWP::readBlock(uint8_t addr, int group, int maxSensorsPerBlock, SENSOR resGroupSensor[]) {
  if (obd2_)    return readBlockObd2(group, maxSensorsPerBlock, resGroupSensor);
  if (kwp2000_) return readBlock2000(addr, group, maxSensorsPerBlock, resGroupSensor);
  Serial.print(F("------readBlock "));
  Serial.println(group);
  char s[64];
  // Resilient request send: a request corrupted by K-line noise (bad TX
  // complement) leaves the block counter untouched, so re-sending the SAME
  // request is protocol-safe. Retry a couple of times before giving up, instead
  // of tearing the session down and paying a full 5-baud reconnect for a glitch.
  softMode_ = true;
  bool sent = false;
  for (int attempt = 0; attempt <= kMaxSendRetries; ++attempt) {
    sprintf(s, "\x04%c\x29%c\x03", blockCounter, group);
    if (KWPSendBlock(s, 5)) { sent = true; break; }
    drainRx(); delay(20);                       // flush the noise, then re-issue
  }
  softMode_ = false;
  if (!sent) { disconnect(); errorData++; return 0; }  // request never got through -> reconnect
  int size = 0;
  // A corrupted RESPONSE can't be safely re-requested (the ECU is mid-exchange),
  // so a receive failure remains a hard drop.
  if (!KWPReceiveBlock(s, 64, size) || s[2] != '\xe7') {
    Serial.println(F("ERROR: invalid answer"));
    disconnect();
    errorData++;
    return 0;
  }
  int count = (size-4) / 3;
  if (count > maxSensorsPerBlock) {
    Serial.println(F("ERROR: max sensors exceded"));
    disconnect();
    errorData++;
    return 0;
  }
  return parseGroup(addr, group, (const uint8_t*)&s[3], count, maxSensorsPerBlock, resGroupSensor);
}

// Turn a measuring block's raw (formula,a,b) triples into labelled SENSORs. Shared
// by the KWP1281 block read and the KWP2000 (service 0x21) read — both encode
// measuring blocks identically, so the scaling table (getSensorData) and label
// table (getBlockDesc) apply to both.
int KWP::parseGroup(uint8_t addr, int group, const uint8_t* triples, int count, int maxSensors, SENSOR res[]) {
  if (count > maxSensors) count = maxSensors;
  if (count < 0) count = 0;
  String blockDescs = getBlockDesc(addr, group);
  int len = blockDescs.length();
  char buf[len + 1];
  blockDescs.toCharArray(buf, len + 1);
  char* command = strtok(buf, ",");
  int j = 0;
  for (int idx = 0; idx < count; idx++) {
    byte k = triples[idx * 3];
    byte a = triples[idx * 3 + 1];
    byte b = triples[idx * 3 + 2];
    String desc = command ? String(command) : String("");
    SENSOR sensor = getSensorData(k, a, b);
    if (desc != "" && sensor.value != "") {
      res[j].type = sensor.type;
      res[j].a = sensor.a;
      res[j].b = sensor.b;
      res[j].desc = desc;
      res[j].value = sensor.value;
      res[j].units = sensor.units;
      j++;
    }
    command = strtok(0, ",");
  }
  return j;
}

SENSOR KWP::getSensorData(byte k, byte a, byte b) {
    SENSOR res;
    Serial.print(F("type="));
    Serial.print(k);
    Serial.print(F("  a="));
    Serial.print(a);
    Serial.print(F("  b="));
    Serial.print(b);
    Serial.print(F("  text="));
    String t = "";
    float v = 0;
    String units = "";
    char buf[32];
    switch (k){
      case 1:  v=0.2*a*b;             units=F("rpm"); break;
      case 2:  v=a*0.002*b;           units=F("%"); break; // case 2:  v=a*0.002*b;            units=F("%%");break;
      case 3:  v=0.002*a*b;           units=F("Deg"); break;
      case 4:  v=abs(b-127)*0.01*a;   units=F("ATDC"); break;
      case 5:  v=a*(b-100)*0.1;       units=F("c");break;
      case 6:  v=0.001*a*b;           units=F("v");break;
      case 7:  v=0.01*a*b;            units=F("km/h");break;
      case 8:  v=0.1*a*b;             units=F(" ");break;
      case 9:  v=(b-127)*0.02*a;      units=F("Deg");break;
      case 10: if (b == 0) t=F("COLD"); else t=F("WARM");break;
      case 11: v=0.0001*a*(b-128)+1;  units = F(" ");break;
      case 12: v=0.001*a*b;           units =F("Ohm");break;
      case 13: v=(b-127)*0.001*a;     units =F("mm");break;
      case 14: v=0.005*a*b;           units=F("bar");break;
      case 15: v=0.01*a*b;            units=F("ms");break;
      case 18: v=0.04*a*b;            units=F("mbar");break;
      case 19: v=a*b*0.01;            units=F("l");break;
      case 20: v=a*(b-128)/128;       units=F("%");break; // case 20: v=a*(b-128)/128;       units=F("%%");break;
      case 21: v=0.001*a*b;           units=F("V");break;
      case 22: v=0.001*a*b;           units=F("ms");break;
      case 23: v=b/256*a;             units=F("%");break; //case 23: v=b/256*a;             units=F("%%");break;
      case 24: v=0.001*a*b;           units=F("A");break;
      case 25: v=(b*1.421)+(a/182);   units=F("g/s");break;
      case 26: v=float(b-a);          units=F("C");break;
      case 27: v=abs(b-128)*0.01*a;   units=F("Deg");break; //case 27: v=abs(b-128)*0.01*a;   units=F("Â°");break;
      case 28: v=float(b-a);          units=F(" ");break;
      case 30: v=b/12*a;              units=F("Deg k/w");break;
      case 31: v=b/2560*a;            units=F("Â°C");break;
      case 33: v=100*b/a;             units=F("%");break; //case 33: v=100*b/a;             units=F("%%");break;
      case 34: v=(b-128)*0.01*a;      units=F("kW");break;
      case 35: v=0.01*a*b;            units=F("l/h");break;
      case 36: v=((unsigned long)a)*2560+((unsigned long)b)*10;  units=F("km");break;
      case 37: v=b; break; // oil pressure ?!
      // ADP: FIXME!
      /*case 37: switch(b){
             case 0: sprintf(buf, F("ADP OK (%d,%d)"), a,b); t=String(buf); break;
             case 1: sprintf(buf, F("ADP RUN (%d,%d)"), a,b); t=String(buf); break;
             case 0x10: sprintf(buf, F("ADP ERR (%d,%d)"), a,b); t=String(buf); break;
             default: sprintf(buf, F("ADP (%d,%d)"), a,b); t=String(buf); break;
          }*/
      case 38: v=(b-128)*0.001*a;        units=F("Deg k/w"); break;
      case 39: v=b/256*a;                units=F("mg/h"); break;
      case 40: v=b*0.1+(25.5*a)-400;     units=F("A"); break;
      case 41: v=b+a*255;                units=F("Ah"); break;
      case 42: v=b*0.1+(25.5*a)-400;     units=F("Kw"); break;
      case 43: v=b*0.1+(25.5*a);         units=F("V"); break;
      case 44: sprintf(buf, "%2d:%2d", a,b); t=String(buf); break;
      case 45: v=0.1*a*b/100;            units=F(" "); break;
      case 46: v=(a*b-3200)*0.0027;      units=F("Deg k/w"); break;
      case 47: v=(b-128)*a;              units=F("ms"); break;
      case 48: v=b+a*255;                units=F(" "); break;
      case 49: v=(b/4)*a*0.1;            units=F("mg/h"); break;
      case 50: v=(b-128)/(0.01*a);       units=F("mbar"); break;
      case 51: v=((b-128)/255)*a;        units=F("mg/h"); break;
      case 52: v=b*0.02*a-a;             units=F("Nm"); break;
      case 53: v=(b-128)*1.4222+0.006*a;  units=F("g/s"); break;
      case 54: v=a*256+b;                units=F("count"); break;
      case 55: v=a*b/200;                units=F("s"); break;
      case 56: v=a*256+b;                units=F("WSC"); break;
      case 57: v=a*256+b+65536;          units=F("WSC"); break;
      case 59: v=(a*256+b)/32768;        units=F("g/s"); break;
      case 60: v=(a*256+b)*0.01;         units=F("sec"); break;
      case 62: v=0.256*a*b;              units=F("S"); break;
      case 64: v=float(a+b);             units=F("Ohm"); break;
      case 65: v=0.01*a*(b-127);         units=F("mm"); break;
      case 66: v=(a*b)/511.12;          units=F("V"); break;
      case 67: v=(640*a)+b*2.5;         units=F("Deg"); break;
      case 68: v=(256*a+b)/7.365;       units=F("deg/s");break;
      case 69: v=(256*a +b)*0.3254;     units=F("Bar");break;
      case 70: v=(256*a +b)*0.192;      units=F("m/s^2");break;
      default: sprintf(buf, "%2x, %2x      ", a, b); break;
    }

    if (units.length() != 0){
      dtostrf(v,4, 2, buf);
      t=String(buf) + " " + units;
    }
    Serial.println(t);

    res.type = k;
    res.a = a;
    res.b = b;
    res.value = String(buf);
    res.units = units;
    return res;
}

String KWP::getBlockDesc(uint8_t addr, int block){
  String blockDescs;
  if(addr == ADR_Dashboard){
    switch (block){
      case 1: blockDescs=F("Speed,Engine Speed,Oil pressure,Time"); break;
      case 2: blockDescs=F("Odometer,Fuel lvl,FuelSend,TAmbient"); break; // case 2: blockDescs=F("Odometer,Fuel level (l),Fuel Sender,Ambient"); break;
      case 3: blockDescs=F("Coolant Temp,Oil level,Oil,[N/A]"); break;
      case 22: blockDescs=F("Starting,Engine (ECM),Key condition,Number of"); break;
      case 23: blockDescs=F("Variable code,Key status,Fixed code,Immobilizer"); break;
      case 24: blockDescs=F("Instrument,Engine control,Emergency,Transponder"); break;
      case 25: blockDescs=F("Immobilizer,[N/A],[N/A],[N/A]"); break;
      case 50: blockDescs=F("Odometer,Engine Speed,Oil,Coolant"); break;
      case 125: blockDescs=F("Engine,Transmission,ABS,[N/A]"); break;
      case 126: blockDescs=F("Steering,Airbag,[N/A],[N/A]"); break;
    default: blockDescs=F(""); break;
    }

  }
  //Label 06A-906-032-AUM.lbl
  else if(addr == ADR_Engine){
    switch (block){
      case 1: blockDescs=F("Engine Speed,Coolant,Lambda Control,Basic Setting"); break;
      case 2: blockDescs=F("Engine Speed,Load,Inj Time,MAF"); break; // case 2: blockDescs=F("Engine Speed,Engine Load,Injection Timing,Mass Air Flow"); break;
      case 3: blockDescs=F("Engine Speed,MAF,Throttle,Ignition"); break; // case 3: blockDescs=F("Engine Speed,Mass Air Flow,Throttle Valve Angle,Ignition"); break;
      case 4: blockDescs=F("Engine Speed,Battery Voltage,Coolant,Intake Air"); break;
      case 5: blockDescs=F("Engine Speed,Engine Load,Vehicle Speed,Load Status"); break;
      case 6: blockDescs=F("Engine Speed,Engine Load,Intake Air,Altitude"); break;
      case 10: blockDescs=F("Engine Speed,Engine Load,Throttle Valve Angle,Ignition"); break;
      case 11: blockDescs=F("Engine Speed,Coolant,Intake Air,Ignition"); break;
      case 14: blockDescs=F("Engine Speed,Engine Load,Misfire,Misfire"); break;
      case 15: blockDescs=F("Misfire1,Misfire2,Misfire3,Misfire4"); break; // case 15: blockDescs=F("Misfire Counter,Misfire Counter,Misfire Counter,Misfire"); break;
      case 16: blockDescs=F("Misfire Counter,Misfire"); break;
      case 18: blockDescs=F("Lower,Upper,Lower,Upper"); break;
      case 20: blockDescs=F("Knock 1-4,Knock 1-4,Knock 1-4,Knock 1-4"); break; //case 20: blockDescs=F("Timing Retardation,Timing Retardation,Timing Retardation,Timing Retardation"); break;
      case 22: blockDescs=F("Engine Speed,Engine Load,Timing Retardation,Timing Retardation"); break;
      case 23: blockDescs=F("Engine Speed,Engine Load,Timing Retardation,Timing Retardation"); break;
      case 26: blockDescs=F("Voltage,Voltage,Voltage,Voltage"); break;
      case 28: blockDescs=F("Engine Speed,Engine Load,Coolant,Result"); break;
      case 30: blockDescs=F("Bank 1,Bank 1"); break;
      case 31: blockDescs=F("LambdDes,LambdAct"); break; // case 31: blockDescs=F("Lambda Control,Lambda Control"); break;
      case 32: blockDescs=F("Trims"); break;
      case 33: blockDescs=F("Lambda Control,Sensor Voltage"); break;
      case 34: blockDescs=F("Engine Speed,Catalytic Converter,Dynamic Factor,Result"); break;
      case 36: blockDescs=F("Sensor Voltage,Result"); break;
      case 37: blockDescs=F("Engine Load,Sensor Voltage"); break;
      case 41: blockDescs=F("Resistance,Heater Condition,Resistance,Heater Condition"); break;
      case 43: blockDescs=F("Engine Speed,Catalytic Converter,Lambda Voltage,Result"); break;
      case 46: blockDescs=F("Engine Speed,Catalytic Converter"); break;
      case 50: blockDescs=F("Engine Speed,Engine Speed,A/C Readiness,A/C Compressor"); break;
      case 51: blockDescs=F("Engine Speed,Engine Speed,Selected Gear,Battery Voltage"); break;
      case 53: blockDescs=F("Engine Speed,Engine Speed,Battery Voltage,Generator"); break;
      case 54: blockDescs=F("Engine Speed,Load Status,Accel. Pedal Pos.,Throttle Valve Angle"); break;
      case 55: blockDescs=F("Engine Speed,Idle Regulator,Idle Stabilization,Operating"); break;
      case 56: blockDescs=F("Engine Speed,Engine Speed,Idle Regulator,Operating"); break;
      case 60: blockDescs=F("Throttle Valve,Throttle Valve,Throttle Adaptation,Result"); break;
      case 61: blockDescs=F("Engine Speed,Battery Voltage,Throttle Valve Angle,Operating"); break;
      case 62: blockDescs=F("Throttle Valve,Throttle Valve,Accel. Pedal Pos.,Accel. Pedal Pos."); break;
      case 64: blockDescs=F("Lower Adaptation,Lower Adaptation,Emergency Air Gap,Emergency Air Gap"); break;
      case 66: blockDescs=F("Vehicle Speed,Switch Positions I,Vehicle Speed,Switch Positions II"); break;
      case 70: blockDescs=F("Evap. Emissions,Lambda Control,Result"); break;
      case 77: blockDescs=F("Engine Speed,Mass Air Flow,Air Mass from,Result"); break;
      case 81: blockDescs=F("Vehicle Ident.,Immobilizer"); break;
      case 86: blockDescs=F("Readiness Bits,Cycle Flags I,Cycle Flags II,Cycle Flags II"); break;
      case 87: blockDescs=F("Readiness Bits,Error Flags I,Error Flags II,Error Flags II"); break;
      case 90: blockDescs=F("Engine Speed,Camshaft Adjustm.,Camshaft Adjustm."); break;
      case 91: blockDescs=F("Engine Speed,Engine Load,Camshaft Adjustm.,Camshaft"); break;
      case 94: blockDescs=F("Engine Speed,Camshaft Adjustm.,Result"); break;
      case 99: blockDescs=F("Engine Speed,Coolant,Lambda Control,Lambda Control"); break;
      case 100: blockDescs=F("Readiness Bits,Coolant,Time since,OBD-Status"); break;
      case 101: blockDescs=F("Engine Speed,Engine Load,Injection Timing,Mass Air Flow"); break;
      case 102: blockDescs=F("Engine Speed,Coolant,Intake Air,Injection Timing"); break;
      case 107: blockDescs=F("Engine Speed,Lambda Control,Result"); break;
      case 110: blockDescs=F("Engine Speed,Coolant,Injection Timing,Throttle Valve Angle"); break;
      case 111: blockDescs=F("RPM Range 1,RPM Range 2,RPM Range 3,RPM Range 4"); break;
      case 113: blockDescs=F("Engine Speed,Engine Load,Throttle Valve Angle,Athmospheric"); break;
      case 114: blockDescs=F("Engine Load,Engine Load,Engine Load,Wastegate (N75)"); break;
      case 115: blockDescs=F("Engine Speed,Load,BoostDes,BoostAct"); break; //case 115: blockDescs=F("Engine Speed,Engine Load,Boost Pressure,Boost Pressure"); break;
      case 116: blockDescs=F("Engine Speed,Fuel Temp.,Coolant Temp.,Intake Air Temp."); break;
      case 117: blockDescs=F("Engine Speed,Accel. Pedal Pos.,Throttle Valve Angle,Boost Pressure"); break;
      case 118: blockDescs=F("Engine Speed,IAT,WGTE N75,Boost"); break; //case 118: blockDescs=F("Engine Speed,Intake Air,Wastegate (N75),Boost Pressure"); break;
      case 119: blockDescs=F("Engine Speed,Charge Limit,Wastegate (N75),Boost Pressure"); break;
      case 120: blockDescs=F("Engine Speed,Torque specified,Engine Torque,Traction Control"); break;
      case 122: blockDescs=F("Engine Speed,Engine Load,Engine Load,Status"); break;
      case 125: blockDescs=F("Transmission,Brake Electronics,Instrument Cluster,Heating/Air"); break;
      default: blockDescs=F(""); break;
    }
  }
  else{
    Serial.println("Not description found for that address");
  }
  return blockDescs;
}

bool KWP::isConnected() {
  return connected;
}

void KWP::obdWrite(uint8_t data) {
  Serial2.write(data);
  Serial2.flush();
  // Half-duplex K-line (via the MC33290): every byte we transmit is reflected
  // back on RX. Read and discard that echo so it isn't mistaken for the module's
  // reply/complement. We confirmed the echo is always present (loopback probe).
  unsigned long start = millis();
  while (!Serial2.available() && millis() - start < 100) {}
  if (Serial2.available()) Serial2.read();   // drop our own echoed byte
}

void KWP::drainRx() {
  while (Serial2.available()) Serial2.read();
}

uint8_t KWP::obdRead() {
  unsigned long timeout = millis() + 1000;
  while (!Serial2.available()){
    if (millis() >= timeout) {
      Serial.println(F("ERROR: obdRead timeout"));
      if (!softMode_) disconnect();   // in soft mode the caller decides (safe retry)
      errorTimeout++;
      return 0;
    }
  }
  uint8_t data = Serial2.read();
  return data;
}

bool KWP::KWP5BaudInit(uint8_t addr){
  Serial.println(F("---KWP 5 baud init"));
  //delay(3000);
  send5baud(addr);
  return true;
}

void KWP::send5baud(uint8_t data) {
  #define bitcount 10
  byte bits[bitcount];
  byte even=1;
  byte bit;
  for (int i=0; i < bitcount; i++){
    bit=0;
    if (i == 0)  bit = 0;
      else if (i == 8) bit = even; // computes parity bit
      else if (i == 9) bit = 1;
      else {
        bit = (byte) ((data & (1 << (i-1))) != 0);
        even = even ^ bit;
      }
    Serial.print(F("bit"));
    Serial.print(i);
    Serial.print(F("="));
    Serial.print(bit);
    if (i == 0) Serial.print(F(" startbit"));
      else if (i == 8) Serial.print(F(" parity"));
      else if (i == 9) Serial.print(F(" stopbit"));
    Serial.println();
    bits[i]=bit;
  }
  for (int i=0; i < bitcount+1; i++){
    if (i != 0){
      delay(initBitMs_);                // 5-baud bit period (Adaptation: init pulse)
      if (i == bitcount) break;
    }
    if (bits[i] == 1){
      digitalWrite(_OBD_TX_PIN, HIGH);
    } else {
      digitalWrite(_OBD_TX_PIN, LOW);
    }
  }
  Serial2.flush();
}

bool KWP::KWPSendBlock(char *s, int size) {
  Serial.print(F("---KWPSend sz="));
  Serial.print(size);
  Serial.print(F(" blockCounter="));
  Serial.println(blockCounter);
  Serial.print(F("OUT:"));
  for (int i=0; i < size; i++){
    uint8_t data = s[i];
    Serial.print(data, HEX);
    Serial.print(" ");
  }
  Serial.println();
  if (blockDelayMs_ > 0) delay(blockDelayMs_);        // W3 inter-frame pause before our block (Adaptation)
  for (int i=0; i < size; i++){
    uint8_t data = s[i];
    if (i > 0 && interByteMs_ > 0) delay(interByteMs_); // W4 inter-byte pause (Adaptation)
    obdWrite(data);
    if (i < size-1){
      uint8_t complement = obdRead();
      if (complement != (data ^ 0xFF)){
        Serial.println(F("ERROR: invalid complement"));
        if (!softMode_) disconnect();   // soft mode: request not fully sent, block counter
        errorData++;                    // not advanced -> caller can safely re-send
        return false;
      }
    }
  }
  blockCounter++;
  return true;
}

bool KWP::KWPReceiveBlock(char s[], int maxsize, int &size, bool init_delay) {
  bool ackeachbyte = false;
  uint8_t data = 0;
  int recvcount = 0;
  if (size == 0) ackeachbyte = true;
  Serial.print(F("---KWPReceive sz="));
  Serial.print(size);
  Serial.print(F(" blockCounter="));
  Serial.println(blockCounter);
  if (size > maxsize) {
    Serial.println("ERROR: invalid maxsize");
    return false;
  }
  unsigned long timeout = millis() + 2000;  // TODO: This allows connect to different Modules
  //unsigned long timeout = millis() + 1000;
  while ((recvcount == 0) || (recvcount != size)) {
    while (Serial2.available()){
      data = obdRead();
      s[recvcount] = data;
      recvcount++;
      if ((size == 0) && (recvcount == 1)) {
        size = data + 1;
        if (size > maxsize) {
          Serial.println("ERROR: invalid maxsize");
          return false;
        }
      }
      if ((ackeachbyte) && (recvcount == 2)) {
        if (data != blockCounter){
          Serial.println(F("ERROR: invalid blockCounter"));
          disconnect();
          errorData++;
          return false;
        }
      }
      if ( ((!ackeachbyte) && (recvcount == size)) ||  ((ackeachbyte) && (recvcount < size)) ){
        if (interByteMs_ > 0) delay(interByteMs_);   // W4 inter-byte pause (Adaptation)
        obdWrite(data ^ 0xFF);
      }
      timeout = millis() + 1000;
    }
    if (millis() >= timeout){
      Serial.println(F("ERROR: timeout"));
      disconnect();
      errorTimeout++;
      return false;
    }
  }
  Serial.print(F("IN: sz="));
  Serial.print(size);
  Serial.print(F(" data="));
  for (int i=0; i < size; i++){
    uint8_t data = s[i];
    Serial.print(data, HEX);
    Serial.print(F(" "));
  }
  Serial.println();
  blockCounter++;
  return true;
}

bool KWP::KWPSendAckBlock() {
  Serial.print(F("---KWPSendAckBlock blockCounter="));
  Serial.println(blockCounter);
  char buf[32];
  sprintf(buf, "\x03%c\x09\x03", blockCounter);
  return (KWPSendBlock(buf, 4));
}

bool KWP::readConnectBlocks() {
  Serial.println(F("------readconnectblocks"));
  String info;
  while (true){
    int size = 0;
    char s[64];
    if (!(KWPReceiveBlock(s, 64, size))) return false;
    if (size == 0) return false;
    if (s[2] == '\x09') break;
    if (s[2] != '\xF6') {
      Serial.println(F("ERROR: unexpected answer"));
      disconnect();
      errorData++;
      return false;
    }
    String text = String(s);
    info += text.substring(3, size-2);
    if (!KWPSendAckBlock()) return false;
  }
  Serial.print("label=");
  Serial.println(info);
  ecuId = info;                       // part number + name, e.g. "038906018AH 1,9l R4 EDC ..."
  dbg += "id: "; dbg += info; dbg += "\n";
  return true;
}

