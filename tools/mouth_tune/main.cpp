#include <Arduino.h>
#include <ESP32Servo.h>

// Standalone sketch for finding mouth servo angles, flap timing, and
// movement speed on the bench, without waiting for error mode to trigger
// on the full firmware.
// Build/upload with: pio run -e mouth_tune -t upload
// Then: pio device monitor -e mouth_tune

#define MOUTH_PIN 4

// Default pulse width range used by Servo::attach(pin) with no explicit
// min/max — used here so writeMicroseconds() can move in fractional
// degrees instead of the whole-degree steps Servo::write(int) is limited
// to, which is what made slow movement look stair-stepped rather than
// smooth.
#define MOUTH_PULSE_MIN_US  544
#define MOUTH_PULSE_MAX_US 2400

#define UPDATE_MS 20UL   // ~50Hz — matches the servo's own PWM refresh rate

Servo mouthServo;

int mouthOpenAngle   = 150;
int mouthClosedAngle = 90;
unsigned long flapMinMs = 120;
unsigned long flapMaxMs = 220;
float degPerSec = 300.0f;   // movement speed; lower = slower/smoother

int lastAngle = 90;
bool flapping = false;
bool flapOpen = false;
unsigned long nextFlapMs = 0;

// Current position glides toward targetAngle at degPerSec, in fractional
// degrees, instead of Servo::write() snapping there instantly.
float servoAngle  = 90;
float targetAngle = 90;
unsigned long lastStepMs = 0;

unsigned long randRange(unsigned long lo, unsigned long hi) {
    if (hi <= lo) return lo;
    return lo + (unsigned long)random((long)(hi - lo));
}

void writeMouthAngle(float angleDeg) {
    angleDeg = constrain(angleDeg, 0.0f, 180.0f);
    float us = MOUTH_PULSE_MIN_US +
        (angleDeg / 180.0f) * (MOUTH_PULSE_MAX_US - MOUTH_PULSE_MIN_US);
    mouthServo.writeMicroseconds((int)(us + 0.5f));
}

void printHelp() {
    Serial.println("Mouth servo tuner");
    Serial.println("  <angle>            move servo to <angle> (0-180) at current speed");
    Serial.println("  open               save last angle as MOUTH_OPEN and move there");
    Serial.println("  closed             save last angle as MOUTH_CLOSED and move there");
    Serial.println("  flap               toggle continuous flap test (open<->closed)");
    Serial.println("  flap <min> <max>   set flap interval bounds (ms) and start test");
    Serial.println("  speed <deg/sec>    set movement speed in degrees per second");
    Serial.println("  print              print #define lines to paste into main.cpp");
    Serial.println("  help               show this message");
}

void printValues() {
    Serial.println("---");
    Serial.printf("#define MOUTH_OPEN            %d\n", mouthOpenAngle);
    Serial.printf("#define MOUTH_CLOSED          %d\n", mouthClosedAngle);
    Serial.printf("#define MOUTH_FLAP_MIN_MS     %luUL\n", flapMinMs);
    Serial.printf("#define MOUTH_FLAP_MAX_MS     %luUL\n", flapMaxMs);
    Serial.printf("#define MOUTH_DEG_PER_SEC     %.1ff\n", degPerSec);
    Serial.println("---");
}

void handleLine(String line) {
    line.trim();
    if (line.length() == 0) return;

    if (line == "help")  { printHelp();  return; }
    if (line == "print") { printValues(); return; }

    if (line == "open") {
        mouthOpenAngle = lastAngle;
        targetAngle    = mouthOpenAngle;
        Serial.printf("MOUTH_OPEN set to %d\n", mouthOpenAngle);
        return;
    }
    if (line == "closed") {
        mouthClosedAngle = lastAngle;
        targetAngle       = mouthClosedAngle;
        Serial.printf("MOUTH_CLOSED set to %d\n", mouthClosedAngle);
        return;
    }
    if (line == "flap") {
        flapping   = !flapping;
        flapOpen   = false;
        targetAngle = mouthClosedAngle;
        if (flapping) {
            nextFlapMs = millis() + randRange(flapMinMs, flapMaxMs);
            Serial.println("Flap test started (type 'flap' again to stop)");
        } else {
            Serial.println("Flap test stopped");
        }
        return;
    }
    if (line.startsWith("flap ")) {
        int sp = line.indexOf(' ', 5);
        if (sp > 0) {
            flapMinMs = line.substring(5, sp).toInt();
            flapMaxMs = line.substring(sp + 1).toInt();
            Serial.printf("Flap interval set to %lu-%lums\n", flapMinMs, flapMaxMs);
            flapping    = true;
            flapOpen    = false;
            targetAngle = mouthClosedAngle;
            nextFlapMs  = millis() + randRange(flapMinMs, flapMaxMs);
        }
        return;
    }
    if (line.startsWith("speed ")) {
        float v = line.substring(6).toFloat();
        if (v > 0) {
            degPerSec = v;
            Serial.printf("Speed set to %.1f deg/sec\n", degPerSec);
        }
        return;
    }

    // Otherwise treat the line as an angle to glide to.
    int angle  = constrain(line.toInt(), 0, 180);
    lastAngle  = angle;
    flapping   = false;
    targetAngle = angle;
    Serial.printf("Moving to %d\n", angle);
}

void setup() {
    Serial.begin(115200);
    mouthServo.attach(MOUTH_PIN);
    servoAngle  = mouthClosedAngle;
    targetAngle = mouthClosedAngle;
    writeMouthAngle(servoAngle);
    randomSeed(analogRead(0));
    printHelp();
}

void loop() {
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        handleLine(line);
    }

    if (flapping && millis() >= nextFlapMs) {
        flapOpen    = !flapOpen;
        targetAngle = flapOpen ? mouthOpenAngle : mouthClosedAngle;
        nextFlapMs  = millis() + randRange(flapMinMs, flapMaxMs);
    }

    unsigned long now = millis();
    if (now - lastStepMs >= UPDATE_MS) {
        lastStepMs = now;
        if (servoAngle != targetAngle) {
            float step  = degPerSec * (UPDATE_MS / 1000.0f);
            float delta = targetAngle - servoAngle;
            servoAngle += (fabs(delta) <= step) ? delta : (delta > 0 ? step : -step);
            writeMouthAngle(servoAngle);
        }
    }
}
