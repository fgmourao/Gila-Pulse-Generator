/* * ======================================================================================
 * PROJECT: Gila Monster Pulse Generator
 * VERSION: 1.0
 * AUTHOR:  Flavio Mourao - Feb, 2026
 *
 * DESCRIPTION:
 * Pulse generator designed for Arduino Uno (ATmega328P) across three operational modes:
 * Continuous, Burst, and Non-Periodic Stimulation (NPS).

 * MODES OF OPERATION:
 * 1. Continuous : Regular, periodic pulse trains at a specific frequency.
 * 2. Burst      : Clustered groups of pulses separated by a customizable inter-burst gap.
 * 3. NPS        : "Non-Periodic Stimulation". Pulses are placed within each 1-second window by a
 *                 sequential shrinking-window algorithm, yielding power-law distributed
 *                 inter-pulse intervals, obeying a minimum interval.
 * Ref: https://doi.org/10.1016/j.yebeh.2019.106609
 * Ref: https://doi.org/10.1016/j.yebeh.2008.09.006
 *
 * KEY FEATURES & SAFETY:
 * - Time Base       : Microsecond resolution (4 us steps). Pulse edges are placed by busy-waiting
 *                     on micros() when an edge is near.
 * - LCD = Output    : Limits are applied to the menu values, so the LCD always shows what the pin
 *                     produces. If a change forces another parameter to change, the LCD shows
 *                     "Auto-adjusted" and the name of that parameter.
 * - Safety Clamps   : Minimum HIGH time 100 us (Cont/Burst) or 50 us (NPS). Minimum LOW time
 *                     100 us between pulses (no 100% Duty Cycle / latched output).
 * - Locked UI       : While generating, the knob, LCD redraws, Save and Comm are disabled.
 *                     Only a click on State (stop) is accepted.
 * - Clean Stop      : Stopping (click or Timer) always lets the current pulse finish.
 * - Single-Shot     : Debounced manual button, fires one pulse with the programmed width.
 * - Non-Volatile    : EEPROM storage of all experimental parameters (with signature).
 * - Diagnostics     : Comm reports pulses fired and timing faults of the last session.
 *
 * PROTOCOL SEMANTICS:
 * - Continuous (Manual) : a pulse every 1/Freq until Timer or stop.
 * - Burst (Manual)      : Count pulses at 1/Freq. Gap is the silence from the falling edge of the
 *                         last pulse to the rising edge of the next burst. Gap = 0: single burst.
 * - NPS (Manual)        : Count pulses in every 1-second window, windows back-to-back.
 * - Continuous (Trigger): one pulse per trigger.
 * - Burst (Trigger)     : one burst per trigger.
 * - NPS (Trigger)       : one 1-second window per trigger.
 * - Triggers arriving while a pulse, burst or NPS window is running are ignored.
 * - Timer               : only pulses whose rising edge is before the end of the session are
 *                         emitted (1 Hz with Timer = 10000 ms gives 10 pulses).
 * - NPS constraints     : Width is Fixed, Count x ITImin < 1000 ms, Pulse <= ITImin - 0.1 ms.
 *
 * HARDWARE I/O & SYNCHRONIZATION :
 * - OUTPUT (Pin D7) : Main 5V TTL stimulation pulse to drive the output stage.
 * - INPUT  (Pin D3) : External Trigger IN. Interrupt-driven, to start protocols synchronously
 *                     with external behavior software.
 * - MANUAL (Pin D6) : Single-Shot manual firing button input (only when State = OFF).
 *
 * MENU REFERENCE (LCD Interface):
 * [0] Mode   : Manual (Auto-run) or Trigger (Waits for external D3 signal).
 * [1] Edge   : Signal type for External Trigger (Rising or Falling edge).
 * [2] Type   : The Generation Engine (Cont, Burst, or NPS).
 * [3] State  : The Master Switch (ON / OFF).
 * [4] Freq   : Frequency (0.10 to 500.00 Hz). Used in Cont./Burst modes.
 * [5] Count  : Pulse amount (Pulses per Burst, or Pulses per Second in NPS).
 * [6] Gap    : Inter-burst silence (ms). Used in BURST mode.
 * [7] ITImin : Minimum time between random pulses (integer ms). Used in NPS mode.
 * [8] Width  : Logic for pulse duration (Fixed time [ms] vs Duty Cycle [%]).
 * [9] Pulse  : The High-time duration.
 * [10] Timer : Session length from switching ON (0 = disabled).
 * [11] Save  : Saves current settings to EEPROM.
 * [12] Comm  : Serial dump of parameters, last session report and NPS simulation data to PC.
 * ======================================================================================
 */

#include <ClickEncoder.h>       // Library for the Rotary Knob
#include <TimerOne.h>           // Library for precise internal clock handling
#include <EEPROM.h>             // Library to save settings when power is off
#include <Wire.h>               // Library for LCD communication
#include <LiquidCrystal_I2C.h>  // Library for the Display

// =================================================================================
// 1. HARDWARE CONNECTIONS
// =================================================================================
#define TRIGGER_PIN 3          // Input for External Trigger

#define ENCODER_PIN_A 2        // Knob Pin A. Rotary Encoder CLK
#define ENCODER_PIN_B 5        // Knob Pin B. Rotary Encoder DT
#define ENCODER_PIN_BUTTON 4   // Rotary Encoder Switch (SW)

#define SINGLE_PULSE_PIN 6     // The manual "Fire" button for single pulse

