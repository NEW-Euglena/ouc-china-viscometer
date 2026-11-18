# STM32 Controller Firmware

## Functions

The STM32G071RB application coordinates DS18B20 temperature acquisition, pump and valve control, upper and lower liquid-level inputs, automated efflux timing, English serial-display updates, and USB/Wi-Fi telemetry.

The serial-display driver uses the component names and communication protocol of the accompanying [English screen project](../display/status_ui_en.HMI). Molecular-weight calculation is implemented in the browser interface.

## Measurement Cycle

Start a single run with the touchscreen or B1. During suction, PC7 closes the C-tube vent before the pump and B-tube valve turn on. The upper sensor triggers 600 ms of additional suction. The pump and B-tube valve then turn off before PC7 opens the vent. The falling liquid starts timing at the upper mark and stops timing at the lower mark. STOP or a second B1 press aborts the active run and opens the vent.

## Included Files

| Location | Contents |
| --- | --- |
| `Core/Inc` and `Core/Src` | Application headers, measurement logic, peripheral configuration, telemetry, and display drivers. |
| `Drivers` | CMSIS and STM32G0 HAL drivers with third-party license files. |
| `cmake` | Build configuration and toolchain files. |
| `code.ioc` | STM32CubeMX project configuration. |
| `startup_stm32g071xx.s` | MCU startup code. |
| `STM32G071xx_FLASH.ld` | Flash linker script. |
| `tools/build_firmware.ps1` | PowerShell build entry point. |

## Building

The project uses CMake, Ninja, and ARM GCC. The PowerShell helper locates tools in the Windows STM32Cube bundles directory.

From this directory, run:

```powershell
./tools/build_firmware.ps1 -BuildDirectory build/english
```

This command configures and compiles the firmware. The ARM GCC toolchain definition is `cmake/gcc-arm-none-eabi.cmake`.

## Wi-Fi Telemetry

Install `pyserial` and run `python tools/configure_network.py --wizard --port COM5` from this directory to configure the device over USB. Replace `COM5` with the device port. The [example configuration](config/network.example.json) lists the required fields.

## English Display Integration

`Core/Src/hmi_screen.c` provides runtime English status text. The matching display package contains the editable `.HMI` project, compiled `.tft` file, background, and fonts.

## Third-Party Components

License files and copyright notices for the included CMSIS and STM32 drivers are retained in `Drivers`.
