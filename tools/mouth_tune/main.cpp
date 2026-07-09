#include <Arduino.h>
#include <ESP32Servo.h>

// Standalone sketch for finding mouth servo angles and open-hold timing on
// the bench, without waiting for error mode to trigger on the full firmware.
// Build/upload with: pio run -e mouth_tune -t upload
// Then: pio device monitor -e mouth_tune

#define MOUTH_PIN 4

Servo mouthServo;

int mouthOpenAngle   = 0;
int mouthClosedAngle = 45;
unsigned long openHoldMs  = 150;   // how long the mouth stays open before snapping shut
unsigned long closeHoldMs = 150;   // how long it stays closed before reopening — must be
                                    // >= the servo's actual travel time for the full
                                    // open<->closed span, or it gets recalled mid-swing

int lastAngle = 90;
bool chomping = false;
bool mouthOpen = false;
unsigned long mouthPhaseUntilMs = 0;

void printHelp() {
    Serial.println("Mouth servo tuner");
    Serial.println("  <angle>       snap the servo to <angle> (0-180)");
    Serial.println("  open          save last angle as MOUTH_OPEN and move there");
    Serial.println("  closed        save last angle as MOUTH_CLOSED and move there");
    Serial.println("  chomp         toggle continuous chomp test (open, hold, snap shut, hold, repeat)");
    Serial.println("  chomp <ms>    set the open-hold duration (ms) and start the test");
    Serial.println("  close <ms>    set the closed-hold duration (ms) and start the test");
    Serial.println("  print         print #define lines to paste into main.cpp");
    Serial.println("  help          show this message");
}

void printValues() {
    Serial.println("---");
    Serial.printf("#define MOUTH_OPEN            %d\n", mouthOpenAngle);
    Serial.printf("#define MOUTH_CLOSED          %d\n", mouthClosedAngle);
    Serial.printf("#define MOUTH_OPEN_HOLD_MS    %luUL\n", openHoldMs);
    Serial.printf("#define MOUTH_CLOSE_HOLD_MS   %luUL\n", closeHoldMs);
    Serial.println("---");
}

void handleLine(String line) {
    line.trim();
    if (line.length() == 0) return;

    if (line == "help")  { printHelp();  return; }
    if (line == "print") { printValues(); return; }

    if (line == "open") {
        mouthOpenAngle = lastAngle;
        mouthServo.write(mouthOpenAngle);
        Serial.printf("MOUTH_OPEN set to %d\n", mouthOpenAngle);
        return;
    }
    if (line == "closed") {
        mouthClosedAngle = lastAngle;
        mouthServo.write(mouthClosedAngle);
        Serial.printf("MOUTH_CLOSED set to %d\n", mouthClosedAngle);
        return;
    }
    if (line == "chomp") {
        chomping  = !chomping;
        mouthOpen = false;
        mouthPhaseUntilMs = millis();
        if (chomping) {
            Serial.println("Chomp test started (type 'chomp' again to stop)");
        } else {
            mouthServo.write(mouthClosedAngle);
            Serial.println("Chomp test stopped");
        }
        return;
    }
    if (line.startsWith("chomp ")) {
        long ms = line.substring(6).toInt();
        if (ms > 0) {
            openHoldMs = ms;
            Serial.printf("Open-hold set to %lums\n", openHoldMs);
            chomping  = true;
            mouthOpen = false;
            mouthPhaseUntilMs = millis();
        }
        return;
    }
    if (line.startsWith("close ")) {
        long ms = line.substring(6).toInt();
        if (ms > 0) {
            closeHoldMs = ms;
            Serial.printf("Close-hold set to %lums\n", closeHoldMs);
            chomping  = true;
            mouthOpen = false;
            mouthPhaseUntilMs = millis();
        }
        return;
    }

    // Otherwise treat the line as an angle to snap to directly.
    int angle = constrain(line.toInt(), 0, 180);
    lastAngle = angle;
    chomping  = false;
    mouthServo.write(angle);
    Serial.printf("Moved to %d\n", angle);
}

void setup() {
    Serial.begin(115200);
    mouthServo.attach(MOUTH_PIN);
    mouthServo.write(mouthClosedAngle);
    printHelp();
}

void loop() {
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        handleLine(line);
    }

    if (chomping) {
        unsigned long now = millis();
        if (now >= mouthPhaseUntilMs) {
            if (mouthOpen) {
                mouthServo.write(mouthClosedAngle);
                mouthOpen         = false;
                mouthPhaseUntilMs = now + closeHoldMs;
            } else {
                mouthServo.write(mouthOpenAngle);
                mouthOpen         = true;
                mouthPhaseUntilMs = now + openHoldMs;
            }
        }
    }
}
