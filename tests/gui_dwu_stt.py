"""Tiny tkinter GUI for testing the DWU end-to-end voice pipeline.

Click the big RECORD button -> ESP32 records 5s of audio via the INMP441 ->
ESP32 POSTs the WAV directly to whisper.cpp on okDemerzel over WiFi ->
the transcription comes back over USB serial and is appended to the GUI.

Run from the project root:
    .venv/Scripts/python tests/gui_dwu_stt.py
"""
import os
import re
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, scrolledtext

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MPREMOTE = os.path.join(PROJECT_ROOT, ".venv", "Scripts", "mpremote.exe")
SCRIPT = os.path.join(PROJECT_ROOT, "tests", "record_and_transcribe.py")
PORT = "COM4"


def run_cycle(callback):
    """Run mpremote, parse output, call callback(status, text|None) along the way."""
    callback("recording", None)
    try:
        proc = subprocess.Popen(
            [MPREMOTE, "connect", "port:" + PORT, "run", SCRIPT],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        text = None
        for line in proc.stdout:
            line = line.rstrip()
            if "POSTing to whisper" in line:
                callback("posting", None)
            elif "Whisper responded" in line:
                callback("transcribing", None)
            elif line.startswith("DWU-STT-TEXT:"):
                m = re.match(r"DWU-STT-TEXT: (.+)$", line)
                if m:
                    raw = m.group(1).strip()
                    # raw is a Python repr like "'hello world'" -- evaluate safely
                    if (raw.startswith("'") and raw.endswith("'")) or (
                        raw.startswith('"') and raw.endswith('"')
                    ):
                        text = raw[1:-1].replace("\\'", "'").replace('\\"', '"').replace("\\n", "\n")
                    else:
                        text = raw
        proc.wait(timeout=60)
        if text is None:
            callback("error", "(no transcription returned)")
        else:
            callback("done", text)
    except Exception as e:
        callback("error", "(exception: %s)" % e)


class App:
    def __init__(self, root):
        self.root = root
        root.title("DWU STT Test")
        root.geometry("700x500")

        self.busy = False

        top = ttk.Frame(root, padding=10)
        top.pack(fill="x")

        self.record_btn = tk.Button(
            top,
            text="\u23FA  RECORD 5s",
            font=("Segoe UI", 18, "bold"),
            bg="#c0392b",
            fg="white",
            activebackground="#e74c3c",
            activeforeground="white",
            relief="flat",
            padx=24,
            pady=12,
            command=self.on_record,
        )
        self.record_btn.pack(side="left")

        self.status = ttk.Label(top, text="Idle", font=("Segoe UI", 12))
        self.status.pack(side="left", padx=20)

        ttk.Label(root, text="Transcriptions", font=("Segoe UI", 11, "bold")).pack(
            anchor="w", padx=10
        )
        self.history = scrolledtext.ScrolledText(
            root, wrap="word", font=("Consolas", 11), height=20
        )
        self.history.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.history.tag_configure("ts", foreground="#888")
        self.history.tag_configure("text", foreground="#000")
        self.history.tag_configure("err", foreground="#c0392b")
        self.history.configure(state="disabled")

    def set_status(self, status, text):
        msgs = {
            "recording": ("Recording 5 s\u2026 SPEAK NOW", "#c0392b"),
            "posting": ("Uploading to whisper\u2026", "#d68910"),
            "transcribing": ("Transcribing\u2026", "#1f618d"),
            "done": ("Idle", "#2e7d32"),
            "error": ("Error", "#c0392b"),
        }
        msg, color = msgs.get(status, (status, "#000"))
        self.status.config(text=msg, foreground=color)

        if status in ("done", "error"):
            ts = time.strftime("%H:%M:%S")
            self.history.configure(state="normal")
            self.history.insert("end", "[" + ts + "]  ", "ts")
            tag = "err" if status == "error" else "text"
            self.history.insert("end", (text or "") + "\n", tag)
            self.history.see("end")
            self.history.configure(state="disabled")
            self.busy = False
            self.record_btn.config(state="normal", bg="#c0392b")

    def on_record(self):
        if self.busy:
            return
        self.busy = True
        self.record_btn.config(state="disabled", bg="#7f8c8d")

        def cb(status, text):
            self.root.after(0, self.set_status, status, text)

        threading.Thread(target=run_cycle, args=(cb,), daemon=True).start()


def main():
    if not os.path.exists(MPREMOTE):
        print("ERROR: mpremote not found at", MPREMOTE)
        print("Run: .venv/Scripts/pip install mpremote")
        sys.exit(1)
    if not os.path.exists(SCRIPT):
        print("ERROR: record_and_transcribe.py not found at", SCRIPT)
        sys.exit(1)
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
