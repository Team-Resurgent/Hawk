HawkLoopback — Xbox-side end-to-end test app for the Hawk emulator
==================================================================

What it does
------------
Watches all four controller ports for an Xbox Communicator (real, or the Hawk
ESP32 emulator). When one appears it:

  1. captures the microphone audio (8 kHz mono 16-bit, 40 ms packets),
  2. plays it back to the communicator's own headphone endpoint, and
  3. plays it to the TV/receiver speakers through a DirectSound stream.

It is headless: status goes to stdout (RXDK "Xbox Title" output / debug
monitor). Once a second it prints, per port, the mic level (avg/peak) and a
zero-crossing frequency estimate. With the Hawk emulator's C5-E5-G5 arpeggio
you should see f~ step through roughly 523 / 659 / 784 Hz — that plus hearing
the arpeggio on the TV proves the device -> Xbox path, and the emulator's own
heartbeat log proves the Xbox -> device (headphone) path.

Build & deploy
--------------
Open this folder in VS Code (RXDK extension) or VS20XX and press F5, or use
the CLI directly; the app deploys and launches over xbdm like any RXDK title.
