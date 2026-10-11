#!/usr/bin/env bash
# Rhasspy "command" TTS: text as arguments (Rhasspy) or on stdin, WAV on stdout. NanoTTS's own --volume
# keeps the synthesis below full scale; its default output clips (~600 samples
# per sentence), which sounds harsh on the AI-VOX3 speaker. Rhasspy's
# text_to_speech.*.volume only rescales the already clipped WAV.
set -euo pipefail
out="$(mktemp --suffix=.wav)"
trap 'rm -f "$out"' EXIT
if (($#)); then printf '%s\n' "$*"; else cat; fi |
  nanotts -v "${NANOTTS_VOICE:-de-DE}" --volume "${NANOTTS_VOLUME:-0.5}" -o "$out" >/dev/null 2>&1
cat "$out"
