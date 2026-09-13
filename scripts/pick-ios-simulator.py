#!/usr/bin/env python3
"""Print the UDID of a usable iOS iPhone simulator on this machine.

Why this exists rather than a `simctl create` call in the workflow:
creating a device from "first iPhone devicetype x first available iOS
runtime" is not a valid pairing in general — a newer runtime drops older
iPhone models, and `simctl create` then fails with

    An error was encountered processing the command
    (domain=com.apple.CoreSimulator.SimError, code=403): Incompatible device

Both the GitHub macOS runner and a local machine ship pre-created,
known-good device/runtime pairs, so selecting one of those avoids the
pairing question entirely.

Selection: the newest numeric iOS runtime that has an available iPhone.
Within that runtime, prefer an already booted device and use a stable
name/UDID tie-breaker, independently of the JSON object or array order.

Exits non-zero with a message on stderr when no candidate exists, so the
calling workflow step fails loudly instead of booting nothing.
"""

import json
import re
import subprocess
import sys


def main() -> int:
    try:
        raw = subprocess.run(
            ["xcrun", "simctl", "list", "devices", "available", "-j"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        data = json.loads(raw)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"error: could not list simulators: {exc}", file=sys.stderr)
        return 1

    chosen = None
    chosen_rank = None
    for runtime, devices in data.get("devices", {}).items():
        match = re.fullmatch(r"com\.apple\.CoreSimulator\.SimRuntime\.iOS-(\d+(?:-\d+)*)", runtime)
        if not match:
            continue
        version = tuple(int(part) for part in match.group(1).split("-"))
        version += (0,) * max(0, 3 - len(version))
        for dev in devices:
            if not dev.get("isAvailable"):
                continue
            if not dev.get("name", "").startswith("iPhone"):
                continue
            if not dev.get("udid"):
                continue
            rank = (version, dev.get("state") == "Booted", dev["name"], dev["udid"])
            if chosen_rank is None or rank > chosen_rank:
                chosen_rank = rank
                chosen = (runtime, dev["name"], dev["udid"])

    if chosen is None:
        print(
            "error: no available iOS iPhone simulator found. "
            "`xcrun simctl list devices available` shows nothing usable.",
            file=sys.stderr,
        )
        return 1

    runtime, name, udid = chosen
    print(f"selected {name} on {runtime}", file=sys.stderr)
    print(udid)
    return 0


if __name__ == "__main__":
    sys.exit(main())