#define OUTPUT_PIN 7           // The output pin

// Initialize the Screen and the Knob
LiquidCrystal_I2C lcd(0x27, 16, 2);
ClickEncoder encoder(ENCODER_PIN_B, ENCODER_PIN_A, ENCODER_PIN_BUTTON, 4);

// =================================================================================
// 2. CONSTANTS
// =================================================================================
#define MIN_HIGH_US_STD   100UL   // Minimum pulse width, Cont/Burst
#define MIN_HIGH_US_NPS    50UL   // Minimum pulse width, NPS
#define MIN_LOW_US        100UL   // Minimum LOW time between two pulses
#define SPIN_WINDOW_US    400UL   // If an edge is closer than this, busy-wait for it
#define LATE_TOLERANCE_US  50UL   // An edge later than this is counted as a timing fault
#define NPS_STEP_GUARD_US 1000UL  // Only compute NPS timestamps when no edge is this close
#define NPS_WINDOW_US  1000000UL  // NPS window: 1 second

#define EEPROM_MAGIC   0x414C4947L  // "GILA"
#define EEPROM_VERSION 1L
#define EEPROM_DATA    8            // Menu values start after magic + version

// =================================================================================
// 3. GLOBAL VARIABLES
// =================================================================================

// Helpers for the Menu System
int edit_digit = 0;                     // Which digit are we editing? (1s, 10s, 100s...)
uint32_t digit_multipliers[] = {100000, 10000, 1000, 100, 10, 1};
byte dinoChar[8] = { B00000, B00111, B00101, B10111, B11100, B11111, B01101, B01100 }; // The little lizard logo

// State Flags (shared with the trigger interrupt)
volatile bool generating = false;       // Master Switch: true = ON, false = OFF
volatile bool triggerEvent = false;     // Did we receive an external signal?
volatile bool isTriggerMode = false;    // External pin?
int last_edge_mode = -1;                // Remembers if we trigger on Rising or Falling edge
bool armed_for_start = false;           // Engine is armed in loop(), after the LCD has been redrawn

// --- PULSE ENGINE STATE (only touched from loop) ---
bool pulse_active = false;              // Output is HIGH right now
uint32_t pulse_start_us = 0;            // Rising edge time of the active pulse
bool train_active = false;              // A next rising edge is scheduled
uint32_t next_pulse_us = 0;             // Scheduled time of the next rising edge
long pulsesRemaining = 0;               // Pulses left in the current burst
bool counted = false;                   // Current train has a finite number of pulses
bool stop_requested = false;            // Stop as soon as the current pulse ends
bool have_last_end = false;             // last_pulse_end_us is valid
bool pending_finish = false;            // A pulse ended; on_pulse_finished() still has to run
uint32_t last_pulse_end_us = 0;         // Falling edge time of the last pulse

// Session report (shown by Comm)
uint32_t session_pulses = 0;            // Pulses fired in the last session
uint16_t timing_faults = 0;             // Edges placed later than LATE_TOLERANCE_US

// --- NPS SPECIFIC VARIABLES (For the Random Mode) ---
#define MAX_NPS_PULSES 50               // Maximum pulses per second allowed in NPS

// One NPS vector, generated one timestamp at a time.
typedef struct {
  uint16_t t[MAX_NPS_PULSES];           // Timestamps (ms) inside the 1 s window
  uint8_t n;                            // Number of pulses in the window
  uint8_t i;                            // How many timestamps are already generated
  uint16_t d;                           // Minimum interval (ms)
  long ini;                             // Algorithm state
  long fim;                             // Algorithm state
  bool ready;                           // All n timestamps generated
} NpsGen;

NpsGen nps_bufs[2];
NpsGen* nps_active = &nps_bufs[0];      // Vector being played
NpsGen* nps_pending = &nps_bufs[1];     // Vector being prepared for the next window
uint8_t nps_idx = 0;                    // Next pulse in the active vector
uint32_t nps_window_start_us = 0;       // Start of the current window
bool nps_window_open = false;           // A window has been started (trigger mode)

// Timing Variables
bool in_menu = false;                   // Are we currently editing a number?
bool val_change = true;                 // Do we need to redraw the screen?
uint32_t period_us = 0;                 // Calculated period in microseconds
uint32_t pulse_on_us = 0;               // Calculated pulse width in microseconds
uint32_t session_start_us = 0;          // Session start (same clock as the pulses, for Timer)
int adjusted_idx = -1;                  // Menu item changed automatically by a limit
// Manual button debounce: the button state only changes after the pin has been stable
// for BTN_STABLE_MS. One press = one pulse, however long it is held.
#define BTN_STABLE_MS 100
bool btnRaw = HIGH;                     // Last raw reading
bool btnStable = HIGH;                  // Debounced state
unsigned long btnChangeTime = 0;        // When the raw reading last changed

// =================================================================================
// 4. MENU STRUCTURE
// =================================================================================
enum menu_type { VALUE, ACTION, OPTION };

typedef struct {
  const char* name_;      // The text shown on LCD
  menu_type type;         // The type (VALUE/ACTION/OPTION)
  long value;             // The current setting
  long lim_min;           // Lowest allowed number
  long lim_max;           // Highest allowed number
  const char* options[3]; // List of text options
  const char* suffix;     // Unit label (Hz, ms, %)
} menu_item;

