#!/usr/bin/env bash
# Installs the AI-VOX3 Rhasspy profile, (re)starts the container, downloads
# missing German model files (several hundred MB on the first run) and trains
# the sentences. Safe to re-run after editing files in rhasspy/profile/.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROFILES="${RHASSPY_PROFILES:-$HOME/.config/rhasspy/profiles}"
API="http://localhost:12101/api"

wait_ready() {
  for _ in $(seq 90); do curl -sf "$API/version" >/dev/null && return 0; sleep 2; done
  echo "Rhasspy did not start; see: docker logs rhasspy" >&2
  exit 1
}

mkdir -p "$PROFILES/de"
install -m 644 "$DIR/profile/profile.json" "$DIR/profile/sentences.ini" "$PROFILES/de/"
install -m 755 "$DIR/profile/tts.sh" "$PROFILES/de/"
mkdir -p "$PROFILES/de/slots"
install -m 644 "$DIR"/profile/slots/* "$PROFILES/de/slots/"
mkdir -p "$PROFILES/de/sounds"
install -m 644 "$DIR"/profile/sounds/*.wav "$PROFILES/de/sounds/"
rm -f "$PROFILES/de/handle.py"   # replaced by the intent-handler container
RHASSPY_PROFILES="$PROFILES" docker compose -f "$DIR/docker-compose.yml" up -d
# Rhasspy generates its service configuration (supervisord.conf) only when the
# container starts; POST /api/restart keeps the old services. Restart the
# containers to load the installed profile and intent_handler.py.
docker restart rhasspy rhasspy-intent-handler >/dev/null
wait_ready
echo "Downloading missing profile files..."
curl -sf -m 3600 -X POST "$API/download-profile" >/dev/null
docker restart rhasspy >/dev/null
wait_ready
# The web server answers before the ASR/NLU services have subscribed to their
# MQTT train topics; a train request sent earlier is lost and times out (10 min).
sleep 20
echo "Training..."
curl -s -X POST "$API/train" | tee /dev/stderr | grep -q "^Training completed" || {
  echo; echo "Training failed; see: docker logs rhasspy" >&2; exit 1;
}
echo; echo "Rhasspy ready: http://localhost:12101 (site ID ai-vox3)"
