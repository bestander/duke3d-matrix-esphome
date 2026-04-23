#!/usr/bin/env python3
"""find_ble_gamepad.py — Scan for BLE gamepads on macOS and configure esphome.yaml.

Scans for BLE devices, highlights HID controllers (service 0x1812), shows their
service UUIDs, then writes the chosen UUID into the ble_gamepad_uuid field in
esphome.yaml so the ESP32 can find it.

Usage:
  pip3 install bleak
  python3 tools/find_ble_gamepad.py [--scan-time N]
"""
import argparse
import asyncio
import re
import sys
from pathlib import Path

try:
    from bleak import BleakScanner
    from bleak.backends.device import BLEDevice
    from bleak.backends.scanner import AdvertisementData
except ImportError:
    print("bleak not installed.  Run:  pip3 install bleak", file=sys.stderr)
    sys.exit(1)

HID_SERVICE = "00001812-0000-1000-8000-00805f9b34fb"
YAML_PATH = Path(__file__).parent.parent / "esphome.yaml"

# Standard BT-SIG 128-bit UUID pattern: 0000XXXX-0000-1000-8000-00805F9B34FB
_SIG_RE = re.compile(r"^0000[0-9a-f]{4}-0000-1000-8000-00805f9b34fb$", re.I)


def _is_sig(uuid: str) -> bool:
    return bool(_SIG_RE.match(uuid))


def _custom_uuids(adv: AdvertisementData) -> list[str]:
    return [u.upper() for u in adv.service_uuids if not _is_sig(u)]


def _has_hid(adv: AdvertisementData) -> bool:
    return any(u.lower() == HID_SERVICE for u in adv.service_uuids)


async def _scan(seconds: int) -> list[tuple[BLEDevice, AdvertisementData]]:
    seen: dict[str, tuple[BLEDevice, AdvertisementData]] = {}

    def _cb(dev: BLEDevice, adv: AdvertisementData) -> None:
        seen[dev.address] = (dev, adv)

    scanner = BleakScanner(_cb)
    print(f"Scanning for {seconds}s — press buttons on your controller to make it visible…\n")
    await scanner.start()
    await asyncio.sleep(seconds)
    await scanner.stop()
    return list(seen.values())


def _print_device(idx: int, dev: BLEDevice, adv: AdvertisementData) -> None:
    hid = _has_hid(adv)
    custom = _custom_uuids(adv)
    tag = " ← HID" if hid else ""
    name = adv.local_name or dev.name or "(unnamed)"
    rssi = f"{adv.rssi} dBm" if adv.rssi else "?"
    print(f"  [{idx}] {name}{tag}  ({rssi})")
    print(f"       addr : {dev.address}")
    if adv.service_uuids:
        for u in adv.service_uuids:
            marker = "  ← custom 128-bit" if not _is_sig(u) else ""
            print(f"       uuid : {u.upper()}{marker}")
    else:
        print("       uuid : (none advertised)")
    if custom:
        print(f"       → firmware can match on: {custom[0]}")
    print()


def _update_yaml(uuid: str) -> bool:
    if not YAML_PATH.exists():
        print(f"esphome.yaml not found at {YAML_PATH}", file=sys.stderr)
        return False
    text = YAML_PATH.read_text()
    new_text, n = re.subn(
        r'(ble_gamepad_uuid\s*:\s*")[^"]*(")',
        rf'\g<1>{uuid}\g<2>',
        text,
    )
    if n == 0:
        print("ble_gamepad_uuid key not found in esphome.yaml", file=sys.stderr)
        return False
    YAML_PATH.write_text(new_text)
    print(f"Updated {YAML_PATH}  →  ble_gamepad_uuid: \"{uuid}\"")
    return True


async def _main(scan_time: int) -> None:
    devices = await _scan(scan_time)

    if not devices:
        print("No BLE devices found. Ensure Bluetooth is on and the controller is advertising.")
        return

    hid_devs = [(d, a) for d, a in devices if _has_hid(a)]
    other_devs = [(d, a) for d, a in devices if not _has_hid(a)]

    print(f"Found {len(devices)} device(s). HID controllers ({len(hid_devs)}):\n")
    idx = 1
    numbered: list[tuple[BLEDevice, AdvertisementData]] = []
    for dev, adv in sorted(hid_devs, key=lambda x: -(x[1].rssi or -999)):
        _print_device(idx, dev, adv)
        numbered.append((dev, adv))
        idx += 1

    if other_devs:
        print(f"Other devices ({len(other_devs)}):\n")
        for dev, adv in sorted(other_devs, key=lambda x: -(x[1].rssi or -999)):
            _print_device(idx, dev, adv)
            numbered.append((dev, adv))
            idx += 1

    if not numbered:
        return

    print("Enter number to select a device (or q to quit): ", end="", flush=True)
    choice = input().strip()
    if choice.lower() == "q" or not choice.isdigit():
        return
    sel = int(choice) - 1
    if sel < 0 or sel >= len(numbered):
        print("Invalid selection.")
        return

    dev, adv = numbered[sel]
    custom = _custom_uuids(adv)

    if custom:
        uuid = custom[0]
        print(f"\nSelected: {adv.local_name or dev.name or dev.address}")
        print(f"UUID to use: {uuid}\n")
        print("Update esphome.yaml now? [y/N]: ", end="", flush=True)
        if input().strip().lower() == "y":
            if _update_yaml(uuid):
                print("\nNext step: esphome run esphome.yaml  (firmware reflash needed)")
    else:
        name = adv.local_name or dev.name or dev.address
        print(f"\n{name} advertises only standard Bluetooth SIG UUIDs (e.g. 0x1812 HID).")
        print("The ESP32 firmware matches on 128-bit custom UUIDs in the advertisement.")
        print("No custom UUID found — the firmware cannot identify this device by UUID.")
        print("\nOptions:")
        print("  1. Check if the controller has a companion app that reveals its custom UUID.")
        print("  2. Modify nimble_gamepad.cpp to also match by device name:")
        print(f'     target name would be: "{adv.local_name or dev.name}"')
        print("  3. Try scanning with --scan-time 20 and press buttons on the controller.")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--scan-time", type=int, default=10, metavar="N",
                   help="seconds to scan (default: 10)")
    args = p.parse_args()
    asyncio.run(_main(args.scan_time))


if __name__ == "__main__":
    main()