#define NUM_ITEMS 13 // Total number of menu pages
menu_item menu[NUM_ITEMS];
int menu_idx = 0;    // Which page are we looking at?

// =================================================================================
// 5. FUNCTION PROTOTYPES
// =================================================================================
void update_calculations();
void update_lcd();
void handle_inputs();
void stop_generation();
void request_stop();
void arm_engine();
void handle_trigger();
void service_pulse_start();
void service_pulse_end();
void on_pulse_finished();
void begin_nps_window(uint32_t t);
void nps_gen_reset(NpsGen* g);
void nps_gen_step(NpsGen* g);
void nps_gen_complete(NpsGen* g);
void nps_background_step();
bool is_hidden(int idx);
bool clamp_item(int idx);
void show_adjust_notice(int idx);
void update_cursor_position();
void execute_action();
void check_single_pulse_btn();
void update_interrupt_config();
long eepromReadLong(int adr);
void eepromUpdateLong(int adr, long wert);

// =================================================================================
// 6. INTERRUPT SERVICE ROUTINES
// =================================================================================

// This runs automatically in the background to read the knob rotation
void encoderTimerIsr() { encoder.service(); }

// This runs immediately when Pin 3 detects a signal (The External Trigger).
// Whether the trigger is accepted is decided in loop() (see handle_trigger).
void triggerIsr() {
  if (isTriggerMode && generating) triggerEvent = true;
}

// =================================================================================
// 7. OUTPUT AND PRECISE WAIT HELPERS
// =================================================================================
static inline void output_high() { PORTD |= (1 << PD7); }
static inline void output_low()  { PORTD &= ~(1 << PD7); }

// Busy-wait until micros() reaches t (wrap-safe)
static inline void spin_until(uint32_t t) {
  while ((int32_t)(micros() - t) < 0) { }
}

// Session Timer: true if time t is at or after the end of the session.
// Uses micros(), the same clock that places the pulses, so a pulse scheduled exactly
// at the end of the session is not emitted (1 Hz for 10 s = 10 pulses).
static inline bool session_over_at(uint32_t t) {
  if (menu[10].value <= 0) return false;
  return (uint32_t)(t - session_start_us) >= (uint32_t)menu[10].value * 1000UL;
}

// =================================================================================
// 8. SETUP (Runs once when you turn it on)
// =================================================================================
void setup() {
  // Configure Port D7 as Output
  DDRD |= (1 << DDD7);
  output_low();

  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  pinMode(SINGLE_PULSE_PIN, INPUT_PULLUP);

  // Initialize random generator using noise from an empty pin
  randomSeed(analogRead(A0));

  Serial.begin(9600);

  // --- CONFIGURE THE MENU ITEMS ---
  // Format: {Name, Type, Default, Min, Max, Options, Unit}
  menu[0] = {"Mode",   OPTION, 0, 0, 1, {"Manual", "Trigger"}, ""};
  menu[1] = {"Edge",   OPTION, 0, 0, 1, {"Falling", "Rising "}, ""};
  menu[2] = {"Type",   OPTION, 0, 0, 2, {"Cont.", "Burst", "NPS  "}, ""};
  menu[3] = {"State",  ACTION, 0, 0, 0, {}, "OFF"};
  menu[4] = {"Freq",   VALUE,  1000, 10, 50000, {}, "Hz"};   // centi-Hz: 1000 = 10.00 Hz

  // [5] COUNT: Integer number (Pulses)
  menu[5] = {"Count",  VALUE,  10, 1, 999, {}, ""};

  // [6] GAP: Integer Time (Burst silence). 1000 = 1000 ms.
  menu[6] = {"Gap ",   VALUE,  1000, 0, 999999, {}, "ms"};

  // [7] ITImin: Integer Time (NPS minimum interval). 20 = 20 ms.
  menu[7] = {"ITImin", VALUE,  20, 1, 999, {}, "ms"};

  menu[8] = {"Width",  OPTION, 0, 0, 1, {"Fixed", "D.Cycle"}, ""};

  // [9] PULSE: Decimal. Fixed: 500 = 5.00 ms (units of 10 us). Duty: 500 = 5.00 %.
  menu[9] = {"Pulse",  VALUE,  500, 5, 99999, {}, "ms"};

  // [10] TIMER: Integer Time. 0 = Disabled.
  menu[10]= {"Timer",  VALUE,  0, 0, 999999, {}, "ms"};

  menu[11]= {"Save",   ACTION, 0, 0, 0, {}, ""};
  menu[12]= {"Comm",   ACTION, 0, 0, 0, {}, ""};

  // Restore saved settings from EEPROM only if the signature is valid
  if (eepromReadLong(0) == EEPROM_MAGIC && eepromReadLong(4) == EEPROM_VERSION) {
    for (int i = 0; i < NUM_ITEMS; i++) {
      if (menu[i].type == ACTION) continue;
      long val = eepromReadLong(EEPROM_DATA + i * 4);
      if (val >= menu[i].lim_min && val <= menu[i].lim_max) menu[i].value = val;
    }
  }

  // Trigger interrupt (edge configured from the menu)
  update_interrupt_config();

  // Setup the Knob Timer (250 microseconds speed)
  Timer1.initialize(250);
  Timer1.attachInterrupt(encoderTimerIsr);

  // Start the LCD Screen
  lcd.init(); lcd.backlight(); lcd.clear();
  lcd.createChar(0, dinoChar);
  lcd.setCursor(0, 0); lcd.write(byte(0)); lcd.print(" Gila Monster");
  lcd.setCursor(0, 1); lcd.print(" Pulse Generator");
  delay(2000); lcd.clear();

  update_calculations();
  adjusted_idx = -1;   // No "Auto-adjusted" notice at boot
  update_lcd();
}

