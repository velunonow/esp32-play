// Focus Dock — an ESP32 desk companion.
//
//   * Focus timer (Pomodoro) on a rotary knob, with an LED ring as the countdown
//   * Real CO2 / temperature / humidity monitor (SCD41) with "open a window" alerts
//   * mmWave presence: stand-up reminders, auto-pause when you walk off, screen sleep
//   * NTP clock and a small web dashboard at http://focusdock.local
//
// Settings live in include/config.h.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>

#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>
#include <SensirionI2cScd4x.h>
#include <WiFiManager.h>

#include "config.h"
#include "webpage.h"

// ------------------------------------------------------------ Hardware ----
#if OLED_IS_SSD1306
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif
Adafruit_NeoPixel ring(RING_PIXELS, PIN_RING, NEO_GRB + NEO_KHZ800);
SensirionI2cScd4x scd;
WebServer server(80);
WiFiManager wm;
Preferences prefs;

// --------------------------------------------------------------- State ----
enum Mode : uint8_t { IDLE, FOCUS, PAUSED, BREAK };
static const char *const MODE_NAMES[] = {"idle", "focus", "paused", "break"};

static Mode mode = IDLE;
static uint32_t totalMs = 0;       // length of the running focus/break block
static uint32_t remainMs = 0;      // what is left of it
static uint32_t focusedMs = 0;     // time actually spent focusing in this block
static uint32_t lastTickMs = 0;
static bool autoPaused = false;    // paused by the radar, not by the user
static uint8_t cycle = 0;          // focus blocks finished since power-up

static uint8_t focusMin = FOCUS_DEFAULT_MIN;
static bool focusDirty = false;
static uint32_t focusDirtyMs = 0;
static bool muted = false;

static uint16_t sessionsToday = 0;
static uint16_t minutesToday = 0;
static int32_t dayKey = 0;         // year*1000 + day-of-year the stats belong to

// Air
static bool scdOk = false;
static bool airValid = false;
static uint16_t co2 = 0;
static float tempC = 0, humidity = 0;
static bool co2Alerted = false;

// CO2 history: one averaged point every 5 minutes, 24 hours deep.
static const uint16_t HIST_N = 288;
static const uint32_t HIST_INTERVAL_MS = 300000UL;
static uint16_t hist[HIST_N];
static uint16_t histCount = 0, histHead = 0;
static uint32_t histSum = 0, histSamples = 0, histLastMs = 0;

// Presence
static bool present = true;
static bool seated = false;
static uint32_t lastSeenMs = 0;
static uint32_t seatedSinceMs = 0;
static uint32_t nextStandMs = 0;
static uint32_t bannerUntilMs = 0; // "stand up" banner is showing until then
static bool bannerActive = false;

// Network
static bool servicesUp = false;
static uint32_t lastReconnectMs = 0;

// Display
static bool displayOn = true;

// --------------------------------------------------------------- Sound ----
static void beep(uint16_t freq, uint16_t ms) {
  if (!muted) tone(PIN_BUZZER, freq, ms);  // queued, non-blocking on ESP32
}
static void chimeStart()     { beep(1047, 70);  beep(1568, 110); }
static void chimeFocusDone() { beep(1319, 120); beep(1568, 120); beep(2093, 220); }
static void chimeBreakDone() { beep(1568, 120); beep(1175, 200); }
static void chimeStand()     { beep(880, 90);   beep(880, 90);   beep(1175, 180); }
static void chimeAir()       { beep(660, 250); }
static void click()          { beep(3000, 8); }

// ------------------------------------------------------------- Encoder ----
static volatile int8_t encAcc = 0;
static volatile int16_t encSteps = 0;
static volatile uint8_t encPrev = 3;
static const DRAM_ATTR int8_t ENC_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1,
                                               -1, 0, 0, 1, 0, 1, -1, 0};

