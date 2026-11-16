#pragma once
// Focus Dock — everything you are likely to change lives in this file.

// ---------------------------------------------------------------- Pins ----
// Classic ESP32 DevKit (ESP32-WROOM-32, 30- or 38-pin). None of these are
// strapping pins, so the board boots normally with everything attached.
#define PIN_SDA       21  // OLED + SCD41 (shared I2C bus)
#define PIN_SCL       22
#define PIN_ENC_CLK   32  // KY-040 "CLK" (A)
#define PIN_ENC_DT    33  // KY-040 "DT"  (B)
#define PIN_ENC_SW    25  // KY-040 "SW"  (push button, active low)
#define PIN_BUZZER    26  // passive buzzer signal
#define PIN_RING      27  // WS2812B ring "DI"
#define PIN_PRESENCE  34  // LD2410C "OUT" (input-only pin, high = someone there)

// ------------------------------------------------------------ Hardware ----
#define OLED_IS_SSD1306   0   // 0 = 1.3" SH1106 (default), 1 = 0.96" SSD1306
#define HAS_PRESENCE      1   // 0 if you leave out the LD2410C radar
#define ENC_REVERSE       0   // 1 if turning clockwise counts down
#define RING_PIXELS       12
#define RING_BRIGHTNESS   40  // 0-255. Keep low: easier on the eyes and on USB

// ---------------------------------------------------------------- Time ----
// POSIX TZ string. Examples:
//   "UTC0"                          UTC
//   "CST-8"                         China / Singapore (UTC+8)
//   "JST-9"                         Japan
//   "IST-5:30"                      India
//   "CET-1CEST,M3.5.0,M10.5.0/3"    Central Europe
//   "GMT0BST,M3.5.0/1,M10.5.0"      UK
//   "EST5EDT,M3.2.0,M11.1.0"        US Eastern
//   "PST8PDT,M3.2.0,M11.1.0"        US Pacific
#define TZ_INFO  "UTC0"

// --------------------------------------------------------------- Timer ----
#define FOCUS_DEFAULT_MIN        25
#define FOCUS_MIN_MIN            5
#define FOCUS_MAX_MIN            90
#define FOCUS_STEP_MIN           5
#define SHORT_BREAK_MIN          5
#define LONG_BREAK_MIN           15
#define SESSIONS_PER_LONG_BREAK  4

// ----------------------------------------------------------------- Air ----
#define CO2_GOOD_PPM    800   // below: fresh
#define CO2_STUFFY_PPM  1000  // above: concentration starts to suffer
#define CO2_BAD_PPM     1400  // above: chirp once and ask for a window

// ------------------------------------------------------------ Presence ----
#define STAND_AFTER_MIN       50   // seated this long -> "stand up" reminder
#define STAND_REPEAT_MIN      10   // nag interval until you actually leave
#define AWAY_RESET_SEC        180  // away this long counts as a real break
#define AUTO_PAUSE_SEC        60   // away this long pauses a focus session
#define DISPLAY_SLEEP_SEC     300  // nobody there this long -> screen + ring off

// ------------------------------------------------------------- Network ----
#define HOSTNAME        "focusdock"        // http://focusdock.local
#define SETUP_AP_NAME   "FocusDock-Setup"  // Wi-Fi setup hotspot name
