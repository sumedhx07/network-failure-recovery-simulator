#!/bin/sh
# Functional smoke test for the C server and its public REST API.
set -eu

PORT="${TEST_PORT:-18081}"
BASE_URL="http://127.0.0.1:${PORT}"
LOG_FILE="$(mktemp)"

HOST=127.0.0.1 PORT="$PORT" ./network-simulator >"$LOG_FILE" 2>&1 &
SERVER_PID=$!

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  rm -f "$LOG_FILE"
}
trap cleanup EXIT INT TERM

ready=0
attempt=0
while [ "$attempt" -lt 40 ]; do
  if curl --silent --fail "$BASE_URL/sim-api/v1/state" >/dev/null 2>&1; then
    ready=1
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  cat "$LOG_FILE"
  echo "Server did not become ready." >&2
  exit 1
fi

state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"totalRouters":6'
printf '%s' "$state" | grep -q '"onlineRouters":6'
printf '%s' "$state" | grep -q '"totalLinks":5'
printf '%s' "$state" | grep -q '"availability":100.0'
echo "PASS: initial state and star metrics"

curl --silent --fail -X POST -H 'Content-Type: application/json' \
  -d '{"topology":"ring"}' "$BASE_URL/sim-api/v1/topology" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"topology":"ring"'
printf '%s' "$state" | grep -q '"totalLinks":6'
echo "PASS: ring topology"

curl --silent --fail -X POST -H 'Content-Type: application/json' \
  -d '{"topology":"mesh"}' "$BASE_URL/sim-api/v1/topology" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"topology":"mesh"'
printf '%s' "$state" | grep -q '"totalLinks":15'
echo "PASS: mesh topology"

curl --silent --fail -X POST -H 'Content-Type: application/json' \
  -d '{"count":2}' "$BASE_URL/sim-api/v1/router-count" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"totalRouters":2'
printf '%s' "$state" | grep -q '"totalLinks":1'
echo "PASS: router count can be reduced to two"

curl --silent --fail -X POST -H 'Content-Type: application/json' \
  -d '{"count":12}' "$BASE_URL/sim-api/v1/router-count" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"totalRouters":12'
printf '%s' "$state" | grep -q '"totalLinks":66'
echo "PASS: router count can be increased to twelve"

invalid_count_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -X POST -H 'Content-Type: application/json' \
  -d '{"count":13}' "$BASE_URL/sim-api/v1/router-count")"
[ "$invalid_count_status" = "400" ]
invalid_count_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -X POST -H 'Content-Type: application/json' \
  -d '{"count":1}' "$BASE_URL/sim-api/v1/router-count")"
[ "$invalid_count_status" = "400" ]

curl --silent --fail -X POST -H 'Content-Type: application/json' \
  -d '{"count":6}' "$BASE_URL/sim-api/v1/router-count" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"totalRouters":6'
printf '%s' "$state" | grep -q '"message":"Router count changed from 12 to 6."'
echo "PASS: router count validation and event logging"

curl --silent --fail -X POST "$BASE_URL/sim-api/v1/routers/3/fail" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"failedRouters":1'
printf '%s' "$state" | grep -q '"availability":83.3'
printf '%s' "$state" | grep -q '"active":false'
echo "PASS: individual router failure and link state"

curl --silent --fail -X POST "$BASE_URL/sim-api/v1/routers/3/recover" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"failedRouters":0'
printf '%s' "$state" | grep -q '"type":"recovery"'
echo "PASS: individual router recovery and event timeline"

curl --silent --fail -X POST "$BASE_URL/sim-api/v1/routers/fail-all" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"failedRouters":6'
printf '%s' "$state" | grep -q '"availability":0.0'
echo "PASS: fail all"

curl --silent --fail -X POST "$BASE_URL/sim-api/v1/routers/recover-all" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"onlineRouters":6'
printf '%s' "$state" | grep -q '"availability":100.0'
echo "PASS: recover all"

curl --silent --fail -X POST -H 'Content-Type: application/json' \
  -d '{"topology":"star"}' "$BASE_URL/sim-api/v1/topology" >/dev/null
curl --silent --fail -X POST "$BASE_URL/sim-api/v1/routers/2/fail" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"reachableRouters":\[1,3,4,5,6\]'
printf '%s' "$state" | grep -q '"unreachableRouters":\[2\]'
echo "PASS: BFS reachability and unreachable router detection"

curl --silent --fail -X POST "$BASE_URL/sim-api/v1/reset" >/dev/null
state="$(curl --silent --fail "$BASE_URL/sim-api/v1/state")"
printf '%s' "$state" | grep -q '"topology":"star"'
printf '%s' "$state" | grep -q '"onlineRouters":6'
printf '%s' "$state" | grep -q '"type":"reset"'
echo "PASS: reset"

error_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -X POST -H 'Content-Type: application/json' \
  -d '{"topology":"invalid"}' "$BASE_URL/sim-api/v1/topology")"
[ "$error_status" = "400" ]
echo "PASS: invalid topology returns HTTP 400"