// Quadrature decode with a transition table. Bounces cancel themselves out and
// a step is only counted when the knob comes to rest in a detent (both high).
static void IRAM_ATTR encoderIsr() {
  uint8_t cur = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  encAcc += ENC_TABLE[(encPrev << 2) | cur];
  encPrev = cur;
  if (cur == 3) {
    if (encAcc >= 2) encSteps++;
    else if (encAcc <= -2) encSteps--;
    encAcc = 0;
  }
}

static int readEncoder() {
  noInterrupts();
  int s = encSteps;
  encSteps = 0;
  interrupts();
  return ENC_REVERSE ? -s : s;
}

enum BtnEvent : uint8_t { BTN_NONE, BTN_SHORT, BTN_LONG };

static BtnEvent readButton(uint32_t now) {
  static bool stable = HIGH, last = HIGH, longFired = false;
  static uint32_t changedMs = 0, downMs = 0;
  bool raw = digitalRead(PIN_ENC_SW);
  if (raw != last) { last = raw; changedMs = now; }
  if (now - changedMs > 25 && raw != stable) {
    stable = raw;
    if (stable == LOW) { downMs = now; longFired = false; }
    else if (!longFired) return BTN_SHORT;
  }
  if (stable == LOW && !longFired && now - downMs > 800) {
    longFired = true;
    return BTN_LONG;
  }
  return BTN_NONE;
}

// --------------------------------------------------------------- Stats ----
static bool timeValid() { return time(nullptr) > 1700000000; }

static void saveStats() {
  prefs.putInt("day", dayKey);
  prefs.putUShort("sessions", sessionsToday);
  prefs.putUShort("minutes", minutesToday);
}

// Reset the daily counters when the calendar day changes.
static void checkDayRollover() {
  if (!timeValid()) return;
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  int32_t key = (tm.tm_year + 1900) * 1000 + tm.tm_yday;
  if (key != dayKey) {
    dayKey = key;
    sessionsToday = 0;
    minutesToday = 0;
    saveStats();
  }
}

// --------------------------------------------------------------- Timer ----
static void startFocus(uint32_t now) {
  mode = FOCUS;
  totalMs = remainMs = (uint32_t)focusMin * 60000UL;
  focusedMs = 0;
  lastTickMs = now;
  autoPaused = false;
  chimeStart();
}

static void startBreak(uint32_t now) {
  bool longBreak = (cycle % SESSIONS_PER_LONG_BREAK) == 0;
  mode = BREAK;
  totalMs = remainMs = (uint32_t)(longBreak ? LONG_BREAK_MIN : SHORT_BREAK_MIN) * 60000UL;
  lastTickMs = now;
}

static void finishFocus(uint32_t now) {
  cycle++;
  sessionsToday++;
  minutesToday += (focusedMs + 30000UL) / 60000UL;
  saveStats();
  chimeFocusDone();
  startBreak(now);
}

static void updateTimer(uint32_t now) {
  uint32_t dt = now - lastTickMs;
  lastTickMs = now;
  if (mode != FOCUS && mode != BREAK) return;
  uint32_t used = dt < remainMs ? dt : remainMs;
  remainMs -= used;
  if (mode == FOCUS) focusedMs += used;
  if (remainMs > 0) return;
  if (mode == FOCUS) {
    finishFocus(now);
  } else {
    chimeBreakDone();
    mode = IDLE;
  }
}

