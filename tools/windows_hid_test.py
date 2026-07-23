#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 seven
"""Windows HID test tool for CH552 Commander Pro compatible firmware.

Dependencies on Windows:
  py -m pip install hidapi

Examples:
  py windows_hid_test.py info
  py windows_hid_test.py modes
  py windows_hid_test.py rpm 1
  py windows_hid_test.py duty 1 60
  py windows_hid_test.py poll --fan 1 --interval 1
"""

import argparse
import sys
import time

VID = 0x1B1C
PID = 0x0C10
OUT_REPORT_LEN = 64
IN_REPORT_LEN = 16

CMD_GET_FIRMWARE = 0x02
CMD_GET_BOOTLOADER = 0x06
CMD_GET_TEMP_CONFIG = 0x10
CMD_GET_TEMP = 0x11
CMD_GET_VOLTS = 0x12
CMD_GET_FAN_MODES = 0x20
CMD_GET_FAN_RPM = 0x21
CMD_GET_FAN_PWM = 0x22
CMD_SET_FAN_DUTY = 0x23
CMD_SET_FAN_TARGET = 0x24
CMD_SET_FAN_PROFILE = 0x25
CMD_SET_FAN_MODE = 0x28
CMD_LED_COMMIT = 0x33
CMD_BEGIN_LED_EFFECT = 0x34
CMD_LED_EFFECT = 0x35
CMD_RESET_LED_CHANNEL = 0x37
CMD_SET_LED_CHANNEL_STATE = 0x38

FAN_MODE_DISCONNECTED = 0x00
FAN_MODE_DC = 0x01
FAN_MODE_PWM = 0x02


def be16(buf, offset=1):
    return (buf[offset] << 8) | buf[offset + 1]


def load_hid():
    try:
        import hid
    except ImportError:
        print("Missing dependency: hidapi", file=sys.stderr)
        print("Install with: py -m pip install hidapi", file=sys.stderr)
        raise
    return hid


def list_devices():
    hid = load_hid()
    devices = hid.enumerate(VID, PID)
    return devices


class CommanderHid:
    def __init__(self, path=None):
        self.hid = load_hid()
        self.dev = self.hid.device()
        if path:
            self.dev.open_path(path)
        else:
            self.dev.open(VID, PID)
        # Avoid blocking forever on reads.
        self.dev.set_nonblocking(False)

    def close(self):
        self.dev.close()

    def exchange(self, cmd, payload=b"", timeout_ms=1000):
        # hidapi on Windows expects the first byte to be report ID.  The firmware
        # report descriptor has no explicit report ID, so this must be 0.  The
        # following 64 bytes are the actual HID OUT report, with command at byte 0.
        report = bytearray(OUT_REPORT_LEN + 1)
        report[0] = 0
        report[1] = cmd
        payload = bytes(payload[: OUT_REPORT_LEN - 1])
        report[2 : 2 + len(payload)] = payload

        written = self.dev.write(report)
        if written <= 0:
            raise OSError(f"hid write failed, returned {written}")

        data = self.dev.read(IN_REPORT_LEN, timeout_ms=timeout_ms)
        if not data:
            raise TimeoutError(f"no HID response for command 0x{cmd:02x}")
        if len(data) < IN_REPORT_LEN:
            data = list(data) + [0] * (IN_REPORT_LEN - len(data))
        return bytes(data)


def mode_name(v):
    return {
        FAN_MODE_DISCONNECTED: "disconnected/off",
        FAN_MODE_DC: "dc",
        FAN_MODE_PWM: "pwm",
    }.get(v, f"unknown(0x{v:02x})")


def open_device(args):
    devices = list_devices()
    if not devices:
        raise SystemExit("No device found: VID_1B1C&PID_0C10")
    if args.verbose:
        print(f"Found {len(devices)} matching HID device(s):")
        for i, d in enumerate(devices):
            print(f"[{i}] path={d.get('path')!r}")
            print(f"    product={d.get('product_string')!r} manufacturer={d.get('manufacturer_string')!r} serial={d.get('serial_number')!r}")
            print(f"    usage_page=0x{d.get('usage_page', 0):04x} usage=0x{d.get('usage', 0):04x} interface={d.get('interface_number')}")
    index = args.index
    if index < 0 or index >= len(devices):
        raise SystemExit(f"Device index {index} out of range, found {len(devices)}")
    return CommanderHid(devices[index]["path"])


def cmd_list(args):
    devices = list_devices()
    if not devices:
        print("No VID_1B1C&PID_0C10 HID devices found")
        return
    for i, d in enumerate(devices):
        print(f"[{i}] path={d.get('path')!r}")
        print(f"    product={d.get('product_string')!r} manufacturer={d.get('manufacturer_string')!r} serial={d.get('serial_number')!r}")
        print(f"    usage_page=0x{d.get('usage_page', 0):04x} usage=0x{d.get('usage', 0):04x} interface={d.get('interface_number')}")


def cmd_info(args):
    dev = open_device(args)
    try:
        fw = dev.exchange(CMD_GET_FIRMWARE)
        bl = dev.exchange(CMD_GET_BOOTLOADER)
        temps = dev.exchange(CMD_GET_TEMP_CONFIG)
        print(f"Firmware: {fw[1]}.{fw[2]}.{fw[3]}  raw={fw.hex(' ')}")
        print(f"Bootloader: {bl[1]}.{bl[2]}  raw={bl.hex(' ')}")
        print(f"Temp probes connected bytes: {list(temps[1:5])}  raw={temps.hex(' ')}")
        for idx, connected in enumerate(temps[1:5], start=1):
            if connected:
                temp = dev.exchange(CMD_GET_TEMP, bytes([idx - 1]))
                print(f"Temp{idx}: {be16(temp, 1) / 100:.2f} C  raw={temp.hex(' ')}")
    finally:
        dev.close()


