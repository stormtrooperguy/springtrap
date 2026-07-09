/*
  Springtrap Eye/Mouth Controller
  ESP32 firmware: animated eyes (servo + WS2812B) and a chomping mouth servo,
  with an "eye movement" idle routine and an "error/reboot" malfunction
  routine. Runs autonomously on a random schedule (auto-loop mode) and can
  also be triggered on demand over a web interface served via WiFi AP.
*/

#include <Arduino.h>
#include <FastLED.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <esp_system.h>  // esp_random() -- hardware RNG, reliable with WiFi active
#include "secrets.h"

// ---------------------------------------------------------------------------
// Identity / WiFi
// ---------------------------------------------------------------------------
const char* ap_ssid     = "fazbear_sec";
const char* ap_password = AP_PASSWORD;
const char* mdns_host   = "springtrap";   // reachable at http://springtrap.local

// ---------------------------------------------------------------------------
// Cross-device coordination: when error/reboot fires, also trigger
// cupcake's bite action (if cupcake is reachable on fazbear_sec) for a
// synchronized jump-scare across both animatronics. Cupcake pins itself to
// a static 192.168.4.2 on this network (see its own firmware), so no
// discovery step is needed -- just hit it directly.
// ---------------------------------------------------------------------------
#define CUPCAKE_IP              IPAddress(192, 168, 4, 2)
#define CUPCAKE_HTTP_TIMEOUT_MS 500UL   // HTTP connect+read timeout

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

#define ERROR_MIN_MS   (3UL * 60 * 1000)   // 3 minutes — auto-loop schedule
#define ERROR_MAX_MS   (5UL * 60 * 1000)   // 5 minutes — auto-loop schedule

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

#define ERROR_LOOK_HOLD_MS    600UL  // how long the eyes hold each side during the error sweep

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
// Routine mode
//
// MODE_EYE_MOVEMENT: the idle routine — white eyes, periodic look-around,
//   and occasional glitch flicker. Always resumes here once error/reboot
//   completes.
// MODE_ERROR_REBOOT: the malfunction routine — red comet chase + eye sweep
//   + mouth chomp, followed immediately by the reboot chase. Runs to
//   completion once triggered; can't be interrupted by another trigger
//   while active (re-triggers are ignored, see triggerErrorReboot()).
// ---------------------------------------------------------------------------
enum Mode {
    MODE_EYE_MOVEMENT,
    MODE_ERROR_REBOOT
};
Mode currentMode = MODE_EYE_MOVEMENT;

// Auto-loop: when enabled, mimics the original always-on behavior by firing
// the error/reboot routine automatically on a random 3-5 minute schedule.
// When disabled, eye movement keeps running but error/reboot only fires
// when triggered remotely via the web interface.
bool autoLoopEnabled = true;
unsigned long nextAutoErrorMs = 0;

// Scheduled trigger times for the eye-movement routine's own idle timing
// (independent of auto-loop / error scheduling).
unsigned long nextGlitchMs = 0;
unsigned long nextLookMs   = 0;

// ---------------------------------------------------------------------------
// Look-around sub-state (eye movement routine)
// ---------------------------------------------------------------------------
bool lookingAround  = false;
int  lookStep       = 0;
unsigned long lookStepMs = 0;

// ---------------------------------------------------------------------------
// Glitch sub-state (eye movement routine)
// ---------------------------------------------------------------------------
bool glitching      = false;
int  glitchStep     = 0;
int  glitchCount    = 0;   // number of flicker cycles so far
int  glitchTarget   = 0;   // total flicker cycles for this glitch
unsigned long glitchStepMs = 0;
bool glitchLeft     = false;
bool glitchRight    = false;

// ---------------------------------------------------------------------------
// Error/reboot sub-state
// ---------------------------------------------------------------------------
enum ErrorPhase { PHASE_ERROR, PHASE_REBOOT };
ErrorPhase errorPhase = PHASE_ERROR;

