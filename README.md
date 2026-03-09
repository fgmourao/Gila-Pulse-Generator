# 🦎 Gila Monster 
### Pulse generator v1.0 

To meet the need for a pulse generator capable of triggering external instruments in neurostimulation protocols with multiple frequency patterns, the Gila Monster v1.0 was developed. Although numerous DIY solutions are available in online repositories, this project intentionally adopts a minimalist hardware design, centered on an Arduino Uno built around the ATmega328P microcontroller.

The design prioritizes the timing engine at the hardware and firmware levels. To ensure that time-critical pulse generation is isolated from user interface operations, the firmware implements a dynamic UI-bypass architecture. While the LCD display inherently relies on synchronous I2C communication, the system suspends all visual updates during active stimulation (Static Operation Mode and no encoder interaction). This hardware-software synergy guarantees that UI-related polling does not introduce latency, I2C blocking overhead, or timing jitter into the output signal.

---

## 1. Firmware Specifications

- **Stimulation Modes:** Multi-engine pulse generation supporting Continuous (periodic), Burst (trains with inter-burst gaps), and Non-Periodic Stimulation (NPS) with randomized intra-window intervals.

- **Time Base & Granularity:** Microsecond-level timing relying on the 16 MHz system clock. Due to AVR hardware prescaling, the fundamental temporal resolution (software polling step) advances in 4 µs increments.

- **Frequency Range & Resolution:** Programmable output frequency from 0.1 Hz to 500 Hz, with a 0.01 Hz resolution capability for precise slow-wave protocols.

- **Pulse Width (PW) Range:** Configurable pulse durations. The reliable lower bound is mode-dependent: 50 µs in NPS mode, where timing is governed by a blocking call, and 100 µs in Continuous and Burst modes, where pulse width is enforced via polling. The upper bound is dynamically limited by the selected frequency period.

- **Output Control & Switching Latency:** Direct port manipulation using PORTD, bit 7 (PD7) to minimize software overhead, achieving a theoretical minimum switching latency of ≈125 ns (limited by the ATmega328P single instruction cycle).

- **Output Logic Level:** 5 V TTL-compatible digital output.

- **External Triggering & Synchronization:** Integrated hardware interrupt support via Pin D3 (INT1), allowing synchronization with external lab equipment. The system supports user-selectable Rising or Falling edge detection with a sub-microsecond hardware response latency. The interrupt service routine includes a hardware interlock that rejects incoming trigger events while the output pin is actively HIGH to prevent pulse overlap.

- **Single-Shot Diagnostic:** Dedicated hardware-level manual pulse capability via Pin D6. Governed by a software-based debounce routine and a logical safety interlock that physically prevents manual firing while the primary stimulation engine is active (State: ON).

- **Duty Cycle Safety Clamp:** Automated logic constraint that prevents 100% duty cycle (DC latching). If the requested PW exceeds the period, the firmware enforces a mandatory low-phase cutoff (PW_max = T − 10 µs).

- **Parameter Storage:** Non-volatile EEPROM integration used to store 13 user-configurable operational parameters, preserving experimental setups across power cycles.

- **Serial Communication:** UART-based serial interface (9600 baud) for logging and reporting of operational state and elapsed timestamps, enabling external data acquisition, real-time monitoring, and protocol reproducibility.

---

## Note on Usage and Constraints

** Detailed usage instructions, as well as comprehensive operational constraints and hardware limitations are thoroughly documented in the official user manual.

---
## Future Development
v2.0 — Hardware Timer Architecture  

The current firmware relies on a polling-based timing engine inside loop(), which makes pulse generation susceptible to I2C blocking overhead from the LCD interface (see "Stop-Click Hazard", Manual Section 6). A natural evolution would be migrating the pulse engine to a hardware timer ISR, completely decoupling stimulus generation from the UI layer.  
The proposed architecture uses Timer2 in CTC mode to control the output pin via ISR, which preempts all other operations including I2C transactions. This would eliminate the "Stop-Click Hazard" by design and improve timing resolution from 4 µs to ~0.5 µs.  
The most viable implementation is a hybrid approach:
- Timer2 ISR → guaranteed rising edge, independent of loop()
- Timer1 ISR → falling edge check after pulse_on_us
- loop()     → UI only (LCD, encoder, menus)  
 
The primary challenge is the NPS mode, which currently relies on random() and state management inside loop(), neither of which is safely portable to an ISR context without a full rewrite of the stochastic scheduling engine. This architectural migration is therefore scoped as a v2.0 effort.

## Author

Flavio Mourao (mourao.fg@gmail.com)  
Federal University of Minas Gerais, Brazil  

Development started: February 2026  
Last update: March 2026  
