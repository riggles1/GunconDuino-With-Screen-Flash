/*******************************************************************************
 * This sketch turns a PSX Guncon controller into a USB absolute mouse
 * or Joystick, using an Arduino Pro Micro (recommended) or Leonardo.
 *
 * It uses the PsxNewLib, ArduinoJoystickLibrary,
 * a modified version of AbsMouse Library (has to be this version to work), and the
 * Keyboard library (Arduino included default)
 *
 * For details on PsxNewLib, see
 * https://github.com/SukkoPera/PsxNewLib
 *
 * For details on ArduinoJoystickLibrary, see
 * https://github.com/MHeironimus/ArduinoJoystickLibrary
 *
 * For details on on the modified AbsMouse, see
 * https://github.com/jonathanedgecombe/absmouse
 *
 * Get the libraries and put them in "Documents/Arduino/Libraries"
 *
 *
 * Press GunCon Trigger to enable absolute mouse for XY-coordinates.
 * Press GunCon A (Left side) to enable joystick output for XY-coordinates (buttons remain as mouse left-middle-right clicks) 
 * Useful for games that used mechanical analog position guns rather than real lightguns.
 *
 * Hold A+B for 2 full seconds, this will enable the infinite XY-hold mode, this makes it so the cursor never goes off-screen.
 * The cursor freeze in place after losing light.
 * For games that weren't made with lightgun flashing in mind (IR or positional guns) there's a lot of contiously held shots.
 * With the XY-hold mode you can aim and shoot while holding the trigger, it'll hold your shot in that same coordinate for as long as you hold the trigger.
 * The only other solution to this would be to make all the black levels bright (so light is always seen), or strobe flash (a real headache to look at)
 *
 * To disable the GunCon (unstick the cursor), press A + B + Trigger
 * 
 *
 * The guncon needs to "scan" the entire screen to get XY min/max before it can properly send
 * the coorinates. Just point it at the screen and move from edge to edge, side to side
 * and top to bottom. The values will be stored as min and max, and will be used
 * to calculate the absolute mouse position.
 * It's recommended to use a full screen white image. 
 * (in RA you can set the screen flash shader as enabled for this, I recommend setting a controller button toggle per game, check the config folder examples)
 * Once the Trigger has been pressed 5 times, the XY min/max values will be locked from calibration (until Arduino disconnect)
 *
 * For the RetroArch Shader (hold) screen flash function, there's an immediate 24 ms Keyboard 'l' pulse on every trigger hardware press 
 * Hold-XY: When light is lost, keep sending last valid XY for 35 ms (Gives a bit more leeway ensuring shots don't miss)
 *
 *
 * Trigger = Mouse Left Click (as well as a keyboard "L" key pulse), A = Mouse Right Click, B = Mouse Middle Click
 *
 *******************************************************************************/
 
#include <PsxControllerHwSpi.h>
#include "AbsMouse.h"
#include <Joystick.h>
#include <Keyboard.h>

const byte PIN_PS2_ATT = 10;

// Guncon XY polling interval (Don't make it faster than this, or it will fail to read bottom of CRT relibably)
const unsigned long POLLING_INTERVAL = 1000U / 500U; // 2 ms (500Hz)

// Trigger debounce + L-pulse timing (microseconds)
const unsigned long TRIGGER_DEBOUNCE_US = 5000UL;   // 5 ms trigger debounce
const unsigned long LPULSE_US          = 24000UL;   // 24 ms keyboard l key pulse (for RetroArch's "Shader (Hold)" hotkey, results in a reliable 1 frame flash)

PsxControllerHwSpi<PIN_PS2_ATT> psx;

// NOTE: buttonCount set to 3 (was 0) to match HID descriptor that Windows recognizes
Joystick_ usbStick(
    JOYSTICK_DEFAULT_REPORT_ID,
    JOYSTICK_TYPE_JOYSTICK,
    3,      // (Use 3 Joystick buttons to ensure proper HID enumeration)
    0,
    true,   // X axis
    true,   // Y axis
    false,false,false,false,false,false,false,false,false
);

const byte ANALOG_DEAD_ZONE = 25U;
const word maxMouseValue = 32767;