// --------------------------------------------------------------- Input ----
static void handleInput(uint32_t now) {
  int steps = readEncoder();
  BtnEvent btn = readButton(now);
  if (steps == 0 && btn == BTN_NONE) return;

  lastSeenMs = now;  // touching the knob proves somebody is here

  if (bannerActive) {  // any input just dismisses the reminder
    bannerActive = false;
    return;
  }

  if (steps != 0) {
    if (mode == IDLE) {
      int v = focusMin + steps * FOCUS_STEP_MIN;
      focusMin = constrain(v, FOCUS_MIN_MIN, FOCUS_MAX_MIN);
      focusDirty = true;
      focusDirtyMs = now;
    } else if (mode == FOCUS || mode == PAUSED) {
      // "five more minutes": nudge the running session a minute per click
      int32_t r = (int32_t)remainMs + steps * 60000L;
      remainMs = constrain(r, 60000L, 120L * 60000L);
      if (remainMs > totalMs) totalMs = remainMs;
    }
  }

  if (btn == BTN_SHORT) {
    switch (mode) {
      case IDLE:   startFocus(now); break;
      case FOCUS:  mode = PAUSED; autoPaused = false; click(); break;
      case PAUSED: mode = FOCUS; autoPaused = false; click(); break;
      case BREAK:  mode = IDLE; click(); break;  // skip the break
    }
  } else if (btn == BTN_LONG) {
    if (mode == IDLE) {
      muted = !muted;
      prefs.putBool("muted", muted);
      beep(muted ? 500 : 1500, 60);  // silent when just muted, audible when unmuted
    } else {
      mode = IDLE;  // abandon the session; it is not counted
      beep(400, 150);
    }
  }
}

// ------------------------------------------------------------ Presence ----
static void updatePresence(uint32_t now) {
#if HAS_PRESENCE
  if (digitalRead(PIN_PRESENCE)) lastSeenMs = now;
  uint32_t awayMs = now - lastSeenMs;
  present = awayMs < 5000;

  if (present && !seated) {
    seated = true;
    seatedSinceMs = now;
    nextStandMs = now + STAND_AFTER_MIN * 60000UL;
  } else if (seated && awayMs > AWAY_RESET_SEC * 1000UL) {
    seated = false;
    bannerActive = false;
  }

  // Walked away mid-session: stop the clock, restart it when they are back.
  if (mode == FOCUS && awayMs > AUTO_PAUSE_SEC * 1000UL) {
    mode = PAUSED;
    autoPaused = true;
  } else if (mode == PAUSED && autoPaused && present) {
    mode = FOCUS;
    autoPaused = false;
    click();
  } else if (mode == PAUSED && autoPaused && awayMs > 30UL * 60000UL) {
    mode = IDLE;  // never came back: drop the session so the screen can sleep
  }

  // Sitting too long. Never interrupt a focus block; the break that follows
  // it is the reminder.
  if (seated && mode != FOCUS && (int32_t)(now - nextStandMs) >= 0) {
    nextStandMs = now + STAND_REPEAT_MIN * 60000UL;
    bannerActive = true;
    bannerUntilMs = now + 30000UL;
    chimeStand();
  }
  if (bannerActive && (int32_t)(now - bannerUntilMs) >= 0) bannerActive = false;
#else
  (void)now;
#endif
}

static bool shouldDisplayBeOn(uint32_t now) {
#if HAS_PRESENCE
  return mode != IDLE || (now - lastSeenMs) < DISPLAY_SLEEP_SEC * 1000UL;
#else
  (void)now;
  return true;
#endif
}

// ----------------------------------------------------------------- Air ----
static void setupSensor() {
  scd.begin(Wire, SCD41_I2C_ADDR_62);
  scd.wakeUp();
  scd.stopPeriodicMeasurement();  // in case the ESP32 rebooted but the sensor did not
  scdOk = scd.startPeriodicMeasurement() == 0;
  if (!scdOk) Serial.println("SCD4x not found - check wiring (SDA 21, SCL 22)");
}

static void pushHistory(uint16_t v) {
  hist[histHead] = v;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}

