# ESP32 Spin Coater with BLDC RPM Control

A DIY spin coater project built around an ESP32, an A2212 brushless DC
motor, a SimonK 30 A ESC, and an A3144 Hall-effect sensor. The firmware
reads motor speed and adjusts the ESC throttle to approach a target RPM
entered through the Serial Monitor.

> Project status: The serial-based RPM measurement and control firmware
> is under development. RPM accuracy, low-speed performance, and
> closed-loop stability must be validated on the assembled hardware. The
> ILI9488 TFT is part of the planned hardware setup, but the firmware
> described here does not yet implement a TFT user interface.

## Contents

-   [Project overview](#project-overview)
-   [Features](#features)
-   [Hardware](#hardware)
-   [System architecture](#system-architecture)
-   [Wiring](#wiring)
-   [Software setup](#software-setup)
-   [How the firmware works](#how-the-firmware-works)
-   [Serial commands](#serial-commands)
-   [RPM calculation](#rpm-calculation)
-   [PI control](#pi-control)
-   [Calibration](#calibration)
-   [Build and test procedure](#build-and-test-procedure)
-   [Troubleshooting](#troubleshooting)
-   [Limitations and future
    improvements](#limitations-and-future-improvements)
-   [Safety](#safety)
-   [Repository structure](#repository-structure)

## Project overview

The goal is to spin a small substrate or sample at a controlled speed so
that a liquid can spread across its surface. A spin coater typically
uses a rotating chuck, a motor drive, speed feedback, and a way to set
the desired speed.

In this build:

1.  The ESP32 receives a target RPM through the Serial Monitor.
2.  The A3144 Hall sensor produces a pulse when its magnetic target
    passes the sensor.
3.  The ESP32 estimates RPM from the time between Hall pulses.
4.  A PI controller compares target RPM with measured RPM.
5.  The controller adjusts the ESC command to reduce the speed error.
6.  The Serial Monitor prints the target, measured RPM, error,
    feedforward PWM estimate, PI correction, and output pulse width.

The motor is driven by an ESC. The ESP32 does not drive the motor phases
directly.

## Features

-   Target RPM input through the Arduino Serial Monitor.
-   Hall-effect speed feedback using an ESP32 interrupt.
-   RPM calculation based on Hall pulse period.
-   Feedforward PWM estimate based on measured motor data.
-   Proportional-integral (PI) correction.
-   PWM slew limiting to reduce abrupt command changes.
-   RPM and controller diagnostics over serial.
-   ESP32 Arduino Core 3.x LEDC API support.

Features not implemented in the firmware described here: - TFT display
interface. - Touchscreen controls. - FreeRTOS task-based architecture. -
Automatic spin recipes or timed stages. - Emergency-stop input. -
Closed-loop verification of a guaranteed ±10 RPM tolerance.

## Hardware

  -----------------------------------------------------------------------
  Component                           Purpose
  ----------------------------------- -----------------------------------
  ESP32-WROOM-32D development board   Main controller and pulse
                                      measurement

  A2212 BLDC motor                    Rotating drive

  SimonK 30 A ESC                     Three-phase motor drive and
                                      throttle interface

  A3144 Hall-effect sensor            Detects a magnet once per
                                      revolution

  Small magnet                        Rotating target for the Hall sensor

  12 V, 2 A adapter                   Power source, subject to motor
                                      startup/current requirements

  7805 regulator                      Provides a nominal 5 V rail for
                                      suitable peripherals

  2.4-inch ILI9488 TFT display        Intended display hardware; not
                                      controlled by the serial-only
                                      firmware below

  Dot PCB / perfboard                 Prototype assembly

  12 V female DC socket               Power input connector

  Wires, connectors, mounting         Electrical and mechanical assembly
  hardware                            

  Rotating chuck or sample holder     Holds the substrate or sample
  -----------------------------------------------------------------------

Check the current and voltage requirements of every module before
connecting it. A 12 V, 2 A adapter may be insufficient for an A2212
motor under load. The ESC and motor can demand much more current during
acceleration or loading.

## System architecture

``` text
Target RPM entered in Serial Monitor
                |
                v
          ESP32 controller
                |
                v
         Feedforward + PI
                |
                v
        ESC throttle signal
                |
                v
          SimonK 30 A ESC
                |
                v
           A2212 BLDC motor
                |
                v
       Magnet on rotating shaft
                |
                v
          A3144 Hall sensor
                |
                v
        ESP32 GPIO 21 interrupt
                |
                +---- RPM feedback to controller
```

## Wiring

The current firmware uses these pins:

  -----------------------------------------------------------------------
  Signal                              ESP32 connection
  ----------------------------------- -----------------------------------
  Hall sensor output                  GPIO 21

  ESC signal input                    GPIO 19

  Hall sensor VCC                     A voltage supported by your A3144
                                      module

  Hall sensor GND                     ESP32 GND

  ESC signal ground                   ESP32 GND, common signal reference

  ESC motor outputs                   A2212 three motor wires

  ESC power input                     Appropriate motor supply, according
                                      to ESC specifications
  -----------------------------------------------------------------------

### Hall sensor notes

The A3144 is commonly used with a pull-up output. The firmware
configures GPIO 21 with `INPUT_PULLUP`. Confirm the exact pinout of your
sensor or breakout board. Bare A3144 devices and modules may have
different pin layouts.

Use a suitable logic-level signal for the ESP32. Do not apply 5 V
directly to an ESP32 GPIO. If your Hall module pulls its output up to 5
V, use a 3.3 V pull-up or an appropriate level-shifting circuit.

Attach a small magnet securely to the rotating part. Position the sensor
so the magnet passes it once per revolution. Keep the sensor and magnet
mechanically secure and away from the propeller or exposed rotating
parts.

### ESC notes

-   Connect the ESC signal wire to GPIO 19.
-   Connect ESP32 GND to ESC signal GND.
-   Connect the A2212's three phase wires to the ESC's three motor
    outputs.
-   Power the ESC according to its specifications.
-   Do not power the motor from the ESP32 5 V pin.
-   Verify the ESC's throttle signal requirements before using the
    firmware.

The firmware uses 50 Hz PWM and calculates the pulse width as a fraction
of a 20,000 microsecond period. Most RC ESCs expect a throttle pulse
near 1000 to 2000 microseconds. A 10,000 microsecond pulse is not a
normal maximum-throttle command for a typical RC ESC. Do not use
`ESC_MAX_US = 10000` unless the specific ESC documentation confirms that
it supports this signal. For a conventional RC ESC, start with a
verified range such as 1000 to 2000 microseconds and calibrate according
to the ESC manufacturer's instructions.

## Software setup

1.  Install the Arduino IDE.
2.  Install the Espressif ESP32 board package using Boards Manager.
3.  Select the correct ESP32 board and COM port.
4.  Use an ESP32 Arduino Core 3.x release for the `ledcAttach()` and
    pin-based `ledcWrite()` API used by the firmware.
5.  Connect the ESP32 by USB.
6.  Upload the firmware.
7.  Open Serial Monitor at 115200 baud.
8.  Set the line ending to Newline or Both NL & CR.
9.  Keep the motor disconnected or mechanically safe during initial
    signal checks.

No external Arduino library is required for the serial-only firmware
beyond the ESP32 Arduino core.

## How the firmware works

### 1. Hall interrupt

The Hall sensor output is connected to GPIO 21. On each rising edge, the
interrupt routine records the time using `micros()` and calculates the
interval since the previous accepted pulse.

The interrupt does very little work. It records timing data so the main
loop can process RPM outside the interrupt.

### 2. RPM calculation

For one pulse per revolution:

``` text
RPM = 60,000,000 / pulse_period_microseconds
```

For example, if the time between pulses is 12,000 microseconds:

``` text
RPM = 60,000,000 / 12,000
RPM = 5,000
```

If the sensor generates more than one pulse per revolution, divide by
the configured `PULSES_PER_REV` value.

### 3. Period averaging and filtering

The firmware stores several pulse periods and averages them to reduce
small timing variations. It then applies a low-pass filter to smooth the
RPM estimate.

Filtering reduces noise, but too much filtering adds delay. The number
of averaged periods and filter coefficient should be tuned for the
required response time and motor speed range.

### 4. Feedforward estimate

The `estimatePWM()` function estimates a starting ESC pulse width from
the target RPM. It uses approximate measurements collected during
testing, such as around 1290 to 1295 microseconds near 4800 to 5000 RPM.

This is a starting estimate, not a universal motor curve. RPM changes
with the motor, ESC, supply voltage, propeller or chuck, mechanical
load, magnet placement, and ESC calibration. Re-measure and update the
table for your own assembly.

### 5. PI controller

The controller calculates:

``` text
error = target_RPM - measured_RPM
```

The proportional term responds to the current error. The integral term
accumulates error over time to correct a persistent offset.

A simplified PI equation is:

``` text
correction = Kp * error + Ki * integral
integral = integral + error * dt
```

The feedforward estimate provides the initial PWM command. The PI
correction adjusts that estimate. The firmware limits the correction and
the PWM change per control update to avoid abrupt changes.

### 6. PWM generation

The ESC signal uses a 50 Hz period, or 20,000 microseconds. The firmware
converts the requested pulse width to a 16-bit LEDC duty value and
writes it to GPIO 19.

The pulse width must stay within the valid range supported by the ESC.
Confirm the limits in the ESC documentation before testing.

### 7. Stop command

Entering `0` sets the ESC command to the configured minimum pulse width
and clears the controller state. Minimum throttle is not a substitute
for a physical emergency stop or power disconnect. The motor may
continue spinning due to inertia.

## Serial commands

Enter a target RPM as a number followed by Enter.

  Command   Action
  --------- ------------------------------------------------------
  `3000`    Request 3000 RPM
  `5000`    Request 5000 RPM
  `7000`    Request 7000 RPM
  `0`       Send minimum throttle and clear the controller state

The serial output includes:

  Field      Meaning
  ---------- ----------------------------------------------------
  `Target`   Requested RPM
  `RPM`      Filtered measured RPM
  `Error`    Target RPM minus measured RPM
  `Base`     Feedforward pulse-width estimate
  `PI`       PI correction applied to the estimate
  `PWM`      Final pulse width sent to the ESC, in microseconds

The target RPM is a setpoint. It is not a guarantee that the motor will
reach or hold that speed.

## Calibration

Calibrate the motor and ESC before relying on closed-loop control.

1.  Secure the motor and rotating assembly. Remove any propeller-like
    blades during bench testing if they are not needed.
2.  Verify the ESC throttle range from its documentation.
3.  Start at minimum throttle and follow the ESC's arming procedure.
4.  Confirm that the Hall sensor generates exactly one valid pulse per
    revolution.
5.  Test several safe pulse widths and record the steady RPM after the
    motor settles.
6.  Build a table of measured pulse width versus RPM.
7.  Update `estimatePWM()` using the measured data.
8.  Test several target speeds and record the steady-state error.
9.  Adjust `Kp` and `Ki` in small steps. Change one parameter at a time.
10. Repeat tests with the actual chuck and sample load, because load
    changes affect RPM.

Example calibration table:

  -----------------------------------------------------------------------
            ESC pulse width              Measured RPM Notes
  ------------------------- ------------------------- -------------------
                    1242 us                      3360 Previously
                                                      observed, test
                                                      conditions may
                                                      differ

                    1246 us              3480 to 3540 Previously
                                                      observed, test
                                                      conditions may
                                                      differ

                    1249 us              3480 to 3600 Previously observed
                                                      data varies

                    1290 us                      4800 Previously observed

                    1295 us              4920 to 5040 Previously observed

                    1300 us                      5040 Previously observed
  -----------------------------------------------------------------------

These are historical measurements from development, not guaranteed
calibration values. Collect a fresh table with a consistent supply,
load, and measurement method.

## Build and test procedure

1.  Assemble the ESP32, Hall sensor, ESC signal connection, and power
    wiring.
2.  Check all grounds and confirm that ESP32 GPIOs only receive safe
    logic levels.
3.  Upload the firmware.
4.  Open Serial Monitor at 115200 baud.
5.  Verify that the Hall sensor produces pulses when the shaft turns.
6.  Confirm that the displayed RPM is plausible compared with an
    independent tachometer if available.
7.  Start with a low, safe target and increase it gradually.
8.  Observe target RPM, measured RPM, error, and PWM.
9.  Tune the feedforward table and PI gains.
10. Test speed holding with the actual mechanical load.

Do not assume the system meets a ±10 RPM tolerance from a short sample.
Record the settled RPM over time and compare it with a reliable
independent tachometer. With one pulse per revolution, period
measurement improves resolution, but sensor timing variation, shaft
alignment, magnet geometry, and filtering still affect the result.

## Troubleshooting

### RPM reads zero while the motor spins

-   Check the Hall sensor supply, ground, and output.
-   Confirm that the magnet passes close enough to the sensor.
-   Verify the sensor pinout.
-   Check that the output signal changes at GPIO 21.
-   Confirm that the code uses the correct `PULSES_PER_REV`.

### RPM readings jump

-   Check for multiple triggers per revolution.
-   Secure the magnet and sensor mechanically.
-   Check for electrical noise from the ESC and motor wires.
-   Keep Hall sensor wiring away from motor phase wires.
-   Verify the sensor output waveform and grounding.
-   Adjust period averaging and filtering after the wiring is correct.

### Motor does not start

-   Confirm the ESC's arming procedure and throttle range.
-   Verify the battery or power supply can supply the required current.
-   Check the ESC signal and common ground.
-   Check that the motor phases are connected properly.
-   Do not assume a 12 V, 2 A adapter can start or run the motor under
    load.

### RPM remains above target

-   Recheck the measured PWM-to-RPM table.
-   Confirm that a lower pulse width actually reduces RPM on your ESC.
-   Reduce feedforward PWM if the motor consistently runs too fast.
-   Tune PI gains only after the feedforward estimate is close.
-   Check whether the ESC has a minimum throttle or arming threshold.

### RPM oscillates around the target

-   Confirm the RPM measurement is stable first.
-   Reduce `Kp` if corrections are too strong.
-   Reduce `Ki` if the controller accumulates too much correction.
-   Avoid changing several parameters at once.
-   Check the motor supply and mechanical load for variation.

### RPM is inaccurate at low speed

With one pulse per revolution, low-speed measurements update slowly
because the interval between Hall pulses becomes long. Period
measurement is better than short-window pulse counting, but the control
response still becomes slower at low RPM. A sensor with more pulses per
revolution can improve feedback resolution.

## Limitations and future improvements

-   Add ILI9488 TFT support for target RPM, actual RPM, PWM, and
    controller status.
-   Add touchscreen or encoder-based target setting.
-   Add timed spin stages with configurable speed and duration.
-   Add a physical emergency-stop circuit that removes motor drive power
    safely.
-   Improve calibration with a measured lookup table or interpolation.
-   Add fault detection for missing Hall pulses, sensor disconnection,
    and invalid RPM.
-   Add a data logging mode for CSV capture over serial.
-   Evaluate a higher-resolution speed sensor if tight low-speed control
    is required.
-   Validate control performance under the actual sample and chuck load.
-   Consider FreeRTOS tasks only if the full application needs separate
    display, control, and logging tasks.

## Safety

-   Secure the motor and chuck before powering the ESC.
-   Keep hands, wires, clothing, and loose objects away from rotating
    parts.
-   Use a balanced chuck and suitable mechanical mounting.
-   Keep liquids away from exposed electronics.
-   Use a supply, wiring, connector, and fuse suitable for the expected
    motor current.
-   Do not rely on software minimum throttle as the only emergency stop.
-   Do not use an unverified ESC pulse range.
-   Test at low speed first and increase gradually.
-   Never touch or adjust the rotating assembly while it is powered.

## Contributing

Suggestions and improvements are welcome. Include the ESP32 board
version, ESC model, Hall sensor setup, target RPM, measured RPM, and
relevant serial logs when reporting a problem.

## License

Choose a license before publishing the repository. For example, the MIT
License permits reuse with attribution. Add the complete license text in
a `LICENSE` file if you choose MIT.

## Acknowledgements

Built as an embedded systems project using ESP32, BLDC motor control,
Hall-effect sensing, PWM, and PI feedback control.
