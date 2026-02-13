/* * ======================================================================================
 * PROJECT: Gila Monster Pulse Generator
 * VERSION: 1.0 
 * AUTHOR:  Flavio Mourao - Feb, 2026

 * * DESCRIPTION:
 * Pulse generator
 * It runs on Arduino Uno and generates electrical pulses in three different modes.
 
 * MODES OF OPERATION:
 * 1. Continuous:  Regular pulses at a specific and periodic frequency (Hz).
 * 2. Burst:       Groups of pulses (trains) separated by a pause (Gap).
 * 3. NPS:         "Non-Periodic Stimulation". A mode where pulses occur randomly within a 1-second window.
 *                  https://doi.org/10.1016/j.yebeh.2019.106609
 *                  https://doi.org/10.1016/j.yebeh.2008.09.006
 
 * KEY FEATURES:
 * - Precision: Uses microsecond timing for accuracy.
 * - Split Intervals: Separate settings for Burst Gaps and NPS Min Intervals.
 * - Safety: Software locks to prevent dangerous duty cycles (100% ON).
 * - External Trigger: Can be started by another machine via Pin D3.
 *
 * MENU REFERENCE (What you see on the screen):
 * [0] Mode   : Manual (Auto-run) or Trigger (Waits for signal).
 * [1] Edge   : Signal type for Trigger (Rising or Falling).
 * [2] Type   : The Engine (Cont, Burst, or NPS).
 * [3] State  : The Master Switch (ON / OFF).
 * [4] Freq   : Frequency in Hz (0.1 to 500 Hz. Only for Cont./Burst).
 * [5] Count  : How many pulses? (Per Burst or Per Second in NPS).
 * [6] Gap    : Time between Bursts (Integer ms). Used in BURST mode.
 * [7] ITImin : Minimum time between random pulses (Decimal ms). Used in NPS.
 * [8] Width  : Logic for pulse duration (Fixed time vs Duty Cycle %).
 * [9] Pulse  : The duration of the pulse (High time).
 * [10] Timer : Auto-stop the system (0 = disabled).
 * [11] Save  : Saves settings to memory (EEPROM).
 * [12] Comm  : Sends protocol to USB/Computer.
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
// 2. GLOBAL VARIABLES
// =================================================================================

// Helpers for the Menu System
int edit_digit = 0;                     // Which digit are we editing? (1s, 10s, 100s...)
uint32_t digit_multipliers[] = {100000, 10000, 1000, 100, 10, 1}; 
byte dinoChar[8] = { B00000, B00111, B00101, B10111, B11100, B11111, B01101, B01100 }; // The little lizard logo

// State Flags
volatile bool generating = false;       // Master Switch: true = ON, false = OFF
volatile bool triggerEvent = false;     // Did we receive an external signal?
volatile long pulsesRemaining = 0;      // How many pulses are left to fire in this burst?

// Safety and Logic flags
volatile bool isTriggerMode = false;    // External pin?
int last_edge_mode = -1;                // Remembers if we trigger on Rising or Falling edge
bool armed_for_start = false;           // Helps start the system instantly without delay

// --- NPS SPECIFIC VARIABLES (For the Random Mode) ---
#define MAX_NPS_PULSES 50                // Maximum pulses per second allowed in NPS
uint16_t nps_timestamps[MAX_NPS_PULSES]; // A list to store the random times
int nps_pulse_idx = 0;                   // Which pulse in the list are we playing?
uint32_t nps_window_start = 0;           // When did the current "1 second window" start?

// Timing Variables (The Clocks)
bool in_menu = false;                   // Are we currently editing a number?
bool val_change = true;                 // Do we need to redraw the screen?
uint32_t period_us = 0;                 // Calculated period in microseconds
uint32_t pulse_on_us = 0;               // Calculated pulse width in microseconds
uint32_t last_micros = 0;               // Time of the last pulse
uint32_t startTime = 0;                 // When did we turn the system ON? (For Timer)
uint32_t lastBurstEndTime = 0;          // When did the last burst finish?
bool lastSingleBtnState = HIGH;         // For the manual button logic
bool btnLocked = false;                 // Locks the button to prevent double-firing

unsigned long lastPulseTime = 0; 
const unsigned long debounceDelay = 200; // Noise filter time for buttons

// =================================================================================
// 3. MENU STRUCTURE
// =================================================================================
//  3 types of menu items:
// VALUE: A number you can change (e.g., 100 Hz).
// ACTION: Something that does a task (e.g., Save, ON/OFF).
// OPTION: A text list you pick from (e.g., Manual/Trigger).

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
// 4. FUNCTION PROTOTYPES
// =================================================================================
void update_calculations();
void update_lcd();
void handle_inputs();
void stop_generation();
void run_pulse_generator();
void run_nps_logic();        
void calculate_nps_vector(); 
void update_cursor_position();
void execute_action();
void check_single_pulse_btn(); 
void update_interrupt_config(); 
long eepromReadLong(int adr);
void eepromUpdateLong(int adr, long wert);

// =================================================================================
// 5. INTERRUPT SERVICE ROUTINES
// =================================================================================

// This runs automatically in the background to read the knob rotation
void encoderTimerIsr() { encoder.service(); }

// This runs immediately when Pin 3 detects a signal (The External Trigger)
void triggerIsr() {
  // Only accept trigger if we are in the right mode, system is ON, 
  // and we are not currently firing a pulse.
  if (isTriggerMode && generating && !(PORTD & (1 << PD7))) { 
    triggerEvent = true; 
  }
}

// =================================================================================
// 6. SETUP (Runs once when you turn it on)
// =================================================================================
void setup() {
  // Configure Port D7 as Output
  DDRD |= (1 << DDD7);         
  PORTD &= ~(1 << PD7);        
  
  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  pinMode(SINGLE_PULSE_PIN, INPUT_PULLUP);
  
  // Initialize random generator using noise from an empty pin
  randomSeed(analogRead(A0));
  
  Serial.begin(9600);
  
  // Set up the trigger
  attachInterrupt(digitalPinToInterrupt(TRIGGER_PIN), triggerIsr, FALLING);

  // --- CONFIGURE THE MENU ITEMS ---
  // Format: {Name, Type, Default, Min, Max, Options, Unit}
  
  menu[0] = {"Mode",   OPTION, 0, 0, 1, {"Manual", "Trigger"}, ""};
  menu[1] = {"Edge",   OPTION, 0, 0, 1, {"Falling", "Rising "}, ""}; 
  menu[2] = {"Type",   OPTION, 0, 0, 2, {"Cont.", "Burst", "NPS  "}, ""};
  menu[3] = {"State",  ACTION, 0, 0, 0, {}, "OFF"}; 
  menu[4] = {"Freq",   VALUE,  1000, 10, 50000, {}, "Hz"}; 
  
  // [5] COUNT: Integer number (Pulses)
  menu[5] = {"Count",  VALUE,  10, 1, 999, {}, ""}; 
  
  // [6] GAP: Integer Time (Burst Interval). 1000 = 1000ms.
  menu[6] = {"Gap ",   VALUE,  1000, 0, 999999, {}, "ms"};
  
  // [7] MIN: Decimal Time (NPS Interval). 2000 = 20.00ms.
  menu[7] = {"ITImin",   VALUE,  2000, 0, 999999, {}, "ms"};
  
  menu[8] = {"Width",  OPTION, 0, 0, 1, {"Fixed", "D.Cycle"}, ""};
  
  // [9] PULSE: Decimal Time. 500 = 5.00ms.
  menu[9] = {"Pulse",  VALUE,  500, 1, 100000, {}, "ms"};  
  
  // [10] TIMER: Integer Time. 0 = Disabled.
  menu[10]= {"Timer",  VALUE,  0, 0, 999999, {}, "ms"}; 
  
  menu[11]= {"Save",   ACTION, 0, 0, 0, {}, ""}; 
  menu[12]= {"Comm",   ACTION, 0, 0, 0, {}, ""};    

  // Restore saved settings from chip memory (EEPROM)
  for (int i = 0; i < NUM_ITEMS; i++) {
    long val = eepromReadLong(i * 4);
    // If memory is valid, load it.
    if (val != -1 && val != 0xFFFFFFFF) {
       if (val >= menu[i].lim_min && val <= menu[i].lim_max) menu[i].value = val;
    }
  }

  isTriggerMode = (menu[0].value == 1);
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
  update_lcd();
}

// =================================================================================
// 7. MAIN LOOP (The brain that runs forever...Hard for me I’m kind of a MATLAB dummy)
// =================================================================================
void loop() {
  handle_inputs(); // Check knob and buttons
  
  // --- INSTANT START LOGIC ---
  // When you switch ON, its arm this logic to start the timer immediately avoiding the lag caused by drawing the screen.
  if (armed_for_start) {
     if (menu[2].value == 2) { // If NPS Mode
         calculate_nps_vector();     
         nps_window_start = millis(); 
     } else { // If Standard Mode
         last_micros = micros(); 
         if (menu[2].value == 1) { // If Burst
             pulsesRemaining = menu[5].value; 
             // Logic for Gap (Menu 6)
             lastBurstEndTime = millis() - menu[6].value; 
         }
     }
     armed_for_start = false; 
  }

  check_single_pulse_btn(); 

  // --- THE PULSE ENGINE ---
  if (generating) {
    // Check Safety Timer
    if (menu[10].value > 0 && (millis() - startTime >= (uint32_t)menu[10].value)) {
      stop_generation();
    } else {
      
      // Decision:
      
      if (menu[2].value == 2) { 
         // Run the Random/NPS Engine
         run_nps_logic();
      }
      else { 
         // Run the Standard Engine (Cont/Burst)
         
         if (menu[0].value == 0) { // Manual Mode
           // Logic to reload bursts
           if (menu[2].value == 1 && pulsesRemaining <= 0) {
             if (millis() - lastBurstEndTime >= (uint32_t)menu[6].value) {
               pulsesRemaining = menu[5].value; 
               last_micros = micros();
             }
           }
           run_pulse_generator(); 
         } 
         else { // Trigger Mode
           if (triggerEvent) {
             triggerEvent = false;
             last_micros = micros(); 
             if (menu[2].value == 1) pulsesRemaining = menu[5].value; 
             else pulsesRemaining = 1; 
             PORTD |= (1 << PD7); // Fire!
           }
           run_pulse_generator();
         }
      }
    }
  } else {
    // Safety: Ensure output is Low when stopped
    PORTD &= ~(1 << PD7); 
    triggerEvent = false;
  }
}

// =================================================================================
// 8. NPS LOGIC
// =================================================================================
// This function pre-calculates a list of random times for the next second.
void calculate_nps_vector() {
  int num_pulses = menu[5].value;
  // Convert Min Interval from decimal format (2000 -> 20ms)
  int min_iti = menu[7].value / 100; 
  
  if (num_pulses > MAX_NPS_PULSES) num_pulses = MAX_NPS_PULSES;
  
  long ini = 0;
  // Calculate remaining space in the 1-second window
  long fim = 1000 - ((num_pulses - 1) * min_iti);
  
  if (fim < 0) fim = 0; 
  
  // Fill the list with random times obeying the minimum interval
  for (int i = 0; i < num_pulses; i++) {
     long auxNorm = random(ini, fim);
     
     // Enforce constraint
     if (auxNorm - ini < min_iti) auxNorm = ini + min_iti;
     
     nps_timestamps[i] = (uint16_t)auxNorm;
     ini = auxNorm;
     fim = fim + min_iti;
     if (fim > 1000) fim = 1000;
  }
  nps_pulse_idx = 0; 
}

// This function checks the clock and fires the pulses calculated above
void run_nps_logic() {
  unsigned long current_millis = millis();
  
  // Window Management (Loop every 1 sec or wait for trigger)
  if (menu[0].value == 0) { 
      if (current_millis - nps_window_start >= 1000) {
          calculate_nps_vector(); 
          nps_window_start = current_millis;
      }
  } 
  else { 
      if (triggerEvent) {
          triggerEvent = false;
          calculate_nps_vector();
          nps_window_start = current_millis;
      }
      if (current_millis - nps_window_start >= 1000) return; 
  }

  // Fire the pulse if the time is right
  if (nps_pulse_idx < menu[5].value) {
     unsigned long offset = current_millis - nps_window_start;
     if (offset >= nps_timestamps[nps_pulse_idx]) {
         PORTD |= (1 << PD7);
         delayMicroseconds(pulse_on_us); 
         PORTD &= ~(1 << PD7);
         nps_pulse_idx++; 
     }
  }
}

// =================================================================================
// 9. HELPER FUNCTIONS
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

// Reads the manual button with noise filtering (Debounce)
void check_single_pulse_btn() {
  if (generating) return; // Don't fire if already running !!!!!!!!...Jesus
  
  bool reading = digitalRead(SINGLE_PULSE_PIN);
  
  // If button pressed (LOW) and wasn't pressed before...
  if (reading == LOW && lastSingleBtnState == HIGH && !btnLocked) {
      PORTD |= (1 << PD7);   
      delay(10); // Small fixed pulse for manual test
      PORTD &= ~(1 << PD7);  
      btnLocked = true; // Lock to prevent double fire
      lastPulseTime = millis(); 
  }
  // Unlock only after button is released and time has passed
  if (reading == HIGH && (millis() - lastPulseTime > debounceDelay)) {
      btnLocked = false;
  }
  lastSingleBtnState = reading;
}

// The Standard Pulse Engine (for Frequencies and Bursts)
void run_pulse_generator() {
  uint32_t current_micros = micros();
  
  // Phase 1: Is it time to start a new cycle?
  if (current_micros - last_micros >= period_us) {
    last_micros += period_us; 
    
    // Burst Logic: Count down pulses
    if ((menu[2].value == 1 || menu[0].value == 1) && pulsesRemaining > 0) {
      pulsesRemaining--;
      if (pulsesRemaining == 0) {
        lastBurstEndTime = millis(); 
        if (menu[0].value == 1) return; // Stop if Trigger mode
        
        // Stop if Manual mode and Gap is 0
        if (menu[0].value == 0 && menu[6].value == 0) {
           stop_generation();
           return;
        }
      }
    }
  }
  
  // Phase 2: Handle Pulse Width (High/Low state)
  if (current_micros - last_micros < pulse_on_us) {
    if ((menu[2].value == 0 && menu[0].value == 0) || pulsesRemaining > 0 || (pulsesRemaining == 0 && (millis() - lastBurstEndTime < 1))) {
      PORTD |= (1 << PD7); // Pin HIGH
    } else {
      PORTD &= ~(1 << PD7); // Pin LOW
    }
  } else {
    PORTD &= ~(1 << PD7); // Pin LOW
  }
}

void stop_generation() {
  generating = false;
  menu[3].suffix = "OFF";
  PORTD &= ~(1 << PD7);    
  val_change = true;
  triggerEvent = false;
  pulsesRemaining = 0;
  armed_for_start = false;
}

// Converts menu numbers into microsecond timing values
void update_calculations() {
  // Frequency to Period conversion
  if (menu[4].value > 0) period_us = 100000000UL / menu[4].value; 
  
  isTriggerMode = (menu[0].value == 1); 
  update_interrupt_config(); 

  // Adjust Pulse Count limits based on Mode (NPS vs Burst)
  if (menu[2].value == 2) menu[5].lim_max = MAX_NPS_PULSES;
  else menu[5].lim_max = 999;
  
  // Correct value if it exceeds new limit
  if (menu[5].value > menu[5].lim_max) menu[5].value = menu[5].lim_max;

  // Calculate Pulse Width (Handling Fixed vs Duty Cycle)
  static int last_tmode = -1;
  if (last_tmode != -1 && last_tmode != (int)menu[8].value) {
    if (menu[8].value == 1) menu[9].value = (menu[9].value * 100000UL) / period_us;
    else menu[9].value = ((uint32_t)menu[9].value * period_us) / 100000UL;
  }
  last_tmode = (int)menu[8].value;

  // Set limits for Pulse Width
  if (menu[8].value == 0) { 
    long lim = (period_us / 10) - 10;
    menu[9].lim_max = (lim < 1) ? 1 : lim;
    menu[9].suffix = "ms";
  } else { 
    menu[9].lim_max = 9990;
    menu[9].suffix = "% ";
  }
  if (menu[9].value > menu[9].lim_max) menu[9].value = menu[9].lim_max; 
  
  // Final Pulse Width calculation
  pulse_on_us = (menu[8].value == 0) ? menu[9].value * 10 : (period_us * (uint32_t)menu[9].value) / 10000UL;
}

// =================================================================================
// 10. UI HANDLERS (Screen and Buttons)
// =================================================================================
void handle_inputs() {
  ClickEncoder::Button b = encoder.getButton();
  
  // --- IF BUTTON CLICKED ---
  if (b == ClickEncoder::Clicked) {
    if (menu[menu_idx].type == VALUE) {
      // Enter Edit Mode
      if (!in_menu) { in_menu = true; edit_digit = 0; } 
      else {
        // Move to next digit
        edit_digit++;
        
        // Define how many digits each menu item has
        int max_d = 4; // Default
        if (menu_idx == 5) max_d = 2; // Count (3 digits)
        if (menu_idx == 6 || menu_idx == 10) max_d = 5; // Gap/Timer (6 digits)
        
        // If we pass the last digit, exit edit mode
        if (edit_digit > max_d) { in_menu = false; update_calculations(); }
      }
    } else if (menu[menu_idx].type == OPTION) {
      // Toggle Options (e.g., Manual -> Trigger)
      in_menu = !in_menu; 
      if (!in_menu) {
        update_calculations();
        // Safety: Reset index if mode changes
        if (menu[0].value == 0 && menu_idx == 1) menu_idx = 0;
      }
    } else if (!in_menu) execute_action();
    val_change = true;
  }

  // --- IF KNOB ROTATED ---
  int16_t diff = encoder.getValue();
  if (diff != 0) {
    if (!in_menu) {
      // NAVIGATION MODE (Scrolling through menu items)
      menu_idx = (menu_idx + diff) % NUM_ITEMS;
      if (menu_idx < 0) menu_idx = NUM_ITEMS - 1;
      
      //  HIDING: Skip items not relevant to current mode
      
      // 1. Skip Edge(1) if in Manual Mode
      if (menu[0].value == 0 && menu_idx == 1) menu_idx = (diff > 0) ? 2 : 0; 
      
      // 2. If Continuous: Skip Gap(6), Min(7), Count(5)
      if (menu[2].value == 0 && (menu_idx >= 5 && menu_idx <= 7)) menu_idx = (diff > 0) ? 8 : 4;
      
      // 3. If Burst: Skip Min(7)
      if (menu[2].value == 1 && menu_idx == 7) menu_idx = (diff > 0) ? 8 : 6;
      
      // 4. If NPS: Skip Freq(4) and Gap(6)
      if (menu[2].value == 2 && menu_idx == 4) menu_idx = (diff > 0) ? 5 : 3;
      if (menu[2].value == 2 && menu_idx == 6) menu_idx = (diff > 0) ? 7 : 5;

    } else {
      // EDIT MODE (Changing a value)
      if (menu[menu_idx].type == VALUE) {
        uint32_t mult;
        
        // Determine the multiplier based on cursor position
        
        // Count(5): Integer Logic
        if (menu_idx == 5) mult = digit_multipliers[edit_digit + 3];
        
        // Gap(6), Timer(10): Long Integer Logic
        else if (menu_idx == 6 || menu_idx == 10) mult = digit_multipliers[edit_digit];
        
        // Freq(4), Min(7), Pulse(9): Decimal Logic (Skipping the dot)
        else mult = digit_multipliers[edit_digit + 1];

        // Apply the change to the specific digit
        int digit = (menu[menu_idx].value / mult) % 10;
        int new_digit = (digit + diff) % 10;
        if (new_digit < 0) new_digit = 9;
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
  for (int i = 0; i < 2; i++) {
    int idx = (menu_idx + i) % NUM_ITEMS;
    
    // VISUAL HIDING (Ensure hidden items are not drawn)
    if (menu[0].value == 0 && idx == 1) idx = (idx + 1) % NUM_ITEMS; 
    if (menu[2].value == 0 && (idx >= 5 && idx <= 7)) idx = 8; 
    if (menu[2].value == 1 && idx == 7) idx = 8; 
    if (menu[2].value == 2 && idx == 4) idx = 5; 
    if (menu[2].value == 2 && idx == 6) idx = 7; 

    // Draw Menu Name
    lcd.setCursor(0, i);
    lcd.print(i == 0 ? ">" : " ");
    lcd.print(menu[idx].name_);
    int nameLen = strlen(menu[idx].name_);
    for(int s=0; s < (6 - nameLen); s++) lcd.print(" ");
    lcd.print(":"); 
    
    lcd.setCursor(8, i); 
    lcd.print("        "); // Clear value area
    lcd.setCursor(8, i); 
    
    // Draw Value
    if (menu[idx].type == VALUE) {
      if (idx == 5) sprintf(buffer, "%03ld", menu[idx].value); // Count (000)
      
      else if (idx == 6 || idx == 10) sprintf(buffer, "%06ld", menu[idx].value); // Gap/Timer (000000)
      
      else { // Decimals: Min(7), Pulse(9), Freq(4)
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
  
  if (menu_idx == 5) lcd.setCursor(col_num + edit_digit, 0); // Count
  
  else if (menu_idx == 6 || menu_idx == 10) lcd.setCursor(col_num + edit_digit, 0); // Gap/Timer
  
  // For decimals, we skip the dot position (Index 3)
  else {
    int offsets[] = {0, 1, 2, 4, 5}; 
    lcd.setCursor(col_num + offsets[edit_digit], 0);
  }
  lcd.blink();
}

// =================================================================================
// 11. EXECUTE ACTION (Switching ON/OFF, Saving, Comm)
// =================================================================================
void execute_action() {
  
  // --- [3] STATE: ON/OFF ---
  if (menu_idx == 3) { 
    generating = !generating; 
    triggerEvent = false;
    pulsesRemaining = 0;
    armed_for_start = false;
    PORTD &= ~(1 << PD7); // Force Pin Low
    
    if (generating) {
       startTime = millis(); 
       if (menu[0].value == 0) { 
          armed_for_start = true; // Flag to start the timer in the main loop
       }
    }
    menu[3].suffix = generating ? "ON " : "OFF"; 
  } 
  
  // --- [11] SAVE: Store parameters to EEPROM ---
  else if (menu_idx == 11) { 
    for (int i = 0; i < NUM_ITEMS; i++) eepromUpdateLong(i * 4, menu[i].value); 
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
       Serial.println(" Hz");
    }

    // Pulse Count (Hide in Continuous mode)
    if (type != 0) {
       Serial.print(F("Count: ")); 
       Serial.println(menu[5].value);
    }

    // Inter-Burst Interval (Show only in Burst mode)
    if (type == 1) {
       Serial.print(F("Gap:   ")); 
       Serial.print(menu[6].value); 
       Serial.println(" ms");
    }

    // NPS Minimum Interval (Show only in NPS mode)
    if (type == 2) {
       Serial.print(F("Min:   ")); 
       Serial.print((float)menu[7].value / 100.0); 
       Serial.println(" ms");
    }

    // Common parameters for all modes
    Serial.print(F("Width: ")); 
    Serial.println(menu[8].value ? "Duty Cycle" : "Fixed Time");

    Serial.print(F("Pulse: ")); 
    Serial.print((float)menu[9].value / 100.0); 
    Serial.println(menu[8].value ? " %" : " ms"); 

    Serial.print(F("Timer: ")); 
    Serial.print(menu[10].value); 
    Serial.println(" ms");
    
    Serial.println(F("============================="));


    // ---------------------------------------------------------
    // PART 2: IF NPS MODE, GENERATE RAW SIMULATION DATA
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
        
        long last_pulse_abs_time = 0;
        long window_offset = 0;
        
        // Run simulation loop - "Monte Carlo" part
        for (int sec = 0; sec < duration_sec; sec++) {
            calculate_nps_vector(); // Generate random timings for this second
            
            for (int i = 0; i < menu[5].value; i++) {
                // Calculate absolute time from the start of simulation
                long current_abs_time = window_offset + nps_timestamps[i];
                
                // ITI = Current pulse time minus Previous pulse time
                long iti = current_abs_time - last_pulse_abs_time;
                
                // Skip the first "false" interval at t=0
                if (window_offset == 0 && i == 0) {
                   // Skip
                } else {
                   Serial.println(iti); // Print value for Histogram
                }
                
                last_pulse_abs_time = current_abs_time;
            }
            window_offset += 1000; // Move window forward by 1 second
        }
        Serial.println(F(">>> End of Data <<<"));
    }

    //  LCD feedback
    lcd.setCursor(8, 0); lcd.print("SENT "); delay(800);
  }
} // <---- FUCK !!! FUCK !!! FUCK !!! FUCK !!!

// =================================================================================
// 12. EEPROM UTILS
// =================================================================================
long eepromReadLong(int adr) { 
  long wert = 0; 
  for(int i=0; i<4; i++) wert |= ((long)EEPROM.read(adr + i) & 0xFF) << (i*8); 
  return wert; 
}

void eepromUpdateLong(int adr, long wert) { 
  for(int i=0; i<4; i++) EEPROM.update(adr + i, (wert >> (i*8)) & 0xFF); 
}

// =================================================================================
// Listening: Sonic Youth - Starpower
// Thu Feb 12 - 16:53 

