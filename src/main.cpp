#include <Arduino.h>
#include <FastLED.h>
#include <ESP32Servo.h>

// ---------------------------------------------------------------------------
// Hardware pin configuration — adjust after physical install
// ---------------------------------------------------------------------------
#define SERVO_PIN       17
#define MOUTH_PIN        4
#define LED_PIN         16
#define NUM_LEDS         7   // per eye
#define TOTAL_LEDS      14   // both eyes in one chain (left=0–6, right=7–13)

// ---------------------------------------------------------------------------
// Servo positions (degrees) — adjust after physical install
// ---------------------------------------------------------------------------
#define SERVO_LEFT      55
#define SERVO_CENTER   110
#define SERVO_RIGHT    145

#define MOUTH_CLOSED     60
#define MOUTH_OPEN        0

// ---------------------------------------------------------------------------
// Timing constants
// ---------------------------------------------------------------------------
#define GLITCH_MIN_MS       2000UL
#define GLITCH_MAX_MS       3000UL

#define ERROR_MIN_MS   (3UL * 60 * 1000)   // 3 minutes
#define ERROR_MAX_MS   (5UL * 60 * 1000)   // 5 minutes

#define LOOK_MIN_MS         8000UL
#define LOOK_MAX_MS        15000UL

#define LOOK_HOLD_MS         800UL   // how long to hold each side position
#define LOOK_SETTLE_MS       400UL   // pause at center before resuming

#define GLITCH_OFF_MS          50UL  // each flicker-off duration
#define GLITCH_ON_MS           40UL  // each flicker-on gap

#define ERROR_BLACKOUT_MS     500UL  // brief off before red
#define ERROR_RED_MS         8000UL  // total time spent in the red chase
#define ERROR_FADEOUT_MS      200UL  // pause after red before reboot

#define ERROR_CHASE_STEP_MS    60UL  // ms per red chase frame
#define ERROR_CHASE_TAIL_LEN     3   // comet length in pixels, including the head

#define MOUTH_OPEN_HOLD_MS     500UL  // how long the mouth stays open before snapping shut — tune via tools/mouth_tune

// How long the mouth stays closed before reopening. Must be at least the
// servo's actual travel time for the full open<->closed span (typically
// ~1.5-3ms per degree for a hobby servo) — too short and it gets recalled
// to open mid-swing before ever reaching closed. Tune via tools/mouth_tune.
#define MOUTH_CLOSE_HOLD_MS    500UL

#define REBOOT_BLACKOUT_MS    800UL  // off duration before chase
#define REBOOT_CHASE_STEP_MS   60UL  // ms per chase frame
#define REBOOT_CHASE_CYCLES     6    // how many full rotations

#define BRIGHTNESS_NORMAL     200
#define BRIGHTNESS_ERROR      255

// ---------------------------------------------------------------------------
// LED array and servo
// Single chain: indices 0–6 = left eye, indices 7–13 = right eye
// ---------------------------------------------------------------------------
CRGB leds[TOTAL_LEDS];
Servo eyeServo;
Servo mouthServo;

// Convenience pointers into the chain
CRGB * const leftEye  = leds;
CRGB * const rightEye = leds + NUM_LEDS;

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum State {
    STATE_NORMAL,
    STATE_GLITCH,
    STATE_ERROR,
    STATE_REBOOT
};

State currentState = STATE_NORMAL;

// Scheduled trigger times
unsigned long nextGlitchMs = 0;
unsigned long nextErrorMs  = 0;
unsigned long nextLookMs   = 0;

// ---------------------------------------------------------------------------
// Look-around sub-state
// ---------------------------------------------------------------------------
bool lookingAround  = false;
int  lookStep       = 0;
unsigned long lookStepMs = 0;

// ---------------------------------------------------------------------------
// Glitch sub-state
// ---------------------------------------------------------------------------
int  glitchStep     = 0;
int  glitchCount    = 0;   // number of flicker cycles so far
int  glitchTarget   = 0;   // total flicker cycles for this glitch
unsigned long glitchStepMs = 0;
bool glitchLeft     = false;
bool glitchRight    = false;

// ---------------------------------------------------------------------------
// Error sub-state
// ---------------------------------------------------------------------------
int  errorStep      = 0;
unsigned long errorStepMs = 0;
bool mouthOpen        = false;   // chomp state: open-and-holding vs snapped shut
unsigned long mouthPhaseUntilMs = 0;
int  errorChasePos      = 0;
unsigned long errorChaseStepMs = 0;

// ---------------------------------------------------------------------------
// Reboot sub-state
// ---------------------------------------------------------------------------
int  rebootStep     = 0;
int  chasePos       = 0;
int  chaseCycles    = 0;
unsigned long rebootStepMs = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
unsigned long randRange(unsigned long lo, unsigned long hi) {
    return lo + (unsigned long)random((long)(hi - lo));
}

