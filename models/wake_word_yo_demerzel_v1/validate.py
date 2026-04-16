"""Hardware-validation harness for the "Yo Demerzel" v1 micro-wake-word model.

Loads the quantized streaming .tflite, runs it over a directory of positive
recordings (should fire) and a directory of negative recordings (should NOT fire),
and sweeps decision cutoffs to report recall and false-positive count at each.

Audio format expected: 16 kHz, 16-bit, mono. Both .wav and .raw (headerless PCM)
are accepted — DWU's tests/record_speech.py drops a .raw, so this saves a
conversion step.

Must run inside the WSL ~/microwakeword/.venv (mww + ai_edge_litert installed).
Typical invocation from Windows:

    wsl -d Ubuntu-20.04 -e bash -lc \
      "source ~/microwakeword/.venv/bin/activate && \
       python /mnt/c/Users/dsm27/claude/demerzel/demerzel_wall_unit/models/wake_word_yo_demerzel_v1/validate.py \
         --positives /mnt/c/Users/dsm27/claude/demerzel/demerzel_wall_unit/models/wake_word_yo_demerzel_v1/recordings/positives \
         --negatives /mnt/c/Users/dsm27/claude/demerzel/demerzel_wall_unit/models/wake_word_yo_demerzel_v1/recordings/negatives"
"""

import argparse
import sys
import wave
from pathlib import Path

import numpy as np

from microwakeword.inference import Model

EXPECTED_RATE = 16000
EXPECTED_BITS = 16
EXPECTED_CHANNELS = 1
TRAINING_STEP_MS = 10  # matches window_step_ms in training_parameters.yaml


def load_audio(path: Path) -> np.ndarray:
    """Load a 16 kHz / 16-bit / mono clip as int16 numpy. Accepts .wav and .raw."""
    suffix = path.suffix.lower()
    if suffix == ".raw":
        data = np.frombuffer(path.read_bytes(), dtype=np.int16)
    elif suffix == ".wav":
        with wave.open(str(path), "rb") as w:
            if w.getnchannels() != EXPECTED_CHANNELS:
                raise ValueError(f"{path}: expected mono, got {w.getnchannels()} channels")
            if w.getsampwidth() != EXPECTED_BITS // 8:
                raise ValueError(f"{path}: expected 16-bit, got {w.getsampwidth() * 8}-bit")
            if w.getframerate() != EXPECTED_RATE:
                raise ValueError(f"{path}: expected {EXPECTED_RATE} Hz, got {w.getframerate()}")
            data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    else:
        raise ValueError(f"{path}: unsupported extension {suffix}")
    return data


def score_clip(model: Model, audio: np.ndarray) -> tuple[float, int]:
    """Run streaming inference on a clip; return (peak_score, peak_frame_index)."""
    predictions = model.predict_clip(audio, step_ms=TRAINING_STEP_MS)
    if not predictions:
        return 0.0, -1
    arr = np.asarray(predictions, dtype=np.float32)
    return float(arr.max()), int(arr.argmax())


def collect_clips(directory: Path) -> list[Path]:
    if not directory.exists():
        return []
    return sorted(p for p in directory.iterdir() if p.suffix.lower() in (".wav", ".raw"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    here = Path(__file__).resolve().parent
    parser.add_argument(
        "--tflite",
        type=Path,
        default=here / "stream_state_internal_quant.tflite",
        help="Path to the quantized streaming .tflite (default: alongside this script)",
    )
    parser.add_argument("--positives", type=Path, default=here / "recordings" / "positives")
    parser.add_argument("--negatives", type=Path, default=here / "recordings" / "negatives")
    parser.add_argument(
        "--cutoffs",
        type=str,
        default="0.50,0.60,0.70,0.80,0.85,0.90,0.93,0.95,0.97,0.99",
        help="Comma-separated decision thresholds to sweep",
    )
    parser.add_argument(
        "--stride",
        type=int,
        default=None,
        help="Streaming stride in spectrogram frames (default: model's input_feature_slices)",
    )
    args = parser.parse_args()

    if not args.tflite.exists():
        print(f"ERROR: tflite not found: {args.tflite}", file=sys.stderr)
        return 2

    cutoffs = sorted({float(c) for c in args.cutoffs.split(",")})
    positives = collect_clips(args.positives)
    negatives = collect_clips(args.negatives)

    if not positives and not negatives:
        print(f"ERROR: no .wav/.raw clips found under {args.positives} or {args.negatives}", file=sys.stderr)
        print("Drop INMP441 recordings (16 kHz / 16-bit / mono) into those dirs and re-run.", file=sys.stderr)
        return 2

    print(f"Loading {args.tflite.name}")
    model = Model(str(args.tflite), stride=args.stride)
    print(f"  input shape: {model.input_details[0]['shape']}")
    print(f"  quantized:   {model.is_quantized_model}")
    print(f"  stride:      {model.stride} feature frames")
    print()

    def score_set(label: str, clips: list[Path]) -> list[tuple[Path, float, int, float]]:
        rows = []
        for clip in clips:
            try:
                audio = load_audio(clip)
            except ValueError as e:
                print(f"  SKIP {clip.name}: {e}", file=sys.stderr)
                continue
            duration_s = len(audio) / EXPECTED_RATE
            peak, frame = score_clip(model, audio)
            peak_time = frame * (TRAINING_STEP_MS / 1000.0) if frame >= 0 else -1.0
            rows.append((clip, peak, frame, peak_time))
            print(f"  {label:9s} {clip.name:40s}  peak={peak:.3f}  @ {peak_time:5.2f}s  (dur={duration_s:.1f}s)")
        return rows

    print(f"=== POSITIVES ({len(positives)} clips, expect peak >= cutoff) ===")
    pos_rows = score_set("POS", positives)
    print()
    print(f"=== NEGATIVES ({len(negatives)} clips, expect peak <  cutoff) ===")
    neg_rows = score_set("NEG", negatives)
    print()

    print("=== CUTOFF SWEEP ===")
    print(f"{'cutoff':>6}  {'recall':>10}  {'fp_count':>9}  {'fp_files'}")
    pos_peaks = np.asarray([r[1] for r in pos_rows], dtype=np.float32)
    neg_peaks = np.asarray([r[1] for r in neg_rows], dtype=np.float32)
    n_pos = max(len(pos_peaks), 1)
    for c in cutoffs:
        true_pos = int((pos_peaks >= c).sum()) if len(pos_peaks) else 0
        false_pos = int((neg_peaks >= c).sum()) if len(neg_peaks) else 0
        recall = true_pos / n_pos if len(pos_peaks) else float("nan")
        fp_files = ", ".join(r[0].name for r in neg_rows if r[1] >= c)
        print(f"{c:>6.2f}  {recall * 100:>8.1f} %  {false_pos:>3d}/{len(neg_peaks):<4d}  {fp_files}")
    print()

    # Recommend the highest-recall cutoff with zero false positives
    if len(pos_peaks) and len(neg_peaks):
        viable = [c for c in cutoffs if int((neg_peaks >= c).sum()) == 0]
        if viable:
            best_c = min(viable)
            best_recall = int((pos_peaks >= best_c).sum()) / n_pos
            print(f"Suggested operating cutoff: {best_c:.2f}  "
                  f"(recall {best_recall * 100:.1f}%, 0 false positives on {len(neg_peaks)} negatives)")
        else:
            print("WARNING: no cutoff in the sweep achieved 0 false positives. "
                  "Either retrain with hard negatives or extend the cutoff sweep upward.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
