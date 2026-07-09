# ESP32 Audi MMI — v2

Rewrite of the Audi A4 B5 MMI bridge (FIS cluster + steering/console controls + BC127 Bluetooth + KWP diagnostics) as a PlatformIO project with a desktop **browser emulator** so most development needs no car.

Plan: `~/.claude/plans/reactive-forging-pearl.md`.

## Prerequisites
PlatformIO CLI. If `pio` isn't on your PATH, it's at:
`/Library/Frameworks/Python.framework/Versions/3.10/bin/pio`

## Build & test (desktop)
```sh
pio run  -e native      # build the emulator host
pio test -e native      # run unit tests
```

## Run the emulator
```sh
./.pio/build/native/program
# then open http://localhost:8080
```
Click the console/steering/rotary buttons and the fake-BC127 panel to drive the
real UI code; the canvas shows the 64x88 FIS exactly as the cluster would.

## Build & flash firmware (ESP32)
First flash is over **USB** (OTA only works once firmware with the web server is
already running):
```sh
pio run -e esp32                     # compile
pio run -e esp32 -t upload           # flash over USB (add --upload-port /dev/cu.XXXX)
pio run -e esp32 -t monitor          # serial console @115200
```
Find the port with `ls /dev/cu.*` (needs CP210x/CH340 driver for most boards).
Later updates go over WiFi (secured upload page / phone pull-OTA).

## Layout
- `src/hal/` — hardware interfaces (`IDisplay/IInputs/IBluetooth/IStorage`) + `Types.h`
- `src/hal/esp32/` — real peripheral impls (firmware only)
- `src/hal/native/` — emulator impls: `EmulatorDisplay`, `FakeBluetooth`, `HttpServer`, …
- `src/ui/` — `InputRouter` (button map §2c), `MenuSystem`, `MenuTree`
- `src/App.{h,cpp}` — hardware-independent core loop
- `test/` — native unit tests
- `lib/` — vendored VAGFIS*/VAGRadioRemote/AnalogMultiButton libraries