int  errorStep      = 0;
unsigned long errorStepMs = 0;
bool mouthOpen        = false;   // chomp state: open-and-holding vs snapped shut
unsigned long mouthPhaseUntilMs = 0;
int  errorChasePos      = 0;
unsigned long errorChaseStepMs = 0;
bool errorLookRight     = false;   // continuous left/right sweep for the whole error sequence
unsigned long errorLookStepMs = 0;

int  rebootStep     = 0;
int  chasePos       = 0;
int  chaseCycles    = 0;
unsigned long rebootStepMs = 0;

// ---------------------------------------------------------------------------
// Action queue: AsyncWebServer handlers enqueue requested paths; loop()
// drains the queue and runs the actual dispatch. Keeps every hardware
// operation (servo, FastLED) single-threaded inside loop() — no concurrent
// access with the async HTTP task.
// ---------------------------------------------------------------------------
#define ACTION_PATH_MAX  32
#define ACTION_QUEUE_DEPTH 8
struct ActionMsg { char path[ACTION_PATH_MAX]; };
QueueHandle_t actionQueue = NULL;

// ---------------------------------------------------------------------------
// Async web server + SSE
// ---------------------------------------------------------------------------
AsyncWebServer server(80);
AsyncEventSource events("/events");
String pageHtml;   // built once in setup(), reused for every "/" request

// ---------------------------------------------------------------------------
// Forward declarations (web layer — referenced by animation code below)
// ---------------------------------------------------------------------------
void buildStatusJson(String &out);
void pushStatus();
void buildPageHtml(String &out);
void dispatchAction(const char* path);
void setupWebServer();

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

void scheduleNextAutoError() {
    nextAutoErrorMs = millis() + randRange(ERROR_MIN_MS, ERROR_MAX_MS);
}

void scheduleNextLook() {
    nextLookMs = millis() + randRange(LOOK_MIN_MS, LOOK_MAX_MS);
}

// ---------------------------------------------------------------------------
// Eye movement routine — idle white eyes with periodic look-around and
// occasional glitch flicker. This is the resting state whenever error/
// reboot isn't running.
// ---------------------------------------------------------------------------
void enterEyeMovement() {
    currentMode = MODE_EYE_MOVEMENT;
    FastLED.setBrightness(BRIGHTNESS_NORMAL);
    eyeServo.write(SERVO_CENTER);
    setEyes(CRGB::White);
    mouthServo.write(MOUTH_OPEN);   // mouth rests open by default
    lookingAround = false;
    glitching     = false;
    scheduleNextGlitch();
    scheduleNextLook();
}

// left → (hold) → right → (hold) → center → done
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

void enterGlitch() {
    glitching     = true;
    glitchStep    = 0;
    glitchCount   = 0;
    glitchTarget  = random(2, 5);   // 2–4 flicker cycles
    glitchStepMs  = millis();

    // Randomly pick which eye(s) to glitch
    int which = random(3);           // 0=left, 1=right, 2=both
    glitchLeft  = (which == 0 || which == 2);
    glitchRight = (which == 1 || which == 2);
}

// Flickers chosen eye(s) on/off glitchTarget times, then restores white.
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
                    // Done — restore and return to idle
                    setEyes(CRGB::White);
                    glitching = false;
                    scheduleNextGlitch();
                }
            }
            break;
    }
}

void updateEyeMovement() {
    unsigned long now = millis();
    if (glitching) {
        updateGlitch();
        return;
    }
    if (now >= nextGlitchMs) {
        enterGlitch();
        return;
    }
    if (!lookingAround && now >= nextLookMs) {
        lookingAround = true;
        lookStep      = 0;
    }
    if (lookingAround) {
        updateLookAround();
    }
}

