/*******************************************************************************
 * This sketch turns a PSX Guncon controller into a USB absolute mouse device
 * using an Arduino Pro Micro (recommended) or Leonardo.
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
 * USAGE:
 *
 * Press trigger to activate the GunCon. 
 * The GunCon needs to "Scan" the screen to get XY min/max before it can properly send XY coorinates
 * Just point it at the screen and move from edge to edge, side to side, top to bottom.
 * The minmax is then used to calculate the absolute mouse position.
 * It's recommended to use a full screen white image and calibrate min-max from the same distance you will play at.
 * (in RA you can set the screen flash shader as enabled for this, I recommend setting a controller button toggle per game, check the config folder examples where I've done this)
 * Once the Trigger has been pressed 10 times, the XY min/max values will be locked from calibration (until Arduino disconnect)
 *
 * To disable the GunCon (unstick the cursor), press A + B + Trigger, you may then press A or Trigger to select either bufferdelay value again.
 *
 * Press GunCon Trigger for a 35ms maximum bufferdelay (trigger press is sent instantly when light is sensed, this is a maximum wait).
 * For extended bufferdelay (static delay), On boot or after guncon disable combo (detailed below): Press GunCon A-button for 1-4 times followed trigger.
 * Example: Disable the guncon, then press A twice, and then trigger. Bufferdelay now becomes 4 frames.
 * 1 A press = 3 frame bufferdelay, 2 A presses = 4 frame bufferdelay, 3 A presses = 5 frame bufferdelay, 6 A presses = 6 frame bufferdelay.
 * 
 * Set whatever bufferdelay depending on your setup, optimal setups will have 1-2 frames of input lag, but for something like Carnevil that plays at 55Hz,
 * you may want to set the buffer delayto be 3 frames (since 55Hz frames are slower than 60Hz frames)
 *
 * At any point, Hold A+B for 2 full seconds, this will toggle Infinite XY-hold mode, this makes it so the cursor never goes off-screen.
 * Meaning the cursor freezes in place after losing light.
 * Used for games that weren't made with lightgun flashing in mind (IR or positional* guns) there's a lot of contiously held shots.
 * With the XY-hold mode you can aim and shoot while holding the trigger, it'll hold your shot in that same coordinate for as long as you hold the trigger.
 * It's necessary for games like Carnevil which has a machine gun.
 * The only other solution to this would be to make all the black levels very bright (so light is always seen), or strobe flash (a real headache to look at)
 *  (*I wouldn't recommend playing positional analog gun games like Jurassic park 1994 with a lightgun, it's not designed for it and plays weird)
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
const unsigned long POLLING_INTERVAL = 1000U / 500U; // 2ms (500Hz)

const byte CALIBRATION_LOCK_PRESSES = 10;   // Number of presss before XY calibration gets locked
const unsigned long TRIGGER_DEBOUNCE_US = 5000UL;   // 5 ms trigger debounce (prevents the shader flash from being more than 1 frame)
const unsigned long LPULSE_US          = 19000UL;   // 19 ms keyboard l key pulse (for RetroArch's "Shader (Hold)" hotkey, results in a reliable 1 frame flash)


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
//y is 25 to 248 (ntsc). y is 32 to 295 (pal) (default recommendation) 

//from personal testing the values below give me 1:1 guncon-sight to cursor (240p and 480i 60Hz up to 256p55-57Hz without any overscan)
//x is 18 to 494.
//y is 14 to 254.

const unsigned short int minPossibleX = 18;
const unsigned short int maxPossibleX = 494;
const unsigned short int minPossibleY = 14;
const unsigned short int maxPossibleY = 254;

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
boolean enableJoystick = false; // joystick mode disabled and commented out elsewhere
bool awaitingModeSelect = true;
byte aPressCount = 0;

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

unsigned long bufferDelayUs = 100UL; //Buffer delay is set elsewhere, this is the value at boot. 
//Change the value for the Trigger or A-button press scenarios elsewhere, search for "bufferDelayUs =".
unsigned long holdXYUs = 70000UL;   // further down it's derived from 2x bufferDelayUs, with mode selection

// Set different bufferdelay values with "A" (58ms for a game like Carnevil that's 55Hz) or "Trigger" (35ms default)
enum ReactivationSource { 
    REACT_NONE = 0,
    REACT_TRIGGER,
    REACT_A
};

ReactivationSource lastReactSource = REACT_NONE;


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

void handleModeSelectionButtons() {
    if (!awaitingModeSelect) return;
  
      // Trigger -> Mouse mode -> 35ms bufferdelay or extended if A gets pressed 3-6 times for selection.
    if (psx.buttonJustPressed(PSB_CIRCLE)) {

      enableReport = true;
      enableMouseMove = true;
      
          // DEFAULT: Trigger mode
        lastReactSource = REACT_TRIGGER;
        bufferDelayUs   = 35000UL;

      // A-mode override based on count
      if (aPressCount >= 1) {
        lastReactSource = REACT_A;

        if (aPressCount == 1)
          bufferDelayUs = 52000UL; //~3 frames 60Hz or 2 frames 55Hz
        else if (aPressCount == 2)
          bufferDelayUs = 72000UL; //~4 frames 60Hz or 3 frames 55Hz
        else if (aPressCount == 3)
          bufferDelayUs = 85000UL; //~5 frames 60Hz
        else if (aPressCount == 4)
          bufferDelayUs = 98000UL; //~6 frames 60Hz
        else
          bufferDelayUs = 100000UL; // cap at 104ms ~6 frames 60Hz
      }
      holdXYUs = (bufferDelayUs * 3); //sets holdXY to be 3.0x the bufferdelay
      
      awaitingModeSelect = false;
      aPressCount = 0; // reset for next time
      return;
    }

    if (psx.buttonJustPressed(PSB_START)) {
        aPressCount++;
    }
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
const unsigned long INFINITE_HOLD_TOGGLE_MS = 1500UL; // "2" seconds

void handleInfiniteHoldToggle() {
    if (!infiniteAwaitHoldAB &&
        psx.buttonPressed(PSB_START) &&
        psx.buttonPressed(PSB_CROSS)) {

        infiniteAwaitHoldAB = true;
        infiniteHoldStartMs = millis();

    } else if (infiniteAwaitHoldAB) {

        if (!(psx.buttonPressed(PSB_START) &&
              psx.buttonPressed(PSB_CROSS))) {

            infiniteAwaitHoldAB = false;

        } else if (millis() - infiniteHoldStartMs >= INFINITE_HOLD_TOGGLE_MS) {

            infiniteHoldEnabled = !infiniteHoldEnabled;
            infiniteAwaitHoldAB = false;
        }
    }
}

// hold-XY state
bool haveLight = false;                 // whether we currently have valid on-screen coordinates
unsigned long holdXYStartUs = 0;
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
      // Arm buffer-canceling only for "Trigger mode"
/*      if (lastReactSource == REACT_TRIGGER) {
        firstLightSinceTrigger = true;
        triggerUsedImmediate = false;
      }
*/      
      if (!calibrationLocked) {
      triggerPressCount++;
        if (triggerPressCount >= CALIBRATION_LOCK_PRESSES) {
        calibrationLocked = true;
        }
      }

            // Record the duration of the press (if the press is short, no click will be sent)
            triggerPressDuration = 0;

            // Immediate l pulse (This is never buffered)
            Keyboard.press('l');
            LpulseActive = true;
            LpulseStartUs = nowUs;

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
    handleInfiniteHoldToggle();
    handleModeSelectionButtons();
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
/*
    // Buffer canceling: if trigger is held and this is the first light since press,
    // cancel buffered DOWN for this shot and press immediately.
    if (lastReactSource == REACT_TRIGGER &&
    triggerDown &&
    firstLightSinceTrigger &&
    !triggerUsedImmediate) {
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
*/
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
        // enableJoystick = false; // joystick mode disabled
    
    // reset for mouse or joystick mode selection
    awaitingModeSelect = true;
    
    // reset A-mode state
    aPressCount = 0;

        // reset trigger/buffer-cancel state
        triggerDown = false;
        firstLightSinceTrigger = false;
        triggerUsedImmediate = false;
        lastReactSource = REACT_NONE; //For bufferdelay millisecond selection, every guncon disable wipes selection.
    bufferedQueueHead = 0;
    bufferedQueueTail = 0;
    bufferedQueueCount = 0;
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

        if (nowUs - holdXYStartUs >= holdXYUs) {
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
        // enableJoystick = false; // joystick mode disabled
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
  
        // === FAST controller polling (buttons update every loop) ===
        if (haveController) {
          noInterrupts();
          boolean ok = psx.read();
          interrupts();

          if (!ok) {
            haveController = false;
            return;
          }
        } else {
          if (psx.begin())
            haveController = true;
        }
        // Fast button polling (now sees fresh state)
        fastButtonPolling();

        // Process buffered events
        processBufferedQueue();

        // XY Polling
        static unsigned long last = 0;
        unsigned long nowMs = millis();

        if (nowMs - last < POLLING_INTERVAL) {
            return; // Too soon for XY polling
        }
        last = nowMs;

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
