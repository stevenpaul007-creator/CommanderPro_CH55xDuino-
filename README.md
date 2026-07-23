# CH552T Commander Pro compatible HID firmware

This repository is now cleaned down to the Arduino IDE / CH55xDuino implementation only.

License: MIT. See [`LICENSE`](LICENSE).

Current target:

- MCU: CH552T / CH55xDuino
- USB identity: Corsair Commander Pro compatible HID
- VID:PID: `1b1c:0c10`
- USB mode: HID only, no CDC
- Physical fan channels: 2 PWM outputs + 2 tach inputs
- Logical Commander Pro fan slots: fan1/fan2 connected, fan3..fan6 disconnected

## Files kept

```text
CommanderPro_CH55xDuino/CommanderPro_CH55xDuino.ino  # firmware
README.md                                             # this document
tools/windows_hid_test.py                             # Windows HID test tool
```

## Arduino IDE build

1. Install CH55xDuino in Arduino IDE.
2. Open:

   ```text
   CommanderPro_CH55xDuino/CommanderPro_CH55xDuino.ino
   ```

3. Select the CH552 / CH552T board.
4. Select a **USER USB RAM** option, not a stock built-in USB mode.

   CH55xDuino examples commonly name these options like:

   ```text
   USER USB setting
   user148
   user266
   ```

   If multiple options are available, use the larger USER USB RAM option first.

5. Compile and upload from Arduino IDE / WCHISPTool.

## Pin plan

CH55xDuino uses decimal pin names: `P3.4` is `34`, `P1.5` is `15`, etc.

| Function | CH552 pin | CH55xDuino number | Notes |
|---|---:|---:|---|
| Fan1 PWM | P1.5 | 15 | hardware PWM1 |
| Fan2 PWM | P3.4 | 34 | hardware PWM2 |
| Fan1 tach | P3.2 | 32 | INT0 |
| Fan2 tach | P3.3 | 33 | INT1 |

Important: `P1.4` / Arduino pin `14` is **not** hardware PWM in CH55xDuino's CH552 pin map. Use `P3.4` / pin `34` for the second hardware PWM channel.

## Hardware protection

Recommended PWM output protection:

```text
CH552 PWM pin -> 270 ohm series -> fan PWM input
protected node -> 1N4148 clamp to +5V
optional protected node -> 10k pulldown to GND
```

Recommended tach input protection:

```text
fan tach -> 10k series -> CH552 INT input P3.2/P3.3
protected node -> 1N4148 clamp to +5V
protected node -> 100k pulldown to GND
optional 10k pull-up to +5V if the fan tach output is open collector and has no pull-up
```

For accidental 12V tach pull-ups, a 10k series resistor limits clamp current to roughly:

```text
(12V - 5.7V) / 10k = 0.63 mA
```

## Linux verification

A healthy dmesg log looks like:

```text
usb ... New USB device found, idVendor=1b1c, idProduct=0c10, bcdDevice= 1.00
usb ... Product: Commander Pro
usb ... Manufacturer: Corsair
usb ... SerialNumber: CH552
corsair-cpro ... hidrawX: USB HID v1.11 Device [Corsair Commander Pro]
```

If `lm-sensors` is installed, `sensors` should show something like:

```text
corsaircpro-hid-*-*
Adapter: HID adapter
in0:          12.00 V
in1:           5.00 V
in2:           3.30 V
fan1 4pin:      0 RPM
fan2 4pin:      0 RPM
```

`0 RPM` is normal if the fan is not spinning or tach is not wired/pulled up yet. The Linux driver binding successfully is the important enumeration/protocol signal.

Useful Linux commands:

```sh
dmesg -w
sensors
grep -R . /sys/class/hwmon/hwmon*/name
```

## Windows verification

Install Python HID dependency:

```powershell
py -m pip install hidapi
```

Run the test tool:

```powershell
cd path\to\ch552-arduino-commander-pro\tools
py windows_hid_test.py list
py windows_hid_test.py info
py windows_hid_test.py modes
py windows_hid_test.py rpm 1
py windows_hid_test.py duty 1 60
py windows_hid_test.py poll --fan 1 --interval 1
```

Windows device check:

```powershell
Get-PnpDevice -PresentOnly | ? InstanceId -match 'VID_1B1C&PID_0C10'
```

## Implemented Commander Pro HID commands

The host sends a 64-byte HID OUT report. Python `hidapi` on Windows writes 65 bytes: report ID `0` followed by the 64-byte report.

The device returns a 16-byte HID IN report. Response byte 0 is a Commander Pro status/error code:

| Byte 0 | Meaning |
|---:|---|
| `0x00` | success |
| `0x01` | invalid command |
| `0x10` | invalid argument |
| `0x11` | disconnected temperature sensor |
| `0x12` | PWM not fixed-duty controlled / unavailable |

Implemented commands:

| Command | Function |
|---:|---|
| `0x02` | firmware version, returned as three bytes for host display like `major.minor.patch` |
| `0x06` | bootloader version `0.1` |
| `0x10` | dummy temperature sensor connection status, all four reported present |
| `0x11` | dummy temperature, 25.00 C; no real thermistor hardware is read |
| `0x12` | dummy rails, 12 V / 5 V / 3.3 V |
| `0x20` | fan modes; fan1/fan2 are 4-pin PWM, fan3..fan6 disconnected |
| `0x21` | fan RPM, big-endian response bytes 1..2 |
| `0x22` | get fixed PWM duty percent |
| `0x23` | set fixed PWM duty percent |
| `0x24` | accept target-RPM write for Linux hwmon compatibility |
| `0x25` | accept profile frame for compatibility; no curve control loop yet |
| `0x28` | set fan mode |
| `0x33` | accept LED commit as a no-op |
| `0x34` | accept begin LED effect as a no-op |
| `0x35` | accept LED effect data as a no-op |
| `0x37` | accept reset LED channel as a no-op |
| `0x38` | accept set LED channel state as a no-op |

## Known current behavior

- HID enumeration works on Windows and Linux.
- Linux `corsair-cpro` can bind and expose `sensors` readings.
- USB serial is spoofed as a Commander-Pro-like string (`CPRO00000001`).
- Voltage readings are fixed dummy values.
- Temperature sensors are reported present but return fixed dummy values.
- LED/lighting commands return success but do not drive real LEDs.
- Only fan1/fan2 are real hardware channels.
- RPM depends on tach wiring and pull-up; many fan tach outputs need a pull-up.

## License

MIT License. See [`LICENSE`](LICENSE).

Copyright (c) 2026 seven.