// Best-effort: fire cupcake's bite action at its known static IP. Silently
// does nothing if cupcake doesn't respond -- springtrap's own routine
// proceeds regardless either way. Bounded to well under a second so it's
// not noticeable inside the blackout that follows.
void triggerCupcakeBite() {
    HTTPClient http;
    String url = "http://" + CUPCAKE_IP.toString() + "/a/bite";
    http.setConnectTimeout(CUPCAKE_HTTP_TIMEOUT_MS);
    http.setTimeout(CUPCAKE_HTTP_TIMEOUT_MS);
    http.begin(url);
    int code = http.GET();
    http.end();
    Serial.print("Triggered cupcake bite (");
    Serial.print(url);
    Serial.print("): HTTP ");
    Serial.println(code);
}

// ---------------------------------------------------------------------------
// Error/reboot routine — brief blackout → red comet chase + eye sweep +
// mouth chomp for 8s → fadeout → reboot chase → back to eye movement.
// ---------------------------------------------------------------------------
void enterErrorPhase() {
    errorPhase    = PHASE_ERROR;
    errorStep     = 0;
    errorStepMs   = millis();
    lookingAround = false;
    errorLookRight  = false;
    errorLookStepMs = millis();
    eyeServo.write(SERVO_LEFT);
    mouthServo.write(MOUTH_OPEN);
    mouthOpen     = true;
    triggerCupcakeBite();
}

void enterRebootPhase() {
    errorPhase   = PHASE_REBOOT;
    rebootStep   = 0;
    chasePos     = 0;
    chaseCycles  = 0;
    rebootStepMs = millis();
}

void finishErrorReboot() {
    enterEyeMovement();
    if (autoLoopEnabled) scheduleNextAutoError();
    pushStatus();
}

void updateErrorPhase() {
    unsigned long now = millis();

    // Eyes sweep left/right continuously through blackout and the red
    // chase, then hold center during the fadeout below (errorStep 3).
    if (errorStep < 3 && now - errorLookStepMs >= ERROR_LOOK_HOLD_MS) {
        errorLookRight = !errorLookRight;
        eyeServo.write(errorLookRight ? SERVO_RIGHT : SERVO_LEFT);
        errorLookStepMs = now;
    }

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
                eyeServo.write(SERVO_CENTER);   // recenter before reboot
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
                enterRebootPhase();
            }
            break;
    }
}

// Blackout → chase (single pixel laps ring 6×) → steady white → eye movement
void updateRebootPhase() {
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
            finishErrorReboot();
            break;
    }
}

void updateErrorReboot() {
    if (errorPhase == PHASE_ERROR) {
        updateErrorPhase();
    } else {
        updateRebootPhase();
    }
}

// Entry point for both the auto-loop scheduler and the remote "error_reboot"
// action. Ignored if the routine is already running (can't preempt itself).
void triggerErrorReboot() {
    if (currentMode == MODE_ERROR_REBOOT) return;
    currentMode = MODE_ERROR_REBOOT;
    enterErrorPhase();
    pushStatus();
}

// ---------------------------------------------------------------------------
// SSE / status
// ---------------------------------------------------------------------------
void buildStatusJson(String &out) {
    out = "{\"mode\":\"";
    out += (currentMode == MODE_ERROR_REBOOT ? "error_reboot" : "eye_movement");
    out += "\",\"autoLoop\":";
    out += (autoLoopEnabled ? "true" : "false");
    out += "}";
}

void pushStatus() {
    if (events.count() == 0) return;
    String json;
    buildStatusJson(json);
    events.send(json.c_str(), "message", millis());
}

// ---------------------------------------------------------------------------
// Action dispatch — runs on the main task (called from loop())
// ---------------------------------------------------------------------------
void dispatchAction(const char* path) {
    if (strcmp(path, "error_reboot") == 0) {
        triggerErrorReboot();
        return;
    }
    if (strcmp(path, "auto_loop") == 0) {
        autoLoopEnabled = !autoLoopEnabled;
        if (autoLoopEnabled) scheduleNextAutoError();
        pushStatus();
        return;
    }
}

