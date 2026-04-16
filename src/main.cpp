#include <Arduino.h>
#include <FastLED.h>
#include <ESP32Servo.h>

// ---------------------------------------------------------------------------
// Hardware pin configuration — adjust after physical install
// ---------------------------------------------------------------------------
#define SERVO_PIN       13
#define LEFT_EYE_PIN    25
#define RIGHT_EYE_PIN   26
#define NUM_LEDS         7

// ---------------------------------------------------------------------------
// Servo positions (degrees) — adjust after physical install
// ---------------------------------------------------------------------------
#define SERVO_LEFT      55
#define SERVO_CENTER    90
#define SERVO_RIGHT    125

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
#define ERROR_RED_MS         5000UL  // time spent red
#define ERROR_FADEOUT_MS      200UL  // pause after red before reboot

#define REBOOT_BLACKOUT_MS    800UL  // off duration before chase
#define REBOOT_CHASE_STEP_MS   60UL  // ms per chase frame
#define REBOOT_CHASE_CYCLES     3    // how many full rotations

// ---------------------------------------------------------------------------
// LED arrays and servo
// ---------------------------------------------------------------------------
CRGB leftEye[NUM_LEDS];
CRGB rightEye[NUM_LEDS];
Servo eyeServo;

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
    eyeServo.write(SERVO_CENTER);
    setEyes(CRGB::White);
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
                setEyes(CRGB::Red);
                errorStep   = 2;
                errorStepMs = now;
            }
            break;
        case 2:
            if (now - errorStepMs >= ERROR_RED_MS) {
                setEyes(CRGB::Black);
                errorStep   = 3;
                errorStepMs = now;
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
                fill_solid(leftEye,  NUM_LEDS, CRGB::Black);
                fill_solid(rightEye, NUM_LEDS, CRGB::Black);
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

    FastLED.addLeds<WS2812B, LEFT_EYE_PIN,  GRB>(leftEye,  NUM_LEDS);
    FastLED.addLeds<WS2812B, RIGHT_EYE_PIN, GRB>(rightEye, NUM_LEDS);
    FastLED.setBrightness(200);

    eyeServo.attach(SERVO_PIN);

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
