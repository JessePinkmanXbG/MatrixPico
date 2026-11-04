#!/usr/bin/env python3
"""
Matrix Pico Console - serial tuning GUI for the Matrix Pico glitcher.

Neon cyan-grid UI with glowing green pill buttons. Auto-finds the Pico's COM
port, pick a console in one click, and drive every tuning feature from the
debug panel. Builds to a single Windows .exe (PyInstaller, assets bundled).

Run from source:  pip install pyserial pillow  &&  python matrixpico_console.py
"""

import os
import sys
import threading
import queue
import tkinter as tk
from tkinter import ttk

import serial
import serial.tools.list_ports
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageTk

RP2040_VID = 0x2E8A
WIN_W, WIN_H = 900, 720

NEON = (57, 255, 20)
CYAN = "#00f0ff"
TERM_BG = "#020612"
TERM_FG = "#39ff14"
ENTRY_BG = "#02061a"
BORDER = "#00aae6"

CONSOLES = ["trinity", "corona", "falcon", "jasper", "zephyr"]
ACTIONS = [("STATUS", "status"), ("LIST", "list"), ("MON", "mon"),
           ("SAVE", "save"), ("LOAD", "load"), ("DEFAULTS", "defaults"),
           ("HELP", "help")]


def asset(name):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, "assets", name)


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


# --------------------------------------------------------------------------
# neon pill button rendering (Pillow, cached)
# --------------------------------------------------------------------------
MARGIN = 14
_font_cache = {}
_img_cache = {}


def _font(size):
    if size not in _font_cache:
        try:
            _font_cache[size] = ImageFont.truetype(asset("Inter-ExtraBoldItalic.otf"), size)
        except Exception:
            try:
                _font_cache[size] = ImageFont.truetype("arialbi.ttf", size)
            except Exception:
                _font_cache[size] = ImageFont.load_default()
    return _font_cache[size]


