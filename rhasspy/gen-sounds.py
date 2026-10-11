#!/usr/bin/env python3
"""Writes the dialogue feedback sounds as 16-kHz mono WAV at -12 dBFS with
soft edges (Rhasspy's defaults are full-scale 44.1-kHz stereo and need
resampling on the satellite). Usage: rhasspy/gen-sounds.py"""
import math
import os
import struct
import wave

RATE, AMP = 16000, 0.25            # -12 dBFS
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profile", "sounds")

SOUNDS = {                          # (frequency Hz, seconds), 0 Hz = pause
    "wake.wav": [(880, 0.08), (0, 0.03), (1320, 0.10)],
    "recorded.wav": [(1320, 0.07), (0, 0.03), (880, 0.09)],
    "error.wav": [(330, 0.15), (0, 0.06), (262, 0.22)],
}

os.makedirs(OUT, exist_ok=True)
for name, parts in SOUNDS.items():
    frames = []
    for freq, seconds in parts:
        n = int(RATE * seconds)
        fade = int(RATE * 0.01)
        for i in range(n):
            env = min(1.0, i / fade, (n - 1 - i) / fade) if freq else 0.0
            frames.append(struct.pack("<h", int(32767 * AMP * env * math.sin(2 * math.pi * freq * i / RATE))))
    with wave.open(os.path.join(OUT, name), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(frames))
    print("wrote", os.path.join("rhasspy/profile/sounds", name))