//XY coordinates min and max possible values
//from document at http://problemkaputt.de/psx-spx.htm#controllerslightgunsnamcoguncon
//x is 77 to 461 (default recommendation)
//y is 25 to 248 (ntsc). y is 32 to 295 (pal)
//from personal testing (240p and 480p 60Hz up to 256p55Hz with no overscan)
//x is 72 to 450.
//y is 22 to 248.

const unsigned short int minPossibleX = 72;
const unsigned short int maxPossibleX = 450;
const unsigned short int minPossibleY = 25;
const unsigned short int maxPossibleY = 295;

const byte maxNoLightCount = 10;

boolean haveController = false;

// Min/max calibration values 
word minX = 1000;
word maxX = 0;
word minY = 1000;
word maxY = 0;

unsigned char noLightCount = 0;
word lastX = 0;
word lastY = 0; 

// Mode flags
boolean enableReport = false;
boolean enableMouseMove = false;
boolean enableJoystick = false;

// Calibration lock system (for XY min/max lock after 5 trigger presses)
bool calibrationLocked = false;
byte triggerPressCount = 0;

// Trigger debounce + l-pulse state
unsigned long triggerPressDuration = 0;
bool triggerDown = false;
unsigned long lastTriggerEventTimeUs = 0;
bool LpulseActive = false;
unsigned long LpulseStartUs = 0;

// Buffer-cancel state (for first-light-after-press logic)
bool firstLightSinceTrigger = false;
bool triggerUsedImmediate = false;

// Buffered click queue
enum BufferedEventType { EVT_DOWN = 0, EVT_UP = 1 };

struct BufferedEvent {
  BufferedEventType type;
  unsigned long scheduledUs;
};

const int BUFFERED_QUEUE_SIZE = 8;
BufferedEvent bufferedQueue[BUFFERED_QUEUE_SIZE];
int bufferedQueueHead = 0; // index of next to pop
int bufferedQueueTail = 0; // index to push
int bufferedQueueCount = 0;

unsigned long bufferDelayUs = 34000UL; // The delay for the mouse-left-click outputs after trigger press.

// queue utilities
void pushBufferedEvent(BufferedEventType t, unsigned long scheduledUs) {
  if (bufferedQueueCount >= BUFFERED_QUEUE_SIZE) {
    // For overflow, drops the oldest
    bufferedQueueHead = (bufferedQueueHead + 1) % BUFFERED_QUEUE_SIZE;
    bufferedQueueCount--;
  }
  bufferedQueue[bufferedQueueTail] = { t, scheduledUs };
  bufferedQueueTail = (bufferedQueueTail + 1) % BUFFERED_QUEUE_SIZE;
  bufferedQueueCount++;
}

bool peekBufferedEvent(BufferedEvent &out) {
  if (bufferedQueueCount == 0) return false;
  out = bufferedQueue[bufferedQueueHead];
  return true;
}

bool popBufferedEvent(BufferedEvent &out) {
  if (bufferedQueueCount == 0) return false;
  out = bufferedQueue[bufferedQueueHead];
  bufferedQueueHead = (bufferedQueueHead + 1) % BUFFERED_QUEUE_SIZE;
  bufferedQueueCount--;
  return true;
}

// Infinite XY-hold toggle state (hold A+B 2s to toggle)
bool infiniteHoldEnabled = false;
bool infiniteAwaitHoldAB = false;
unsigned long infiniteHoldStartMs = 0;
const unsigned long INFINITE_HOLD_TOGGLE_MS = 2000UL; // 2 seconds

// hold-XY state
bool haveLight = false;                 // whether we currently have valid on-screen coordinates
unsigned long holdXYStartUs = 0;
const unsigned long HOLD_XY_US = 35000UL; // 35ms (holds XY for this amount of time after losing light)
bool holdXYActive = false;

word convertRange(double gcMin, double gcMax, double value) {
    double scale = (double)maxMouseValue / (gcMax - gcMin);
    long v = (long)((value - gcMin) * scale);
    if (v < 0) v = 0;
    if (v > maxMouseValue) v = maxMouseValue;
    return (word)v;
}