static void updateSensor(uint32_t now) {
  static uint32_t lastPollMs = 0;
  if (!scdOk || now - lastPollMs < 1000) return;
  lastPollMs = now;

  bool ready = false;
  if (scd.getDataReadyStatus(ready) != 0 || !ready) return;
  uint16_t c;
  float t, h;
  if (scd.readMeasurement(c, t, h) != 0 || c == 0) return;
  co2 = c;
  tempC = t;
  humidity = h;
  airValid = true;

  histSum += c;
  histSamples++;
  if (now - histLastMs >= HIST_INTERVAL_MS) {
    pushHistory(histSum / histSamples);
    histSum = histSamples = 0;
    histLastMs = now;
  }

  // One chirp per excursion, with hysteresis, and never during a focus block.
  if (co2 >= CO2_BAD_PPM && !co2Alerted && mode != FOCUS && present) {
    co2Alerted = true;
    chimeAir();
  } else if (co2 < CO2_BAD_PPM - 150) {
    co2Alerted = false;
  }
}

static const char *airWord() {
  if (!airValid) return "warming up";
  if (co2 < CO2_GOOD_PPM) return "fresh";
  if (co2 < CO2_STUFFY_PPM) return "ok";
  if (co2 < CO2_BAD_PPM) return "stuffy";
  return "open window";
}

// ---------------------------------------------------------------- Ring ----
static void fillRing(uint8_t r, uint8_t g, uint8_t b, float scale) {
  uint32_t c = ring.Color(r * scale, g * scale, b * scale);
  for (uint8_t i = 0; i < RING_PIXELS; i++) ring.setPixelColor(i, c);
}

// The ring empties as the block runs down; the leading pixel fades out.
static void drawProgress(uint8_t r, uint8_t g, uint8_t b, float scale) {
  float lit = totalMs ? (float)remainMs / totalMs * RING_PIXELS : 0;
  for (uint8_t i = 0; i < RING_PIXELS; i++) {
    float k = constrain(lit - i, 0.0f, 1.0f) * scale;
    ring.setPixelColor(i, ring.Color(r * k, g * k, b * k));
  }
}

static void renderRing(uint32_t now) {
  if (!displayOn) {
    ring.clear();
    ring.show();
    return;
  }
  float breath = (sinf(now / 1000.0f * 1.5f) + 1.0f) * 0.5f;  // 0..1, ~4 s period
  switch (mode) {
    case FOCUS:
      drawProgress(255, 60, 20, 1.0f);
      break;
    case PAUSED:
      drawProgress(255, 150, 0, (now / 600) % 2 ? 1.0f : 0.25f);
      break;
    case BREAK:
      drawProgress(20, 220, 80, 0.5f + 0.5f * breath);
      break;
    case IDLE:
      if (bannerActive)                 fillRing(80, 120, 255, (now / 300) % 2 ? 1.0f : 0.1f);
      else if (!airValid)               fillRing(30, 60, 160, 0.15f);
      else if (co2 < CO2_GOOD_PPM)      fillRing(0, 180, 80, 0.12f + 0.10f * breath);
      else if (co2 < CO2_STUFFY_PPM)    fillRing(120, 180, 0, 0.12f + 0.12f * breath);
      else if (co2 < CO2_BAD_PPM)       fillRing(255, 140, 0, 0.15f + 0.20f * breath);
      else                              fillRing(255, 0, 0, 0.20f + 0.50f * breath);
      break;
  }
  ring.show();
}

// ------------------------------------------------------------- Display ----
static void drawRight(int16_t xRight, int16_t y, const char *s) {
  oled.drawStr(xRight - oled.getStrWidth(s), y, s);
}
static void drawCentered(int16_t y, const char *s) {
  oled.drawStr((128 - oled.getStrWidth(s)) / 2, y, s);
}

