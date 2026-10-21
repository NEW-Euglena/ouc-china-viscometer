# English Monitoring Interface

## Features

- Device temperature, measurement stage, pump/valve status, and upper/lower liquid-level indicators.
- Temperature-history chart and receiver-side record storage.
- Manual input of sample time, solvent reference time, and concentration.
- Automatic input from a normally completed device measurement.
- Relative-viscosity calculation, single-point intrinsic-viscosity estimation, and calibrated molecular-weight estimation.
- USB serial reception and HTTP telemetry through the Python receiver.

## Included Files

| File | Purpose |
| --- | --- |
| [dashboard.html](dashboard.html) | English browser interface and calculation controls. |
| [telemetry_server.py](telemetry_server.py) | Local web server, telemetry receiver, and history service. |
| [requirements.txt](requirements.txt) | Python dependency list. |
| [start-monitor.ps1](start-monitor.ps1) | PowerShell launch helper. |
| [check_calibration.cjs](check_calibration.cjs) | Calibration and calculation-boundary check script. |

## Manual Calculation

Open `dashboard.html` directly in a browser and enter the sample efflux time, solvent reference time, and sample concentration. Manual calculation operates independently of the device connection.

## Local Receiver

With Python 3.10 or later, open PowerShell in this directory and run:

```powershell
python -X utf8 telemetry_server.py --host 127.0.0.1 --port 8080
```

Open `http://127.0.0.1:8080/` in a browser. Omitting `--serial` starts the receiver without opening a serial port.

For USB telemetry, install dependencies and specify the device port:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -X utf8 telemetry_server.py --host 127.0.0.1 --port 8080 --serial COM5
```

Replace `COM5` with the device port. The launch helper also supports:

```powershell
./start-monitor.ps1 -Port COM5
```

The receiver uses 115200 baud for USB telemetry. For Wi-Fi reception, use `--host 0.0.0.0` and configure the device with the receiver address.

## Data Interfaces

| Endpoint | Function |
| --- | --- |
| `GET /api/latest` | Latest device record and freshness information. |
| `GET /api/history` | Temperature-history records. |
| `POST /api/telemetry` | Incoming HTTP telemetry. |

History is stored in `../logs/telemetry.jsonl`; the `--log` option selects another path. Automatic calculation uses normally completed measurements without reported errors. The interface displays freshness and fault information.

## Calibration and Units

The calculation uses `K = 0.00025048647658895034` and `alpha = 0.6479793832009052`; the interface displays `K = 2.50486 x 10^-4` and `alpha = 0.647979`.

The two calibration points are `(350000 Da, 0.98 dL/g)` and `(3020000 Da, 3.96 dL/g)`.

| Quantity | Unit |
| --- | --- |
| Efflux time | s |
| Concentration | g/dL |
| Intrinsic viscosity | dL/g |
| Molecular Weight Estimate | Da |

## Calculation Checks

With Node.js installed, run:

```powershell
node check_calibration.cjs
```
