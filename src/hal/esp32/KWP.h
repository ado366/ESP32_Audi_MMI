#ifndef KWP_h
#define KWP_h

#include <inttypes.h>
#include <Arduino.h>

#define ADR_Engine 0x01 // Engine
#define ADR_Gears  0x02 // Auto Trans
#define ADR_ABS_Brakes 0x03
#define ADR_Airbag 0x15
#define ADR_Dashboard 0x17 // Instruments
#define ADR_Immobilizer 0x25
#define ADR_Central_locking 0x35
#define ADR_Navigation 0x37

struct KWP_MODULE{
  String name;
  uint8_t addr;
  int *groups;
  int ngroups;
};

struct SENSOR {
  int type;
  int a;
  int b;
  String desc;
  String value;
  String units;
};

class KWP {
  public:
    KWP(uint8_t receivePin, uint8_t transmitPin);
    ~KWP();
    bool connect(uint8_t addr, int baudrate);
    void disconnect();
    int readBlock(uint8_t addr, int group, int maxSensorsPerGroup, SENSOR resGroupSensor[]);
    SENSOR getSensorData(byte k, byte a, byte b);
    String getBlockDesc(uint8_t addr, int block);
    bool isConnected();
    uint8_t getCurrAddr();
    String probe(int rxPin = -1, int txPin = -1);   // K-line loopback self-test (writes result to dbg)
    // ISO 14230-2 (KWP2000) fast-init PROBE. Non-invasive: wakes the K-line with
    // the 25ms/25ms init pattern, sends a StartCommunication request and reports
    // the raw ECU reply. Read-only; does NOT open a session or touch KWP1281
    // state. Answers "does this ECU speak KWP2000 fast-init?" — if it does, the
    // whole engine path could migrate off the slow 5-baud KWP1281 init. Result
    // (also written to dbg, so /kwpdbg surfaces it) reports each target address
    // tried; a positive StartComm reply is 83 F1 <addr> C1 <KB1> <KB2> <cs>.
    String fastInitProbe(uint8_t addr);
    // Per-vehicle KWP1281 timing (Maxi-K "Adaptation"). initBitMs = 5-baud bit
    // period (~200); interByteMs = inter-byte W4 pause before each complement ACK
    // (0 = as fast as possible; raise it if an ECU drops blocks); blockDelayMs =
    // inter-frame W3 pause before sending a request/ack block.
    void setTiming(int initBitMs, int interByteMs, int blockDelayMs) {
      if (initBitMs  > 0) initBitMs_  = initBitMs;
      if (interByteMs >= 0) interByteMs_ = interByteMs;
      if (blockDelayMs >= 0) blockDelayMs_ = blockDelayMs;
    }
    String dbg;     // recent connect-flow trace, surfaced over WiFi for on-car debug
    String ecuId;   // connect-block ASCII: part number + ECU name
    // True once the last connect() saw the 0x55 0x01 0x8A sync — i.e. the baud is
    // correct for this address, even if a LATER stage (connect blocks / noise on a
    // running engine) then failed. Lets the caller keep the proven baud instead of
    // flapping to the wrong one on every failure. See Esp32Diag::ensureConnected.
    bool magicOk = false;
    // KWP1281 stored fault codes. readFaultCodes returns the count read (or -1 on
    // comms error); each DTC is a 16-bit code + 1-byte elaboration/status.
    int readFaultCodes(uint16_t codes[], uint8_t info[], int maxCodes);
    bool clearFaultCodes();
  private:
    uint8_t _OBD_RX_PIN;
    uint8_t _OBD_TX_PIN;

    bool connected = false;
    uint8_t currAddr = 0;
    uint8_t blockCounter = 0;
    volatile int initBitMs_ = 200;    // 5-baud bit period (Adaptation: init pulse)
    volatile int interByteMs_ = 0;    // W4 pause before each complement ACK (inter-byte)
    volatile int blockDelayMs_ = 0;   // W3 pause before sending a block (inter-frame)
    uint8_t errorTimeout = 0;
    uint8_t errorData = 0;

    //Serial *obd;

    void obdWrite(uint8_t data);
    uint8_t obdRead();
    void send5baud(uint8_t data);
    bool KWP5BaudInit(uint8_t addr);
    bool KWPSendBlock(char *s, int size);
    bool KWPReceiveBlock(char s[], int maxsize, int &size, bool init_delay = false);
    bool KWPSendAckBlock();
    bool readConnectBlocks();

};

#endif


