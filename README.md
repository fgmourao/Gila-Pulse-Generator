# 🦎 Gila Monster 
### Pulse generator v1.0 

To meet the need for a pulse generator capable of triggering external instruments in neurostimulation protocols with multiple frequency patterns, the Gila Monster v1.0 was developed. Although numerous DIY solutions are available in online repositories, this project intentionally adopts a minimalist hardware design, centered on an Arduino Uno built around the ATmega328P microcontroller.

The design prioritizes the timing engine at the hardware and firmware levels. To ensure that time-critical pulse generation is isolated from user interface operations, the firmware implements a dynamic UI-bypass architecture. While the LCD display inherently relies on synchronous I2C communication, the system intelligently suspends all visual updates during active stimulation (Static Operation Mode). This hardware-software synergy guarantees that UI-related polling does not introduce latency, I2C blocking overhead, or timing jitter into the output signal.

---

## 1. Firmware Specifications

**Stimulation Modes:** Multi-engine pulse generation supporting Continuous (periodic), Burst (trains with inter-burst gaps), and Non-Periodic Stimulation (NPS) with randomized intra-window intervals.

**Time Base & Granularity:** Microsecond-level timing relying on the 16 MHz system clock. Due to AVR hardware prescaling, the fundamental temporal resolution (software polling step) advances in 4 µs increments.

**Frequency Range & Resolution:** Programmable output frequency from 0.1 Hz to 500 Hz, with a 0.01 Hz resolution capability for precise slow-wave protocols.

**Pulse Width (PW) Range:** Configurable pulse durations. To ensure uncorrupted signal integrity and avoid collision with the 250 µs background ISR preemption, the hardcoded reliable lower bound is 50 µs. The upper bound is dynamically limited by the selected frequency period (up to 999.99 ms).

**Output Control & Switching Latency:** Direct port manipulation using PORTD, bit 7 (PD7) to minimize software overhead, achieving a theoretical minimum switching latency of ≈125 ns (limited by the ATmega328P single instruction cycle).

**Output Logic Level:** 5 V TTL-compatible digital output.

**External Triggering & Synchronization:** Integrated hardware interrupt support via Pin D3 (INT1), allowing synchronization with external lab equipment. The system supports user-selectable Rising or Falling edge detection with a sub-microsecond hardware response latency.

**Single-Shot Diagnostic:** Dedicated hardware-level manual pulse capability via Pin D6. Governed by a software-based debounce routine and a logical safety interlock that physically prevents manual firing while the primary stimulation engine is active (State: ON).

**Duty Cycle Safety Clamp:** Automated logic constraint that prevents 100% duty cycle (DC latching). If the requested PW exceeds the period, the firmware enforces a mandatory low-phase cutoff (PW_max = T − 10 µs).

**Parameter Storage:** Non-volatile EEPROM integration used to store 13 user-configurable operational parameters, preserving experimental setups across power cycles.

**Serial Communication:** UART-based serial interface (115200 baud) for logging and reporting of operational state and elapsed timestamps, enabling external data acquisition, real-time monitoring, and protocol reproducibility.

---

## Note on Usage and Constraints:

** Detailed usage instructions, as well as comprehensive operational constraints and hardware limitations are thoroughly documented in the official user manual.
