#!/usr/bin/env python3
"""
Matrix Pico Console - a tiny serial terminal for the Matrix Pico glitcher.

It auto-finds the Pico's USB serial port, shows everything the firmware prints,
and lets you type tuning commands (set / save / status / use ...). Built as a
single Windows .exe so there's nothing to install.

Run from source:  pip install pyserial  &&  python matrixpico_console.py
"""

import threading
import queue
import tkinter as tk
from tkinter import ttk, scrolledtext

import serial
import serial.tools.list_ports

RP2040_VID = 0x2E8A  # Raspberry Pi (RP2040 USB CDC)


def find_ports():
    """Return [(score, device, description)] best-match first."""
    found = []
    for p in serial.tools.list_ports.comports():
        score = 0
        if p.vid == RP2040_VID:
            score = 2
        elif (p.description and ("USB Serial" in p.description or "CDC" in p.description)) \
                or "ACM" in (p.device or ""):
            score = 1
        found.append((score, p.device, p.description or ""))
    found.sort(reverse=True)
    return found


class App:
    def __init__(self, root):
        self.root = root
        root.title("Matrix Pico Console")
        root.geometry("760x520")

        self.ser = None
        self.stop = threading.Event()
        self.rxq = queue.Queue()
        self.devices = []

        top = ttk.Frame(root, padding=6)
        top.pack(fill="x")
        ttk.Label(top, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(top, textvariable=self.port_var, width=34, state="readonly")
        self.port_cb.pack(side="left", padx=4)
        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(side="left")
        self.connect_btn = ttk.Button(top, text="Connect", command=self.toggle)
        self.connect_btn.pack(side="left", padx=4)
        self.status_var = tk.StringVar(value="not connected")
        ttk.Label(top, textvariable=self.status_var).pack(side="left", padx=8)

        self.out = scrolledtext.ScrolledText(
            root, bg="black", fg="#d8d8d8", insertbackground="#d8d8d8",
            font=("Consolas", 10), wrap="word")
        self.out.pack(fill="both", expand=True, padx=6)
        self.out.configure(state="disabled")

        bottom = ttk.Frame(root, padding=6)
        bottom.pack(fill="x")
        self.cmd_var = tk.StringVar()
        self.entry = ttk.Entry(bottom, textvariable=self.cmd_var)
        self.entry.pack(side="left", fill="x", expand=True)
        self.entry.bind("<Return>", lambda e: self.send_cmd())
        ttk.Button(bottom, text="Send", command=self.send_cmd).pack(side="left", padx=4)

        quick = ttk.Frame(root, padding=(6, 0, 6, 6))
        quick.pack(fill="x")
        ttk.Label(quick, text="quick:").pack(side="left")
        for label, cmd in [("status", "status"), ("list", "list"),
                           ("save", "save"), ("help", "help")]:
            ttk.Button(quick, text=label,
                       command=lambda c=cmd: self.send(c)).pack(side="left", padx=2)

        self.refresh_ports(auto_connect=True)
        self.root.after(50, self.drain)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def log(self, text):
        self.out.configure(state="normal")
        self.out.insert("end", text)
        self.out.see("end")
        self.out.configure(state="disabled")

    def refresh_ports(self, auto_connect=False):
        ports = find_ports()
        self.devices = [d for _, d, _ in ports]
        self.port_cb["values"] = [f"{d}   {desc}".strip() for _, d, desc in ports]
        if self.devices:
            self.port_cb.current(0)
        if auto_connect and ports and ports[0][0] >= 1:
            self.connect()

    def toggle(self):
        self.disconnect() if self.ser else self.connect()

    def connect(self):
        idx = self.port_cb.current()
        if idx < 0 or idx >= len(self.devices):
            self.status_var.set("no port selected")
            return
        dev = self.devices[idx]
        try:
            self.ser = serial.Serial(dev, 115200, timeout=0.1)
        except Exception as e:
            self.status_var.set(f"error: {e}")
            self.ser = None
            return
        self.stop.clear()
        threading.Thread(target=self._read_loop, daemon=True).start()
        self.status_var.set(f"connected {dev}")
        self.connect_btn.config(text="Disconnect")
        self.log(f"\n--- connected to {dev} ---\n")
        self.send("")  # nudge the firmware to show its prompt

    def disconnect(self):
        self.stop.set()
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.status_var.set("not connected")
        self.connect_btn.config(text="Connect")

    def _read_loop(self):
        while not self.stop.is_set() and self.ser:
            try:
                data = self.ser.read(256)
                if data:
                    self.rxq.put(data.decode("utf-8", "replace"))
            except Exception as e:
                self.rxq.put(f"\n[read error: {e}]\n")
                break

    def drain(self):
        try:
            while True:
                self.log(self.rxq.get_nowait())
        except queue.Empty:
            pass
        self.root.after(50, self.drain)

    def send(self, cmd):
        if not self.ser:
            self.status_var.set("not connected")
            return
        try:
            self.ser.write((cmd + "\r\n").encode())
        except Exception as e:
            self.status_var.set(f"write error: {e}")

    def send_cmd(self):
        self.send(self.cmd_var.get())
        self.cmd_var.set("")

    def on_close(self):
        self.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
