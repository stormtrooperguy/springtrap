#include <Arduino.h>
#include <ESP32Servo.h>

// Standalone sketch for finding mouth servo angles and open-hold timing on
// the bench, without waiting for error mode to trigger on the full firmware.
// Build/upload with: pio run -e mouth_tune -t upload
// Then: pio device monitor -e mouth_tune

#define MOUTH_PIN 4

Servo mouthServo;

int mouthOpenAngle   = 150;
int mouthClosedAngle = 90;
unsigned long openHoldMs = 150;   // how long the mouth stays open before snapping shut

int lastAngle = 90;
bool chomping = false;
bool mouthOpen = false;
unsigned long mouthOpenUntilMs = 0;

void printHelp() {
    Serial.println("Mouth servo tuner");
    Serial.println("  <angle>       snap the servo to <angle> (0-180)");
    Serial.println("  open          save last angle as MOUTH_OPEN and move there");
    Serial.println("  closed        save last angle as MOUTH_CLOSED and move there");
    Serial.println("  chomp         toggle continuous chomp test (open, hold, snap shut, repeat)");
    Serial.println("  chomp <ms>    set the open-hold duration (ms) and start the test");
    Serial.println("  print         print #define lines to paste into main.cpp");
    Serial.println("  help          show this message");
}

void printValues() {
    Serial.println("---");
    Serial.printf("#define MOUTH_OPEN            %d\n", mouthOpenAngle);
    Serial.printf("#define MOUTH_CLOSED          %d\n", mouthClosedAngle);
    Serial.printf("#define MOUTH_OPEN_HOLD_MS    %luUL\n", openHoldMs);
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
        if (!mouthOpen) {
            mouthServo.write(mouthOpenAngle);
            mouthOpen        = true;
            mouthOpenUntilMs = now + openHoldMs;
        } else if (now >= mouthOpenUntilMs) {
            mouthServo.write(mouthClosedAngle);
            mouthOpen = false;
        }
    }
}