void moveToCoords(word x, word y) {
    if (enableMouseMove)
        AbsMouse.move(x, y);

    if (enableJoystick) {
        usbStick.setXAxis(x);
        usbStick.setYAxis(y);
    }
}

void releaseAllButtons() {
    AbsMouse.release(MOUSE_LEFT);
    AbsMouse.release(MOUSE_RIGHT);
    AbsMouse.release(MOUSE_MIDDLE);
    AbsMouse.report();
}

// Called to execute a buffered event immediately (when its scheduled time has arrived)
void executeBufferedEvent(const BufferedEvent &ev) {
    if (ev.type == EVT_DOWN) {
        AbsMouse.press(MOUSE_LEFT);
    } else {
        AbsMouse.release(MOUSE_LEFT);
    }
}

// Handle trigger input (down-/up-edges).
// Still performs immediate l key pulse on down-edge.
void handleTrigger() {
    bool pressed = psx.buttonPressed(PSB_CIRCLE);
    unsigned long nowUs = micros();

    // Down-edge detection with debounce
    if (pressed && !triggerDown) {
        if (nowUs - lastTriggerEventTimeUs >= TRIGGER_DEBOUNCE_US) {
            triggerDown = true;
            lastTriggerEventTimeUs = nowUs;

            // Record the duration of the press (if the press is short, no click will be sent)
            triggerPressDuration = 0;

            // Immediate l pulse (This is never buffered)
            Keyboard.press('l');
            LpulseActive = true;
            LpulseStartUs = nowUs;

            // Clear XY-hold state on trigger press (fresh XY required)
            haveLight = false;
            holdXYActive = false;
            lastX = 0;
            lastY = 0;

            // Buffer the DOWN event scheduled at now + bufferDelayUs
            pushBufferedEvent(EVT_DOWN, nowUs + bufferDelayUs);
        }
    }

    // Up-edge detection
    if (!pressed && triggerDown) {
        triggerDown = false;
        lastTriggerEventTimeUs = nowUs;

        // Check if the press duration was less than 5ms (skip click if so)
        if (triggerPressDuration < TRIGGER_DEBOUNCE_US) {
            // Don't send a click if press was too short (skip the buffer and immediate click)
            // Just skip the buffered events in this case.
            pushBufferedEvent(EVT_UP, nowUs + bufferDelayUs);
        } else {
            // Normal release event
            pushBufferedEvent(EVT_UP, nowUs + bufferDelayUs);
            AbsMouse.release(MOUSE_LEFT);
        }

        // Reset trigger state
        triggerPressDuration = 0;
    }

    // Non-blocking l pulse release timer
    if (LpulseActive && (nowUs - LpulseStartUs >= LPULSE_US)) {
        Keyboard.release('l');
        LpulseActive = false;
    }
}

void handleABbuttons() {
    // A = Right-click
    if (psx.buttonJustPressed(PSB_START))
        AbsMouse.press(MOUSE_RIGHT);
    if (psx.buttonJustReleased(PSB_START))
        AbsMouse.release(MOUSE_RIGHT);

    // B = Middle-click
    if (psx.buttonJustPressed(PSB_CROSS))
        AbsMouse.press(MOUSE_MIDDLE);
    if (psx.buttonJustReleased(PSB_CROSS))
        AbsMouse.release(MOUSE_MIDDLE);
}

void fastButtonPolling() {
    handleTrigger();
    handleABbuttons();
}

// Called by readGuncon when guncon gives coordinates
void onLightSensed(word x, word y) {
    // mark we have light
    haveLight = true;
    holdXYActive = false; // cancel any hold-XY active
    lastX = x;
    lastY = y;

    // Buffer canceling: if trigger is held and this is the first light since press,
    // cancel buffered DOWN for this shot and press immediately.
    if (triggerDown && firstLightSinceTrigger && !triggerUsedImmediate) {
        // remove the most recently queued event (the DOWN for this trigger)
        if (bufferedQueueCount > 0) {
            int lastIndex = (bufferedQueueTail - 1 + BUFFERED_QUEUE_SIZE) % BUFFERED_QUEUE_SIZE;
            // Optional sanity check: ensure it's a DOWN event
            if (bufferedQueue[lastIndex].type == EVT_DOWN) {
                bufferedQueueTail = lastIndex;
                bufferedQueueCount--;
            }
        }

        AbsMouse.press(MOUSE_LEFT);
        triggerUsedImmediate = true;
        firstLightSinceTrigger = false;
    }
}

