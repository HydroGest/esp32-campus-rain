#pragma once

// Copy this file to wifi_config.h and fill in your own credentials.
// ESP32-S3 is 2.4GHz only; use the 2.4G campus SSID.
#define WIFI_SSID "SYSU-SECURE-2.4G"
#define WIFI_USERNAME "your_student_id"
#define WIFI_PASSWORD "your_password"
// PEAP outer identity. Some campus RADIUS servers want "anonymous" here.
#define WIFI_IDENTITY WIFI_USERNAME