// =================================================================================
// 9. MAIN LOOP
// =================================================================================
void loop() {
  handle_inputs(); // Check knob and buttons (does nothing visible while generating)

  // --- INSTANT START LOGIC ---
  // The engine is armed only after the LCD has been redrawn, so the first pulse is not delayed.
  if (armed_for_start) {
    armed_for_start = false;
    arm_engine();
  }

  check_single_pulse_btn();

  // --- THE PULSE ENGINE ---
  if (generating) {
    // Session Timer: request a stop, the current pulse is allowed to finish
    if (!stop_requested && session_over_at(micros())) request_stop();

    service_pulse_end();
    if (pending_finish) { pending_finish = false; on_pulse_finished(); }

    if (stop_requested) {
      if (!pulse_active) stop_generation();
    } else {
      handle_trigger();
      service_pulse_start();
      if (pending_finish) { pending_finish = false; on_pulse_finished(); }
      nps_background_step();
    }
  } else {
    // Safety: Ensure output is Low when stopped
    output_low();
    triggerEvent = false;
  }
}

// =================================================================================
// 10. PULSE ENGINE
// =================================================================================

// Prepares the engine right after switching ON
void arm_engine() {
  output_low();
  pulse_active = false;
  train_active = false;
  stop_requested = false;
  have_last_end = false;
  nps_window_open = false;
  nps_idx = 0;
  session_pulses = 0;
  timing_faults = 0;

  int type = menu[2].value;
  if (type == 2) {
    // First NPS vector is computed now (nothing is firing yet)
    nps_gen_reset(nps_pending);
    nps_gen_complete(nps_pending);
  }

  // The session starts now; in Manual mode the first pulse is at exactly this instant
  session_start_us = micros();
  if (type == 2) {
    if (!isTriggerMode) begin_nps_window(session_start_us);
  } else if (!isTriggerMode) {
    counted = (type == 1);
    pulsesRemaining = counted ? menu[5].value : 0;
    next_pulse_us = session_start_us;
    train_active = true;
  }
  triggerEvent = false;
}

// Accepts an external trigger only when the previous protocol has completed
void handle_trigger() {
  if (!isTriggerMode || !triggerEvent) return;
  triggerEvent = false;
  if (pulse_active || train_active) return;           // Still running: ignore

  uint32_t now = micros();
  if (have_last_end && (now - last_pulse_end_us) < MIN_LOW_US) return;

  if (menu[2].value == 2) {
    // NPS: one full 1 s window per trigger
    if (nps_window_open && (int32_t)(now - nps_window_start_us) < (int32_t)NPS_WINDOW_US) return;
    begin_nps_window(now);
  } else {
    // Cont: one pulse. Burst: Count pulses.
    counted = true;
    pulsesRemaining = (menu[2].value == 1) ? menu[5].value : 1;
    next_pulse_us = now;
    train_active = true;
  }
}

// Places the rising edge of the next scheduled pulse
void service_pulse_start() {
  if (pulse_active || !train_active) return;

  uint32_t now = micros();
  int32_t until = (int32_t)(next_pulse_us - now);
  if (until > (int32_t)SPIN_WINDOW_US) return;       // Not yet

  // A pulse scheduled at or after the end of the session is not emitted
  uint32_t due = (until >= 0) ? next_pulse_us : now;
  if (session_over_at(due)) { request_stop(); return; }

  // Final approach with interrupts masked: no ISR can run between the time check and the
  // port write. Masked time is at most SPIN_WINDOW_US + a short pulse (< 1 ms, so millis()
  // never loses a Timer0 overflow).
  uint8_t sreg = SREG;
  cli();

  uint32_t t;
  if (until >= 0) {
    spin_until(next_pulse_us);
    t = next_pulse_us;
  } else {
    t = micros();                                    // Late: fire now and report it
    if (-until > (int32_t)LATE_TOLERANCE_US) timing_faults++;
  }

  output_high();
  pulse_active = true;
  pulse_start_us = t;
  session_pulses++;

  if (menu[2].value == 2) {
    nps_idx++;
    if (nps_idx < nps_active->n) {
      next_pulse_us = nps_window_start_us + (uint32_t)nps_active->t[nps_idx] * 1000UL;
    } else {
      train_active = false;                          // Window complete
    }
  } else {
    next_pulse_us = t + period_us;
    if (counted) {
      pulsesRemaining--;
      if (pulsesRemaining <= 0) train_active = false; // This was the last pulse of the burst
    }
  }

  service_pulse_end();   // Short pulses are finished right here, with a precise wait
  SREG = sreg;
}

// Places the falling edge of the active pulse
void service_pulse_end() {
  if (!pulse_active) return;

  uint32_t end_t = pulse_start_us + pulse_on_us;
  int32_t rem = (int32_t)(end_t - micros());
  if (rem > (int32_t)SPIN_WINDOW_US) return;         // Not yet

  uint8_t sreg = SREG;
  cli();                                             // Final approach with interrupts masked
  if (rem >= 0) {
    spin_until(end_t);
  } else {
    if (-rem > (int32_t)LATE_TOLERANCE_US) timing_faults++;
    end_t = micros();
  }

  output_low();
  SREG = sreg;
  pulse_active = false;
  last_pulse_end_us = end_t;
  have_last_end = true;
  pending_finish = true;
}

