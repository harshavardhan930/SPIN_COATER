# ESP32 Spin Coater with FreeRTOS, LVGL UI, and BLDC RPM Control

A DIY spin coater controller built around an ESP32-WROOM-32D, an A2212 BLDC motor, a SimonK 30 A ESC, an A3144 Hall-effect sensor, and an ILI9488 touchscreen display.

**Important project highlight: this firmware uses FreeRTOS tasks.** The touchscreen interface and motor-control calculations run as separate tasks pinned to different ESP32 cores. This keeps LVGL user-interface handling separate from the recurring RPM measurement and PI-control work.

> **Project status:** This is a prototype. Calibrate and validate RPM measurement, motor-control stability, timing, and all electrical/mechanical safety features on the actual hardware. The code's displayed RPM can snap to the target when the measured value is within ±100 RPM; this is a display behavior, not proof that the real motor speed is exactly at the target.

## Contents

- [Project overview](#project-overview)
- [Features](#features)
- [Hardware](#hardware)
- [System architecture](#system-architecture)
- [Wiring](#wiring)
- [Software requirements](#software-requirements)
- [How the firmware works](#how-the-firmware-works)
- [FreeRTOS implementation](#freertos-implementation)
- [Motor-control sequence](#motor-control-sequence)
- [RPM calculation and PI control](#rpm-calculation-and-pi-control)
- [Saved settings and NVS](#saved-settings-and-nvs)
- [Build and test](#build-and-test)
- [Troubleshooting](#troubleshooting)
- [Limitations and future improvements](#limitations-and-future-improvements)
- [Safety](#safety)

## Project overview

A spin coater rotates a sample at a controlled speed so that a liquid can spread across its surface. This project combines a touchscreen UI, stored coating presets, Hall-sensor RPM feedback, and ESC control.

The ESP32 handles the UI and motor control locally. There is no separate motor MCU or motor-control UART in this version.

Typical workflow:

1. Power on the ESP32 and display. A splash screen is shown for approximately five seconds.
2. On the main screen, configure target RPM, ramp time, and coating time.
3. Optionally save those values into one of three settings slots.
4. Press Start. The ESP32 resets the run counters and begins controlling the ESC.
5. The A3144 Hall sensor produces pulses as the magnet passes it. The ESP32 measures the pulse count over a short time window to estimate RPM.
6. The PI controller compares measured RPM with target RPM and adjusts the ESC PWM command.
7. When RPM first reaches the target's ±100 RPM window, the coating timer starts.
8. When coating time finishes, the firmware ramps the ESC command down over the configured ramp time and then stops the run.
9. Pressing Stop requests an immediate software stop by sending the ESC its configured minimum pulse command.

The ESC drives the motor phases. The ESP32 does not drive the BLDC phases directly.

## Features

- Touchscreen UI built with LVGL and an EEZ Studio-generated UI.
- ILI9488 TFT display using `MCUFRIEND_kbv`.
- Resistive touchscreen input using `TouchScreen`.
- Separate FreeRTOS tasks for UI handling and motor control.
- Tasks pinned to separate ESP32 cores.
- Hall-sensor interrupt input on GPIO 21.
- ESC PWM output on GPIO 19 using the ESP32 Arduino Core 3.x LEDC API.
- Target RPM, ramp time, and coating-time settings.
- RPM feedback with a simple low-pass filter.
- Feedforward PWM estimate plus PI correction.
- Ramp-up and ramp-down behavior.
- Automatic transition from ramp-up to coating timer, then ramp-down and auto-stop.
- Three saved parameter presets using ESP32 Preferences/NVS.
- Target RPM validation in the UI.
- Live RPM, ramp timer, and coating timer display.
- UI button-color changes while a run is active.
- Serial diagnostic messages at 115200 baud.

## Hardware

| Component | Purpose |
|---|---|
| ESP32-WROOM-32D development board | Main controller and FreeRTOS host |
| ILI9488 TFT display | User interface display |
| Resistive touchscreen compatible with the selected shield | Touch input |
| A2212 BLDC motor | Rotating drive |
| SimonK 30 A ESC | BLDC motor drive |
| A3144 Hall-effect sensor | Motor rotation feedback |
| Small magnet | Hall-sensor trigger on the rotating assembly |
| 12 V, 2 A adapter | Prototype supply; may be insufficient for motor startup or load |
| 7805 regulator | 5 V supply for suitable peripherals, where appropriate |
| Dot PCB/perfboard, wires, connectors | Prototype assembly |
| 12 V female DC socket | Power input connector |
| Suitable chuck/sample holder and mechanical mounting | Holds the sample |

Check the voltage and current requirements of each module before connecting it. A 12 V, 2 A adapter may be insufficient for an A2212 motor under load. The ESC and motor can draw substantially more current during acceleration.

## System architecture

```text
                     +-----------------------------+
                     |           ESP32             |
                     |                             |
Touchscreen -------->| UI_Task (Core 1)             |
                     |  - LVGL event handling      |
                     |  - screen and label updates |
                     |  - settings and run buttons |
                     |              |              |
                     |              | shared state  |
                     |              v              |
Hall sensor -------->| Motor_Control (Core 0)       |
GPIO 21 interrupt   |  - RPM calculation           |
                     |  - RPM filtering             |
                     |  - feedforward + PI          |
                     |  - ramp and timer logic      |
                     |              |              |
                     +--------------|--------------+
                                    |
                              PWM GPIO 19
                                    |
                                    v
                              SimonK ESC
                                    |
                                    v
                               A2212 BLDC
                                    |
                          Magnet on rotating shaft
                                    |
                                    v
                              A3144 sensor
```

The Hall interrupt only increments a pulse counter. The motor task periodically snapshots that counter and performs RPM and controller calculations outside the interrupt. LVGL calls are intended to stay in the UI task.

## Wiring

The code defines the following motor-control pins:

| Signal | ESP32 connection | Notes |
|---|---|---|
| Hall sensor output | GPIO 21 | Configured as `INPUT_PULLUP`; rising-edge interrupt |
| ESC signal | GPIO 19 | LEDC PWM output |
| Hall sensor GND | ESP32 GND | Confirm the sensor module pinout |
| ESC signal GND | ESP32 GND | Common signal reference is required |
| ESC motor outputs | A2212's three motor wires | Connect according to ESC instructions |
| ESC power input | Suitable motor supply | Follow ESC voltage/current requirements |

The TFT and touch shield wiring depends on the exact ILI9488 shield and the pin definitions in the sketch. Check the `XP`, `XM`, `YP`, `YM` definitions and the display library's supported pin mapping before wiring a different shield.

### Hall sensor notes

The code configures GPIO 21 with `INPUT_PULLUP` and counts `RISING` edges. Confirm the pinout of the exact A3144 module you own. Bare sensors and breakout boards may use different pin layouts.

ESP32 GPIOs are not 5 V tolerant. If the Hall module's output is pulled up to 5 V, use a 3.3 V pull-up or suitable level shifting. Secure the magnet mechanically and ensure it cannot detach at speed.

### ESC signal notes

The code defines a 50 Hz PWM frequency and converts the requested pulse width to a duty value for a 20,000 microsecond period. In the uploaded sketch, `ESC_MIN_US` is 1200 and `ESC_MAX_US` is 1600. These values are firmware settings, not a guarantee that they are appropriate for every ESC. Verify the exact SimonK ESC's signal and arming requirements before connecting the motor. Do not assume that the ESC's signal ground or power wiring can be omitted.

## Software requirements

- Arduino IDE.
- ESP32 board package compatible with the ESP32-WROOM-32D.
- ESP32 Arduino Core 3.x, because the sketch uses `ledcAttach(pin, frequency, resolution)` and pin-based `ledcWrite(pin, duty)`.
- LVGL version compatible with the generated EEZ Studio UI code.
- `MCUFRIEND_kbv`, `Adafruit_GFX`, `TouchScreen`, and ESP32 `Preferences` libraries.
- The EEZ Studio-generated UI source files, including `ui.h` and the associated generated files.

The exact LVGL and generated UI versions should match the project files. If you publish this project, include the UI-generated source files or clearly document how to generate them.

## How the firmware works

### 1. Startup and display initialization

`setup()` initializes Serial, the Hall sensor interrupt, ESC PWM, display/touch input, LVGL, and the saved settings. It loads the selected preset, shows the splash screen, initializes the RPM/control timing variables, and creates the two FreeRTOS tasks.

The splash screen is configured for approximately 5000 ms. After that, the main panel is shown.

### 2. Touchscreen and UI

LVGL creates and updates the interface. The user can edit target speed, ramp time, and coating time, choose one of three presets, save values, and start or stop a run.

The code validates target RPM against the defined range (`TARGET_SPEED_MIN_RPM` and `TARGET_SPEED_MAX_RPM`). Check these constants in the sketch if you change the intended operating range.

While the motor is running, the UI updates labels for RPM, ramp progress, and coating progress. The display code snaps the shown RPM to the target when the filtered reading is within ±100 RPM. This is only a display convenience and can make the displayed value look more exact than the sensor reading.

### 3. Hall-sensor interrupt

The `hallISR()` function runs on a rising edge at GPIO 21:

```cpp
void IRAM_ATTR hallISR()
{
    pulseCount++;
}
```

It does only one small operation: increment the counter. It does not update LVGL, calculate RPM, print serial messages, or run the PI controller. Keeping interrupt work short reduces interrupt latency.

The motor task copies and clears `pulseCount` in a short interrupt-disabled section before using the captured pulse count.

### 4. RPM measurement

The sketch uses a 100 ms measurement window (`RPM_INTERVAL_MS`). It calculates RPM from the number of pulses collected during the elapsed interval:

```text
RPM = pulses * 60000 / (PULSES_PER_REV * elapsed_ms)
```

The uploaded sketch sets `PULSES_PER_REV` to **2**, so the calculation assumes two counted Hall pulses per motor revolution. If your magnet/sensor arrangement produces one pulse per revolution, change this constant to `1`; otherwise the calculated RPM will be incorrect by a factor of two.

A simple exponential low-pass filter smooths the measured RPM using `RPM_FILTER`. Filtering reduces jitter but also adds some response delay.

### 5. Feedforward estimate and PI correction

The `estimatePWM()` function estimates an initial ESC pulse width for the target RPM using a piecewise mapping based on measured motor behavior. This is the feedforward component.

The PI controller calculates:

```text
error = target RPM - filtered RPM
correction = Kp * error + Ki * integral(error)
desired PWM = feedforward PWM + correction
```

The integral is constrained to a limited range, and the output command is constrained by the configured ESC pulse range. The controller also limits how quickly the PWM command changes. These limits are intended to reduce abrupt changes and excessive corrections; they do not guarantee stable control without calibration.

The gain values in the uploaded sketch are `Kp = 0.010` and `Ki = 0.04`. These are starting values in this particular code and may need to be tuned carefully on the real hardware. Change one parameter at a time and record the response.

### 6. Ramp-up, coating timer, ramp-down, and stop

- On Start, the firmware clears RPM and timer state, sets the ESC command to its configured minimum, and marks the coater as running.
- During ramp-up, the PWM command is moved gradually toward the feedforward-plus-PI request according to the configured ramp time.
- When the filtered RPM enters the target ±100 RPM window, the firmware marks ramp-up complete and starts the coating timer.
- Once the coating duration has elapsed, ramp-down begins.
- During ramp-down, the ESC command is reduced over the configured ramp time.
- When ramp-down completes, the run state and controller state are reset and the ESC receives the configured minimum pulse.
- The Stop button resets the run state and sends the configured minimum pulse immediately in software.

**Important:** Sending minimum throttle is not the same as removing motor power. A physical emergency-stop or power-disconnect solution is recommended for a real machine.

## FreeRTOS implementation

**FreeRTOS is actively used in this firmware, not merely listed as a future improvement.** The sketch creates two tasks using `xTaskCreatePinnedToCore()`.

### Task configuration

| Task | Core | Stack size argument | Priority argument | Main responsibility |
|---|---:|---:|---:|---|
| `UI_Task` | Core 1 | 8192 | 2 | Splash screen, LVGL timers/handler, UI labels, display state |
| `Motor_Control` | Core 0 | 4096 | 2 | RPM measurement, filtering, PI control, ramp/timer state, ESC command |

These are the values passed by the sketch to `xTaskCreatePinnedToCore()`. Stack-size units and the appropriate stack headroom should be verified for the exact ESP32 Arduino/FreeRTOS environment.

### UI task

The `uiTask()` function runs an endless loop. Each iteration checks the splash screen, handles the motor auto-stop UI update, updates live motor labels, calls `ui_tick()` and `lv_timer_handler()`, then waits using `vTaskDelayUntil()` with a 5 ms period.

This task owns the LVGL calls. Keeping LVGL calls in one task avoids having the UI and motor task update LVGL objects concurrently.

### Motor-control task

The `motorTask()` function runs an endless loop. It calls `calculateRPM()` and `updatePI()`, then uses `vTaskDelayUntil()` with a 10 ms period.

The functions themselves use elapsed-time checks: RPM measurement is updated every 100 ms and PI control every 100 ms. So the task wakes about every 10 ms, but the RPM and PI calculations do not necessarily perform a full update on every wake.

The motor task does not run LVGL code or wait for UART. This keeps the recurring motor work separate from display handling.

### Why pin tasks to separate cores?

On the dual-core ESP32 target, the UI task is pinned to Core 1 and motor-control task to Core 0. The goal is to reduce competition between display/UI work and recurring motor-control work. Core pinning does not by itself guarantee hard real-time behavior; task priorities, execution time, shared data, interrupts, and other system tasks still matter.

### How the tasks are created

The key setup calls in the sketch are equivalent to:

```cpp
xTaskCreatePinnedToCore(
    uiTask, "UI_Task", 8192, NULL, 2, &uiTaskHandle, 1
);

xTaskCreatePinnedToCore(
    motorTask, "Motor_Control", 4096, NULL, 2, &motorTaskHandle, 0
);
```

The Arduino `loop()` is intentionally almost empty because the application work is performed by these FreeRTOS tasks:

```cpp
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

### Concurrency notes for developers

- The Hall interrupt and motor task share `pulseCount`; the sketch protects the counter snapshot/reset with interrupts disabled briefly.
- UI and motor tasks share run parameters, measured RPM, timers, and run-state flags. For a more robust production implementation, consider protecting multi-field updates with a mutex/critical section or passing commands and telemetry through FreeRTOS queues.
- Keep all LVGL object access in the UI task unless LVGL access is explicitly protected according to the version's requirements.
- Check task stack high-water marks and monitor task watchdog behavior during long-duration testing.
- A task delay period is not a guarantee that every operation completes exactly on schedule.

## Motor-control sequence

```text
User presses Start
        |
        v
Reset counters and PI state
        |
        v
Set initial ESC command
        |
        v
Motor task measures Hall pulses
        |
        v
Calculate and filter RPM
        |
        v
Target RPM - measured RPM
        |
        v
Feedforward estimate + PI correction
        |
        v
Limit PWM change and update ESC
        |
        v
Target reached within ±100 RPM?
        | No
        +--------------------> Continue ramp/control
        |
       Yes
        |
        v
Start coating timer
        |
        v
Coating duration completed?
        | No
        +--------------------> Continue speed control
        |
       Yes
        |
        v
Ramp down ESC command
        |
        v
Return to minimum command and finish
```

## Saved settings and NVS

The sketch uses the ESP32 `Preferences` library, which stores key/value data in non-volatile storage (NVS). Three setting slots are available. Each slot stores:

- Target speed in RPM.
- Ramp time in seconds.
- Coating time in seconds.

When the user saves a preset, the values are written to NVS. At startup, the firmware loads saved values and the selected preset. This allows the configuration to persist after a restart or power cycle.

## Build and test

1. Review the wiring and verify the Hall sensor output is safe for a 3.3 V ESP32 GPIO.
2. Confirm the exact ESC throttle range and arming procedure from the ESC documentation.
3. Install the required Arduino libraries and matching LVGL/EEZ Studio UI files.
4. Open the sketch in Arduino IDE and select the correct ESP32 board and port.
5. Compile and upload the firmware.
6. Open Serial Monitor at 115200 baud.
7. Initially test the display and touch interface with the motor disconnected or mechanically secured.
8. Verify the Hall sensor pulse count at low speed before relying on the RPM display.
9. Test the ESC signal without an unsafe rotating assembly, following the ESC's arming instructions.
10. Secure the motor and chuck, start at a conservative setting, and increase speed gradually.
11. Record target RPM, measured RPM, PWM command, and behavior under the intended mechanical load before tuning PI gains.

## Troubleshooting

### RPM is approximately half or double the expected value

Check `PULSES_PER_REV`. The uploaded sketch sets it to `2`. Set it to the actual number of Hall pulses per revolution.

### RPM reading jumps or is unstable

Check sensor alignment, magnet security, wiring, pull-up voltage, grounding, and noise. Confirm the sensor generates one clean edge per intended pulse. Filtering can smooth the display but cannot correct bad pulse data.

### Motor overshoots or oscillates

First verify the feedforward mapping against measured PWM/RPM data. Then tune PI gains carefully, changing one at a time. Check supply voltage, load, sensor quality, and the ESC's supported signal range.

### Touch/display behavior is incorrect

Check the ILI9488 shield pin mapping, touch calibration values, `TOUCH_Y_OFFSET`, and compatibility between the installed LVGL version and generated EEZ Studio files.

### ESP32 resets or a task watchdog triggers

Check Serial logs, task execution time, stack headroom, and any blocking work added to either task. Keep the Hall ISR short. Avoid calling LVGL from the motor task. If sharing more data between tasks, use a deliberate synchronization strategy.

## Limitations and future improvements

- Add robust fault detection for missing Hall pulses, disconnected sensors, stalled motor, and implausible RPM.
- Add a hardware emergency-stop that safely removes motor drive power.
- Consider FreeRTOS queues for commands and telemetry instead of unsynchronized shared state.
- Review thread-safety of all shared values and make cross-task updates atomic where necessary.
- Add task stack high-water-mark reporting and long-duration stress tests.
- Calibrate the feedforward PWM map under the real chuck and sample load.
- Add explicit overspeed and sensor-failure behavior.
- Validate RPM accuracy with an independent tachometer.
- Add data logging for tuning and repeatability.
- Consider a higher-pulse-count sensor if improved speed resolution is needed.

## Safety

- Secure the motor, magnet, and chuck before powering the ESC.
- Keep hands, wires, clothing, and loose objects away from rotating parts.
- Use a balanced chuck and suitable mechanical mounting.
- Keep liquids away from exposed electronics.
- Use a power supply, wiring, connector, and fuse suitable for expected motor current.
- Do not rely on software minimum throttle as the only emergency stop.
- Do not use an unverified ESC pulse range.
- Test at low speed first and increase gradually.
- Never touch or adjust the rotating assembly while powered.
- Wear suitable eye protection and use a protective enclosure during high-speed testing.
