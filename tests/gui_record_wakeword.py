"""Interactive prompter for capturing wake-word validation clips.

Walks through a fixed plan of positives ("Yo Demerzel" in different intonations
from Dan + Erin) and hard negatives ("damsel", "Denzel", "hey dim the lights",
ambient room noise...). For each clip it shows the phrase + speaker hint, runs
tests/record_speech.py on the ESP32 via mpremote, pulls /speech.raw back to
models/wake_word_yo_demerzel_v1/recordings/{positives,negatives}/, plays it back
through Windows audio, and offers Save & Next / Re-record / Skip.

Run from the project root:
    .venv/Scripts/python tests/gui_record_wakeword.py

Skip clips you don't want (e.g. Erin's clips if she isn't around). When done,
run validate.py — the message at the end has the exact command.
"""
import os
import subprocess
import sys
import threading
import time
import tkinter as tk
import wave
from pathlib import Path
from tkinter import ttk

PROJECT_ROOT = Path(__file__).resolve().parents[1]
MPREMOTE = PROJECT_ROOT / ".venv" / "Scripts" / "mpremote.exe"
RECORD_SCRIPT = PROJECT_ROOT / "tests" / "record_wakeword.py"
DEST_ROOT = PROJECT_ROOT / "models" / "wake_word_yo_demerzel_v1" / "recordings"
TMP_PLAYBACK = PROJECT_ROOT / "tests" / ".tmp_playback.wav"
PORT = "COM4"
RUN_TIMEOUT_S = 60   # recording itself is ~6 s; allow slack for mpremote startup
CP_TIMEOUT_S = 180   # 96 KB at ~3 KB/s over mpremote raw-paste needs ~30-40 s

# (category, speaker, phrase, intonation hint, count)
PLAN = [
    ("positives", "dan",  "Yo Demerzel", "normal volume",            5),
    ("positives", "dan",  "Yo Demerzel", "quiet / under your breath", 1),
    ("positives", "dan",  "Yo Demerzel", "loud / projected",         1),
    ("positives", "dan",  "Yo Demerzel", "fast / clipped",           1),
    ("positives", "erin", "Yo Demerzel", "normal volume",            5),
    ("positives", "erin", "Yo Demerzel", "fast / clipped",           1),
    ("negatives", "dan",  "Hey, dim the lights", "normal", 2),
    ("negatives", "dan",  "Damsel",              "normal", 1),
    ("negatives", "dan",  "Denzel",              "normal", 1),
    ("negatives", "dan",  "Demerit",             "normal", 1),
    ("negatives", "dan",  "Dim the room",        "normal", 1),
    ("negatives", "dan",  "Yo, there",           "normal", 1),
    ("negatives", "dan",  "Yo mama",             "normal", 1),
    ("negatives", "_room", "(silence — say nothing)", "ambient room noise", 1),
    ("negatives", "_room", "(let TV / conversation play)", "ambient background", 1),
]


def expand_plan():
    """Expand PLAN into per-clip records, skipping ones whose .raw already exists.

    Counters still increment for skipped slots, so numbering stays stable across
    re-runs: if dan_yo_demerzel_01..03 are already on disk, the next session
    starts at _04, never re-emitting an existing file.
    """
    counters: dict[tuple, int] = {}
    out = []
    skipped = 0
    for cat, spk, phrase, hint, count in PLAN:
        for _ in range(count):
            key = (cat, spk, phrase)
            counters[key] = counters.get(key, 0) + 1
            n = counters[key]
            slug = phrase.lower()
            for ch in "()/,'\"":
                slug = slug.replace(ch, "")
            slug = slug.replace("—", "").replace(" ", "_").strip("_")[:32]
            basename = f"{spk}_{slug}_{n:02d}"
            dest = DEST_ROOT / cat / f"{basename}.raw"
            if dest.exists():
                skipped += 1
                continue
            out.append((cat, spk, phrase, hint, basename))
    return out, skipped


CLIPS, ALREADY_RECORDED = expand_plan()


def speaker_label(spk: str) -> str:
    return {"dan": "Dan", "erin": "Erin", "_room": "Room"}.get(spk, spk)


