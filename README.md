# 🦎 Gila Monster 
### Pulse generator v1.0 

To meet the need for a pulse generator capable of triggering external instruments in neurostimulation protocols with multiple frequency patterns, the Gila Monster v1.0 was developed. Although numerous DIY solutions are available in online repositories, this project intentionally adopts a minimalist hardware design, centered on an Arduino Uno built around the ATmega328P microcontroller.

The design prioritizes the timing engine at the firmware level, keeping time-critical pulse generation.

---

## 1. Firmware Specifications

- **Stimulation Modes:** Multi-engine pulse generation supporting Continuous (periodic), Burst (trains with inter-burst gaps), and Non-Periodic Stimulation (NPS) with randomized intra-window intervals (for NPS, see https://doi.org/10.1016/j.yebeh.2019.106609).

- **Time Base:** Microsecond-level timing relying on the 16 MHz system clock. Due to AVR hardware prescaling, the fundamental temporal resolution (software polling step) advances in 4 µs increments. Pulses are scheduled on an absolute time grid, so timing errors do not accumulate from one pulse to the next.

- **Frequency Range & Resolution:** Programmable output frequency from 0.1 Hz to 500 Hz, with a 0.01 Hz resolution capability for precise slow-wave protocols.

- **Pulse Width (PW) Range:** Configurable pulse durations, as a "fixed" time or as a duty cycle. The lower bound is mode-dependent: 50 µs in NPS mode and 100 µs in Continuous and Burst modes. The upper bound is dynamically limited by the selected frequency period, or by the minimum inter-stimulus interval in NPS mode. Measured edge accuracy is ±4 µs in all modes, the resolution of the time base.

- **Output Control & Switching Latency:** Direct port manipulation using PORTD, bit 7 (PD7) to minimize software overhead, achieving a theoretical minimum switching latency of ≈125 ns (limited by the ATmega328P single instruction cycle).

- **Output Logic Level:** 5 V TTL-compatible digital output.

- **External Triggering & Synchronization:** Integrated hardware interrupt support via Pin D3 (INT1), allowing synchronization with external lab equipment. The system supports user-selectable Rising or Falling edge detection with a sub-microsecond hardware response latency; the protocol itself starts in the main loop, with a measured trigger-to-output latency of 24 to 81 µs. Triggers arriving while a pulse, a burst or an NPS window is still running are ignored, so a protocol that has started always completes as programmed.

- **Single-Shot Diagnostic:** Dedicated manual pulse capability via Pin D6, delivering one pulse with the programmed width per press. Governed by a software-based debounce routine and a logical safety interlock that prevents manual firing while the primary stimulation engine is active (State: ON).

- **Duty Cycle Safety Clamp:** Automated logic constraint that prevents 100% duty cycle (DC latching). The output always returns LOW for at least 100 µs between pulses, so the maximum pulse width is PW_max = T − 100 µs, applied to the displayed value.

- **Parameter Storage:** Non-volatile EEPROM integration used to store 13 user-configurable operational parameters, protected by a signature, preserving experimental setups across power cycles.

- **Serial Communication:** UART-based serial interface (9600 baud) reporting the active configuration, the effective pulse timing in microseconds, a report of the last session (pulses delivered and timing faults) and simulated NPS interval data for offline analysis, enabling protocol reproducibility.

---

## Note on Usage and Constraints

Detailed usage instructions, as well as comprehensive operational constraints, hardware limitations and measured timing accuracy, are thoroughly documented in the official user manual.

---
## Future Development
v2.0 — Hardware Timer Architecture  

The current firmware relies on a polling-based timing engine inside loop(). Interference from the user interface is avoided by locking the interface during stimulation, and edge placement is protected by masking interrupts around each edge, but the resolution remains bound to the 4 µs step of the software time base (see Manual, Section 6.2). A natural evolution would be migrating the pulse engine to a hardware timer ISR, completely decoupling stimulus generation from the UI layer.  
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
