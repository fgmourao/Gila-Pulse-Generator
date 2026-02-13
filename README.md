# 🦎 Gila Monster 
### Pulse generator v1.0 

Given the requirement for a pulse generator capable of triggering external instruments in neurostimulation protocols with multiple frequency patterns, the **Gila Monster v1.0** was developed. While numerous DIY solutions exist across online repositories, this project deliberately adopts a minimalist hardware approach, centered on an **Arduino Uno** based on the **ATmega328P** microcontroller. 

The design prioritizes the timing engine at the hardware and firmware levels, ensuring that time-critical pulse generation is isolated from user interface operations. Encoder polling and LCD updates are handled via interrupts and non-blocking routines so that UI-related tasks do not introduce latency or timing jitter into the output signal.

---

## 1. Firmware Specifications

* **Time Base:** Microsecond (µs) resolution derived from the 16 MHz system clock.
* **Output Control:** Direct port manipulation using `PORTD, bit 7 (PD7)` to minimize software overhead.
* **Switching Latency:** ≈ 125 ns (theoretical minimum, limited by the ATmega328P instruction cycle at 16 MHz).
* **External Triggering & Synchronization:** Integrated hardware interrupt support via **Pin D3 (INT1)**, allowing synchronization with external lab equipment. The system supports user-selectable **Rising** or **Falling edge** detection with a sub-microsecond response latency.
* **Frequency Range & Resolution:** Programmable output frequency from **0.1 Hz to 500 Hz**, with **0.01 Hz resolution**.
* **Output Logic Level:** 5 V TTL-compatible digital output.
* **Single-Shot Test:** Dedicated hardware-level manual pulse capability via **Pin D6**. This function is governed by a software-based debounce routine and a logical interlock that prevents manual firing while the primary stimulation engine is active (`State: ON`).
* **Parameter Storage:** Non-volatile EEPROM used to store **13 user-configurable operational parameters**, preserved across power cycles.
* **Serial Communication:** UART-based serial interface for logging and reporting of operational and experimental parameters, enabling external data acquisition, monitoring, and reproducibility of stimulation protocols.