// ---------------------------------------------------------------------------
// Page HTML — built once at startup into pageHtml
// ---------------------------------------------------------------------------
void buildPageHtml(String &out) {
    out.reserve(4096);
    out = "<!DOCTYPE html><html>"
          "<head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<link rel=\"icon\" href=\"data:,\">"
          "<style>"
          "* { margin:0; padding:0; box-sizing:border-box; }"
          "html { font-family:Helvetica,Arial,sans-serif; }"
          "body { background:#1a1a1a; color:#fff; padding:11px; padding-bottom:84px; }"
          "h1 { text-align:center; margin-bottom:15px; font-size:24px; }"
          "h2 { text-align:center; margin:15px 0 8px; font-size:17px; color:#aaa; }"
          ".action-wrap { max-width:800px; margin:0 auto 15px; display:flex; justify-content:center; }"
          ".btn-error { width:200px; height:200px; border:none; border-radius:50%; background:#c0392b;"
          "color:#fff; font-family:inherit; font-size:22px; font-weight:bold; cursor:pointer;"
          "transition:all .2s; box-shadow:0 4px 8px rgba(0,0,0,.4); }"
          ".btn-error:hover { transform:translateY(-2px); box-shadow:0 6px 12px rgba(0,0,0,.5); opacity:.9; }"
          ".btn-error:active { transform:translateY(0); box-shadow:0 2px 4px rgba(0,0,0,.3); }"
          ".toggle-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr));"
          "gap:8px; max-width:800px; margin:0 auto 15px; }"
          ".toggle { background-color:#2a2a2a; border:1px solid #444; border-radius:6px; padding:12px 16px;"
          "cursor:pointer; display:flex; align-items:center; justify-content:space-between; gap:12px;"
          "font-size:15px; font-weight:bold; color:white; transition:all .2s;"
          "box-shadow:0 3px 5px rgba(0,0,0,.3); user-select:none; -webkit-tap-highlight-color:transparent; }"
          ".toggle:hover { background-color:#353535; transform:translateY(-2px); box-shadow:0 5px 9px rgba(0,0,0,.4); }"
          ".toggle:active { transform:translateY(0); }"
          ".toggle-switch { width:42px; height:24px; background:#555; border-radius:12px;"
          "position:relative; flex-shrink:0; transition:background .2s; }"
          ".toggle-switch::before { content:''; position:absolute; top:3px; left:3px;"
          "width:18px; height:18px; background:white; border-radius:50%; transition:transform .2s; }"
          ".toggle.on { background-color:#1f3a2a; border-color:#4ade80; }"
          ".toggle.on .toggle-switch { background:#4ade80; }"
          ".toggle.on .toggle-switch::before { transform:translateX(18px); }"
          ".status-bar { position:fixed; bottom:0; left:0; right:0; background:#2a2a2a;"
          "border-top:2px solid #444; padding:8px 11px; box-shadow:0 -2px 8px rgba(0,0,0,.5); }"
          ".status-bar h3 { margin:0 0 6px; font-size:11px; color:#888; text-align:center; }"
          ".status-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(120px,1fr));"
          "gap:6px; max-width:800px; margin:0 auto; font-size:9px; }"
          ".status-item { background:#1a1a1a; padding:5px 8px; border-radius:3px; border:1px solid #444; }"
          ".status-item strong { color:#aaa; margin-right:5px; }"
          "</style></head>"
          "<body><h1>Springtrap</h1>";

    // Manual trigger — full-width on its own row
    out += "<h2>Actions</h2><div class=\"action-wrap\">"
           "<button class=\"btn-error\" data-path=\"error_reboot\" onclick=\"t('error_reboot')\">ERROR /<br>REBOOT</button>"
           "</div>";

    // Toggles section
    out += "<h2>Toggles</h2><div class=\"toggle-grid\">"
           "<div class=\"toggle\" id=\"tog-auto\" onclick=\"t('auto_loop')\">"
           "<span>Auto Loop</span><span class=\"toggle-switch\"></span>"
           "</div>"
           "</div>";

    // Status bar
    out += "<div class=\"status-bar\"><h3>System Status</h3><div class=\"status-grid\">"
           "<div class=\"status-item\"><strong>Network:</strong> fazbear_sec (192.168.4.1)</div>"
           "<div class=\"status-item\"><strong>Mode:</strong> <span id=\"md\">&mdash;</span></div>"
           "<div class=\"status-item\"><strong>Auto Loop:</strong> <span id=\"al\">&mdash;</span></div>"
           "</div></div>";

    // Embedded JS: SSE for live status pushes, fire-and-forget action triggers
    out += "<script>"
           "function r(d){if(!d)return;"
           "document.getElementById('md').textContent=d.mode==='error_reboot'?'Error / Reboot':'Eye Movement';"
           "document.getElementById('al').textContent=d.autoLoop?'On':'Off';"
           "document.getElementById('tog-auto').classList.toggle('on',!!d.autoLoop);}"
           "async function t(p){try{await fetch('/a/'+p);}catch(e){}}"
           "const es=new EventSource('/events');"
           "es.onmessage=e=>{try{r(JSON.parse(e.data));}catch(err){}};"
           "</script>"
           "</body></html>";
}

