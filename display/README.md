# English Serial Display

## Interface

The project targets the TJC3224T124_011C_A01 serial display: 2.4-inch, 240 x 320 pixels, portrait orientation. Communication uses 9600 baud, 8N1, and page 0. The project encoding is gb2312, with ASCII-compatible English commands.

## Included Files

- [status_ui_en.HMI](status_ui_en.HMI): editable USART HMI project with embedded background and font resources.
- [status_ui_en.tft](status_ui_en.tft): compiled screen file.
- [assets/background.png](assets/background.png): English interface background.
- [assets/english16.zi](assets/english16.zi): 16-pixel Arial ASCII status font.
- `assets/digits40.zi`, `assets/status24.zi`, and `assets/status24v2.zi`: additional font resources.
- [create_background.py](create_background.py): background-generation script using Python, Pillow, and Windows Arial fonts.
- [hmi_screen.c](../firmware/Core/Src/hmi_screen.c): matching STM32 display driver.
- [Simulator preview](hmi-en-preview.png): interface preview with default placeholders.

## Display Components

| Component | Displayed information |
| --- | --- |
| `tTemp` | Temperature with two decimal places; `--.--` when unavailable. |
| `tTime` | Elapsed time or retained result, such as `12.345 s`; `--.--- s` when unavailable. |
| `tStage` | Idle, Suction, Extra suction, Draining, Timing, Complete, Aborted, Fault, or a specific fault status. |
| `tUpper` | Wet/Dry and Pending/Started. |
| `tLower` | Wet/Dry and Pending/Finished. |
| `tRun` | Pump/valve On/Off. |
| `tSensor` | Temperature OK, No probe, Data error, Waiting, or Stale data. |

## Project and Driver

Open `status_ui_en.HMI` in the compatible USART HMI editor to inspect the interface, component layout, and embedded resources. The English `.tft` and matching STM32 driver provide the static labels and dynamic status text.

The driver retains the component names, page identifier, acknowledgement responses, and `0xFF 0xFF 0xFF` command terminator. Stage and status components use font ID 5. Temperature and time retain numeric font IDs 2 and 3; font IDs 0, 1, and 4 remain in the project.

The background-generation script is available as:

```powershell
python create_background.py
```
