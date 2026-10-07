# mosqai-device

ESP32-CAM firmware for the MosqAI Shield device. It handles Wi-Fi, device
identity, secure MQTT, heartbeat, camera capture and HTTPS image upload, fan /
UV-A / CO₂ control, temperature and voltage/current monitoring, fault detection,
the RGB status LED, buzzer, physical button, manual/automatic modes, command
acknowledgement with verified state, and offline recovery.

Primary owner: Developer 2. MQTT behaviour must match
[contracts/mqtt.md](https://github.com/MosqAI-Shield/mosqai-docs/blob/develop/contracts/mqtt.md).

## Technology

C/C++ · PlatformIO · Arduino-ESP32 framework · MQTT client · esp32-camera

## Setup

> Not scaffolded yet. First M1 issue: "Initialize ESP32 firmware".

Planned: install the PlatformIO extension for VS Code, copy
`include/secrets.example.h` → `include/secrets.h` (git-ignored), then
`pio run -t upload` and `pio device monitor`.

## Environment / configuration

Wi-Fi credentials, device ID, MQTT credentials and the CA certificate are
per-device. They live in `include/secrets.h` / `certs/`, which are both
git-ignored. Production provisioning (writing credentials to NVS at
manufacturing time) is tracked as a separate issue.

## Hardware rules

- Fan, UV-A LEDs and CO₂ actuator are switched through MOSFET or driver stages. They are never driven directly from GPIO.
- After applying a command, re-read the actual state (current sense, tachometer, pin feedback) before sending the ack.
- Report what the hardware is doing, not what was requested.

## Development

The firmware is a set of non-blocking modules (`net`, `mqtt`, `commands`,
`actuators`, `sensors`, `camera`, `status_led`, `scheduler`) driven by the main
loop. Long blocking calls are not allowed, so heartbeats and acks never stall.

## Testing

Native unit tests (`pio test -e native`) cover the command parser, state machine
and schedule evaluation. Hardware-in-the-loop checks are listed per release.

## Deployment

Versioned firmware binaries are attached to GitHub Releases. OTA updates come in
a later milestone.

## Contribution workflow

Branch from `develop` (`feature/…`, `fix/…`, `refactor/…`), use Conventional
Commits, open a PR into `develop`, one approval. Full rules:
[mosqai-docs/workflow.md](https://github.com/MosqAI-Shield/mosqai-docs/blob/develop/workflow.md).
