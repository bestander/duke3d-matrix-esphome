#!/usr/bin/env python3
"""find_ble_gamepad.py — Scan for BLE gamepads on macOS and configure esphome.yaml.

scan mode (default):
  Scans for BLE devices, highlights HID controllers (service 0x1812), shows their
  service UUIDs, then writes the chosen UUID into esphome.yaml.

map mode (--map):
  Connects to the controller and runs a guided session to map each game action to
  HID report bytes/bits.  Prints a summary you can paste into ble_gamepad.cpp.

Usage:
  pip3 install bleak
  python3 tools/find_ble_gamepad.py [--scan-time N]
  python3 tools/find_ble_gamepad.py --map [--name "ShanWan Q36"] [--scan-time N]
"""
import argparse
import asyncio
import json
import re
import sys
from pathlib import Path

try:
    from bleak import BleakScanner, BleakClient
    from bleak.backends.device import BLEDevice
    from bleak.backends.scanner import AdvertisementData
    from bleak.backends.characteristic import BleakGATTCharacteristic
except ImportError:
    print("bleak not installed.  Run:  pip3 install bleak", file=sys.stderr)
    sys.exit(1)

HID_SERVICE    = "00001812-0000-1000-8000-00805f9b34fb"
HID_REPORT_CHR = "00002a4d-0000-1000-8000-00805f9b34fb"
YAML_PATH      = Path(__file__).parent.parent / "esphome.yaml"
MAP_PATH       = Path(__file__).parent / "gamepad_map.json"

_SIG_RE = re.compile(r"^0000[0-9a-f]{4}-0000-1000-8000-00805f9b34fb$", re.I)

# Game actions that ble_gamepad.cpp needs, in the order we prompt for them.
GAME_ACTIONS = [
    ("forward",      "Left stick UP  (or D-pad Up)"),
    ("back",         "Left stick DOWN (or D-pad Down)"),
    ("turn_left",    "Left stick LEFT (or D-pad Left)"),
    ("turn_right",   "Left stick RIGHT (or D-pad Right)"),
    ("strafe_left",  "LB / L1 (left shoulder)"),
    ("strafe_right", "RB / R1 (right shoulder)"),
    ("fire",         "A / Cross (bottom face button)"),
    ("use",          "B / Circle (right face button)"),
    ("open_map",     "X / Square (left face button)"),
    ("menu",         "Start / Menu"),
]


# ── helpers ────────────────────────────────────────────────────────────────────

def _is_sig(uuid: str) -> bool:
    return bool(_SIG_RE.match(uuid))


def _custom_uuids(adv: AdvertisementData) -> list[str]:
    return [u.upper() for u in adv.service_uuids if not _is_sig(u)]


def _has_hid(adv: AdvertisementData) -> bool:
    return any(u.lower() == HID_SERVICE for u in adv.service_uuids)


def _fmt_byte(val: int, changed: bool) -> str:
    s = f"{val:02X}"
    return f"\033[1;33m{s}\033[0m" if changed else s


def _fmt_bits(val: int) -> str:
    return "".join("1" if val & (1 << (7 - i)) else "." for i in range(8))


def _diff_desc(idle: bytes, report: bytes) -> str:
    """One-line summary of what changed from idle."""
    parts = []
    for i in range(min(len(idle), len(report))):
        if idle[i] != report[i]:
            diff = idle[i] ^ report[i]
            bits = [b for b in range(8) if diff & (1 << b)]
            set_bits  = [b for b in bits if report[i] & (1 << b)]
            clr_bits  = [b for b in bits if not (report[i] & (1 << b))]
            desc = f"[{i}] {idle[i]:02X}→{report[i]:02X}"
            if set_bits:
                desc += f" set:{','.join(map(str,set_bits))}"
            if clr_bits:
                desc += f" clr:{','.join(map(str,clr_bits))}"
            parts.append(desc)
    return "  ".join(parts) if parts else "(no change)"


def _print_report_row(report: bytes, idle: bytes | None) -> None:
    n = len(report)
    changed = [idle[i] != report[i] for i in range(n)] if idle and len(idle) == n else [False]*n
    hex_row = " ".join(_fmt_byte(report[i], changed[i]) for i in range(n))
    print(f"  {hex_row}")
    for i in range(n):
        if changed[i]:
            old = idle[i] if idle else 0
            diff = old ^ report[i]
            bits = [b for b in range(8) if diff & (1 << b)]
            set_b = [b for b in bits if report[i] & (1 << b)]
            clr_b = [b for b in bits if not (report[i] & (1 << b))]
            parts = []
            if set_b: parts.append(f"set bits {set_b}")
            if clr_b: parts.append(f"clr bits {clr_b}")
            print(f"    byte[{i}]  {old:02X}({_fmt_bits(old)}) → {report[i]:02X}({_fmt_bits(report[i])})  {' '.join(parts)}")


# ── scan mode ──────────────────────────────────────────────────────────────────

async def _do_scan(seconds: int) -> list[tuple[BLEDevice, AdvertisementData]]:
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