void readGuncon() {
    word x=0, y=0, convertedX=0, convertedY=0;
    GunconStatus gcStatus = psx.getGunconCoordinates(x, y);

    // allow A + B + Trigger to disable GunCon regardless of on/off-screen
    if (psx.buttonPressed(PSB_CIRCLE) &&
        psx.buttonPressed(PSB_START) &&
        psx.buttonPressed(PSB_CROSS)) {

        releaseAllButtons();
        enableReport = false;
        enableMouseMove = false;
        enableJoystick = false;

        // reset trigger/buffer-cancel state
        triggerDown = false;
        firstLightSinceTrigger = false;
        triggerUsedImmediate = false;

        delay(1000);
        return;
    }

    if (gcStatus == GUNCON_OK) {
        noLightCount = 0;

        if (x >= minPossibleX && x <= maxPossibleX && y >= minPossibleY && y <= maxPossibleY) {
            // on-screen
            onLightSensed(x, y);

            if (!calibrationLocked) {
                if (x < minX) minX = x;
                else if (x > maxX) maxX = x;

                if (y < minY) minY = y;
                else if (y > maxY) maxY = y;
            }

            if (enableMouseMove || enableJoystick) {
                convertedX = convertRange(minX, maxX, x);
                convertedY = convertRange(minY, maxY, y);
                moveToCoords(convertedX, convertedY);
            }
        }
    }
    else if (gcStatus == GUNCON_NO_LIGHT) {

        // lost light: start hold-XY timer if we had a valid lastX/Y
        if (haveLight && (lastX != 0 || lastY != 0)) {

            // start hold-XY only if not already active
            if (!holdXYActive) {
                holdXYActive = true;
                holdXYStartUs = micros();
            }

            // continue sending last valid XY during hold (or indefinitely if infiniteHoldEnabled)
            if (enableMouseMove || enableJoystick) {
                convertedX = convertRange(minX, maxX, lastX);
                convertedY = convertRange(minY, maxY, lastY);
                moveToCoords(convertedX, convertedY);
            }

            // increment noLightCount (kept for compatibility with older logic)
            noLightCount++;
            if (noLightCount > maxNoLightCount) {
                // Too long without light
                noLightCount = 0;
                if (infiniteHoldEnabled) {
                    // keep lastX/lastY and keep haveLight = true so XY is held indefinitely
                    haveLight = true; // explicit: remain as if we have light
                } else {
                    // treat as fully off-screen (only when infinite hold is disabled)
                    haveLight = false;
                    lastX = 0;
                    lastY = 0;
                }
            }
        }
        else {
            // no previously valid XY
        }
    }

    // hold-XY timeout handling (interruptible by onLightSensed)
    if (holdXYActive) {
        unsigned long nowUs = micros();

        if (nowUs - holdXYStartUs >= HOLD_XY_US) {
            // Hold expired
            holdXYActive = false;
            if (infiniteHoldEnabled) {
                // Infinite hold: do not go off-screen; keep lastXY and haveLight true
                haveLight = true; // explicit: remain as if we have light
            } else {
                // Normal behavior: go off-screen
                haveLight = false;

                // OFF-SCREEN XY positioning (only done AFTER hold-XY ends)
                if (enableMouseMove)
                    AbsMouse.move(0, maxMouseValue);

                if (enableJoystick) {
                    usbStick.setXAxis(16383);
                    usbStick.setYAxis(16383);
                }

                // Don't delete lastX/lastY, they are needed for smooth recovery
            }
        }
    }
}