static void drawIdle() {
  char buf[32];

  // Clock
  oled.setFont(u8g2_font_logisoso24_tn);
  if (timeValid()) {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  } else {
    strcpy(buf, "--:--");
  }
  oled.drawStr(0, 26, buf);

  // Next session length (what the knob adjusts)
  oled.setFont(u8g2_font_5x8_tr);
  drawRight(128, 8, "FOCUS");
  oled.setFont(u8g2_font_6x12_tf);
  snprintf(buf, sizeof(buf), "%u min", focusMin);
  drawRight(128, 22, buf);

  oled.drawHLine(0, 31, 128);

  // Air
  oled.setFont(u8g2_font_helvB12_tr);
  if (airValid) snprintf(buf, sizeof(buf), "%u", co2);
  else strcpy(buf, "---");
  oled.drawStr(0, 47, buf);
  int16_t x = oled.getStrWidth(buf) + 3;
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(x, 47, "ppm");
  oled.setFont(u8g2_font_6x12_tf);
  drawRight(128, 46, airWord());

  // Bottom line: climate on the left, today's tally on the right
  if (wm.getConfigPortalActive()) {
    oled.drawStr(0, 62, "WiFi: " SETUP_AP_NAME);
    return;
  }
  if (airValid) {
    snprintf(buf, sizeof(buf), "%.1f\xb0" "C %.0f%%", tempC, humidity);
    oled.drawStr(0, 62, buf);
  }
  snprintf(buf, sizeof(buf), "%s%ux", muted ? "mute " : "", sessionsToday);
  drawRight(128, 62, buf);
}

static void drawTimer() {
  char buf[32];
  oled.setFont(u8g2_font_6x12_tf);
  if (mode == BREAK) {
    oled.drawStr(0, 10, "BREAK");
  } else if (mode == PAUSED) {
    if ((millis() / 600) % 2) oled.drawStr(0, 10, autoPaused ? "AWAY" : "PAUSED");
  } else {
    snprintf(buf, sizeof(buf), "FOCUS %u/%u",
             (cycle % SESSIONS_PER_LONG_BREAK) + 1, SESSIONS_PER_LONG_BREAK);
    oled.drawStr(0, 10, buf);
  }
  if (airValid) {
    snprintf(buf, sizeof(buf), "CO2 %u", co2);
    drawRight(128, 10, buf);
  }

  uint32_t secs = (remainMs + 999) / 1000;
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
  oled.setFont(u8g2_font_logisoso32_tn);
  drawCentered(50, buf);

  oled.drawFrame(0, 57, 128, 7);
  if (totalMs) oled.drawBox(2, 59, (uint32_t)124 * (totalMs - remainMs) / totalMs, 3);
}

static void drawBanner(uint32_t now) {
  char buf[32];
  oled.drawFrame(0, 0, 128, 64);
  oled.setFont(u8g2_font_helvB14_tr);
  drawCentered(26, "Stand up!");
  oled.setFont(u8g2_font_6x12_tf);
  snprintf(buf, sizeof(buf), "seated %lu min", (unsigned long)((now - seatedSinceMs) / 60000UL));
  drawCentered(42, buf);
  drawCentered(57, "walk, stretch, water");
}

static void renderOled(uint32_t now) {
  bool on = shouldDisplayBeOn(now);
  if (on != displayOn) {
    displayOn = on;
    oled.setPowerSave(on ? 0 : 1);
  }
  if (!displayOn) return;
  oled.clearBuffer();
  if (bannerActive) drawBanner(now);
  else if (mode == IDLE) drawIdle();
  else drawTimer();
  oled.sendBuffer();
}

// ----------------------------------------------------------------- Web ----
static void handleStatus() {
  char buf[420];
  snprintf(buf, sizeof(buf),
           "{\"mode\":\"%s\",\"remaining_s\":%lu,\"total_s\":%lu,\"focus_min\":%u,"
           "\"air_valid\":%s,\"co2\":%u,\"temp_c\":%.1f,\"humidity\":%.1f,\"air\":\"%s\","
           "\"present\":%s,\"seated_min\":%lu,"
           "\"sessions_today\":%u,\"focus_min_today\":%u,\"muted\":%s,\"uptime_s\":%lu}",
           MODE_NAMES[mode], (unsigned long)(remainMs / 1000), (unsigned long)(totalMs / 1000),
           focusMin, airValid ? "true" : "false", co2, tempC, humidity, airWord(),
           present ? "true" : "false",
           (unsigned long)(seated ? (millis() - seatedSinceMs) / 60000UL : 0),
           sessionsToday, minutesToday, muted ? "true" : "false",
           (unsigned long)(millis() / 1000));
  server.send(200, "application/json", buf);
}

