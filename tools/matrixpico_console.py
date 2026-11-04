#!/usr/bin/env python3
"""
Matrix Pico Console - serial tuning GUI for the Matrix Pico glitcher.

Silver-gradient UI, green terminal styling. Auto-finds the Pico's COM port,
pick a console in one click, and drive every tuning feature from the debug
panel. Builds to a single Windows .exe (PyInstaller). No external assets.

Run from source:  pip install pyserial  &&  python matrixpico_console.py
"""

import threading
import queue
import tkinter as tk
from tkinter import ttk, scrolledtext

import serial
import serial.tools.list_ports

RP2040_VID = 0x2E8A

# ---- palette --------------------------------------------------------------
SILVER_TOP   = (0xEC, 0xEE, 0xF2)
SILVER_MID   = (0xC2, 0xC6, 0xCC)
SILVER_LOW   = (0xDC, 0xDF, 0xE4)
SILVER_BOT   = (0xBC, 0xC0, 0xC6)

GREEN        = "#2ecc40"
GREEN_NEON   = "#39ff14"
GREEN_DK     = "#0a2a0a"
GREEN_BORDER = "#1f7a1f"
INK          = "#121512"
PANEL        = "#e7e9ed"
PANEL_HEAD   = "#1b1f1b"
TERM_BG      = "#050805"
TERM_FG      = "#33ff33"

CONSOLES = [
    ("TRINITY", "trinity"),
    ("CORONA",  "corona"),
    ("FALCON",  "falcon"),
    ("JASPER",  "jasper"),
    ("ZEPHYR",  "zephyr"),
]

ACTIONS = [
    ("STATUS", "status"), ("LIST", "list"), ("MON", "mon"),
    ("SAVE", "save"), ("LOAD", "load"), ("DEFAULTS", "defaults"),
    ("HELP", "help"),
]


def lerp(a, b, t):
    return tuple(int(x + (y - x) * t) for x, y in zip(a, b))


def find_ports():
    out = []
    for p in serial.tools.list_ports.comports():
        score = 0
        if p.vid == RP2040_VID:
            score = 2
        elif (p.description and ("USB Serial" in p.description or "CDC" in p.description)) \
                or "ACM" in (p.device or ""):
            score = 1
        out.append((score, p.device, p.description or ""))
    out.sort(reverse=True)
    return out


def term_button(parent, text, cmd, width=10, big=False):
    return tk.Button(
        parent, text=text, command=cmd, cursor="hand2",
        bg="#0d0d0d", fg=GREEN_NEON,
        activebackground=GREEN_DK, activeforeground="#7CFC00",
        font=("Consolas", 12 if big else 10, "bold"),
        relief="flat", bd=0, highlightthickness=1,
        highlightbackground=GREEN_BORDER, highlightcolor=GREEN_BORDER,
        padx=6, pady=6 if big else 3, width=width)


