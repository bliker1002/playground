#!/usr/bin/env python3
"""
Quick protocol check for a Dragonfly spectrometer from a Mac / Windows / Linux computer.

Confirms that the device speaks the TI NIRscan Nano USB-HID protocol this app relies on, before
you go through the Xcode / iPad setup. It reads the firmware version and stored scan configs,
runs one scan and saves the raw scan blob.

    pip install hidapi
    python3 df_check.py            # (Linux: may need sudo or a udev rule)
"""
import struct
import sys
import time

import hid  # pip install hidapi

VID, PID = 0x0451, 0x4200


class Dragonfly:
    def __init__(self):
        self.d = hid.device()
        self.d.open(VID, PID)
        self.seq = 0

    def _send(self, msg):
        for off in range(0, len(msg), 64):
            chunk = msg[off:off + 64]
            self.d.write([0x00] + chunk + [0] * (64 - len(chunk)))  # leading 0 = report ID

    def _recv(self, timeout_ms=3000):
        pkt = self.d.read(64, timeout_ms)
        if not pkt:
            raise TimeoutError("no response")
        resp = (pkt[0] >> 4) & 3
        length = pkt[2] | pkt[3] << 8
        if resp == 2:
            raise BlockingIOError("busy")
        if resp == 1:
            raise IOError("device NACK")
        data = list(pkt[4:4 + min(length, 60)])
        while len(data) < length:
            pkt = self.d.read(64, timeout_ms)
            if not pkt:
                raise TimeoutError("truncated response")
            data += pkt[:length - len(data)]
        return bytes(data)

    def read(self, group, cmd, param=None):
        body = [cmd, group] + (list(struct.pack("<I", param)) if param is not None else [])
        self._send([0xC0, 0, len(body), 0] + body)
        data = self._recv()
        if not data:
            raise IOError("device NACK (empty reply)")
        return data

    def write(self, group, cmd, data=b""):
        body = [cmd, group] + list(data)
        self._send([0x40, self.seq & 0xFF, len(body) & 0xFF, len(body) >> 8] + body)
        self.seq += 1
        return self._recv()


def ver(v):
    return f"{(v >> 16) & 255}.{(v >> 8) & 255}.{v & 255}"


def main():
    dev = Dragonfly()
    print("Opened", dev.d.get_manufacturer_string(), "/", dev.d.get_product_string())

    v = struct.unpack("<7I", dev.read(0x02, 0x16)[:28])
    print("Tiva firmware", ver(v[0]), "| DLPC", ver(v[1]), "| spectrum lib", ver(v[3]))
    try:
        print("Serial number:", dev.read(0x02, 0x33)[:8].split(b"\0")[0].decode(errors="replace"))
    except Exception as e:  # not on all firmware
        print("Serial number: n/a", e)

    n = dev.read(0x02, 0x22)[0]
    active = dev.read(0x02, 0x23)[0]
    print(f"{n} stored scan configs, active = {active}")

    est = struct.unpack("<I", dev.read(0x02, 0x37)[:4])[0]
    print(f"Starting scan (estimated {est} ms)...")
    dev.write(0x02, 0x18, b"\x00")
    t0 = time.time()
    while True:
        time.sleep(0.05)
        try:
            status = struct.unpack("<I", dev.read(0x04, 0x03)[:4])[0]
        except BlockingIOError:
            continue
        if not status & 0x02:
            break
        if time.time() - t0 > est / 1000 * 3 + 10:
            sys.exit("scan timed out")
    size = struct.unpack("<I", dev.read(0x00, 0x2D, 0)[:4])[0]
    blob = b""
    while len(blob) < size:
        blob += dev.read(0x00, 0x2E)
    blob = blob[:size]
    with open("scan_blob.bin", "wb") as f:
        f.write(blob)
    print(f"Scan done in {time.time() - t0:.1f} s, downloaded {size} bytes -> scan_blob.bin")
    print("Blob starts with", blob[:3], "(expected b'tpl' - TI spectrum-library format)")
    print("Protocol check OK - the iPad app should work with this device.")


if __name__ == "__main__":
    main()
