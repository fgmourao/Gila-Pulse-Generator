# 🦎 Gila Monster 
### Pulse generator v1.0 

To meet the need for a pulse generator capable of triggering external instruments in neurostimulation protocols with multiple frequency patterns, the Gila Monster v1.0 was developed. Although numerous DIY solutions are available in online repositories, this project intentionally adopts a minimalist hardware design, centered on an Arduino Uno built around the ATmega328P microcontroller.

The design prioritizes the timing engine at both the hardware and firmware levels to ensure reliable, time-critical pulse generation.

---

## 1. Firmware Specifications

- **Stimulation Modes:** Multi-engine pulse generation supporting Continuous (periodic), Burst (trains with inter-burst gaps), and Non-Periodic Stimulation (NPS) with randomized intra-window intervals (for NPS, see https://doi.org/10.1016/j.yebeh.2019.106609).

- **Time Base:** Microsecond-level timing relying on the 16 MHz system clock. Due to AVR hardware prescaling, the fundamental temporal resolution (software polling step) advances in 4 µs increments. Pulses are scheduled on a time grid, so timing errors do not accumulate from one pulse to the next.

- **Frequency Range & Resolution:** Programmable output frequency from 0.1 Hz to 500 Hz, with a 0.01 Hz resolution.

- **Pulse Width (PW) Range:** Fixed time from 0.10 ms (Continuous and Burst) or 0.05 ms (NPS) up to 999.99 ms, in 0.01 ms steps, or Duty Cycle in 0.01 % steps. Measured edge accuracy is ±4 µs in all modes (Section 6.3).

- **Output Control & Switching Latency:** Direct port manipulation using PORTD, bit 7 (PD7) to minimize software overhead, achieving a theoretical minimum switching latency of ≈125 ns (limited by the ATmega328P single instruction cycle).

- **Output Logic Level:** 5 V TTL-compatible digital output.

- **External Triggering & Synchronization:** Hardware interrupt on Pin D3 (INT1) with user-selectable Rising or Falling edge detection. In Trigger mode, the external trigger then starts the whole programmed session, exactly as switching State ON does in Manual mode, and the session Timer is counted from the trigger. Triggers arriving while a session is running are ignored. The interrupt registers the event and the session starts in the main loop, with a measured trigger-to-output latency ~40 µs.

- **Single-Shot Diagnostic:** Manual pulse via Pin D6, debounced in firmware (100 ms of stable contact), delivering one pulse with the programmed width per press. Interlocked with the master state: operational only when State is OFF.

- **Duty Cycle Safety Clamp:** The output always returns LOW for at least 100 µs between pulses, which prevents 100 % duty cycle (DC latching). The maximum pulse width is PWmax = T − 100 µs, applied to the displayed value.

- **Parameter Storage:** Non-volatile EEPROM storage of the 13 operational parameters, protected by a signature. Absent or invalid data loads the default settings.

- **Serial Communication:** UART interface (9600 baud) reporting the active configuration, the effective pulse timing in microseconds, a report of the last session and NPS simulation data for offline analysis.

---

## Note on Usage and Constraints

Detailed usage instructions, as well as comprehensive operational constraints, hardware limitations and measured timing accuracy, are thoroughly documented in the official user manual.

---
## Future Development
v2.0 — Hardware Timer Architecture  

The current firmware relies on a polling-based timing engine inside loop(). Interference from the user interface is avoided by locking the interface during stimulation, and edge placement is protected by masking interrupts around each edge, but the resolution remains bound to the 4 µs step of the software time base (see Manual). A natural evolution would be migrating the pulse engine to a hardware timer ISR, completely decoupling stimulus generation from the UI layer.  
The proposed architecture uses Timer2 in CTC mode to control the output pin via ISR, which preempts all other operations. This would improve timing resolution from 4 µs to well below 1 µs and would allow the interface to remain live during stimulation.  
The most viable implementation is a hybrid approach:
- Timer2 ISR → guaranteed rising edge, independent of loop()
- Timer1 ISR → falling edge check after pulse_on_us
- loop()     → UI only (LCD, encoder, menus)  
 
The primary challenge is the NPS mode, which currently relies on random() and state management inside loop(), neither of which is safely portable to an ISR context without a full rewrite of the stochastic scheduling engine. This architectural migration is therefore scoped as a v2.0 effort.

## Author

Flavio Mourao (mourao.fg@gmail.com)  
Federal University of Minas Gerais, Brazil  

Development started: February 2024  
Last update: Sep 2026  
