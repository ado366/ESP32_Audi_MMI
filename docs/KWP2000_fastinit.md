# KWP2000 fast-init — investigation & on-car probe

## The question
The engine ECU (EDC15, `038906019CC` "1,9l R4 EDC") connects over the K-line with
the **slow 5-baud init** (~2 s) followed by the **KWP1281** block protocol. Cold
connects while driving are flaky (~15–20 s, several retries) because a byte
garbled by engine/alternator noise aborts the handshake and forces a full
5-baud re-init. FIS-Control exposes a "fast-init" option; could it fix this?

## Finding: fast-init is a *different protocol*, not a faster 5-baud
- **Fast init** = ISO 14230-2: idle-high, **25 ms low / 25 ms high** wake pulse,
  then a `StartCommunication` request at 10400 8N1. It replaces the ~2 s 5-baud
  address send with ~55 ms.
- But fast init **always establishes a KWP2000 (ISO 14230) session**. There is no
  "KWP1281 over fast init." KWP2000 messages are service-based
  (`Fmt Tgt Src SID … CS`; SID 0x10 StartDiagnosticSession, 0x21/0x22
  ReadDataByLocalIdentifier, 0x3E TesterPresent, 0x18/0x14 for DTCs) and do **not
  coexist** with KWP1281 blocks (length + block-counter + title + complement-ACK
  per byte, keywords `0x55 01 8A`).
- So adopting fast init means migrating the **entire engine path** to KWP2000 —
  worth it only if *this* ECU actually answers fast init.

The direct ancestor (FISCuntrol) is **KWP1281-only** (5-baud, no fast init).
However, **PD-family EDC15+ ECUs (this car, AJM/ATJ/AUY labels) frequently do
support KWP2000**, unlike the older KWP1281-only EDC15s — so it's worth testing
rather than assuming.

## What was built: a non-invasive on-car probe
Rather than build a whole KWP2000 stack on an unverified assumption, this adds a
read-only probe that answers "does 019CC speak fast init?" at **zero risk** to the
working KWP1281 flow.

- `KWP::fastInitProbe(addr)` — does the 25/25 ms wake pulse, sends
  `StartCommunication` (`C1 <tgt> F1 81 <cs>`), and reports the raw ECU reply. It
  tries the ECU's physical address first, then the ISO 14230-4 functional `0x33`
  (VAG ECUs vary on which they answer). It never opens a session or touches
  KWP1281 state.
- Marshaled through the existing `Esp32Diag` KWP task (`requestFastInit`), like the
  loopback probe, so there's no K-line race with live reads.
- Web endpoint **`GET /kwpfastinit`** (optional `?ecu=<hex>`, default `01`);
  the result appears at **`/kwpdbg`**.

### How to run it (next drive, engine ECU powered)
```
curl http://<car-ip>/kwpfastinit          # trigger (engine ECU)
curl http://<car-ip>/kwpdbg               # read the result ~2 s later
```
- **Positive** reply looks like `83 F1 01 C1 <KB1> <KB2> <cs>` →
  `RESULT: fast-init SUPPORTED -> KWP2000 path is viable`.
- **No reply / unexpected** on both target addresses →
  `RESULT: no fast-init response -> ECU is KWP1281-only (keep 5-baud)`.

## Next step (only if the probe is positive)
Build the KWP2000 read path for the engine ECU: `StartCommunication` →
`StartDiagnosticSession` (0x10) → `TesterPresent` (0x3E) keep-alive →
`ReadDataByLocalIdentifier` (0x21 grp / 0x22 DID) for measuring blocks and
0x18/0x14 for DTCs. Keep the KWP1281 path for the cluster (0x17) and as a
fallback if fast init NAKs.

## Caveats
- The 25/25 ms pulses are bit-banged; a WiFi/timer ISR can stretch them slightly,
  but ISO 14230 allows ±1 ms and this is only a probe (the 5-baud init has the
  same exposure and works).
- Firmware carrying the probe: **2.9.13** (not yet flashed — car was offline).
</content>
