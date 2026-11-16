# Focus Dock

An ESP32 desk companion for studying and working: a knob-driven focus timer, a
real CO2 monitor, and a presence radar that reminds you to stand up.

- **Focus timer** – turn the knob to set the length, press to start. An LED ring
  drains as the session runs; a break follows automatically (5 min, 15 after every 4th).
- **Air monitor** – true CO2 (SCD41), temperature and humidity. The ring glows
  green → amber → red in the background and chirps once when it is time to open a window.
- **Presence** – an mmWave radar pauses the timer when you walk away, reminds you
  to stand after 50 minutes seated, and turns the screen off when the desk is empty.
- **Clock + dashboard** – NTP clock on the OLED and a web page at
  `http://focusdock.local` with live readings, today's tally and 24 h of CO2 history.

Works fully offline except for the clock and the dashboard.

## Parts

Search terms work on AliExpress, Amazon, Taobao, eBay and similar. Prices are
rough ballparks, not checked against live listings.

| # | Part | Search for | Approx. USD |
|---|------|------------|-------------|
| 1 | ESP32 dev board | `ESP32 DevKit ESP32-WROOM-32 USB-C` (30- or 38-pin) | 4–6 |
| 2 | CO2 sensor | `SCD41 module` (or `SCD40 module`) | 12–22 |
| 3 | Display | `1.3 inch OLED I2C SH1106 4 pin` | 3 |
| 4 | Knob | `KY-040 rotary encoder module` | 1 |
| 5 | LED ring | `WS2812B ring 12 LED` | 2 |
| 6 | Buzzer | `passive buzzer module KY-006` | 0.5 |
| 7 | Presence radar | `HLK-LD2410C` | 3–4 |
| 8 | Breadboard + jumper wires | `830 breadboard`, `dupont jumper wires M-M M-F` | 4 |
| 9 | USB cable + any 5 V / 1 A phone charger | | – |

Total: roughly **$30–45**. Optional: 470 µF capacitor and 330 Ω resistor for the
ring, a 7×9 cm perfboard and a project box if you want it permanent.

Buy a real SCD40/SCD41. Cheap "CO2" boards built on MQ-135, CCS811, SGP30 or
ENS160 only estimate CO2 from other gases and are not usable for this.

## Wiring

| Module | Module pin | ESP32 pin |
|--------|-----------|-----------|
| OLED | VCC / GND | 3V3 / GND |
| OLED | SDA / SCL | GPIO21 / GPIO22 |
| SCD41 | VCC / GND | 3V3 / GND |
| SCD41 | SDA / SCL | GPIO21 / GPIO22 (same bus as the OLED) |
| KY-040 | + / GND | **3V3** / GND (not 5 V) |
| KY-040 | CLK / DT / SW | GPIO32 / GPIO33 / GPIO25 |
| WS2812B ring | 5V / GND | 5V (VIN) / GND |
| WS2812B ring | DI | GPIO27 |
| Buzzer | S / − | GPIO26 / GND |
| LD2410C | VCC / GND | 5V (VIN) / GND |
| LD2410C | OUT | GPIO34 (TX and RX stay unconnected) |

## Flash

```bash
pip install platformio
```

Set your time zone (`TZ_INFO`) in `include/config.h`, plug the board in, then:

```bash
pio run -t upload
```

On first boot the device opens a Wi-Fi hotspot called `FocusDock-Setup`. Join it
from your phone, pick your network, and the dashboard appears at
`http://focusdock.local`. Hold the knob down while plugging in to forget the
saved network.

Arduino IDE also works: install the *esp32* board package (2.x) and the
libraries *U8g2*, *Adafruit NeoPixel*, *Sensirion I2C SCD4x* and *WiFiManager*
(tzapu), then put `main.cpp` (renamed `.ino`), `config.h` and `webpage.h` in one
sketch folder.

## Controls

| | Idle | Focus | Paused | Break |
|---|---|---|---|---|
| **Turn** | session length ±5 min | ±1 min | ±1 min | – |
| **Press** | start | pause | resume | skip break |
| **Hold** | mute / unmute | abandon | abandon | end break |

## Things to know

- **Radar range.** Out of the box the LD2410C sees the whole room. Use the
  maker's phone app (HLKRadarTool, Bluetooth) to cut the range to about 1.5 m so
  only someone at the desk counts. No radar? Set `HAS_PRESENCE 0`.
- **Ring data level.** The ESP32 sends 3.3 V data to a 5 V ring. This nearly
  always works with a short wire. If the ring flickers, power it from 3V3
  instead, or add a 74AHCT125 level shifter.
- **CO2 accuracy.** The SCD41 self-calibrates on the assumption that it sees
  fresh air (about 400 ppm) at least once a week, so air the room regularly.
  Keep it a few centimetres from the ESP32, whose heat skews the temperature.
- **OLED type.** If the picture is shifted or garbled you have an SSD1306
  panel: set `OLED_IS_SSD1306 1`.

## Status

The firmware compiles cleanly with PlatformIO (espressif32 6.x, Arduino core
2.0.17) for `esp32dev`. It has not been run on physical hardware yet, so expect
to tune small things (knob direction via `ENC_REVERSE`, brightness) on first power-up.
