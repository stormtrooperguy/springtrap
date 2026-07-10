# Springtrap Eye Controller

ESP32 firmware for controlling the animated eyes and mouth of a Halloween animatronic. Both eyes share a single chain of 14 WS2812B LEDs (7 per eye). One servo moves both eyes left and right, and a second servo opens and closes the mouth. Runs autonomously on a random schedule and can also be controlled remotely from a tablet over a web interface served via WiFi access point.

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

## Web Interface

Springtrap is the hub of the rig: it always hosts the `fazbear_sec` AP at `192.168.4.1` and never joins another network. Connect to `fazbear_sec` and navigate to **http://192.168.4.1** (or **http://springtrap.local** via mDNS, on platforms that support it).

**Static-IP reservation:** the softAP's DHCP server is constrained to hand out `192.168.4.100–.150`, leaving `.2–.99` free for satellites that assign themselves a static address (cupcake pins itself to `192.168.4.2`). Without this, the DHCP server's default pool starts at `.2` and would hand that address to some other client, colliding with cupcake — which is exactly what happened before the pool was moved up. Any future statically-addressed satellite should also use an address in `.2–.99`.

The password is defined in `src/secrets.h` (gitignored). Copy `src/secrets.h.example` to `src/secrets.h` and set your own password before building:

```cpp
#define AP_PASSWORD "yourpassword"
```

| Control | What it does |
|---|---|
| **ERROR / REBOOT** button | Manually triggers the error/reboot routine on demand, regardless of Auto Loop state. Ignored if the routine is already running. |
| **Auto Loop** toggle | On (default): the error/reboot routine also fires automatically on a random 3–5 minute schedule, matching the original always-on behavior. Off: eye movement keeps running, but error/reboot only fires when triggered manually. |

The status bar shows the current routine (`Eye Movement` / `Error / Reboot`) and Auto Loop state, updated live via server-sent events — no polling.

## Behavior

The firmware is built around two routines. Only one runs at a time; error/reboot always runs to completion once started (a re-trigger while it's already running is ignored), then control returns to eye movement.

### Eye movement routine (idle)
Eyes are lit white and the mouth rests open. Every 8–15 seconds the eyes look left, then right, then return to center. Every 2–3 seconds, one or both eyes briefly flicker on and off 2–4 times, then return to white, simulating a malfunctioning animatronic.

### Error/reboot routine
Triggered either automatically (Auto Loop on, every 3–5 minutes) or manually from the web interface. Eyes go dark briefly, then for 8 seconds a red comet (bright head with a short fading tail) spins continuously around both rings at full brightness — then eyes and mouth go still and dark, followed immediately by the reboot chase. Through the blackout and red chase, the eye servo sweeps continuously left/right, recentering during the brief fadeout before reboot; the mouth chomps the whole time: snaps open as fast as the servo can move, holds open briefly, snaps shut, and immediately repeats. The reboot phase then goes dark for ~800ms, a single white pixel chases around each ring for 6 full rotations, and the eyes come back on steady white before handing control back to the eye movement routine.

Running the eye sweep, mouth chomp, and full-brightness LED chase simultaneously draws enough current to brown out an underpowered supply — this was confirmed on a marginal setup and fixed by upgrading to a well-regulated 5V/6A supply. If the sequence starts cutting short, suspect power first.

### Cross-device coordination
Springtrap drives cupcake in sync with its error/reboot routine, hitting cupcake directly at its known static IP (`192.168.4.2`, whether cupcake is on `fazbear_sec` or its own AP):

- **When the routine starts** (during the initial blackout): `GET /a/eye_red` then `GET /a/bite_multi` — cupcake's eyes go red and it chomps repeatedly for ~8 seconds, matching springtrap's jaw flapping, rather than a single snap.
- **When the reboot completes**: `GET /a/eye_yellow` — cupcake's eyes return to their normal yellow as springtrap recovers.

All of these are best-effort: if cupcake isn't connected to `fazbear_sec` or doesn't respond within ~500ms each, springtrap logs it to serial and proceeds with its own routine regardless — nothing here can block or fail the local sequence.

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

#define MOUTH_CLOSED     60
#define MOUTH_OPEN        0
```

### Brightness
```cpp
#define BRIGHTNESS_NORMAL     200   // eye movement routine
#define BRIGHTNESS_ERROR      255   // full brightness during the red error chase
```

### Timing
```cpp
#define GLITCH_MIN_MS       2000UL   // minimum time between glitches
#define GLITCH_MAX_MS       3000UL   // maximum time between glitches
#define ERROR_MIN_MS   (3UL * 60 * 1000)   // 3 minutes — auto-loop schedule
#define ERROR_MAX_MS   (5UL * 60 * 1000)   // 5 minutes — auto-loop schedule
#define LOOK_MIN_MS         8000UL   // minimum time between look-arounds
#define LOOK_MAX_MS        15000UL   // maximum time between look-arounds
#define ERROR_CHASE_STEP_MS   60UL   // ms per red chase frame during error
#define ERROR_CHASE_TAIL_LEN    3    // comet length in pixels, including the head
#define ERROR_LOOK_HOLD_MS   600UL   // how long the eyes hold each side during the error sweep
#define MOUTH_OPEN_HOLD_MS   500UL   // how long the mouth stays open before snapping shut
#define MOUTH_CLOSE_HOLD_MS  500UL   // how long it stays closed before reopening
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

`MOUTH_OPEN`, `MOUTH_CLOSED`, `MOUTH_OPEN_HOLD_MS`, and `MOUTH_CLOSE_HOLD_MS`
only take effect during the error/reboot routine, which triggers randomly
every 3–5 minutes (or on demand from the web interface, but that still means
flashing the full firmware to test) — impractical to tune by repeatedly
waiting for it live. Instead, [`tools/mouth_tune/main.cpp`](tools/mouth_tune/main.cpp)
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
| `<angle>` | Snap the servo to `<angle>` (0–180) |
| `open` | Save the last angle as `MOUTH_OPEN` and move there |
| `closed` | Save the last angle as `MOUTH_CLOSED` and move there |
| `chomp` | Toggle a continuous chomp loop (open, hold, snap shut, hold, repeat), to preview the error-mode look |
| `chomp <ms>` | Set the open-hold duration (ms) and start the loop |
| `close <ms>` | Set the closed-hold duration (ms) and start the loop |
| `print` | Print `#define` lines ready to paste into `src/main.cpp` |

Movement is always `Servo::write()` at full speed — open and close are
commanded as instantaneous snaps. But the servo still takes real time to
physically travel there (roughly 1.5–3ms per degree for a typical hobby
servo), so both the open-hold and closed-hold durations need to be at least
that long — too short and the servo gets recalled to the other position
mid-swing, before it ever reaches the target, which looks like a small
partial movement instead of a full chomp. If that happens, raise the
relevant hold with `chomp <ms>` / `close <ms>` until each side completes
its full travel.

Once you're happy with the feel, copy the printed values into the
`MOUTH_OPEN` / `MOUTH_CLOSED` / `MOUTH_OPEN_HOLD_MS` / `MOUTH_CLOSE_HOLD_MS`
`#define`s in [`src/main.cpp`](src/main.cpp), then reflash the main firmware
with `pio run -t upload` (no `-e` needed — `esp32dev` is the default
environment).