// Decides what comes after a pulse has ended
void on_pulse_finished() {
  if (stop_requested || isTriggerMode || train_active) return;

  if (menu[2].value == 2) {
    // Manual NPS: next window starts exactly 1 s after the current one
    begin_nps_window(nps_window_start_us + NPS_WINDOW_US);
  } else if (menu[2].value == 1 && counted) {
    // Manual Burst finished
    if (menu[6].value == 0) {
      request_stop();                                // Gap 0 = single burst
    } else {
      pulsesRemaining = menu[5].value;
      next_pulse_us = last_pulse_end_us + (uint32_t)menu[6].value * 1000UL;
      train_active = true;
    }
  }
}

void request_stop() {
  stop_requested = true;
  train_active = false;
}

void stop_generation() {
  generating = false;
  menu[3].suffix = "OFF";
  output_low();
  val_change = true;
  triggerEvent = false;
  pulse_active = false;
  train_active = false;
  pulsesRemaining = 0;
  armed_for_start = false;
  stop_requested = false;
}

// =================================================================================
// 11. NPS LOGIC
// =================================================================================
// Sequential shrinking-window algorithm, split so that one timestamp can be generated
// at a time (in the background, away from pulse edges).

void nps_gen_reset(NpsGen* g) {
  long n = menu[5].value;
  if (n > MAX_NPS_PULSES) n = MAX_NPS_PULSES;
  g->n = (uint8_t)n;
  g->d = (uint16_t)menu[7].value;
  g->i = 0;
  g->ini = 0;
  g->fim = 1000L - ((long)(g->n - 1) * g->d);
  if (g->fim < 0) g->fim = 0;
  g->ready = (g->n == 0);
}

void nps_gen_step(NpsGen* g) {
  if (g->ready) return;
  long auxNorm = random(g->ini, g->fim);

  // Enforce constraint
  if (auxNorm - g->ini < g->d) auxNorm = g->ini + g->d;

  g->t[g->i] = (uint16_t)auxNorm;
  g->ini = auxNorm;
  g->fim = g->fim + g->d;
  if (g->fim > 1000) g->fim = 1000;
  g->i++;
  if (g->i >= g->n) g->ready = true;
}

void nps_gen_complete(NpsGen* g) {
  while (!g->ready) nps_gen_step(g);
}

// Starts a new 1 s window at time t with the pending vector
void begin_nps_window(uint32_t t) {
  if (!nps_pending->ready) {
    timing_faults++;                     // Vector was not ready in time
    nps_gen_complete(nps_pending);
  }
  NpsGen* tmp = nps_active;
  nps_active = nps_pending;
  nps_pending = tmp;

  nps_window_start_us = t;
  nps_window_open = true;
  nps_idx = 0;
  if (nps_active->n > 0) {
    next_pulse_us = t + (uint32_t)nps_active->t[0] * 1000UL;
    train_active = true;
  }

  nps_gen_reset(nps_pending);            // Prepared in the background
}

// Generates one timestamp of the next vector, only when no edge is close
void nps_background_step() {
  if (menu[2].value != 2 || nps_pending->ready) return;
  uint32_t now = micros();
  if (pulse_active && (int32_t)(pulse_start_us + pulse_on_us - now) < (int32_t)NPS_STEP_GUARD_US) return;
  if (train_active && (int32_t)(next_pulse_us - now) < (int32_t)NPS_STEP_GUARD_US) return;
  nps_gen_step(nps_pending);
}

// =================================================================================
// 12. HELPER FUNCTIONS
// =================================================================================

// Configures the Trigger Pin to react to Rising or Falling signal
void update_interrupt_config() {
  if (last_edge_mode != (int)menu[1].value) {
    detachInterrupt(digitalPinToInterrupt(TRIGGER_PIN));
    int mode = (menu[1].value == 0) ? FALLING : RISING;
    attachInterrupt(digitalPinToInterrupt(TRIGGER_PIN), triggerIsr, mode);
    last_edge_mode = (int)menu[1].value;
  }
}

// Reads the manual button with debounce and fires one pulse with the programmed width
// on each confirmed press. Works only when State = OFF.
void check_single_pulse_btn() {
  bool reading = digitalRead(SINGLE_PULSE_PIN);
  unsigned long now = millis();

  if (reading != btnRaw) {             // Contact moved (or bounced): restart the stability timer
    btnRaw = reading;
    btnChangeTime = now;
  }

  if (reading != btnStable && (now - btnChangeTime) >= BTN_STABLE_MS) {
    btnStable = reading;               // State confirmed
    if (btnStable == LOW && !generating) {
      uint8_t sreg = SREG;
      cli();
      uint32_t t = micros();
      output_high();
      if (pulse_on_us > SPIN_WINDOW_US) {            // Long pulse: interrupts on until close to the end
        SREG = sreg;
        spin_until(t + pulse_on_us - SPIN_WINDOW_US);
        cli();
      }
      spin_until(t + pulse_on_us);
      output_low();
      SREG = sreg;
    }
  }
}

