# Camera connector PWM continuity test

This test is for the STM32U575OIY6QTR board described by
`..\..\Netlist_Schematic2_2026-09-03-02.tel` (relative to this README).

The firmware drives the 16 camera signal nets in ascending FPC pin order. It
uses software timing from the Cortex-M33 DWT cycle counter, so no camera
peripheral or timer alternate-function mapping is required. Every line is
configured as open-drain with an internal pull-up. The intended waveform is
approximately 50% duty cycle; measure frequency rather than duty cycle.

## Safety

Disconnect the camera module before running the test. PCLK, VSYNC, HREF and
D[7:0] are normally outputs of the camera, while this test deliberately drives
those nets from the MCU. Probe the camera-side connector/FPC end with the
module disconnected. This avoids output contention and prevents powering or
back-powering an unpowered sensor through its I/O protection structures.

## Frequency table

| Test # | FPC pin | Signal | STM32 pin | Frequency |
|---:|---:|---|---|---:|
| 1 | 3 | SDA | PC1 | 1.0 kHz |
| 2 | 5 | SCL | PC0 | 1.5 kHz |
| 3 | 6 | RESETB | PA2 | 2.0 kHz |
| 4 | 7 | VSYNC | PB7 | 2.5 kHz |
| 5 | 8 | PWDN | PA1 | 3.0 kHz |
| 6 | 9 | HREF | PA4 | 3.5 kHz |
| 7 | 12 | D7/Y9 | PE6 | 4.0 kHz |
| 8 | 13 | XCLK | PA8 | 4.5 kHz |
| 9 | 14 | D6/Y8 | PB8 | 5.0 kHz |
| 10 | 16 | D5/Y7 | PB6 | 5.5 kHz |
| 11 | 17 | PCLK | PA6 | 6.0 kHz |
| 12 | 18 | D4/Y6 | PE4 | 6.5 kHz |
| 13 | 19 | D0/Y2 | PC6 | 7.0 kHz |
| 14 | 20 | D3/Y5 | PC9 | 7.5 kHz |
| 15 | 21 | D1/Y3 | PC7 | 8.0 kHz |
| 16 | 22 | D2/Y4 | PC8 | 8.5 kHz |

FPC 2 and 15 are GND, FPC 4 is the AVDD path, FPC 10 is 1V5, and FPC 11 is
2V8. They are intentionally not driven as PWM signals.

## Build and flash

From the project root:

```powershell
.\build.ps1 APP=test_pwm all
.\build.ps1 APP=test_pwm flash-dap
```

The UART log is on USART1: PA9 = TX and PA10 = RX, at the project default
baud rate of 921600 8N1 unless overridden at build time.
