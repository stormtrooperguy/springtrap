#include <Arduino.h>
#include <ESP32Servo.h>

// Standalone sketch for finding mouth servo angles, flap timing, and
// movement speed on the bench, without waiting for error mode to trigger
// on the full firmware.
// Build/upload with: pio run -e mouth_tune -t upload
// Then: pio device monitor -e mouth_tune

#define MOUTH_PIN 4

Servo mouthServo;

int mouthOpenAngle   = 150;
int mouthClosedAngle = 90;
unsigned long flapMinMs = 120;
unsigned long flapMaxMs = 220;
unsigned long stepDelayMs = 15;   // ms per 1-degree step; lower = faster

int lastAngle = 90;
bool flapping = false;
bool flapOpen = false;
unsigned long nextFlapMs = 0;

// Current position glides toward targetAngle at stepDelayMs per degree,
// instead of Servo::write() snapping there instantly.
int servoAngle   = 90;
int targetAngle  = 90;
unsigned long lastStepMs = 0;

unsigned long randRange(unsigned long lo, unsigned long hi) {
    if (hi <= lo) return lo;
    return lo + (unsigned long)random((long)(hi - lo));
}

void printHelp() {
    Serial.println("Mouth servo tuner");
    Serial.println("  <angle>            move servo to <angle> (0-180) at current speed");
    Serial.println("  open               save last angle as MOUTH_OPEN and move there");
    Serial.println("  closed             save last angle as MOUTH_CLOSED and move there");
    Serial.println("  flap               toggle continuous flap test (open<->closed)");
    Serial.println("  flap <min> <max>   set flap interval bounds (ms) and start test");
    Serial.println("  speed <ms>         set ms per degree of movement (lower = faster)");
    Serial.println("  print              print #define lines to paste into main.cpp");
    Serial.println("  help               show this message");
}

void printValues() {
    Serial.println("---");
    Serial.printf("#define MOUTH_OPEN            %d\n", mouthOpenAngle);
    Serial.printf("#define MOUTH_CLOSED          %d\n", mouthClosedAngle);
    Serial.printf("#define MOUTH_FLAP_MIN_MS     %luUL\n", flapMinMs);
    Serial.printf("#define MOUTH_FLAP_MAX_MS     %luUL\n", flapMaxMs);
    Serial.printf("#define MOUTH_STEP_DELAY_MS   %luUL\n", stepDelayMs);
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
        long ms = line.substring(6).toInt();
        if (ms > 0) {
            stepDelayMs = ms;
            Serial.printf("Speed set to %lums per degree\n", stepDelayMs);
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
    mouthServo.write(mouthClosedAngle);
    servoAngle  = mouthClosedAngle;
    targetAngle = mouthClosedAngle;
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

    if (servoAngle != targetAngle && millis() - lastStepMs >= stepDelayMs) {
        servoAngle += (servoAngle < targetAngle) ? 1 : -1;
        mouthServo.write(servoAngle);
        lastStepMs = millis();
    }
}
