// Copy to secrets.h (same folder, git-ignored) and fill in per device.
// Never commit secrets.h.
#pragma once

#define WIFI_SSID        "your-wifi"
#define WIFI_PASSWORD    "your-wifi-password"

// mosqai-ai base URL, no trailing slash: http://<laptop-ip>:8000 locally,
// or the hosted https:// address.
#define AI_SERVER_URL    "http://192.168.1.10:8000"

// The camera's id in the AI service. The backend links it to a device (Device.cameraId).
#define TRAP_ID          "TRAP-PROTOTYPE-01"

// Same value as AI_SERVICE_TOKEN in mosqai-ai's .env. Sent as X-Service-Token.
#define AI_SERVICE_TOKEN "paste-the-token-here"
