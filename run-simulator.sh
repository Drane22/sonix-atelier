#!/usr/bin/env bash
set -euo pipefail

# Resolve repo paths regardless of where this launcher is invoked from.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$ROOT_DIR/sonix-player"
SIM_DATA="$ROOT_DIR/.sim-data"

mkdir -p "$SIM_DATA/sd"

PANEL="${SONIX_PANEL:-480x800}"
REBUILD=0

for arg in "$@"; do
	case "$arg" in
		r1|R1|480x800) PANEL="480x800" ;;
		r3|r3pii|R3PII|480x720) PANEL="480x720" ;;
		build|rebuild|-b|--build) REBUILD=1 ;;
	esac
done

cd "$APP_DIR"

if [ "$REBUILD" -eq 1 ] || [ ! -x "./sonix_player_host" ]; then
	echo "Building sonix_player_host..."
	make host -j"$(nproc)"
fi

# Close any previous simulator window so runs never stack up.
pkill -x sonix_player_host 2>/dev/null || true

export SONIX_PANEL="$PANEL"
export SONIX_SD_ROOT="${SONIX_SD_ROOT:-$SIM_DATA/sd}"
export SONIX_CONFIG="${SONIX_CONFIG:-$SIM_DATA/sonix.conf}"

exec ./sonix_player_host
