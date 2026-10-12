# MOSQAI Shield - ESP32-CAM Device Firmware Node

This repository handles the core configuration, flash arrays, and lifecycle loop setups for the hardware-based mosquito tracking container trap node.

## Hardware Specifications & Platform Setup
This codebase is written and compiled explicitly for the **AI-Thinker ESP32-CAM development board** bundled with an OmniVision **OV2640 camera sensor module**.

### Compilation & Flash Configurations:
1. Open this directory inside the **Arduino IDE**.
2. Navigate to `Tools` -> `Board` -> `ESP32 Arduino` and select **ESP32 Wrover Module** (or *AI Thinker ESP32-CAM* depending on your core library installations).
3. Set your internal compilation properties as follows:
   * **Upload Speed:** `115200`
   * **Flash Frequency:** `80MHz`
   * **Flash Mode:** `QIO`
   * **Partition Scheme:** `Huge APP (3MB No OTA/1MB SPIFFS)` — *Required to safely store the heavy JPEG matrix array streaming allocations!*
   * **PSRAM:** `Enabled`

---

## Data Pipeline & Endpoint Communication Structure
The node runs an **Hourly Delta-Polling Framework** to maximize outdoor battery life:
1. On boot, the node initializes the OV2640 sensor configuration bounds to **VGA (640x480)** to match our YOLOv8 ONNX model constraints.
2. The board performs an immediate network handshake check against `GET /api/v1/health`.
3. Every hour, the microcontroller captures a frame, structures it into an industry-standard `multipart/form-data` payload array stream, and dispatches it over the local network via `POST /api/v1/trap/frame`.

---

## Local Verification & Debug Testing Guide
To verify end-to-end hardware-to-AI connectivity before merging your work branch:
1. Keep your local AI Docker container active (`docker run -d -p 8000:8000 --name mosqai-ai-container mosqai-ai-core`).
2. Flash this firmware code to your physical ESP32-CAM board.
3. Keep the device plugged into your computer and open the **Arduino Serial Monitor** set to **`115200 baud`**.
4. Check the logs: you will see the network connection establish, followed by an output response from the server displaying your exact target metrics JSON tracking schema:
   ```json
   {
       "image_uid": "mosqai_traps/trap_TRAP-PROTOTYPE-01/frame_1029432",
       "new_mosquitoes": 0
   }
   ```