// ---------------------------------------------------------------------------
// Web server setup
// ---------------------------------------------------------------------------
void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html; charset=utf-8", pageHtml);
    });

    // SSE endpoint — clients open once, receive pushes
    events.onConnect([](AsyncEventSourceClient *client) {
        String json;
        buildStatusJson(json);
        client->send(json.c_str(), "message", millis());
    });
    server.addHandler(&events);

    // /a/<path> action dispatcher — enqueue for loop() to execute
    server.onNotFound([](AsyncWebServerRequest *req) {
        const String &url = req->url();
        if (req->method() == HTTP_GET && url.startsWith("/a/")) {
            if (actionQueue) {
                ActionMsg msg;
                strncpy(msg.path, url.c_str() + 3, ACTION_PATH_MAX - 1);
                msg.path[ACTION_PATH_MAX - 1] = '\0';
                xQueueSend(actionQueue, &msg, 0);  // non-blocking; drop if full
            }
            req->send(200, "text/plain", "OK");
        } else {
            req->send(404, "text/plain", "Not Found");
        }
    });

    server.begin();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("Springtrap starting...");

    // Action queue (async handlers -> loop())
    actionQueue = xQueueCreate(ACTION_QUEUE_DEPTH, sizeof(ActionMsg));

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, TOTAL_LEDS);
    FastLED.setBrightness(200);

    eyeServo.attach(SERVO_PIN);
    mouthServo.attach(MOUTH_PIN);   // enterEyeMovement() below sets the resting position

    randomSeed(esp_random());   // hardware RNG — reliable even with WiFi active

    enterEyeMovement();
    scheduleNextAutoError();    // auto-loop defaults on; seed the first trigger

    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(ap_ssid, ap_password);

    Serial.print("AP started: "); Serial.println(ap_ssid);
    Serial.print("IP: ");         Serial.println(WiFi.softAPIP());

    if (MDNS.begin(mdns_host)) {
        MDNS.addService("http", "tcp", 80);
        Serial.print("mDNS responder started: http://");
        Serial.print(mdns_host);
        Serial.println(".local");
    } else {
        Serial.println("mDNS init failed");
    }

    buildPageHtml(pageHtml);   // static content, built once
    setupWebServer();

    Serial.println("Ready.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
    // Drain any actions queued by the async HTTP task
    if (actionQueue) {
        ActionMsg msg;
        while (xQueueReceive(actionQueue, &msg, 0) == pdTRUE) {
            dispatchAction(msg.path);
        }
    }

    unsigned long now = millis();

    if (currentMode == MODE_EYE_MOVEMENT) {
        updateEyeMovement();
        if (autoLoopEnabled && now >= nextAutoErrorMs) {
            triggerErrorReboot();
        }
    } else {
        updateErrorReboot();
    }

    delay(2);
}
