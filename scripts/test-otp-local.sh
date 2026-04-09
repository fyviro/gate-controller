#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./scripts/test-otp-local.sh <villa> <owner_secret>
# Example:
#   ./scripts/test-otp-local.sh 45 yniXGDorYu

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <villa> <owner_secret>"
  exit 1
fi

VILLA="$1"
SECRET="$2"
PORT="${PORT:-8080}"
BASE_URL="${BASE_URL:-http://127.0.0.1:${PORT}}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SERVER_PID=""
cleanup() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

echo "[1/4] Starting mock gate server..."
node "${ROOT_DIR}/scripts/mock-gate-server.js" >/tmp/mock-gate.log 2>&1 &
SERVER_PID="$!"
sleep 1

echo "[2/4] Generating OTP..."
TS="$(date +%s)"
OTP="$(node "${ROOT_DIR}/scripts/otp-generate.js" "${SECRET}" "${TS}")"
echo "villa=${VILLA} otp=${OTP} ts=${TS}"

echo "[3/4] Calling OTP endpoint..."
curl -i "${BASE_URL}/open?otp=true&v=${VILLA}&c=${OTP}&t=${TS}"
echo

echo "[4/4] Done."