// Clamps a menu value to its limits. Returns true if the value changed.
bool clamp_item(int idx) {
  long v = menu[idx].value;
  if (v < menu[idx].lim_min) v = menu[idx].lim_min;
  if (v > menu[idx].lim_max) v = menu[idx].lim_max;
  if (v != menu[idx].value) {
    menu[idx].value = v;
    if (adjusted_idx < 0) adjusted_idx = idx;
    return true;
  }
  return false;
}

// Converts menu numbers into microsecond timing values.
// All limits are applied to the MENU VALUES, so the LCD always shows what is produced.
void update_calculations() {
  bool nps = (menu[2].value == 2);

  // Frequency to Period conversion (menu[4] is in centi-Hz)
  if (menu[4].value > 0) period_us = 100000000UL / (uint32_t)menu[4].value;

  isTriggerMode = (menu[0].value == 1);
  update_interrupt_config();

  // --- Width: NPS has no period, so only Fixed time makes sense ---
  menu[8].lim_max = nps ? 0 : 1;
  if (menu[8].value > menu[8].lim_max) menu[8].value = menu[8].lim_max;

  // Convert the pulse value when the Width logic changes (64-bit to avoid overflow)
  static int last_tmode = -1;
  if (last_tmode != -1 && last_tmode != (int)menu[8].value) {
    if (menu[8].value == 1) menu[9].value = (long)(((uint64_t)menu[9].value * 100000ULL) / period_us);
    else                    menu[9].value = (long)(((uint64_t)menu[9].value * period_us) / 100000ULL);
  }
  last_tmode = (int)menu[8].value;

  // --- Count: in NPS, Count x ITImin must fit inside the 1 s window ---
  if (nps) {
    long max_by_iti = 999L / menu[7].value;
    menu[5].lim_max = (max_by_iti < MAX_NPS_PULSES) ? max_by_iti : MAX_NPS_PULSES;
    if (menu[5].lim_max < 1) menu[5].lim_max = 1;
  } else {
    menu[5].lim_max = 999;
  }
  clamp_item(5);

  // --- Pulse width limits in microseconds ---
  uint32_t min_us = nps ? MIN_HIGH_US_NPS : MIN_HIGH_US_STD;
  uint32_t max_us = nps ? ((uint32_t)menu[7].value * 1000UL - MIN_LOW_US)
                        : (period_us - MIN_LOW_US);

  if (menu[8].value == 0) {
    // Fixed: value in units of 10 us, editor allows up to 999.99 ms
    menu[9].lim_min = (long)((min_us + 9) / 10);
    uint32_t mx = max_us / 10;
    menu[9].lim_max = (mx > 99999UL) ? 99999L : (long)mx;
    menu[9].suffix = "ms";
  } else {
    // Duty: value in units of 0.01 %, rounded so the real width stays inside the limits
    menu[9].lim_min = (long)(((uint64_t)min_us * 10000ULL + period_us - 1) / period_us);
    menu[9].lim_max = (long)(((uint64_t)max_us * 10000ULL) / period_us);
    menu[9].suffix = "% ";
  }
  clamp_item(9);

  // Final Pulse Width (exactly what the LCD shows)
  if (menu[8].value == 0) pulse_on_us = (uint32_t)menu[9].value * 10UL;
  else pulse_on_us = (uint32_t)(((uint64_t)period_us * (uint32_t)menu[9].value) / 10000ULL);
}

// =================================================================================
// 13. UI HANDLERS (Screen and Buttons)
// =================================================================================

// Items that do not apply to the current configuration are skipped
bool is_hidden(int idx) {
  if (menu[0].value == 0 && idx == 1) return true;                    // Edge in Manual
  if (menu[2].value == 0 && idx >= 5 && idx <= 7) return true;        // Cont: Count, Gap, ITImin
  if (menu[2].value == 1 && idx == 7) return true;                    // Burst: ITImin
  if (menu[2].value == 2 && (idx == 4 || idx == 6)) return true;      // NPS: Freq, Gap
  return false;
}

void show_adjust_notice(int idx) {
  lcd.noBlink();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Auto-adjusted:");
  lcd.setCursor(0, 1); lcd.print(menu[idx].name_);
  delay(1500);
  lcd.clear();
}

