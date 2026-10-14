# MOSQAI Shield - ESP32-CAM Device Firmware Node

Firmware for the camera node of the mosquito trap. It watches the tube entrance
where the fan pulls mosquitoes in and, when something enters, sends a short burst
of photos (with a photo of the empty tube) to the `mosqai-ai` service, which
decides whether they show mosquitoes and counts them.

## Hardware Specifications & Platform Setup
This codebase is written and compiled for the **AI-Thinker ESP32-CAM development board** with an OmniVision **OV2640 camera sensor module**. **PSRAM is required** (the AI-Thinker board has it): photo bursts are held there while they upload.

### Mounting
- Point the camera at the tube entrance, against the **dark tube interior**, with the on-board LED lighting the opening: a passing mosquito then shows up bright.
- A mosquito should be at least ~10 px long in the 640x480 photo, so mount the camera close to the opening.
- Mount the camera rigidly and apart from the fan: vibration makes edges flicker and triggers motion on nothing.

### Secrets
Copy `src/mosqai_camera_node/secrets.example.h` to `src/mosqai_camera_node/secrets.h`
and fill in the Wi-Fi credentials, trap id, AI server URL and the service token
(the same value as `AI_SERVICE_TOKEN` in `mosqai-ai/.env`). `secrets.h` is
git-ignored: never commit it or put credentials in the `.ino`.

### Compilation & Flash Configurations:
1. Open `src/mosqai_camera_node/` in the **Arduino IDE** (ESP32 Arduino core 3.x).
2. Select `Tools` -> `Board` -> `ESP32 Arduino` -> **AI Thinker ESP32-CAM**.
3. Set `Partition Scheme` to `Huge APP (3MB No OTA/1MB SPIFFS)`.

Or from the command line:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32cam:PartitionScheme=huge_app" src/mosqai_camera_node
```

---

## Data Pipeline & Endpoint Communication Structure
The node needs **constant power (USB / adapter)**: it stays awake and watches continuously. Work is split over the ESP32's two cores so the camera never stops watching while photos upload:

- **Core 1, camera loop:** about 10–15 times a second, each frame is shrunk to 160x120 greyscale and compared with the background:
  - **A small area changed** (something entered): the frame that triggered and 2 more, 150 ms apart, are copied to PSRAM as one **motion event**, with a fresh `event_id` and a copy of the latest **empty-tube photo**. The event is queued; the loop is watching again within about half a second. Motion in the next 1.5 s is treated as the same insect.
  - **Most of the picture changed** (lights, shadows): reset the background, send nothing.
  - **Nothing changed**: blend the frame slowly into the background; every 20 s keep it as the empty-tube photo; if nothing was sent for an hour, queue a `heartbeat` photo so a dead trap is noticed.
- **Core 0, upload task:** sends queued events photo by photo as `POST /api/v1/trap/frame` (`multipart/form-data`, `X-Service-Token` header) with `trap_id`, `reason`, `event_id`, `captured_at` (NTP time) and `background`. Network and `5xx` errors are retried up to 3 times; a resent photo is never counted twice. Up to 6 events wait in the queue; beyond that new events are dropped (logged). Wi-Fi reconnects automatically.

On boot: start the OV2640 at **VGA (640x480)** with a short exposure (less blur on a flying insect), connect to Wi-Fi, sync the clock, check `GET /api/v1/health`, switch the white LED on at low brightness, record the empty tube, and send a `startup` photo.

The server counts each motion event once (as many mosquitoes as its best photo shows), stores in Cloudinary only the photos that showed new mosquitoes, and logs everything in Supabase. See the `mosqai-ai` README.

### Tuning (top of the `.ino`)
The Serial Monitor prints `[MOTION]: N changed pixels` every 10 s to help with this.
- `MIN_CHANGED_PIXELS` (default 4): lower to catch smaller/farther insects, raise if it triggers on nothing.
- `PIXEL_THRESHOLD` (default 30): how much darker/brighter a pixel must get to count as changed.
- `LIGHT_BRIGHTNESS` (0–255, default 40): the LED stays on, so keep it low; it gets hot at high brightness.
- `BURST_COUNT`, `BURST_INTERVAL_MS`, `EVENT_COOLDOWN_MS`: photos per event and their timing. Raise the interval if mosquitoes linger at the entrance; lower it if the fan pulls them in faster.
- `AE_LEVEL` (default -1): darker but sharper photos. If mosquitoes are blurred streaks, set `MANUAL_EXPOSURE = true` and tune `EXPOSURE_VALUE` / `GAIN_VALUE` until one is sharp and clearly visible.
- `EMPTY_SNAPSHOT_MS`, `EVENT_QUEUE_LENGTH`, `HEARTBEAT_MS`.

---

## Local Verification & Debug Testing Guide
To verify end-to-end hardware-to-AI connectivity before merging your work branch:
1. Start the AI service (`docker compose up -d --build` in `mosqai-ai`) and put the machine's LAN address in `AI_SERVER_BASE_URL`.
2. Flash this firmware to your ESP32-CAM board.
3. Keep the device plugged in and open the **Arduino Serial Monitor** at **`115200 baud`**.
4. You will see the Wi-Fi connection, the handshake and the server's response for each photo, for example:
   ```json
   {
       "trap_id": "TRAP-PROTOTYPE-01",
       "event_id": "1a2b3c4d-7",
       "timestamp": "2026-10-14T19:26:40.250000Z",
       "detected_count": 1,
       "new_mosquitoes": 1,
       "total_mosquitoes": 15,
       "image_uid": "mosqai_traps/trap_TRAP-PROTOTYPE-01/abc123",
       "model_version": "rules-v2",
       "dataset_uid": null
   }
   ```
   `401` means the token in `secrets.h` doesn't match the server's `AI_SERVICE_TOKEN`; `503` on every photo usually means `db/schema.sql` hasn't been re-run in Supabase.
5. Wave a thin dark-and-light object (a wire, a hair) through the entrance: the Serial Monitor should show `[MOTION]: Triggered...` and three `Transmitting motion photo n/3` lines.
