# Aquatic Biomonitor Firmware (v3.0.0)

An embedded Real-Time Operating System (RTOS) firmware designed for the Nordic nRF52840 and Analog Devices AD8232 to provide wireless telemetric monitoring of aquatic vital signs. 

This system captures submerged electrocardiogram (ECG) data, processes it via an ARM Cortex-M4F DSP pipeline, and broadcasts instantaneous biological metrics over a custom Bluetooth Low Energy (BLE) GATT service. It is engineered for ultra-low power consumption and dynamic signal adaptation in varying aquatic conductivities.

## 1. Hardware Architecture

The firmware targets a custom 4-layer PCB with a continuous ground plane for noise isolation, integrating mixed-signal components:

*   **Microcontroller:** Nordic nRF52840 (ARM Cortex-M4F) executing Zephyr RTOS.
*   **Analog Front-End (AFE):** AD8232 Heart Rate Monitor Front End.
    *   **Gain:** 100 V/V (fixed instrumentation amplifier paired with a unity-gain buffer).
    *   **Filtering Topology:** 1st-order active high-pass at 0.16 Hz; 2nd-order Sallen-Key low-pass at 40.8 Hz.
    *   **Noise Rejection:** Submerged Right Leg Drive (RLD) actively cancels 60 Hz mains interference, providing a loop gain of ~20 and injecting an additional ~26 dB of common-mode noise rejection on top of the base 80 dB CMRR.
    *   **Input-Referred Noise:** Guaranteed 20 µVp-p within the 0.5–40 Hz passband.
*   **ADC Input:** `P0.02` (AIN0) hardware-configured for 100 Hz sampling, 12-bit resolution, 4x hardware oversampling, and 0.6V internal reference.
*   **Power Profile:** Runs on a single 220 mAh CR2032 coin cell. Peak BLE transmission spikes are smoothed by 10–22 µF bulk decoupling capacitors. The system draws ~3–5 mA during active telemetry and ~2.5 µA in deep sleep (nRF52840 system OFF + AD8232 `SDN` pin pulled low).

## 2. Firmware Architecture (Zephyr RTOS)

The system utilizes a preemptive, multi-threaded RTOS architecture to guarantee hard real-time constraints for analog sampling while managing asynchronous Bluetooth events.

*   **ADC Thread (Priority 2 - Highest):** Woken at exactly 100 Hz by a hardware timer (`k_timer`) and semaphore (`k_sem`). Triggers the SAADC, reads the 12-bit integer, and pushes it to `raw_sample_queue` via `k_msgq_put(..., K_NO_WAIT)`.
*   **DSP Thread (Priority 3):** Blocks on the raw queue. Upon receiving a sample, it executes CMSIS-DSP filtering and peak detection, classifies the health state, and pushes a 26-byte struct to `telem_queue`.
*   **BLE Thread (Priority 5 - Lowest):** Blocks on the telemetry queue. Discards data if no central is subscribed, or transmits payloads over the air via `bt_gatt_notify`.
*   **State Protection:** A priority-inheriting Mutex (`k_mutex`) protects the active species profile, ensuring the BLE thread can hot-swap DSP filter coefficients without corrupting active floating-point calculations.

## 3. Digital Signal Processing & Analytics

Because aquatic environments present constantly shifting baseline conductivities and movement artifacts, static voltage thresholds cannot be used. The firmware implements dynamic thresholding via **Welford's Online Algorithm**, maintaining a rolling 2-second window (200 samples) of the signal's mean (µ) and standard deviation (σ).

### Digital Filtering (CMSIS-DSP)
*   **Notch Filter:** 60 Hz IIR biquad cascade to eliminate residual mains hum not canceled by the RLD.
*   **Species Bandpass:** 2nd-order Butterworth bandpass (e.g., 1.5–3.0 Hz for *Chromis viridis*). Coefficients are dynamically reloaded from flash NVS when the user changes the species profile.

### Metric Calculations
1.  **Heart Rate (BPM):** Triggered when the filtered voltage crosses `T_peak = µ + 2.5σ`. After enforcing a 250ms refractory period, the R-R interval (`RR_ms`) is logged.
    *   `BPM = 60000 / RR_ms`
2.  **Heart Rate Variability (HRV):** Calculated using the Root Mean Square of Successive Differences (RMSSD) over a rolling window of the last 8 beats (N=8). Highly sensitive to acute autonomic stress.
    *   `RMSSD = sqrt( sum( (RR_i - RR_{i-1})^2 ) / (N-1) )`
3.  **Ventilation Rate (VR):** Opercular movement modulates the cardiac baseline. VR is estimated directly from the cardiac rate using a species-specific physiological ratio.
    *   `VR = BPM / Ratio`
4.  **Cough Detection:** A violent clearing of the gills generates a massive electromyographic spike. The DSP thread isolates these from heartbeats using a severely elevated Welford threshold (`T_cough = µ + 4.5σ`). Valid timestamps are pushed into a 32-slot circular buffer, yielding an exact rolling "coughs per minute" metric over a 60-second window.

## 4. Bluetooth Low Energy (GATT)

The device utilizes a custom 128-bit UUID service (`...CDEF0`) to expose the following characteristics:

*   **Acquisition Control (`...CDEF3`):** [READ | WRITE] The central writes `0x01` to start the 100 Hz sampling timer, or `0x00` to stop it and drop the amplifier into sleep mode.
*   **Species Write (`...CDEF1`):** [WRITE] Accepts a 1-byte ID to hot-swap the active DSP profile. The selection is saved to the non-volatile storage (NVS) filesystem to persist across hardware resets.
*   **Telemetry Notify (`...CDEF2`):** [NOTIFY] Broadcasts 26-byte packed structs containing:
    *   `[0:3]` BPM (Float32)
    *   `[4:7]` HRV RMSSD (Float32)
    *   `[8:11]` Ventilation Rate (Float32)
    *   `[12:15]` Cough Rate (Float32)
    *   `[16:19]` Filtered ECG Amplitude (Float32)
    *   `[20]` Health State Enum (UInt8: Normal, Tachycardia, Bradycardia, Distress, Low HRV, No Signal)
    *   `[21]` Species ID (UInt8)
    *   `[22:25]` Reserved Padding

## 5. Build and Deployment

Compiled using the nRF Connect SDK (v3.4.1) and the Zephyr `west` toolchain.

**1