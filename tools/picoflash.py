"""
Flash a Pico straight from the Matrix Pico tool (no drag-and-drop).

Flow:  find a Pico in BOOTSEL mode (the RPI-RP2 drive)  ->  copy the bundled
firmware onto it  ->  wait for it to restart as a serial device  ->  send
`use <console>` + `save` so the chosen console profile is active and stored.

A blank Pico enters BOOTSEL by itself when plugged in. One already running
Matrix Pico firmware is rebooted into BOOTSEL by opening its port at 1200 baud
(the stock Pico SDK USB reset), so updates need no button press either.

This module has no GUI code so it can be tested without a display.
"""

import os
import string
import struct
import sys
import time

import serial
import serial.tools.list_ports

RP2040_VID = 0x2E8A
UF2_MAGIC0 = 0x0A324655          # "UF2\n"
UF2_MAGIC1 = 0x9E5D5157
UF2_FAMILY_RP2040 = 0xE48BFF56


class FlashError(Exception):
    pass


def firmware_dir():
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, "assets", "firmware")


def firmware_path(console):
    """Each board has its own tested binary: firmware/<console>.uf2."""
    return os.path.join(firmware_dir(), f"{console}.uf2")


def available_boards():
    try:
        return sorted(n[:-4] for n in os.listdir(firmware_dir()) if n.endswith(".uf2"))
    except OSError:
        return []


def validate_uf2(path):
    """Refuse anything that is not a well-formed RP2040 UF2 file."""
    try:
        data = open(path, "rb").read()
    except OSError as e:
        raise FlashError(f"firmware file not found ({e.strerror or e})")
    if len(data) == 0 or len(data) % 512:
        raise FlashError("firmware file is not a valid UF2 (bad size)")
    nblocks = len(data) // 512
    for i in range(nblocks):
        m0, m1, flags, addr, size, bno, nb, fam = struct.unpack_from("<8I", data, i * 512)
        end = struct.unpack_from("<I", data, i * 512 + 508)[0]
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1 or end != 0x0AB16F30:
            raise FlashError("firmware file is not a valid UF2 (bad block)")
        if (flags & 0x2000) and fam != UF2_FAMILY_RP2040:
            raise FlashError("firmware is not for an RP2040")
        if nb != nblocks or bno != i:
            raise FlashError("firmware file is not a valid UF2 (block count)")
    return data


def _candidate_mounts():
    env = os.environ.get("MATRIXPICO_BOOTSEL_DIR")
    if env:
        yield env
    if os.name == "nt":
        for letter in string.ascii_uppercase:
            yield f"{letter}:\\"
    else:
        for root in ("/media", "/run/media", "/Volumes", "/mnt"):
            if not os.path.isdir(root):
                continue
            try:
                for entry in os.listdir(root):
                    p = os.path.join(root, entry)
                    yield p
                    try:
                        for sub in os.listdir(p):
                            yield os.path.join(p, sub)
                    except OSError:
                        pass
            except OSError:
                pass


def find_bootsel_drives():
    """Return mount points that look like a Pico in BOOTSEL mode."""
    found = []
    for m in _candidate_mounts():
        info = os.path.join(m, "INFO_UF2.TXT")
        try:
            if os.path.isfile(info):
                txt = open(info, "r", errors="replace").read()
                if "RP2" in txt or "RPI-RP2" in txt:
                    found.append(m)
        except OSError:
            continue
    return found


def list_pico_ports():
    return [p.device for p in serial.tools.list_ports.comports() if p.vid == RP2040_VID]


def reboot_to_bootsel(port):
    """Opening the port at 1200 baud makes the stock Pico USB stack reboot to BOOTSEL."""
    try:
        s = serial.Serial(port, 1200)
        time.sleep(0.2)
        s.close()
    except Exception as e:
        raise FlashError(f"could not reboot {port} to BOOTSEL: {e}")


def copy_uf2(data, drive):
    """Write the UF2 to the BOOTSEL drive. The drive vanishes when the Pico
    reboots, which can make the final close() fail - that is a success."""
    dest = os.path.join(drive, "MatrixPico.uf2")
    try:
        with open(dest, "wb") as f:
            f.write(data)
            f.flush()
            try:
                os.fsync(f.fileno())
            except OSError:
                pass
    except OSError:
        time.sleep(0.5)
        if os.path.isdir(drive) and os.path.isfile(os.path.join(drive, "INFO_UF2.TXT")):
            raise FlashError("could not write to the Pico drive")
        # drive disappeared after the write: the Pico rebooted into the firmware


def _read_until(ser, needles, timeout):
    buf = ""
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk.decode("utf-8", "replace")
            if all(n in buf for n in needles):
                return buf
    return buf


def configure_console(port, console, log=lambda s: None):
    """Select the console profile on the freshly flashed Pico and store it."""
    ser = None
    for _ in range(20):                      # port may need a moment after enumeration
        try:
            ser = serial.Serial(port, 115200, timeout=0.1)
            break
        except Exception:
            time.sleep(0.25)
    if ser is None:
        raise FlashError(f"could not open {port}")
    try:
        time.sleep(0.4)
        ser.reset_input_buffer()
        ser.write(b"\r\n")
        _read_until(ser, [], 0.5)
        ser.write(f"use {console}\r\n".encode())
        got = _read_until(ser, ["active ->"], 3.0)
        log(got)
        if f"active -> {console}" not in got:
            raise FlashError(f"Pico did not accept profile '{console}'")
        ser.write(b"save\r\n")
        got = _read_until(ser, ["saved to flash"], 4.0)
        log(got)
        if "saved to flash" not in got:
            raise FlashError("Pico did not confirm saving the profile")
    finally:
        try:
            ser.close()
        except Exception:
            pass


def flash_pico(console, log=lambda s: None, status=lambda s: None,
               fw=None, wait_bootsel=15.0, wait_port=20.0):
    """Full flash flow. Raises FlashError with a plain-English message."""
    fw = fw or firmware_path(console)
    if not os.path.isfile(fw):
        raise FlashError(f"no tested firmware for {console} yet")
    status("checking firmware...")
    data = validate_uf2(fw)

    drives = find_bootsel_drives()
    if not drives:
        ports = list_pico_ports()
        if ports:
            status("rebooting Pico into update mode...")
            log(f"Pico running on {ports[0]}; rebooting to BOOTSEL\n")
            reboot_to_bootsel(ports[0])
        end = time.time() + wait_bootsel
        while time.time() < end and not drives:
            time.sleep(0.3)
            drives = find_bootsel_drives()
    if not drives:
        raise FlashError("no Pico found. Plug in a blank Pico, or hold BOOTSEL while plugging in")
    if len(drives) > 1:
        raise FlashError("more than one Pico in BOOTSEL mode - unplug the extra one")

    status(f"flashing firmware to {drives[0]} ...")
    log(f"BOOTSEL drive: {drives[0]}\n")
    copy_uf2(data, drives[0])

    status("waiting for the Pico to restart...")
    end = time.time() + wait_port
    port = None
    while time.time() < end:
        time.sleep(0.4)
        ports = list_pico_ports()
        if ports and not find_bootsel_drives():
            port = ports[0]
            break
    if not port:
        raise FlashError("flashed, but the Pico did not show up as a serial port")

    # No profile write here: each board's firmware already boots with its own
    # profile active, and writing the flash config sector changed nothing but
    # risk. The flashed Pico is byte-identical to a drag-and-drop flash.
    status(f"Pico successfully flashed ({console})")
    return port