void readDualShock() {
    word x, y;
    byte analogX = ANALOG_IDLE_VALUE;
    byte analogY = ANALOG_IDLE_VALUE;

    if (psx.getLeftAnalog(analogX, analogY)) {
        int8_t dx = analogX - ANALOG_IDLE_VALUE;
        if (abs(dx) < ANALOG_DEAD_ZONE) analogX = ANALOG_IDLE_VALUE;
        int8_t dy = analogY - ANALOG_IDLE_VALUE;
        if (abs(dy) < ANALOG_DEAD_ZONE) analogY = ANALOG_IDLE_VALUE;
    }

    x = convertRange(ANALOG_MIN_VALUE, ANALOG_MAX_VALUE, analogX);
    y = convertRange(ANALOG_MIN_VALUE, ANALOG_MAX_VALUE, analogY);
    moveToCoords(x, y);

    if (psx.buttonPressed(PSB_SELECT)) {
        releaseAllButtons();
        enableReport = false;
        enableMouseMove = false;
        enableJoystick = false;
        delay(1000);
    }
}

void setup() {
    // Initialize joystick HID descriptor like the original working sketch
    usbStick.begin(false);
    usbStick.setXAxisRange(0, maxMouseValue);
    usbStick.setYAxisRange(0, maxMouseValue);

    Keyboard.begin(); // initialize keyboard
    // initialize AbsMouse if required by your edited library (keep original init if needed)
}

// Process any buffered click events whose scheduled time has arrived
void processBufferedQueue() {
    BufferedEvent ev;
    unsigned long nowUs = micros();
    while (peekBufferedEvent(ev)) {
        // careful with micros() wraparound: we rely on unsigned subtraction
        if ((long)(nowUs - ev.scheduledUs) >= 0) {
            // execute and pop
            popBufferedEvent(ev);
            executeBufferedEvent(ev);
        } else {
            // next event not ready yet
            break;
        }
    }
}

void loop() {

    // Fast button polling
    fastButtonPolling();

    // Process buffered events each loop (microseconds)
    processBufferedQueue();

    // XY Polling
    static unsigned long last = 0;
    unsigned long nowMs = millis();

    if (nowMs - last >= POLLING_INTERVAL) {
        last = nowMs;

        if (!haveController) {
            if (psx.begin())
                haveController = true;
        }
        else {
            noInterrupts();
            boolean ok = psx.read();
            interrupts();

            if (!ok) {
                haveController = false;
            }
            else {
                // Check for entry into Infinite XY-hold toggle (hold A+B for 2s)
                if (!infiniteAwaitHoldAB && psx.buttonPressed(PSB_START) && psx.buttonPressed(PSB_CROSS)) {
                    // start hold timer
                    infiniteAwaitHoldAB = true;
                    infiniteHoldStartMs = millis();
                } else if (infiniteAwaitHoldAB) {
                    if (!(psx.buttonPressed(PSB_START) && psx.buttonPressed(PSB_CROSS))) {
                        // released too early — cancel
                        infiniteAwaitHoldAB = false;
                    } else {
                        // still holding, check duration
                        if (millis() - infiniteHoldStartMs >= INFINITE_HOLD_TOGGLE_MS) {
                            // toggle infinite hold mode
                            infiniteHoldEnabled = !infiniteHoldEnabled;
                            infiniteAwaitHoldAB = false;
                        }
                    }
                }

                if (!enableReport) {
                    if (!enableMouseMove && !enableJoystick) {
                        if (psx.buttonJustPressed(PSB_CIRCLE)) {
                            enableReport = true;
                            enableMouseMove = true;
                            return;
                        }
                        else if (psx.buttonJustPressed(PSB_START)) {
                            enableReport = true;
                            enableJoystick = true;
                            return;
                        }
                    }
                    else if (psx.buttonJustPressed(PSB_CIRCLE) ||
                             psx.buttonJustPressed(PSB_START) ||
                             psx.buttonJustPressed(PSB_CROSS)) {
                        enableReport = true;
                        return;
                    }
                }

                if (enableReport) {
                    PsxControllerProtocol proto = psx.getProtocol();
                    switch (proto) {
                        case PSPROTO_GUNCON:
                            readGuncon();
                            break;
                        case PSPROTO_DUALSHOCK:
                        case PSPROTO_DUALSHOCK2:
                            readDualShock();
                            break;
                        default:
                            break;
                    }
                }

                if (enableReport) {
                    if (enableMouseMove)
                        AbsMouse.report();
                    else if (enableJoystick)
                        usbStick.sendState();
                }
            }
        }
    }
}



