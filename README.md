<h1 align="center">Hawk</h1>

<p align="center"><b>The original Xbox Communicator (voice headset), emulated on an ESP32-S3</b></p>

<p align="center">
  <a href="https://github.com/Team-Resurgent/Hawk/blob/master/LICENSE.md"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPL v3"></a>
  <a href="https://discord.gg/VcdSfajQGK"><img src="https://img.shields.io/badge/chat-on%20discord-7289da.svg?logo=discord" alt="Discord"></a>
</p>

<p align="center">
  <a href="https://ko-fi.com/J3J7L5UMN"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi"></a>
  <a href="https://www.patreon.com/teamresurgent"><img src="https://img.shields.io/badge/Patreon-F96854?style=for-the-badge&logo=patreon&logoColor=white" alt="Patreon"></a>
</p>

<p align="center">
  <a href="https://github.com/Team-Resurgent/Hawk/releases/latest"><img src="https://img.shields.io/badge/download-latest-brightgreen.svg?style=for-the-badge&logo=github" alt="Download"></a>
</p>

Hawk makes an ESP32-S3 present itself as the **original Xbox Communicator**
(the controller headset — Microsoft's internal codename for it really was
*Hawk*), so an **unmodified** Xbox title sees a real communicator: microphone
in, headphone out. Sibling of
[Talon](https://github.com/Team-Resurgent/Talon) (controller) and
[Falcon](https://github.com/Team-Resurgent/Falcon) (camera).

**Milestone 1 (this repo) — working, hardware-verified:** the board has no
microphone yet, so it "speaks" a recognizable **C5–E5–G5 arpeggio** into the
mic endpoint and measures whatever the Xbox plays back to the headphone
endpoint. With the bundled Xbox-side test app the loop closes end to end on a
real console:

```
ESP32 tone ──iso IN──▶ Xbox mic XMO ──▶ TV speakers   (you hear the arpeggio)
                              │
                              └──▶ headphone XMO ──iso OUT──▶ ESP32 stats
                                   (the ESP32 heartbeat logs level + frequency)
```

The Xbox reads the tone (peak 12000, ~491 Hz averaged across the arpeggio),
plays it out the TV **and** loops it back to the headphone endpoint, which the
ESP32 receives and measures — proving both directions of the audio path.

> The Xbox side needs the **Hawk USB class driver** present in the kernel/XAPI
> USB stack. It ships in the retail `xvoice.lib`; the open-source
> [RXDK](https://github.com/Team-Resurgent) toolchain carries a port of it (see
> the RXDK-Libs `libxapi/usb/hawk` driver). Stock setups without that driver
> will enumerate the device but cannot open the communicator XMOs.

Planned next: stream a decoded **OGG file** as the mic source, WiFi
provisioning/web UI (ported from Talon's `wifi_net`), and eventually a real
I2S microphone/speaker path.

## Protocol (recovered from the leaked XDK + xemu)

The communicator is one USB 1.1 device, VID/PID `045E:0283`, with **two
vendor-specific interfaces of class `0x78`**, one isochronous endpoint each:

| Interface | Endpoint | Direction | Role |
|---|---|---|---|
| 0 | `0x04` iso OUT | Xbox → device | headphone (playback) |
| 1 | `0x85` iso IN | device → Xbox | microphone (capture) |

Audio is 16-bit mono PCM, ≤50-byte packets, one per 1 ms USB frame. The rate is
set over EP0 with a vendor `SET_FEATURE` (bmRequestType `0x41`, bRequest `0x03`):
`wIndex 0` selects a sample rate by index into `{8000, 11025, 16000, 22050,
24000}` Hz (`wValue = 0x0100 | index`), `wIndex 1` toggles AGC. At 11.025 and
22.05 kHz some frames carry one extra sample (`hawk_rates[]` in
`main/usb_descriptors.c` mirrors the driver's own table).

Descriptors are byte-faithful to Microsoft's USB simulator of the device
(`usbsim` in the leak); the Hawk class driver matches by interface class and
endpoint direction, not VID/PID. Prior art:
[Ryzee119/hawk](https://github.com/Ryzee119/hawk) (communicator firmware) and
xemu's `usb-xblc` device.

## Layout

- `main/` — ESP-IDF firmware: descriptors (`usb_descriptors.c`), the custom
  TinyUSB class driver owning both iso endpoints + the vendor EP0 requests
  (`hawk_class.c`), the tone source (`tone.c`), optional WiFi UDP log mirror
  (`wifi_log.c`, Kconfig-gated off).
- `xbox/HawkLoopback/` — RXDK test app: mic → headphone **and** mic → TV
  speakers, with a per-second level/frequency printout (see its readme).
- `patches/` — the dwc2 EP0-SETUP-DMA fix for `esp_tinyusb`, auto-applied at
  configure time (inherited from Falcon).

## Build & flash (firmware)

ESP-IDF ≥ 5.5, ESP32-S3. The **native USB port is the communicator**, so
flash/log over the **UART bridge**:

```bash
idf.py set-target esp32s3
idf.py -p COM3 build flash monitor
```

Plug the ESP32's native USB into a controller's communicator (top expansion)
slot — the Xbox's Hawk driver expects the device behind the controller's
internal hub, and rejects the bottom slot. The heartbeat line on the monitor
shows enumeration state, the rate the Xbox selected, mic packets flowing out,
and the level/±frequency of headphone audio coming back.

WiFi logging (UDP broadcast of the log, port 5556) is **off by default** — on
Falcon the radio load broke the Xbox's timing-strict enumeration. Enable
`HAWK_WIFI_LOG` in menuconfig for bench debugging only (needs
`main/wifi_creds.h`, copy the example).

## Build & run (Xbox test app)

`xbox/HawkLoopback` is a standard RXDK project: open the folder in VS Code
(RXDK extension) or VS20XX and press F5 — it builds, deploys over xbdm, and
launches. Watch the "Xbox Title" output for the per-port frequency estimate.

The same app also works against **xemu** (Input → attach an Xbox Live
Communicator) and against a **real communicator** — speak into it and you'll
hear yourself on the TV.
