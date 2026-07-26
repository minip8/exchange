#!/usr/bin/env bash
#
# End-to-end check over real sockets, using the shipped binaries rather than
# the in-process loopback in net_smoke. Two things this proves that the
# in-process test cannot:
#
#   1. exchange_cli and exchange_server actually interoperate, so the
#      reference client is a real client and not a second implementation of
#      the server's assumptions.
#   2. The market-data claim, automatably: a client that reconstructs the
#      book purely from the snapshot and the deltas that follow it ends up
#      with the same ladder the server restates in a fresh snapshot. That is
#      the gate for the market-data phase.
#
# Usage: scripts/net_e2e.sh [debug|release]

set -uo pipefail

cd "$(dirname "$0")/.."
CONFIG="${1:-debug}"
BIN="build/${CONFIG}/src/net"
PORT="${PORT:-19311}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null' EXIT

if [[ ! -x "$BIN/exchange_server" || ! -x "$BIN/exchange_cli" ]]; then
  echo "build them first:  cmake --build --preset ${CONFIG}" >&2
  exit 1
fi

TRADERS=config/traders.json
[[ -f "$TRADERS" ]] || TRADERS=config/traders.example.json
ALICE_KEY=$(python3 -c "import json,sys;print(json.load(open('$TRADERS'))['traders'][0]['api_key'])")
BOB_KEY=$(python3 -c "import json,sys;print(json.load(open('$TRADERS'))['traders'][1]['api_key'])")

"$BIN/exchange_server" --binary-port "$PORT" --traders "$TRADERS" \
  > "$TMP/server.log" 2>&1 &
SERVER_PID=$!

for _ in $(seq 50); do
  grep -q 'binary ' "$TMP/server.log" 2>/dev/null && break
  sleep 0.1
done

failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# --- set the book up, and leave resting orders on both sides -------------
{
  echo "book NVDA"
  sleep 0.3
  echo "sell 0 100 51"
  echo "sell 0 40 52"
  echo "buy 0 70 49"
  echo "buy 0 30 48"
  sleep 0.5
} | "$BIN/exchange_cli" --key "$ALICE_KEY" --port "$PORT" \
    > "$TMP/setup.log" 2>&1

grep -q 'ACK order' "$TMP/setup.log" || fail "the setup orders were not acked"

# --- the gate: follow the feed while the book changes underneath ---------
"$BIN/exchange_cli" --key "$BOB_KEY" --port "$PORT" \
  --tail 0 --seconds 3 --verify > "$TMP/tail.log" 2>&1 &
TAIL_PID=$!
sleep 0.7

# Trade and cancel against the book while the tail is running, so the
# verification is over a delta stream that actually moved.
{
  echo "buy 0 60 51"
  sleep 0.3
  echo "sell 0 20 49"
  sleep 0.3
  echo "buy 0 25 47"
  sleep 0.5
} | "$BIN/exchange_cli" --key "$ALICE_KEY" --port "$PORT" \
    > "$TMP/trade.log" 2>&1

wait "$TAIL_PID"
tail_status=$?

grep -q 'FILL' "$TMP/trade.log" || fail "the crossing order did not fill"
grep -q 'MD GAP' "$TMP/tail.log" && fail "the market-data stream had a gap"
if [[ $tail_status -ne 0 ]]; then
  fail "delta-reconstructed book did not match a fresh snapshot"
fi
grep -q 'OK: the delta-reconstructed book matches' "$TMP/tail.log" ||
  fail "the verification did not run to completion"

kill -INT "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=

if [[ $failures -eq 0 ]]; then
  echo "net_e2e: all checks passed"
  exit 0
fi

echo "--- server ---"; cat "$TMP/server.log"
echo "--- tail ---";   cat "$TMP/tail.log"
echo "--- trade ---";  cat "$TMP/trade.log"
echo "net_e2e: $failures failure(s)"
exit 1