static void handleHistory() {
  String out;
  out.reserve(64 + histCount * 5);
  out += "{\"interval_s\":300,\"co2\":[";
  uint16_t start = (histHead + HIST_N - histCount) % HIST_N;
  for (uint16_t i = 0; i < histCount; i++) {
    if (i) out += ',';
    out += hist[(start + i) % HIST_N];
  }
  out += "]}";
  server.send(200, "application/json", out);
}

static void startServices() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.cloudflare.com");
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  server.on("/", []() { server.send_P(200, "text/html", WEBPAGE); });
  server.on("/api/status", handleStatus);
  server.on("/api/history", handleHistory);
  server.begin();
  servicesUp = true;
  Serial.printf("Online: http://%s.local  (%s)\n", HOSTNAME, WiFi.localIP().toString().c_str());
}

static void updateNetwork(uint32_t now) {
  wm.process();  // runs the Wi-Fi setup portal while it is open
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !servicesUp && !wm.getConfigPortalActive()) startServices();
  if (servicesUp) server.handleClient();

  // Router was off at boot or dropped out: keep trying quietly.
  if (!connected && !wm.getConfigPortalActive() && wm.getWiFiIsSaved() &&
      now - lastReconnectMs > 60000UL) {
    lastReconnectMs = now;
    WiFi.reconnect();
  }
}

// ---------------------------------------------------------------- Main ----
void setup() {
  Serial.begin(115200);

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  pinMode(PIN_PRESENCE, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  ring.begin();
  ring.setBrightness(RING_BRIGHTNESS);
  ring.clear();
  ring.show();

  Wire.begin(PIN_SDA, PIN_SCL);
  oled.begin();
  oled.setContrast(120);
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB14_tr);
  drawCentered(30, "Focus Dock");
  oled.setFont(u8g2_font_6x12_tf);
  drawCentered(48, "starting...");
  oled.sendBuffer();

  prefs.begin("focusdock", false);
  focusMin = constrain(prefs.getUChar("focus", FOCUS_DEFAULT_MIN), FOCUS_MIN_MIN, FOCUS_MAX_MIN);
  muted = prefs.getBool("muted", false);
  dayKey = prefs.getInt("day", 0);
  sessionsToday = prefs.getUShort("sessions", 0);
  minutesToday = prefs.getUShort("minutes", 0);

  setupSensor();

  encPrev = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), encoderIsr, CHANGE);

  // Wi-Fi: joins the saved network, or opens the "FocusDock-Setup" hotspot so
  // you can pick one from your phone. Everything except the clock and the web
  // dashboard works without it. Hold the knob down while plugging in to
  // forget the saved network.
  WiFi.mode(WIFI_STA);
  wm.setHostname(HOSTNAME);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(300);
  if (digitalRead(PIN_ENC_SW) == LOW) wm.resetSettings();
  wm.autoConnect(SETUP_AP_NAME);

  uint32_t now = millis();
  lastSeenMs = lastTickMs = histLastMs = now;
  chimeStart();
}

void loop() {
  uint32_t now = millis();
  static uint32_t lastRingMs = 0, lastOledMs = 0, lastSlowMs = 0;

  updateNetwork(now);
  handleInput(now);
  updatePresence(now);
  updateTimer(now);
  updateSensor(now);

  if (now - lastRingMs >= 40) { lastRingMs = now; renderRing(now); }
  if (now - lastOledMs >= 250) { lastOledMs = now; renderOled(now); }

  if (now - lastSlowMs >= 5000) {
    lastSlowMs = now;
    checkDayRollover();
    // Save the knob setting once it has been left alone (spares the flash).
    if (focusDirty && now - focusDirtyMs > 3000) {
      prefs.putUChar("focus", focusMin);
      focusDirty = false;
    }
  }
  delay(2);
}