class App:
    def __init__(self, root):
        self.root = root
        root.title("MatrixPico")
        root.geometry("900x720")
        root.minsize(900, 720)
        root.configure(bg="#c9cdd3")

        self.ser = None
        self.stop = threading.Event()
        self.rxq = queue.Queue()
        self.devices = []
        self.active_console = None
        self.console_btns = {}

        # ---- gradient background (fills the window) ----
        self.bg = tk.Canvas(root, highlightthickness=0, bd=0)
        self.bg.place(x=0, y=0, relwidth=1, relheight=1)
        self.bg.bind("<Configure>", self._draw_bg)

        self._build_logo()
        self._build_connection()
        self._build_console_panel()
        self._build_debug_panel()

        self.refresh_ports(auto_connect=True)
        self.root.after(60, self.drain)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---- background + logo -------------------------------------------------
    def _draw_bg(self, event=None):
        c = self.bg
        c.delete("bgfill")
        w = c.winfo_width() or 900
        h = c.winfo_height() or 720
        for i in range(h):
            t = i / max(h - 1, 1)
            if t < 0.5:
                col = lerp(SILVER_TOP, SILVER_MID, t * 2)
            else:
                col = lerp(SILVER_LOW, SILVER_BOT, (t - 0.5) * 2)
            c.create_line(0, i, w, i, fill="#%02x%02x%02x" % col, tags="bgfill")
        # diagonal sheen
        c.create_polygon(0, 0, int(w * 0.42), 0, 0, int(h * 0.55),
                         fill="#f2f3f6", outline="", stipple="gray25", tags="bgfill")
        c.tag_lower("bgfill")

    def _build_logo(self):
        # Doom-style blocky logo: 5x7 bitmap letters drawn as beveled green
        # blocks on the canvas (font-independent, looks the same everywhere).
        c = self.bg
        glyphs = {
            "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
            "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
            "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
            "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
            "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
            "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
            "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
            "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
            "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
        }
        word = "MATRIXPICO"
        cell, gap = 9, 12
        total_w = len(word) * 5 * cell + (len(word) - 1) * gap
        x0 = (900 - total_w) // 2
        y0 = 14
        shades = ["#6bff6b", "#39ff14", "#2ecc40", "#23a834", "#17802a", "#0f5a1d", "#0a3d14"]
        for li, ch in enumerate(word):
            lx = x0 + li * (5 * cell + gap)
            for r, row in enumerate(glyphs[ch]):
                for col, bit in enumerate(row):
                    if bit != "1":
                        continue
                    x, y = lx + col * cell, y0 + r * cell
                    c.create_rectangle(x + 3, y + 3, x + cell + 3, y + cell + 3,
                                       fill="#1c2a1c", outline="")          # drop shadow
                    c.create_rectangle(x, y, x + cell, y + cell,
                                       fill=shades[r], outline="#0a3d14")  # block
                    c.create_line(x + 1, y + 1, x + cell - 1, y + 1, fill="#c8ffc8")  # top bevel
                    c.create_line(x + 1, y + 1, x + 1, y + cell - 1, fill="#9dffa0")  # left bevel
        c.create_text(450, 90, text="R G H   G L I T C H E R   C O N S O L E",
                      font=("Consolas", 10, "bold"), fill="#2c3a2c")

    # ---- connection bar ----------------------------------------------------
    def _build_connection(self):
        bar = tk.Frame(self.root, bg=PANEL, highlightthickness=1,
                       highlightbackground=GREEN_BORDER)
        bar.place(x=20, y=104, width=860, height=46)
        tk.Label(bar, text="PORT", bg=PANEL, fg=INK,
                 font=("Consolas", 10, "bold")).place(x=12, y=12)
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(bar, textvariable=self.port_var, width=26,
                                    state="readonly")
        self.port_cb.place(x=62, y=11, width=250)
        term_button(bar, "REFRESH", self.refresh_ports, width=9).place(x=324, y=7)
        self.connect_btn = term_button(bar, "CONNECT", self.toggle, width=10)
        self.connect_btn.place(x=438, y=7)
        self.status_var = tk.StringVar(value="not connected")
        tk.Label(bar, textvariable=self.status_var, bg=PANEL, fg="#8a1f1f",
                 font=("Consolas", 10, "bold")).place(x=560, y=12)

    # ---- console picker ----------------------------------------------------
    def _build_console_panel(self):
        panel = tk.Frame(self.root, bg=PANEL, highlightthickness=1,
                         highlightbackground=GREEN_BORDER)
        panel.place(x=20, y=164, width=250, height=540)
        head = tk.Frame(panel, bg=PANEL_HEAD, height=30)
        head.pack(fill="x")
        tk.Label(head, text="// CONSOLE", bg=PANEL_HEAD, fg=GREEN_NEON,
                 font=("Consolas", 12, "bold")).pack(side="left", padx=10, pady=4)

        body = tk.Frame(panel, bg=PANEL)
        body.pack(fill="both", expand=True, padx=14, pady=12)
        tk.Label(body, text="select a board:", bg=PANEL, fg="#333",
                 font=("Consolas", 9)).pack(anchor="w", pady=(0, 6))
        for label, name in CONSOLES:
            b = term_button(body, label, lambda n=name: self.pick_console(n),
                            width=16, big=True)
            b.pack(fill="x", pady=5)
            self.console_btns[name] = b

        tk.Frame(body, bg="#c7cbd1", height=2).pack(fill="x", pady=(14, 10))
        self.engine_var = tk.StringVar(value="engine: --")
        tk.Label(body, textvariable=self.engine_var, bg=PANEL, fg=GREEN_BORDER,
                 font=("Consolas", 10, "bold")).pack(anchor="w")
        tk.Label(body, text="(save after tuning)", bg=PANEL, fg="#777",
                 font=("Consolas", 8)).pack(anchor="w", pady=(2, 0))

    def pick_console(self, name):
        self.send("use " + name)
        self.send("status")
        self.active_console = name
        for n, b in self.console_btns.items():
            if n == name:
                b.configure(bg=GREEN_NEON, fg="#061006")
            else:
                b.configure(bg="#0d0d0d", fg=GREEN_NEON)
        self.engine_var.set("engine: " + ("EXT_CLK" if name == "zephyr" else "RGH1.2"))

    # ---- debug / terminal --------------------------------------------------
    def _build_debug_panel(self):
        panel = tk.Frame(self.root, bg=PANEL, highlightthickness=1,
                         highlightbackground=GREEN_BORDER)
        panel.place(x=284, y=164, width=596, height=540)
        head = tk.Frame(panel, bg=PANEL_HEAD, height=30)
        head.pack(fill="x")
        tk.Label(head, text="// DEBUG", bg=PANEL_HEAD, fg=GREEN_NEON,
                 font=("Consolas", 12, "bold")).pack(side="left", padx=10, pady=4)
        tk.Button(head, text="CLEAR", command=self.clear, cursor="hand2",
                  bg=PANEL_HEAD, fg="#9aa", activebackground="#333",
                  font=("Consolas", 9, "bold"), relief="flat", bd=0).pack(
                      side="right", padx=8)

        btns = tk.Frame(panel, bg=PANEL)
        btns.pack(fill="x", padx=12, pady=(10, 6))
        for i, (label, cmd) in enumerate(ACTIONS):
            term_button(btns, label, lambda c=cmd: self.send(c), width=8).grid(
                row=0, column=i, padx=2)

        self.out = scrolledtext.ScrolledText(
            panel, bg=TERM_BG, fg=TERM_FG, insertbackground=TERM_FG,
            font=("Consolas", 10), wrap="word", relief="flat", bd=0,
            highlightthickness=1, highlightbackground=GREEN_BORDER)
        self.out.pack(fill="both", expand=True, padx=12, pady=6)
        self.out.configure(state="disabled")

        cmd = tk.Frame(panel, bg=PANEL)
        cmd.pack(fill="x", padx=12, pady=(0, 12))
        tk.Label(cmd, text=">", bg=PANEL, fg=GREEN_BORDER,
                 font=("Consolas", 13, "bold")).pack(side="left")
        self.cmd_var = tk.StringVar()
        self.entry = tk.Entry(cmd, textvariable=self.cmd_var, bg=TERM_BG,
                              fg=TERM_FG, insertbackground=TERM_FG,
                              font=("Consolas", 11), relief="flat",
                              highlightthickness=1, highlightbackground=GREEN_BORDER)
        self.entry.pack(side="left", fill="x", expand=True, padx=6, ipady=4)
        self.entry.bind("<Return>", lambda e: self.send_cmd())
        term_button(cmd, "SEND", self.send_cmd, width=7).pack(side="left")

    # ---- serial plumbing ---------------------------------------------------
    def log(self, text):
        self.out.configure(state="normal")
        self.out.insert("end", text)
        self.out.see("end")
        self.out.configure(state="disabled")

    def clear(self):
        self.out.configure(state="normal")
        self.out.delete("1.0", "end")
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
        self.connect_btn.configure(text="DISCONNECT")
        self.log(f"\n--- connected to {dev} ---\n")
        self.send("")

    def disconnect(self):
        self.stop.set()
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.status_var.set("not connected")
        self.connect_btn.configure(text="CONNECT")

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
        self.root.after(60, self.drain)

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
