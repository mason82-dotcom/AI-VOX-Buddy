#!/usr/bin/env python3
"""Writes rhasspy/profile/slots/zahl: German number words 0..100, one per line,
from intent_handler.german_numbers() so recognition and parsing stay in sync.
Usage: rhasspy/gen-slots.py"""
import os
import sys

DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, DIR)
from intent_handler import german_numbers  # noqa: E402

words = sorted((w for w in german_numbers() if not w.startswith("ein") or w in ("eins", "einhundert")
                or "und" in w), key=lambda w: (german_numbers()[w], w))
os.makedirs(os.path.join(DIR, "profile", "slots"), exist_ok=True)
path = os.path.join(DIR, "profile", "slots", "zahl")
with open(path, "w", encoding="utf-8") as f:
    f.write("\n".join(words) + "\n")
print(f"wrote {len(words)} words to rhasspy/profile/slots/zahl")