async def _scan_main(scan_time: int) -> None:
    devices = await _do_scan(scan_time)
    if not devices:
        print("No BLE devices found.")
        return

    hid_devs   = [(d, a) for d, a in devices if _has_hid(a)]
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


# ── map mode ───────────────────────────────────────────────────────────────────

async def _find_device(name_filter: str | None, scan_time: int) -> BLEDevice | None:
    # Check if already connected to macOS (paired device may not advertise).
    if name_filter:
        connected = await BleakScanner.find_device_by_filter(
            lambda d, _a: (d.name or "").lower() == name_filter.lower(),
            timeout=2.0,
        )
        if connected:
            print(f"Found (already connected): {connected.name}  ({connected.address})")
            return connected

    print(f"Scanning {scan_time}s for BLE HID devices…")
    seen: dict[str, tuple[BLEDevice, AdvertisementData]] = {}

    def _cb(dev: BLEDevice, adv: AdvertisementData) -> None:
        seen[dev.address] = (dev, adv)

    async with BleakScanner(_cb):
        await asyncio.sleep(scan_time)

    candidates = [(d, a) for d, a in seen.values() if _has_hid(a)]
    if name_filter:
        named = [(d, a) for d, a in candidates
                 if (a.local_name or d.name or "").lower() == name_filter.lower()]
        if named:
            candidates = named

    if not candidates and name_filter:
        candidates = [(d, a) for d, a in seen.values()
                      if (a.local_name or d.name or "").lower() == name_filter.lower()]

    if not candidates:
        print("No matching device found.")
        return None
    if len(candidates) == 1:
        dev, adv = candidates[0]
        print(f"Found: {adv.local_name or dev.name}  ({dev.address})")
        return dev

    print("Multiple candidates:")
    for i, (d, a) in enumerate(candidates, 1):
        print(f"  [{i}] {a.local_name or d.name or '(unnamed)'}  {d.address}")
    print("Select [1]: ", end="", flush=True)
    raw = input().strip()
    idx = (int(raw) - 1) if raw.isdigit() else 0
    return candidates[idx][0]


