# Springtrap Eye Controller

ESP32 firmware for controlling the animated eyes and mouth of a Halloween animatronic. Both eyes share a single chain of 14 WS2812B LEDs (7 per eye). One servo moves both eyes left and right, and a second servo opens and closes the mouth.

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32 (DevKit) |
| Eye LEDs | 2× ring of 7 WS2812B (NeoPixel) LEDs, wired as one chain |
| Eye movement | 1× servo motor |
| Mouth movement | 1× servo motor |

## Wiring

| Signal | ESP32 GPIO | Notes |
|---|---|---|
| Eye servo | 17 | Signal wire only |
| Mouth servo | 4 | Signal wire only |
| LED chain data | 16 | Via 300–500 Ω series resistor |

The LED chain runs left eye first (indices 0–6), then right eye (indices 7–13). The servos and LED rings are powered externally (5V). All grounds must be common — external supply ground and ESP32 GND tied together.

## Behavior

### Normal mode (default)
Eyes are lit white. Every 8–15 seconds the eyes look left, then right, then return to center.

### Glitch mode
Every 2–3 seconds, one or both eyes briefly flicker on and off 2–4 times, then return to white. Simulates a malfunctioning animatronic.

### Error mode
Triggered randomly every 3–5 minutes. Eyes go dark briefly, then illuminate red at full brightness for 8 seconds — during which the mouth flaps open and closed at a random, irregular interval — then eyes and mouth go still and dark. Always followed immediately by reboot mode.

### Reboot mode
Eyes are dark for ~800 ms, then a single white pixel chases around each ring for 6 full rotations, then the eyes come back on steady white at normal brightness and return to normal mode.

## Configuration

All tunable values are `#define` constants at the top of [`src/main.cpp`](src/main.cpp).

### Pin assignments
```cpp
#define SERVO_PIN       17
#define MOUTH_PIN        4
#define LED_PIN         16
```

### Servo positions (degrees)
```cpp
#define SERVO_LEFT      55
#define SERVO_CENTER   110
#define SERVO_RIGHT    145

#define MOUTH_CLOSED     90
#define MOUTH_OPEN      150
```

The mouth glides between positions rather than snapping — see `MOUTH_STEP_DELAY_MS` below.

### Brightness
```cpp
#define BRIGHTNESS_NORMAL     200   // normal operation
#define BRIGHTNESS_ERROR      255   // full brightness during red error sequence
```

### Timing
```cpp
#define GLITCH_MIN_MS       2000UL   // minimum time between glitches
#define GLITCH_MAX_MS       3000UL   // maximum time between glitches
#define ERROR_MIN_MS   (3UL * 60 * 1000)   // 3 minutes
#define ERROR_MAX_MS   (5UL * 60 * 1000)   // 5 minutes
#define LOOK_MIN_MS         8000UL   // minimum time between look-arounds
#define LOOK_MAX_MS        15000UL   // maximum time between look-arounds
#define MOUTH_FLAP_MIN_MS    120UL   // fastest mouth open/close interval during error
#define MOUTH_FLAP_MAX_MS    220UL   // slowest mouth open/close interval during error
#define MOUTH_STEP_DELAY_MS   15UL   // ms per degree of mouth movement (lower = faster)
```

## Building and Flashing

This project uses [PlatformIO](https://platformio.org/).

```bash
# Build
pio run

# Flash
pio run --target upload

# Monitor serial output
pio device monitor
```

## Tuning the mouth servo

`MOUTH_OPEN`, `MOUTH_CLOSED`, and the flap timing only take effect during error
mode, which triggers randomly every 3–5 minutes — impractical to tune by
repeatedly waiting for it live. Instead, [`tools/mouth_tune/main.cpp`](tools/mouth_tune/main.cpp)
is a standalone interactive sketch that drives the mouth servo directly from
the serial monitor:

```bash
# Build and flash the tuner instead of the main firmware
pio run -e mouth_tune -t upload
pio device monitor -e mouth_tune
```

Serial commands:

| Command | Effect |
|---|---|
| `<angle>` | Glide the servo to `<angle>` (0–180) at the current speed |
| `open` | Save the last angle as `MOUTH_OPEN` and move there |
| `closed` | Save the last angle as `MOUTH_CLOSED` and move there |
| `flap` | Toggle a continuous open/closed flap loop, to preview the error-mode look |
| `flap <min> <max>` | Set the flap interval bounds (ms) and start the loop |
| `speed <ms>` | Set ms per degree of movement — lower is faster, higher is slower/smoother |
| `print` | Print `#define` lines ready to paste into `src/main.cpp` |

Once you're happy with the feel, copy the printed values into the
`MOUTH_OPEN` / `MOUTH_CLOSED` / `MOUTH_FLAP_MIN_MS` / `MOUTH_FLAP_MAX_MS` /
`MOUTH_STEP_DELAY_MS` `#define`s in [`src/main.cpp`](src/main.cpp), then
reflash the main firmware with `pio run -t upload` (no `-e` needed —
`esp32dev` is the default environment).