def make_btn(w, h, text, state, big):
    key = (w, h, text, state, big)
    if key in _img_cache:
        return _img_cache[key]
    K = 3
    M = MARGIN
    W, H = (w + 2 * M) * K, (h + 2 * M) * K
    r = h * K // 2
    box = [M * K, M * K, (M + w) * K, (M + h) * K]
    glow_r = {"normal": 4, "hover": 7, "press": 3, "active": 9}[state]
    glow_a = {"normal": 150, "hover": 220, "press": 110, "active": 255}[state]
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(layer).rounded_rectangle(box, radius=r, outline=NEON + (glow_a,), width=2 * K)
    glow = layer.filter(ImageFilter.GaussianBlur(glow_r * K))
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    img.alpha_composite(glow)
    d = ImageDraw.Draw(img)
    if state == "active":
        fill = NEON + (255,)
    elif state == "hover":
        fill = (10, 46, 14, 255)
    elif state == "press":
        fill = (18, 70, 22, 255)
    else:
        fill = (6, 28, 10, 255)
    d.rounded_rectangle(box, radius=r, fill=fill, outline=NEON + (255,), width=int(1.8 * K))
    if state != "active":
        hl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(hl).rounded_rectangle(
            [box[0] + 8 * K, box[1] + 3 * K, box[2] - 8 * K, box[1] + (h // 2) * K - K],
            radius=(h // 4) * K, fill=(255, 255, 255, 24))
        img.alpha_composite(hl)
        d = ImageDraw.Draw(img)
    f = _font(int((17 if big else 13) * K))
    dy = K if state == "press" else 0
    col = (4, 8, 14, 255) if state == "active" else NEON + (255,)
    d.text(((M + w / 2) * K, (M + h / 2) * K + dy), text, font=f, fill=col, anchor="mm")
    out = img.resize((w + 2 * M, h + 2 * M), Image.LANCZOS)
    photo = ImageTk.PhotoImage(out)
    _img_cache[key] = photo
    return photo


class NeonButton:
    """A rounded neon button drawn as a canvas item (so glow blends with the bg)."""

    def __init__(self, canvas, x, y, w, h, text, command, big=False):
        self.c, self.x, self.y, self.w, self.h = canvas, x, y, w, h
        self.text, self.command, self.big = text, command, big
        self.active = self.hover = self.pressed = False
        self.item = canvas.create_image(x - MARGIN, y - MARGIN, anchor="nw",
                                        image=make_btn(w, h, text, "normal", big))
        for ev, fn in (("<Enter>", self._enter), ("<Leave>", self._leave),
                       ("<ButtonPress-1>", self._press), ("<ButtonRelease-1>", self._release)):
            canvas.tag_bind(self.item, ev, fn)

    def _state(self):
        if self.pressed:
            return "press"
        if self.active:
            return "active"
        return "hover" if self.hover else "normal"

    def _refresh(self):
        self.c.itemconfigure(self.item, image=make_btn(self.w, self.h, self.text, self._state(), self.big))

    def _enter(self, e):
        self.hover = True
        self.c.configure(cursor="hand2")
        self._refresh()

    def _leave(self, e):
        self.hover = self.pressed = False
        self.c.configure(cursor="")
        self._refresh()

    def _press(self, e):
        self.pressed = True
        self._refresh()

    def _release(self, e):
        was = self.pressed
        self.pressed = False
        self._refresh()
        if was and self.x <= e.x <= self.x + self.w and self.y <= e.y <= self.y + self.h:
            self.command()

    def set_active(self, flag):
        self.active = flag
        self._refresh()

    def set_text(self, text):
        self.text = text
        self._refresh()


# --------------------------------------------------------------------------
class App:
    def __init__(self, root):
        self.root = root
        root.title("MatrixPico")
        root.geometry(f"{WIN_W}x{WIN_H}")
        root.resizable(False, False)
        root.configure(bg="#020818")

        self.ser = None
        self.stop = threading.Event()
        self.rxq = queue.Queue()
        self.devices = []
        self.console_btns = {}

        self.canvas = tk.Canvas(root, width=WIN_W, height=WIN_H, highlightthickness=0, bd=0)
        self.canvas.place(x=0, y=0)
        try:
            self.bg = tk.PhotoImage(file=asset("bg.png"))
            self.canvas.create_image(0, 0, anchor="nw", image=self.bg)
        except Exception:
            self.canvas.configure(bg="#020818")

        self._style()
        self._build_connection()
        self._build_consoles()
        self._build_debug()

        self.refresh_ports(auto_connect=True)
        self.root.after(60, self.drain)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---- ttk dark styling ----------------------------------------------------
    def _style(self):
        st = ttk.Style()
        st.theme_use("clam")
        st.configure("Neon.TCombobox", fieldbackground="#04101f", background="#04101f",
                     foreground=TERM_FG, arrowcolor=CYAN, bordercolor=BORDER,
                     lightcolor="#04101f", darkcolor="#04101f", selectbackground="#04101f",
                     selectforeground=TERM_FG)
        st.map("Neon.TCombobox", fieldbackground=[("readonly", "#04101f")],
               foreground=[("readonly", TERM_FG)], selectbackground=[("readonly", "#04101f")],
               selectforeground=[("readonly", TERM_FG)])
        st.configure("Neon.Vertical.TScrollbar", background="#06304a", troughcolor=TERM_BG,
                     bordercolor=TERM_BG, arrowcolor=CYAN, lightcolor="#06304a", darkcolor="#06304a")
        st.map("Neon.Vertical.TScrollbar", background=[("active", "#0a5a80"), ("!active", "#06304a")],
               arrowcolor=[("!disabled", CYAN)])
        self.root.option_add("*TCombobox*Listbox.background", "#04101f")
        self.root.option_add("*TCombobox*Listbox.foreground", TERM_FG)
        self.root.option_add("*TCombobox*Listbox.selectBackground", "#0a4a1a")

    # ---- connection bar ------------------------------------------------------
    def _build_connection(self):
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(self.root, textvariable=self.port_var, state="readonly",
                                    style="Neon.TCombobox", font=("Consolas", 10))
        self.port_cb.place(x=78, y=119, width=250, height=24)
        NeonButton(self.canvas, 340, 118, 92, 28, "REFRESH", self.refresh_ports)
        self.connect_btn = NeonButton(self.canvas, 446, 118, 100, 28, "CONNECT", self.toggle)
        self.dot = self.canvas.create_oval(566, 125, 576, 135, fill="#ff5a5a", outline="")
        self.status_id = self.canvas.create_text(584, 130, anchor="w", text="not connected",
                                                 fill="#ff5a5a", font=("Consolas", 10, "bold"))

    def set_status(self, text, ok):
        col = "#3cff78" if ok else "#ff5a5a"
        self.canvas.itemconfigure(self.dot, fill=col)
        self.canvas.itemconfigure(self.status_id, text=text, fill=col)

    # ---- console picker ------------------------------------------------------
    def _build_consoles(self):
        for i, name in enumerate(CONSOLES):
            b = NeonButton(self.canvas, 36, 232 + i * 52, 218, 40, name.upper(),
                           lambda n=name: self.pick_console(n), big=True)
            self.console_btns[name] = b
        self.engine_id = self.canvas.create_text(36, 520, anchor="w", text="engine: --",
                                                 fill=CYAN, font=("Consolas", 10, "bold"))

    def pick_console(self, name):
        self.send("use " + name)
        self.send("status")
        for n, b in self.console_btns.items():
            b.set_active(n == name)
        self.canvas.itemconfigure(self.engine_id,
                                  text="engine: " + ("EXT_CLK" if name == "zephyr" else "RGH1.2"))

    # ---- debug section -------------------------------------------------------
    def _build_debug(self):
        NeonButton(self.canvas, 792, 168, 76, 26, "CLEAR", self.clear)
        x = 298
        for label, cmd in ACTIONS:
            w = 78 if cmd == "defaults" else 70
            NeonButton(self.canvas, x, 208, w, 28, label, lambda c=cmd: self.send(c))
            x += w + 6

        self.out = tk.Text(self.root, bg=TERM_BG, fg=TERM_FG, insertbackground=TERM_FG,
                           font=("Consolas", 10), wrap="word", relief="flat", bd=0,
                           highlightthickness=0, padx=8, pady=6)
        self.out.place(x=299, y=249, width=548, height=410)
        sb = ttk.Scrollbar(self.root, orient="vertical", command=self.out.yview,
                           style="Neon.Vertical.TScrollbar")
        sb.place(x=848, y=249, width=17, height=410)
        self.out.configure(yscrollcommand=sb.set, state="disabled")

        self.cmd_var = tk.StringVar()
        self.entry = tk.Entry(self.root, textvariable=self.cmd_var, bg=ENTRY_BG, fg=TERM_FG,
                              insertbackground=TERM_FG, font=("Consolas", 11), relief="flat",
                              bd=0, highlightthickness=0)
        self.entry.place(x=316, y=672, width=460, height=20)
        self.entry.bind("<Return>", lambda e: self.send_cmd())
        NeonButton(self.canvas, 790, 668, 76, 28, "SEND", self.send_cmd)

    # ---- serial plumbing -----------------------------------------------------
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
        self.port_cb["values"] = [f"{d}  {desc}".strip() for _, d, desc in ports]
        if self.devices:
            self.port_cb.current(0)
        if auto_connect is True and ports and ports[0][0] >= 1 and not self.ser:
            self.connect()

    def toggle(self):
        self.disconnect() if self.ser else self.connect()

    def connect(self):
        idx = self.port_cb.current()
        if idx < 0 or idx >= len(self.devices):
            self.set_status("no port selected", False)
            return
        dev = self.devices[idx]
        try:
            self.ser = serial.Serial(dev, 115200, timeout=0.1)
        except Exception as e:
            self.set_status(f"error: {e}"[:44], False)
            self.ser = None
            return
        self.stop.clear()
        threading.Thread(target=self._read_loop, daemon=True).start()
        self.set_status(f"connected {dev}", True)
        self.connect_btn.set_text("DISCONNECT")
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
        self.set_status("not connected", False)
        self.connect_btn.set_text("CONNECT")

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
            self.set_status("not connected", False)
            return
        try:
            self.ser.write((cmd + "\r\n").encode())
        except Exception as e:
            self.set_status(f"write error: {e}"[:44], False)

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