async def _map_main(device_name: str | None, scan_time: int) -> None:
    dev = await _find_device(device_name, scan_time)
    if dev is None:
        return

    loop = asyncio.get_event_loop()
    report_queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=64)
    latest: list[bytes] = [b""]

    def _notify(char: BleakGATTCharacteristic, data: bytearray, label: str = "") -> None:
        b = bytes(data)
        latest[0] = b
        try:
            report_queue.put_nowait((label, b))
        except asyncio.QueueFull:
            pass

    print(f"Connecting to {dev.name or dev.address}…")
    client = BleakClient(dev)
    try:
        await client.connect()
        print("Connected!\n")

        # Dump all services and characteristics for diagnostics
        print("GATT services discovered:")
        for svc in client.services:
            print(f"  Service {svc.uuid}  ({svc.description})")
            for char in svc.characteristics:
                print(f"    Char {char.uuid}  handle={char.handle}  props={char.properties}")

        print()

        # Subscribe to every notifiable/indicatable characteristic across all services.
        # Cheap controllers often use vendor-specific UUIDs instead of standard 0x2A4D.
        subscribed = 0
        char_labels: dict[int, str] = {}  # handle → short label for display
        for svc in client.services:
            for char in svc.characteristics:
                props = char.properties
                if "notify" not in props and "indicate" not in props:
                    continue
                label = f"{char.uuid[:8]}(h{char.handle})"
                try:
                    await client.start_notify(char, lambda c, d, lbl=label: _notify(c, d, lbl))
                    char_labels[char.handle] = label
                    print(f"  Subscribed to {char.uuid}  handle={char.handle}  [{svc.description}]")
                    subscribed += 1
                except Exception as e:
                    print(f"  Warning: handle {char.handle}: {e}")

        if subscribed == 0:
            print("No notifiable characteristics found at all.")
            return
        print(f"\nSubscribed to {subscribed} characteristic(s).\n")

        # ── Capture baseline ────────────────────────────────────────────────
        print("Keep controller IDLE (no buttons) for 3 seconds to capture baseline…")
        while not report_queue.empty():
            report_queue.get_nowait()
        await asyncio.sleep(3.0)
        idle = latest[0]
        if not idle:
            print("No HID reports received — is the controller awake?")
            return

        n_bytes = len(idle)
        header  = "  " + " ".join(f"{i:>2}" for i in range(n_bytes))
        print(f"\nBaseline ({n_bytes} bytes):")
        print(header)
        print("  " + " ".join(f"{b:02X}" for b in idle))

        # ── Live view ───────────────────────────────────────────────────────
        print("\n" + "─" * 60)
        print("LIVE VIEW  (Ctrl+C to move to guided mapping)")
        print("─" * 60)
        print(header)

        live_task_running = True
        last_printed = idle

        async def _live():
            nonlocal last_printed
            while live_task_running:
                try:
                    lbl, report = await asyncio.wait_for(report_queue.get(), timeout=0.15)
                except asyncio.TimeoutError:
                    continue
                if report == last_printed:
                    continue
                last_printed = report
                diff = _diff_desc(idle, report)
                row = " ".join(_fmt_byte(report[i], idle[i] != report[i]) for i in range(len(report)))
                print(f"  [{lbl}]  {row}   {diff}")

        live_task = asyncio.ensure_future(_live())
        try:
            await asyncio.get_event_loop().run_in_executor(None, input)  # wait for Enter
        except KeyboardInterrupt:
            pass
        finally:
            live_task_running = False
            live_task.cancel()
            try:
                await live_task
            except (asyncio.CancelledError, Exception):
                pass

        # ── Guided mapping ──────────────────────────────────────────────────
        print("\n" + "═" * 60)
        print("GUIDED MAPPING  —  map each game action to HID bytes/bits")
        print("═" * 60)
        print("For each action: perform it, hold it, press Enter to capture.")
        print("Press Enter without performing the action to skip.\n")

        mapping: dict[str, dict] = {}

        for action_key, action_desc in GAME_ACTIONS:
            print(f"→ \033[1m{action_desc}\033[0m")
            print(f"  Perform and HOLD, then press Enter (or just Enter to skip): ", end="", flush=True)

            while not report_queue.empty():
                report_queue.get_nowait()

            await loop.run_in_executor(None, sys.stdin.readline)
            await asyncio.sleep(0.05)  # let final notify flush

            cap = latest[0]
            if not cap or cap == idle:
                print("  (no change — skipped)\n")
                continue

            # Collect diffs
            diffs = []
            for i in range(min(n_bytes, len(cap))):
                if cap[i] == idle[i]:
                    continue
                diff_bits = cap[i] ^ idle[i]
                # Axis: no individual bits, just a value change
                if bin(diff_bits).count("1") > 3:
                    diffs.append({"type": "axis", "byte": i,
                                  "idle_val": idle[i], "pressed_val": cap[i]})
                else:
                    for bit in range(8):
                        if diff_bits & (1 << bit):
                            diffs.append({"type": "button", "byte": i, "bit": bit,
                                          "idle_val": idle[i], "pressed_val": cap[i],
                                          "set": bool(cap[i] & (1 << bit))})

            if not diffs:
                print("  (no bit/byte change detected — skipped)\n")
                continue

            print(f"  Detected:")
            for d in diffs:
                if d["type"] == "axis":
                    print(f"    byte[{d['byte']}]  axis  {d['idle_val']:02X} → {d['pressed_val']:02X}"
                          f"  (range 0x00–0xFF, center ~0x7F)")
                else:
                    state = "SET" if d["set"] else "CLEARED"
                    print(f"    byte[{d['byte']}]  bit {d['bit']}  {d['idle_val']:02X} → {d['pressed_val']:02X}  {state}")

            mapping[action_key] = {
                "description": action_desc,
                "diffs": diffs,
                "idle_report":    list(idle),
                "pressed_report": list(cap),
            }
            print(f"  \033[32m✓ {action_key} mapped\033[0m\n")

        # ── Summary ─────────────────────────────────────────────────────────
        if not mapping:
            print("\nNo actions mapped.")
            return

        print("\n" + "═" * 60)
        print(f"MAPPING COMPLETE  —  {len(mapping)} action(s)")
        print("═" * 60)

        print("\nble_gamepad.cpp parse_report() layout:")
        print(f"  (baseline report: " + " ".join(f"{b:02X}" for b in idle) + ")\n")

        for action_key, info in mapping.items():
            for d in info["diffs"]:
                if d["type"] == "axis":
                    print(f"  {action_key:15s}  byte[{d['byte']}] axis  "
                          f"(lo<0x{64:02X} = one dir, hi>0x{192:02X} = other dir)")
                else:
                    print(f"  {action_key:15s}  byte[{d['byte']}] bit {d['bit']}  "
                          f"(data[o+{d['byte']}] & (1 << {d['bit']}))")

        MAP_PATH.write_text(json.dumps(mapping, indent=2))
        print(f"\nFull mapping saved to {MAP_PATH}")
        print("\nIf the layout differs from the current ble_gamepad.cpp, update parse_report().")
    finally:
        if client.is_connected:
            print("\nDisconnecting…")
            await client.disconnect()
            print("Disconnected.")


# ── main ───────────────────────────────────────────────────────────────────────

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--map", action="store_true",
                   help="Connect and interactively map buttons (map mode)")
    p.add_argument("--name", metavar="NAME", default="ShanWan Q36",
                   help="Device name to target in map mode (default: ShanWan Q36)")
    p.add_argument("--scan-time", type=int, default=10, metavar="N",
                   help="Seconds to scan (default: 10)")
    args = p.parse_args()

    if args.map:
        asyncio.run(_map_main(args.name, args.scan_time))
    else:
        asyncio.run(_scan_main(args.scan_time))


if __name__ == "__main__":
    main()