void handle_inputs() {
  ClickEncoder::Button b = encoder.getButton();
  int16_t diff = encoder.getValue();

  // --- LOCKED WHILE GENERATING: only a click on State (stop) is accepted, nothing is drawn ---
  if (generating) {
    if (b == ClickEncoder::Clicked && menu_idx == 3 && !stop_requested) request_stop();
    return;
  }

  // --- IF BUTTON CLICKED ---
  if (b == ClickEncoder::Clicked) {
    if (menu[menu_idx].type == VALUE) {
      // Enter Edit Mode
      if (!in_menu) { in_menu = true; edit_digit = 0; }
      else {
        // Move to next digit
        edit_digit++;

        // Define how many digits each menu item has
        int max_d = 4; // Default (decimals: 5 digits)
        if (menu_idx == 5 || menu_idx == 7) max_d = 2;   // Count, ITImin (3 digits)
        if (menu_idx == 6 || menu_idx == 10) max_d = 5;  // Gap/Timer (6 digits)

        // If we pass the last digit, exit edit mode
        if (edit_digit > max_d) {
          in_menu = false;
          adjusted_idx = -1;
          update_calculations();
          if (adjusted_idx >= 0 && adjusted_idx != menu_idx) show_adjust_notice(adjusted_idx);
        }
      }
    } else if (menu[menu_idx].type == OPTION) {
      // Toggle Options (e.g., Manual -> Trigger)
      in_menu = !in_menu;
      if (!in_menu) {
        adjusted_idx = -1;
        update_calculations();
        if (adjusted_idx >= 0 && adjusted_idx != menu_idx) show_adjust_notice(adjusted_idx);
        // Safety: never stay on a hidden item
        while (is_hidden(menu_idx)) menu_idx = (menu_idx + 1) % NUM_ITEMS;
      }
    } else if (!in_menu) execute_action();
    val_change = true;
  }

  // --- IF KNOB ROTATED ---
  if (diff != 0) {
    if (!in_menu) {
      // NAVIGATION MODE: one item per step, skipping hidden items
      int step = (diff > 0) ? 1 : -1;
      int steps = (diff > 0) ? diff : -diff;
      for (int s = 0; s < steps; s++) {
        do {
          menu_idx = (menu_idx + step + NUM_ITEMS) % NUM_ITEMS;
        } while (is_hidden(menu_idx));
      }
    } else {
      // EDIT MODE (Changing a value)
      if (menu[menu_idx].type == VALUE) {
        uint32_t mult;

        // Count(5), ITImin(7): 3-digit integer
        if (menu_idx == 5 || menu_idx == 7) mult = digit_multipliers[edit_digit + 3];

        // Gap(6), Timer(10): 6-digit integer
        else if (menu_idx == 6 || menu_idx == 10) mult = digit_multipliers[edit_digit];

        // Freq(4), Pulse(9): Decimal Logic (Skipping the dot)
        else mult = digit_multipliers[edit_digit + 1];

        // Apply the change to the specific digit
        int digit = (menu[menu_idx].value / mult) % 10;
        int new_digit = (digit + diff) % 10;
        if (new_digit < 0) new_digit += 10;
        menu[menu_idx].value = menu[menu_idx].value - (long)digit * mult + (long)new_digit * mult;

        // Enforce Limits
        if (menu[menu_idx].value < menu[menu_idx].lim_min) menu[menu_idx].value = menu[menu_idx].lim_min;
        if (menu[menu_idx].value > menu[menu_idx].lim_max) menu[menu_idx].value = menu[menu_idx].lim_max;
      }
      else {
        // Option Menu Logic (Cycling choices)
        menu[menu_idx].value += diff;
        if (menu[menu_idx].value > menu[menu_idx].lim_max) menu[menu_idx].value = 0;
        if (menu[menu_idx].value < 0) menu[menu_idx].value = menu[menu_idx].lim_max;
      }
    }
    val_change = true;
  }
  if (val_change) { update_lcd(); if (in_menu) update_cursor_position(); else lcd.noBlink(); val_change = false; }
}

void update_lcd() {
  char buffer[12];

  // Line 0: current item. Line 1: next visible item.
  int idx_line[2];
  idx_line[0] = menu_idx;
  int nxt = menu_idx;
  do { nxt = (nxt + 1) % NUM_ITEMS; } while (is_hidden(nxt));
  idx_line[1] = nxt;

  for (int i = 0; i < 2; i++) {
    int idx = idx_line[i];

    // Draw Menu Name
    lcd.setCursor(0, i);
    lcd.print(i == 0 ? ">" : " ");
    lcd.print(menu[idx].name_);
    int nameLen = strlen(menu[idx].name_);
    for (int s = 0; s < (6 - nameLen); s++) lcd.print(" ");
    lcd.print(":");

    lcd.setCursor(8, i);
    lcd.print("        "); // Clear value area
    lcd.setCursor(8, i);

    // Draw Value
    if (menu[idx].type == VALUE) {
      if (idx == 5 || idx == 7) sprintf(buffer, "%03ld", menu[idx].value);       // Count, ITImin (000)

      else if (idx == 6 || idx == 10) sprintf(buffer, "%06ld", menu[idx].value); // Gap/Timer (000000)

      else { // Decimals: Pulse(9), Freq(4)
        float display_val = (float)menu[idx].value / 100.0;
        if (display_val < 100.0) lcd.print("0");
        if (display_val < 10.0) lcd.print("0");
        dtostrf(display_val, 1, 2, buffer);
      }
      lcd.print(buffer);
      lcd.print(menu[idx].suffix);
    } else if (menu[idx].type == OPTION) lcd.print(menu[idx].options[menu[idx].value]);
    else lcd.print(menu[idx].suffix);
  }
}

void update_cursor_position() {
  int col_num = 8;
  if (menu[menu_idx].type == OPTION) { lcd.setCursor(col_num, 0); lcd.blink(); return; }

  // Logic to place the blinking cursor on the right digit
  if (menu_idx == 5 || menu_idx == 7) lcd.setCursor(col_num + edit_digit, 0); // Count, ITImin

  else if (menu_idx == 6 || menu_idx == 10) lcd.setCursor(col_num + edit_digit, 0); // Gap/Timer

  // For decimals, we skip the dot position (Index 3)
  else {
    int offsets[] = {0, 1, 2, 4, 5};
    lcd.setCursor(col_num + offsets[edit_digit], 0);
  }
  lcd.blink();
}