void showEyes(CRGB left, CRGB right) {
    fill_solid(leftEye,  NUM_LEDS, left);
    fill_solid(rightEye, NUM_LEDS, right);
    FastLED.show();
}

void setEyes(CRGB color) {
    showEyes(color, color);
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------
void scheduleNextGlitch() {
    nextGlitchMs = millis() + randRange(GLITCH_MIN_MS, GLITCH_MAX_MS);
}

void scheduleNextError() {
    nextErrorMs = millis() + randRange(ERROR_MIN_MS, ERROR_MAX_MS);
}

void scheduleNextLook() {
    nextLookMs = millis() + randRange(LOOK_MIN_MS, LOOK_MAX_MS);
}

// ---------------------------------------------------------------------------
// State entry points
// ---------------------------------------------------------------------------
void enterNormal() {
    currentState = STATE_NORMAL;
    FastLED.setBrightness(BRIGHTNESS_NORMAL);
    eyeServo.write(SERVO_CENTER);
    setEyes(CRGB::White);
    mouthServo.write(MOUTH_OPEN);   // mouth rests open by default
    lookingAround = false;
    scheduleNextGlitch();
    scheduleNextError();
    scheduleNextLook();
}

void enterGlitch() {
    currentState  = STATE_GLITCH;
    glitchStep    = 0;
    glitchCount   = 0;
    glitchTarget  = random(2, 5);   // 2–4 flicker cycles
    glitchStepMs  = millis();

    // Randomly pick which eye(s) to glitch
    int which = random(3);           // 0=left, 1=right, 2=both
    glitchLeft  = (which == 0 || which == 2);
    glitchRight = (which == 1 || which == 2);
}

void enterError() {
    currentState  = STATE_ERROR;
    errorStep     = 0;
    errorStepMs   = millis();
    lookingAround = false;
    eyeServo.write(SERVO_CENTER);
    mouthServo.write(MOUTH_OPEN);
    mouthOpen     = true;
}

void enterReboot() {
    currentState  = STATE_REBOOT;
    rebootStep    = 0;
    chasePos      = 0;
    chaseCycles   = 0;
    rebootStepMs  = millis();
}

// ---------------------------------------------------------------------------
// Look-around update (runs inside STATE_NORMAL)
// left → (hold) → right → (hold) → center → done
// ---------------------------------------------------------------------------
void updateLookAround() {
    unsigned long now = millis();
    switch (lookStep) {
        case 0:
            eyeServo.write(SERVO_LEFT);
            lookStep    = 1;
            lookStepMs  = now;
            break;
        case 1:
            if (now - lookStepMs >= LOOK_HOLD_MS) {
                eyeServo.write(SERVO_RIGHT);
                lookStep   = 2;
                lookStepMs = now;
            }
            break;
        case 2:
            if (now - lookStepMs >= LOOK_HOLD_MS) {
                eyeServo.write(SERVO_CENTER);
                lookStep   = 3;
                lookStepMs = now;
            }
            break;
        case 3:
            if (now - lookStepMs >= LOOK_SETTLE_MS) {
                lookingAround = false;
                scheduleNextLook();
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Glitch update
// Flickers chosen eye(s) on/off glitchTarget times, then restores white.
// ---------------------------------------------------------------------------
void updateGlitch() {
    unsigned long now = millis();
    switch (glitchStep) {
        case 0: {   // turn off glitched eye(s)
            CRGB l = glitchLeft  ? CRGB::Black : CRGB::White;
            CRGB r = glitchRight ? CRGB::Black : CRGB::White;
            showEyes(l, r);
            glitchStep   = 1;
            glitchStepMs = now;
            break;
        }
        case 1:     // hold off
            if (now - glitchStepMs >= GLITCH_OFF_MS) {
                setEyes(CRGB::White);
                glitchStep   = 2;
                glitchStepMs = now;
            }
            break;
        case 2:     // brief on, then decide whether to flicker again
            if (now - glitchStepMs >= GLITCH_ON_MS) {
                glitchCount++;
                if (glitchCount < glitchTarget) {
                    glitchStep = 0;   // another flicker cycle
                } else {
                    // Done — restore and return to normal
                    setEyes(CRGB::White);
                    currentState = STATE_NORMAL;
                    scheduleNextGlitch();
                }
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Error update
// Brief blackout → red for 5 s → off → enter reboot
// ---------------------------------------------------------------------------
void updateError() {
    unsigned long now = millis();
    switch (errorStep) {
        case 0:
            setEyes(CRGB::Black);
            errorStep   = 1;
            errorStepMs = now;
            break;
        case 1:
            if (now - errorStepMs >= ERROR_BLACKOUT_MS) {
                FastLED.setBrightness(BRIGHTNESS_ERROR);
                errorStep       = 2;
                errorStepMs     = now;
                errorChasePos   = 0;
                errorChaseStepMs = now;
                mouthOpen        = false;
                mouthPhaseUntilMs = now;   // triggers an immediate chomp on the next iteration
            }
            break;
        case 2:
            if (now - errorStepMs >= ERROR_RED_MS) {
                setEyes(CRGB::Black);
                mouthServo.write(MOUTH_OPEN);   // back to the default open rest position
                mouthOpen   = true;
                errorStep   = 3;
                errorStepMs = now;
                break;
            }
            // Red comet chase: a bright head with a short fading tail spins
            // continuously around both rings for the whole red phase.
            if (now - errorChaseStepMs >= ERROR_CHASE_STEP_MS) {
                fill_solid(leds, TOTAL_LEDS, CRGB::Black);
                for (int t = 0; t < ERROR_CHASE_TAIL_LEN; t++) {
                    int pos = (errorChasePos - t + NUM_LEDS) % NUM_LEDS;
                    CRGB c  = CRGB::Red;
                    c.nscale8(255 / (t + 1));
                    leftEye[pos]  = c;
                    rightEye[pos] = c;
                }
                FastLED.show();
                errorChasePos    = (errorChasePos + 1) % NUM_LEDS;
                errorChaseStepMs = now;
            }
            // Chomp: snap open as fast as possible, hold, snap shut, hold
            // briefly (see MOUTH_CLOSE_HOLD_MS), repeat.
            if (now >= mouthPhaseUntilMs) {
                if (mouthOpen) {
                    mouthServo.write(MOUTH_CLOSED);
                    mouthOpen         = false;
                    mouthPhaseUntilMs = now + MOUTH_CLOSE_HOLD_MS;
                } else {
                    mouthServo.write(MOUTH_OPEN);
                    mouthOpen         = true;
                    mouthPhaseUntilMs = now + MOUTH_OPEN_HOLD_MS;
                }
            }
            break;
        case 3:
            if (now - errorStepMs >= ERROR_FADEOUT_MS) {
                enterReboot();
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Reboot update
// Blackout → chase (single pixel laps ring 3×) → steady white → normal
// ---------------------------------------------------------------------------
void updateReboot() {
    unsigned long now = millis();
    switch (rebootStep) {
        case 0:
            setEyes(CRGB::Black);
            rebootStep   = 1;
            rebootStepMs = now;
            break;
        case 1:
            if (now - rebootStepMs >= REBOOT_BLACKOUT_MS) {
                rebootStep   = 2;
                chasePos     = 0;
                chaseCycles  = 0;
                rebootStepMs = now;
            }
            break;
        case 2: {   // chase frame
            if (now - rebootStepMs >= REBOOT_CHASE_STEP_MS) {
                fill_solid(leds, TOTAL_LEDS, CRGB::Black);
                leftEye[chasePos]  = CRGB::White;
                rightEye[chasePos] = CRGB::White;
                FastLED.show();

                chasePos++;
                if (chasePos >= NUM_LEDS) {
                    chasePos = 0;
                    chaseCycles++;
                    if (chaseCycles >= REBOOT_CHASE_CYCLES) {
                        rebootStep   = 3;
                        rebootStepMs = now;
                        break;
                    }
                }
                rebootStepMs = now;
            }
            break;
        }
        case 3:
            setEyes(CRGB::White);
            enterNormal();
            break;
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("Springtrap eye controller starting...");

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, TOTAL_LEDS);
    FastLED.setBrightness(200);

    eyeServo.attach(SERVO_PIN);
    mouthServo.attach(MOUTH_PIN);   // enterNormal() below sets the resting position

    randomSeed(analogRead(0));

    enterNormal();
    Serial.println("Ready.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
    unsigned long now = millis();

    switch (currentState) {
        case STATE_NORMAL:
            // Error takes priority over glitch
            if (now >= nextErrorMs) {
                enterError();
                break;
            }
            if (now >= nextGlitchMs) {
                enterGlitch();
                break;
            }
            // Look-around (doesn't block glitch/error checks next iteration)
            if (!lookingAround && now >= nextLookMs) {
                lookingAround = true;
                lookStep      = 0;
            }
            if (lookingAround) {
                updateLookAround();
            }
            break;

        case STATE_GLITCH:
            updateGlitch();
            break;

        case STATE_ERROR:
            updateError();
            break;

        case STATE_REBOOT:
            updateReboot();
            break;
    }
}
