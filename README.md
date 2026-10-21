# Automated Ubbelohde Viscometer

English interface and source files for the automated Ubbelohde viscometer developed by OUC-China.

## Packages

| Package | Contents |
| --- | --- |
| `hmi-en.zip` | Editable serial-display project, compiled screen file, background and font resources, background-generation script, and simulator preview. |
| `firmware-en.zip` | STM32 application source, peripheral drivers, CubeMX configuration, startup code, linker script, and build tools. |
| `web-en.zip` | Browser-based monitoring interface, telemetry receiver, launch script, dependency list, and calibration-check script. |

Extract the packages into the same directory to retain the links between the display project, firmware, and documentation.

```text
viscometer/
  README.md
  attachments/
    hmi-en/
    firmware-en/
    web-en/
  images/
    hmi-en-preview.png
```

## Serial Display

The 240 x 320 portrait interface displays temperature, measurement stage, elapsed time, upper and lower liquid-level states, pump/valve status, and temperature-probe status. The package contains an editable `.HMI` project and a compiled `.tft` screen file.

## Controller Firmware

The STM32G071RB firmware coordinates temperature acquisition, pneumatic actuation, liquid-level detection, efflux timing, serial-display updates, and telemetry. The project includes CMake configuration and an ARM GCC toolchain file.



## Monitoring and Calculation Interface

The English browser interface provides device status, temperature history, manual calculation, and automatic timing-data input. The Python receiver supports USB serial telemetry and HTTP telemetry.

The calculation workflow converts efflux time into relative viscosity, estimates intrinsic viscosity with the Solomon-Ciuta single-point method, and estimates molecular weight using the project calibration relationship. Concentration is expressed in g/dL, intrinsic viscosity in dL/g, and molecular weight estimates in Da.

## Dependencies and Resources

- Display editing: a compatible USART HMI editor for the TJC3224T124_011C_A01 screen.
- Firmware building: STM32Cube tools, CMake, Ninja, and ARM GCC.
- Telemetry receiver: Python 3.10 or later; USB serial reception uses `pyserial`.
- Calibration checks: Node.js.

Third-party driver license files and copyright notices are included in the firmware package.