// =================================================================================
// 14. EXECUTE ACTION (Switching ON/OFF, Saving, Comm)
// =================================================================================
// Only reached while NOT generating (the UI is locked during generation).
void execute_action() {

  // --- [3] STATE: ON ---
  if (menu_idx == 3) {
    generating = true;
    triggerEvent = false;
    armed_for_start = true;     // Engine is armed in loop(), after the LCD redraw
    menu[3].suffix = "ON ";
  }

  // --- [11] SAVE: Store parameters to EEPROM ---
  else if (menu_idx == 11) {
    eepromUpdateLong(0, EEPROM_MAGIC);
    eepromUpdateLong(4, EEPROM_VERSION);
    for (int i = 0; i < NUM_ITEMS; i++) eepromUpdateLong(EEPROM_DATA + i * 4, menu[i].value);
    lcd.setCursor(8,0); lcd.print("SAVED"); delay(600);
  }

  // --- [12] COMM: Serial Dump & NPS Simulation ---
  else if (menu_idx == 12) {

    // ---------------------------------------------------------
    // PART 1: PRINT CURRENT SETTINGS (FOR ALL MODES)
    // ---------------------------------------------------------
    Serial.println(F("=== Gila Monster Settings ==="));

    Serial.print(F("Mode:  "));
    Serial.println(menu[0].value ? "Trigger" : "Manual");

    // Only show Trigger Edge if in Trigger Mode
    if (menu[0].value == 1) {
       Serial.print(F("Edge:  "));
       Serial.println(menu[1].value ? "Rising" : "Falling");
    }

    Serial.print(F("Type:  "));
    int type = menu[2].value; // 0=Cont, 1=Burst, 2=NPS
    if (type == 0) Serial.println("Continuous");
    else if (type == 1) Serial.println("Burst");
    else Serial.println("NPS");

    Serial.println(F("--- Parameters ---"));

    // Frequency (Hide in NPS mode)
    if (type != 2) {
       Serial.print(F("Freq:  "));
       Serial.print((float)menu[4].value / 100.0);
       Serial.print(F(" Hz (period "));
       Serial.print(period_us);
       Serial.println(F(" us)"));
    }

    // Pulse Count (Hide in Continuous mode)
    if (type != 0) {
       Serial.print(F("Count: "));
       Serial.println(menu[5].value);
    }

    // Inter-Burst silence (Show only in Burst mode)
    if (type == 1) {
       Serial.print(F("Gap:   "));
       Serial.print(menu[6].value);
       Serial.println(menu[6].value == 0 ? " ms (single burst)" : " ms");
    }

    // NPS Minimum Interval (Show only in NPS mode)
    if (type == 2) {
       Serial.print(F("Min:   "));
       Serial.print(menu[7].value);
       Serial.println(" ms");
    }

    // Common parameters for all modes
    Serial.print(F("Width: "));
    Serial.println(menu[8].value ? "Duty Cycle" : "Fixed Time");

    Serial.print(F("Pulse: "));
    Serial.print((float)menu[9].value / 100.0);
    Serial.print(menu[8].value ? " %" : " ms");
    Serial.print(F(" (HIGH time "));
    Serial.print(pulse_on_us);
    Serial.println(F(" us)"));

    Serial.print(F("Timer: "));
    Serial.print(menu[10].value);
    Serial.println(" ms");

    Serial.println(F("--- Last session ---"));
    Serial.print(F("Pulses fired:  "));
    Serial.println(session_pulses);
    Serial.print(F("Timing faults: "));
    Serial.println(timing_faults);

    Serial.println(F("============================="));

    // ---------------------------------------------------------
    // PART 2: IF NPS MODE, GENERATE RAW SIMULATION DATA
    // (same generator the engine uses; windows back-to-back as in Manual mode)
    // ---------------------------------------------------------
    if (type == 2) {

        // Calculate simulation duration based on the Timer parameter
        long duration_sec = 10; // Default to 10s if Timer is disabled (0)
        if (menu[10].value > 0) {
            duration_sec = menu[10].value / 1000;
            if (duration_sec < 1) duration_sec = 1; // Minimum 1 second
        }

        Serial.println(); // Blank line for readability
        Serial.print(F(">>> NPS RAW ITI DATA ("));
        Serial.print(duration_sec);
        Serial.println(F(" sec simulation. Default 10s to illustrate distribution if Timer is 0) <<<"));
        Serial.println(F("[ITI_ms]")); // CSV Header

        NpsGen* g = nps_pending;   // Free while not generating
        long last_pulse_abs_time = 0;
        long window_offset = 0;
        bool first = true;

        for (long sec = 0; sec < duration_sec; sec++) {
            nps_gen_reset(g);
            nps_gen_complete(g);

            for (int i = 0; i < g->n; i++) {
                long current_abs_time = window_offset + g->t[i];
                if (!first) Serial.println(current_abs_time - last_pulse_abs_time);
                first = false;
                last_pulse_abs_time = current_abs_time;
            }
            window_offset += 1000; // Move window forward by 1 second
        }
        Serial.println(F(">>> End of Data <<<"));
    }

    //  LCD feedback
    lcd.setCursor(8, 0); lcd.print("SENT "); delay(800);
  }
}

// =================================================================================
// 15. EEPROM UTILS
// =================================================================================
long eepromReadLong(int adr) {
  long wert = 0;
  for (int i = 0; i < 4; i++) wert |= ((long)EEPROM.read(adr + i) & 0xFF) << (i*8);
  return wert;
}

void eepromUpdateLong(int adr, long wert) {
  for (int i = 0; i < 4; i++) EEPROM.update(adr + i, (wert >> (i*8)) & 0xFF);
}

// =================================================================================
// Listening: Sonic Youth - Starpower
// Thu Feb 12 - 16:53