def cmd_modes(args):
    dev = open_device(args)
    try:
        res = dev.exchange(CMD_GET_FAN_MODES)
        for i, m in enumerate(res[1:7], start=1):
            print(f"fan{i}: {mode_name(m)} ({m})")
        print(f"raw={res.hex(' ')}")
    finally:
        dev.close()


def cmd_rpm(args):
    dev = open_device(args)
    try:
        res = dev.exchange(CMD_GET_FAN_RPM, bytes([args.fan - 1]))
        print(f"fan{args.fan}: {be16(res, 1)} rpm  raw={res.hex(' ')}")
    finally:
        dev.close()


def cmd_pwm(args):
    dev = open_device(args)
    try:
        res = dev.exchange(CMD_GET_FAN_PWM, bytes([args.fan - 1]))
        status = res[0]
        if status == 0:
            print(f"fan{args.fan}: {res[1]}% PWM  raw={res.hex(' ')}")
        else:
            print(f"fan{args.fan}: error status=0x{status:02x}  raw={res.hex(' ')}")
    finally:
        dev.close()


def cmd_duty(args):
    dev = open_device(args)
    try:
        res = dev.exchange(CMD_SET_FAN_DUTY, bytes([args.fan - 1, args.percent]))
        print(f"set fan{args.fan} duty={args.percent}%  raw={res.hex(' ')}")
        time.sleep(0.1)
        rpm = dev.exchange(CMD_GET_FAN_RPM, bytes([args.fan - 1]))
        print(f"fan{args.fan}: {be16(rpm, 1)} rpm")
    finally:
        dev.close()


def cmd_mode(args):
    mode = FAN_MODE_PWM if args.mode == "pwm" else FAN_MODE_DISCONNECTED
    dev = open_device(args)
    try:
        # Commander Pro set mode payload:
        # [0x02, fan_num, mode]
        res = dev.exchange(CMD_SET_FAN_MODE, bytes([0x02, args.fan - 1, mode]))
        print(f"set fan{args.fan} mode={args.mode}  raw={res.hex(' ')}")
        modes = dev.exchange(CMD_GET_FAN_MODES)
        print(f"fan{args.fan}: {mode_name(modes[args.fan])}")
    finally:
        dev.close()


def cmd_poll(args):
    dev = open_device(args)
    try:
        if args.percent is not None:
            dev.exchange(CMD_SET_FAN_DUTY, bytes([args.fan - 1, args.percent]))
            print(f"set fan{args.fan} duty={args.percent}%")
        while True:
            res = dev.exchange(CMD_GET_FAN_RPM, bytes([args.fan - 1]))
            print(f"{time.strftime('%H:%M:%S')} fan{args.fan}: {be16(res, 1)} rpm")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("stopped")
    finally:
        dev.close()


def cmd_led_noop(args):
    dev = open_device(args)
    try:
        tests = [
            (CMD_RESET_LED_CHANNEL, bytes([args.channel])),
            (CMD_BEGIN_LED_EFFECT, bytes([args.channel])),
            (CMD_SET_LED_CHANNEL_STATE, bytes([args.channel, 1])),
            (CMD_LED_EFFECT, bytes([args.channel, 0, 0, 0, 0])),
            (CMD_LED_COMMIT, bytes([0xff])),
        ]
        for cmd, payload in tests:
            res = dev.exchange(cmd, payload)
            print(f"LED no-op cmd 0x{cmd:02x}: status={res[0]} raw={res.hex(' ')}")
    finally:
        dev.close()


def main():
    parser = argparse.ArgumentParser(description="Test CH552 Commander Pro compatible HID firmware")
    parser.add_argument("--index", type=int, default=0, help="matching HID device index")
    parser.add_argument("--verbose", "-v", action="store_true")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list matching HID devices")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("info", help="read firmware/bootloader/temp config")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("modes", help="read fan modes")
    p.set_defaults(func=cmd_modes)

    p = sub.add_parser("rpm", help="read fan RPM")
    p.add_argument("fan", type=int, choices=[1, 2])
    p.set_defaults(func=cmd_rpm)

    p = sub.add_parser("pwm", help="read current fan PWM duty percent")
    p.add_argument("fan", type=int, choices=[1, 2])
    p.set_defaults(func=cmd_pwm)

    p = sub.add_parser("duty", help="set fan duty percent")
    p.add_argument("fan", type=int, choices=[1, 2])
    p.add_argument("percent", type=int, choices=range(0, 101), metavar="0..100")
    p.set_defaults(func=cmd_duty)

    p = sub.add_parser("mode", help="set fan mode")
    p.add_argument("fan", type=int, choices=[1, 2])
    p.add_argument("mode", choices=["pwm", "off"])
    p.set_defaults(func=cmd_mode)

    p = sub.add_parser("poll", help="poll RPM repeatedly")
    p.add_argument("--fan", type=int, choices=[1, 2], default=1)
    p.add_argument("--percent", type=int, choices=range(0, 101), metavar="0..100")
    p.add_argument("--interval", type=float, default=1.0)
    p.set_defaults(func=cmd_poll)

    p = sub.add_parser("led-noop", help="exercise LED no-op compatibility commands")
    p.add_argument("--channel", type=int, default=0)
    p.set_defaults(func=cmd_led_noop)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
