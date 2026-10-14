// Copy to secrets.h (git-ignored) in this folder and fill in per device. Never commit secrets.h.
#pragma once

#define WIFI_SSID          "RCA-OFFICE"
#define WIFI_PASSWORD      "RCA@2024"

// Must match ^[A-Za-z0-9_-]{1,64}$ (the AI service rejects anything else).
#define TRAP_ID            "TRAP-PROTOTYPE-01"

// mosqai-ai base URL, no trailing slash.
#define AI_SERVER_BASE_URL "http://10.12.72.235:8000/api/v1"
// Same value as AI_SERVICE_TOKEN in mosqai-ai/.env.
#define AI_SERVICE_TOKEN   "change-me"