def write_wav(raw_path: Path, wav_path: Path) -> None:
    pcm = raw_path.read_bytes()
    with wave.open(str(wav_path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)


def play_raw(raw_path: Path) -> None:
    try:
        import winsound
        write_wav(raw_path, TMP_PLAYBACK)
        winsound.PlaySound(str(TMP_PLAYBACK), winsound.SND_FILENAME | winsound.SND_NODEFAULT)
    except Exception as e:
        print(f"playback error: {e}", file=sys.stderr)


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("DWU wake-word recorder")
        root.geometry("760x620")
        root.lift()
        root.attributes("-topmost", True)
        root.after(400, lambda: root.attributes("-topmost", False))
        root.focus_force()

        self.idx = 0
        self.busy = False
        self.last_dest: Path | None = None

        self.header = tk.Label(root, text="", font=("Segoe UI", 13, "bold"), fg="#444")
        self.header.pack(pady=(14, 4))

        self.phrase = tk.Label(
            root, text="", font=("Segoe UI", 30, "bold"), fg="#1f3a8a", wraplength=720, justify="center"
        )
        self.phrase.pack(pady=(8, 4))

        self.hint = tk.Label(root, text="", font=("Segoe UI", 12), fg="#666")
        self.hint.pack(pady=(0, 12))

        self.status = tk.Label(root, text="", font=("Segoe UI", 16, "bold"), fg="#000", height=2)
        self.status.pack(pady=(4, 8))

        self.btn_frame = tk.Frame(root)
        self.btn_frame.pack(pady=8)

        self.btn_record = tk.Button(
            self.btn_frame, text="\u23FA  Begin Recording", font=("Segoe UI", 13, "bold"),
            bg="#c0392b", fg="white", activebackground="#e74c3c", activeforeground="white",
            relief="flat", padx=18, pady=10, command=self.start_recording, width=20,
        )
        self.btn_save = tk.Button(
            self.btn_frame, text="\u2713  Save & Next", font=("Segoe UI", 13, "bold"),
            bg="#27ae60", fg="white", activebackground="#2ecc71", activeforeground="white",
            relief="flat", padx=18, pady=10, command=self.save_and_next, width=18,
        )
        self.btn_redo = tk.Button(
            self.btn_frame, text="\u21BB  Re-record", font=("Segoe UI", 13),
            relief="flat", padx=14, pady=10, command=self.start_recording, width=14,
        )
        self.btn_skip = tk.Button(
            self.btn_frame, text="Skip", font=("Segoe UI", 12),
            relief="flat", padx=14, pady=10, command=self.skip, width=10,
        )
        self.btn_quit = tk.Button(
            self.btn_frame, text="Quit", font=("Segoe UI", 12),
            relief="flat", padx=14, pady=10, command=root.quit, width=10,
        )

        ttk.Separator(root, orient="horizontal").pack(fill="x", padx=10, pady=(8, 4))
        tk.Label(root, text="ESP32 output", font=("Segoe UI", 10, "bold"), fg="#555").pack(anchor="w", padx=14)
        self.log = tk.Text(root, height=10, font=("Consolas", 9), bg="#f5f5f5", relief="flat")
        self.log.pack(fill="both", expand=True, padx=10, pady=(2, 10))

        if ALREADY_RECORDED:
            self._log_line(
                f"Resuming session: {ALREADY_RECORDED} clip(s) already on disk, "
                f"{len(CLIPS)} remaining. To re-record an existing clip, delete its "
                f".raw file and re-launch."
            )
        self.show_clip()

    def _set_buttons(self, mode: str) -> None:
        for b in (self.btn_record, self.btn_save, self.btn_redo, self.btn_skip, self.btn_quit):
            b.pack_forget()
        if mode == "idle":
            self.btn_record.pack(side="left", padx=4)
            self.btn_skip.pack(side="left", padx=4)
            self.btn_quit.pack(side="left", padx=4)
        elif mode == "recording":
            self.btn_record.config(state="disabled")
            self.btn_record.pack(side="left", padx=4)
            self.btn_skip.pack(side="left", padx=4)
            self.btn_quit.pack(side="left", padx=4)
            self.btn_skip.config(state="disabled")
        elif mode == "review":
            self.btn_save.pack(side="left", padx=4)
            self.btn_redo.pack(side="left", padx=4)
            self.btn_skip.pack(side="left", padx=4)
            self.btn_quit.pack(side="left", padx=4)
            self.btn_skip.config(state="normal")
        elif mode == "done":
            self.btn_quit.pack(side="left", padx=4)
        self.btn_record.config(state="normal")

    def show_clip(self) -> None:
        if self.idx >= len(CLIPS):
            self.header.config(text=f"All {len(CLIPS)} clips done")
            self.phrase.config(text="\u2713 Recording complete", fg="#27ae60")
            cmd = (
                "wsl -d Ubuntu-20.04 -e bash -lc \""
                "source ~/microwakeword/.venv/bin/activate && "
                "python /mnt/c/Users/dsm27/claude/demerzel/demerzel_wall_unit/"
                "models/wake_word_yo_demerzel_v1/validate.py\""
            )
            self.hint.config(text="Next: run validate.py")
            self.status.config(text="See log box for the command", fg="#000")
            self.log_line("")
            self.log_line("=" * 60)
            self.log_line("All clips done. Run validation:")
            self.log_line(cmd)
            self._set_buttons("done")
            return
        cat, spk, phrase, hint, basename = CLIPS[self.idx]
        self.header.config(text=f"Clip {self.idx + 1} of {len(CLIPS)}   ·   {cat.upper()}")
        self.phrase.config(text=phrase, fg="#1f3a8a" if cat == "positives" else "#7f1d1d")
        self.hint.config(text=f"Speaker: {speaker_label(spk)}   ·   {hint}   ·   file: {basename}.raw")
        self.status.config(text="Click Begin Recording when ready", fg="#000")
        self._set_buttons("idle")
        self.last_dest = None

    def start_recording(self) -> None:
        if self.busy:
            return
        self.busy = True
        self._set_buttons("recording")
        threading.Thread(target=self._record_thread, daemon=True).start()

    def _record_thread(self) -> None:
        cat, spk, phrase, hint, basename = CLIPS[self.idx]
        dest_dir = DEST_ROOT / cat
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / f"{basename}.raw"

        self._set_status("Connecting to ESP32\u2026", "#666")
        self._log_line(f"--- Clip {self.idx + 1}: {cat}/{basename}.raw ---")

        try:
            proc = subprocess.Popen(
                [str(MPREMOTE), "connect", "port:" + PORT, "run", str(RECORD_SCRIPT)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                line = line.rstrip()
                if not line:
                    continue
                self._log_line(line)
                stripped = line.strip()
                if stripped == "Recording in 3...":
                    self._set_status("3\u2026", "#d68910")
                elif stripped == "2...":
                    self._set_status("2\u2026", "#d68910")
                elif stripped == "1...":
                    self._set_status("1\u2026", "#d68910")
                elif "SPEAK NOW" in line:
                    self._set_status("\U0001F534  SPEAK NOW", "#c0392b")
                elif "Done recording" in line:
                    self._set_status("Processing\u2026", "#1f618d")
                elif line.startswith("Saved"):
                    self._set_status("Pulling file\u2026", "#1f618d")
            proc.wait(timeout=RUN_TIMEOUT_S)
            if proc.returncode != 0:
                raise RuntimeError(f"mpremote run exited with code {proc.returncode}")

            cp = subprocess.run(
                [str(MPREMOTE), "connect", "port:" + PORT, "cp", ":speech.raw", str(dest)],
                capture_output=True, text=True, timeout=CP_TIMEOUT_S,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            if cp.returncode != 0:
                raise RuntimeError(f"mpremote cp failed: {(cp.stderr or cp.stdout).strip()}")

            size = dest.stat().st_size
            self._log_line(f"Saved {dest.name} ({size:,} bytes, ~{size / 32000:.1f} s)")
            self._set_status("\u25B6 Playing back\u2026", "#1f618d")
            play_raw(dest)
            self.last_dest = dest
            self._set_status("Save it, re-record, or skip?", "#000")
            self.root.after(0, lambda: self._set_buttons("review"))
        except Exception as e:
            self._log_line(f"ERROR: {e}")
            self._set_status(f"ERROR — see log", "#c0392b")
            self.root.after(0, lambda: self._set_buttons("idle"))
        finally:
            self.busy = False

    def save_and_next(self) -> None:
        self._log_line(f"Kept {self.last_dest.name if self.last_dest else '?'}")
        self._log_line("")
        self.idx += 1
        self.show_clip()

    def skip(self) -> None:
        if self.last_dest is not None and self.last_dest.exists():
            try:
                self.last_dest.unlink()
                self._log_line(f"Discarded {self.last_dest.name}")
            except OSError:
                pass
        else:
            self._log_line(f"Skipped clip {self.idx + 1}")
        self._log_line("")
        self.idx += 1
        self.show_clip()

    def _set_status(self, text: str, color: str = "#000") -> None:
        self.root.after(0, lambda: self.status.config(text=text, fg=color))

    def _log_line(self, text: str) -> None:
        def append():
            self.log.insert("end", text + "\n")
            self.log.see("end")
        self.root.after(0, append)


def main() -> int:
    if not MPREMOTE.exists():
        print(f"ERROR: mpremote not found at {MPREMOTE}", file=sys.stderr)
        print("Run: .venv/Scripts/pip install mpremote", file=sys.stderr)
        return 2
    if not RECORD_SCRIPT.exists():
        print(f"ERROR: {RECORD_SCRIPT} not found", file=sys.stderr)
        return 2
    root = tk.Tk()
    App(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
