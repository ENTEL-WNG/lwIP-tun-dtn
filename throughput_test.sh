#!/usr/bin/env bash
# throughput_test.sh — End-to-end DTN throughput experiment.
#
# Steps:
#   1.  Generate per-node configs + docker-compose from the contact plan
#   2.  docker compose up -d --build
#   3.  Wait for all containers to be running
#   4.  Start monitor.py on the host (background)
#   5.  Start traffic_recv.py inside the receiver container (background)
#   6.  Sleep 1 s to let the receiver bind
#   7.  Run traffic_gen.py inside the sender container (foreground; blocks until done)
#   8.  Wait --wait-after seconds for in-flight / stored packets to drain
#   9.  Collect docker compose logs → captures/logs.txt
#   10. docker cp sent.csv and recv.csv from containers
#   11. Stop monitor.py
#   12. docker compose down
#   13. Merge per-node tcpdump files → captures/tcpdump.txt
#   14. Run analyze.py → captures/report.{txt,json,html}
#
# Usage:
#   bash throughput_test.sh [options]
#
# Options:
#   --contact-plan FILE   Contact plan TOML (default: networks/contact-plan-throughput.toml)
#   --rate N              Packets/second sent by traffic_gen (default: 50)
#   --duration N          Sender duration in seconds (default: 30)
#   --size N              Payload size in bytes (default: 512)
#   --port N              UDP port (default: 5005)
#   --sender NODE         Sender container name (default: node1)
#   --receiver NODE       Receiver container name (default: node3)
#   --relay NODE          DTN relay container name (default: node2)
#   --dst-addr ADDR       IPv6 destination address for traffic_gen (default: fd00:2:3::3)
#   --wait-after N        Seconds to wait after sender finishes (default: 15)
#   --no-analyze          Skip the analyze.py step
#   --keep-up             Do not call docker compose down at the end

#   bash throughput_test.sh \
#     --rate 100        \  # packets/sec
#     --duration 60     \  # test window in seconds
#     --size 1024       \  # bytes per packet
#     --wait-after 30   \  # drain time for store-and-forward
#     --contact-plan networks/contact-plan-udp-cgr.toml  # any topology

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
CONTACT_PLAN="${SCRIPT_DIR}/networks/contact-plan-throughput.toml"
RATE=100
DURATION=30
SIZE=512
PORT=5005
SENDER_NODE=node1
RECV_NODE=node3
RELAY_NODE=node2
DST_ADDR="fd00:2:3::3"
WAIT_AFTER=15
RUN_ANALYZE=1
KEEP_UP=0

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --contact-plan) CONTACT_PLAN="$2"; shift 2 ;;
        --rate)         RATE="$2";         shift 2 ;;
        --duration)     DURATION="$2";     shift 2 ;;
        --size)         SIZE="$2";         shift 2 ;;
        --port)         PORT="$2";         shift 2 ;;
        --sender)       SENDER_NODE="$2";  shift 2 ;;
        --receiver)     RECV_NODE="$2";    shift 2 ;;
        --relay)        RELAY_NODE="$2";   shift 2 ;;
        --dst-addr)     DST_ADDR="$2";     shift 2 ;;
        --wait-after)   WAIT_AFTER="$2";   shift 2 ;;
        --no-analyze)   RUN_ANALYZE=0;     shift ;;
        --keep-up)      KEEP_UP=1;         shift ;;
        -h|--help)
            sed -n '/^# Usage:/,/^[^#]/p' "$0" | grep '^#' | sed 's/^# *//'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Validate
# ---------------------------------------------------------------------------
if [[ ! -f "$CONTACT_PLAN" ]]; then
    echo "ERROR: Contact plan not found: $CONTACT_PLAN" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 1 — Generate network configs
# ---------------------------------------------------------------------------
echo ""
echo "=== Throughput test: $CONTACT_PLAN ==="
echo "    rate=${RATE} pkt/s  duration=${DURATION}s  size=${SIZE}B  port=${PORT}"
echo "    ${SENDER_NODE} -> ${DST_ADDR}:${PORT} via ${RELAY_NODE} -> ${RECV_NODE}"
echo ""

echo "--- [1/8] Generating network configs ---"
cd "$SCRIPT_DIR"
GEN_OUTPUT=$(python3 generate-network.py "$CONTACT_PLAN")
echo "$GEN_OUTPUT"

COMPOSE_DIR=$(echo "$GEN_OUTPUT" | grep '^OUTPUT_DIR=' | tail -1 | cut -d= -f2)
if [[ -z "$COMPOSE_DIR" ]]; then
    echo "ERROR: generate-network.py did not print OUTPUT_DIR=..." >&2
    exit 1
fi

COMPOSE_FILE="${COMPOSE_DIR}/docker-compose.yml"
if [[ ! -f "$COMPOSE_FILE" ]]; then
    echo "ERROR: docker-compose.yml not found: $COMPOSE_FILE" >&2
    exit 1
fi

ENV_FILE="${COMPOSE_DIR}/.env"
TEST_CASE_NUMBER=0
if [[ -f "$ENV_FILE" ]]; then
    TEST_CASE_NUMBER=$(grep '^TEST_CASE_NUMBER=' "$ENV_FILE" | cut -d= -f2)
fi

CAPTURES_DIR="${COMPOSE_DIR}/captures/${TEST_CASE_NUMBER}"
mkdir -p "$CAPTURES_DIR"

# Derive plan name (e.g. contact_plan_throughput) for DB path
PLAN_NAME=$(basename "$COMPOSE_DIR")
# DB written inside the container at ./dtn_storage/<plan_name>/<name>_dtn_packets.db
# which maps to <repo_root>/dtn_storage/<plan_name>/<relay>_dtn_packets.db on the host
DB_PATH="${SCRIPT_DIR}/dtn_storage/${PLAN_NAME}/${RELAY_NODE}_dtn_packets.db"

echo ""
echo "    Compose dir : $COMPOSE_DIR"
echo "    Captures dir: $CAPTURES_DIR"
echo "    DB path     : $DB_PATH"

MONITOR_PID=""
RECV_PID=""

# ---------------------------------------------------------------------------
# Cleanup trap — always runs on exit (success or failure)
# ---------------------------------------------------------------------------
cleanup() {
    local exit_code=$?
    echo ""
    echo "--- [cleanup] ---"

    # Stop monitor
    if [[ -n "$MONITOR_PID" ]] && kill -0 "$MONITOR_PID" 2>/dev/null; then
        echo "Stopping monitor (pid $MONITOR_PID)..."
        kill "$MONITOR_PID" 2>/dev/null || true
        wait "$MONITOR_PID" 2>/dev/null || true
    fi

    # Stop receiver (best-effort; it may already have exited)
    if [[ -n "$RECV_PID" ]]; then
        docker exec "$RECV_NODE" sh -c \
            "kill \$(pgrep -f traffic_recv.py) 2>/dev/null || true" 2>/dev/null || true
    fi

    # Bring compose down (unless --keep-up)
    if [[ $KEEP_UP -eq 0 ]]; then
        echo "Stopping docker compose..."
        docker compose -f "$COMPOSE_FILE" down 2>/dev/null || true
    fi

    exit $exit_code
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Step 2 — docker compose up
# ---------------------------------------------------------------------------
echo ""
echo "--- [2/8] Starting docker compose ---"
docker compose -f "$COMPOSE_FILE" up -d --build

# ---------------------------------------------------------------------------
# Step 3 — Wait for all containers to be running
# ---------------------------------------------------------------------------
echo "--- [3/8] Waiting for containers to start ---"
TIMEOUT=90
ELAPSED=0
while true; do
    TOTAL=$(docker compose -f "$COMPOSE_FILE" ps -q 2>/dev/null | wc -l | tr -d ' ')
    RUNNING=$(docker compose -f "$COMPOSE_FILE" ps --status running -q 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$TOTAL" -gt 0 && "$RUNNING" -eq "$TOTAL" ]]; then
        echo "All $TOTAL container(s) running."
        break
    fi
    if [[ "$ELAPSED" -ge "$TIMEOUT" ]]; then
        echo "ERROR: Timed out — only ${RUNNING}/${TOTAL} container(s) running after ${TIMEOUT}s" >&2
        docker compose -f "$COMPOSE_FILE" ps >&2
        exit 1
    fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

# Give lwip_tun a moment to finish initialising (TUN setup, CGR, etc.)
sleep 3

# ---------------------------------------------------------------------------
# Step 4 — Start monitor.py on the host (background)
# ---------------------------------------------------------------------------
echo ""
echo "--- [4/8] Starting monitor.py (host, background) ---"
CONTAINERS="${SENDER_NODE},${RELAY_NODE},${RECV_NODE}"
python3 "${SCRIPT_DIR}/networks/monitor.py" \
    --containers "$CONTAINERS" \
    --db-map "${RELAY_NODE}=${DB_PATH}" \
    --interval 1 \
    --out "${CAPTURES_DIR}/metrics.jsonl" \
    &
MONITOR_PID=$!
echo "monitor.py pid: $MONITOR_PID"

# ---------------------------------------------------------------------------
# Step 5 — Start receiver inside its container (background via docker exec)
# ---------------------------------------------------------------------------
echo ""
echo "--- [5/8] Starting traffic_recv.py in ${RECV_NODE} ---"
docker exec -d "$RECV_NODE" \
    python3 /repo/networks/traffic_recv.py "$PORT" \
        --out /tmp/recv.csv
RECV_PID=1  # sentinel — we track it via pgrep inside the container

# Let the receiver socket bind before the sender starts
sleep 1

# ---------------------------------------------------------------------------
# Step 6 — Run sender inside its container (foreground — blocks until done)
# ---------------------------------------------------------------------------
echo ""
echo "--- [6/8] Running traffic_gen.py in ${SENDER_NODE} ---"
echo "    ${SENDER_NODE} -> [${DST_ADDR}]:${PORT}  rate=${RATE} pkt/s  ${DURATION}s  ${SIZE}B"
docker exec "$SENDER_NODE" \
    python3 /repo/networks/traffic_gen.py "$DST_ADDR" "$PORT" \
        --rate "$RATE" \
        --duration "$DURATION" \
        --size "$SIZE" \
        --out /tmp/sent.csv

echo "Sender finished."

# ---------------------------------------------------------------------------
# Step 7 — Wait for in-flight / stored packets to drain
# ---------------------------------------------------------------------------
echo ""
echo "--- [7/8] Waiting ${WAIT_AFTER}s for in-flight packets to drain ---"
sleep "$WAIT_AFTER"

# ---------------------------------------------------------------------------
# Step 8 — Collect artefacts
# ---------------------------------------------------------------------------
echo ""
echo "--- [8/8] Collecting artefacts ---"

# Stop the receiver (sends SIGTERM so it writes the CSV)
echo "Stopping receiver..."
docker exec "$RECV_NODE" sh -c \
    "kill \$(pgrep -f traffic_recv.py) 2>/dev/null || true" 2>/dev/null || true
sleep 1

# Copy CSVs
echo "Copying sent.csv from ${SENDER_NODE}..."
docker cp "${SENDER_NODE}:/tmp/sent.csv" "${CAPTURES_DIR}/sent.csv" 2>/dev/null \
    || echo "WARNING: sent.csv not found in ${SENDER_NODE}"

echo "Copying recv.csv from ${RECV_NODE}..."
docker cp "${RECV_NODE}:/tmp/recv.csv" "${CAPTURES_DIR}/recv.csv" 2>/dev/null \
    || echo "WARNING: recv.csv not found in ${RECV_NODE}"

# Collect docker compose logs (time-sorted)
echo "Collecting docker compose logs..."
docker compose -f "$COMPOSE_FILE" logs --no-color --timestamps 2>&1 \
    | sed 's/^\([^ ]*\) *|/\1 |/' \
    | sort -k3 \
    > "${CAPTURES_DIR}/logs.txt"
echo "Logs → ${CAPTURES_DIR}/logs.txt"

# Stop monitor
echo "Stopping monitor.py..."
if kill -0 "$MONITOR_PID" 2>/dev/null; then
    kill "$MONITOR_PID"
    wait "$MONITOR_PID" 2>/dev/null || true
fi
MONITOR_PID=""
echo "Metrics → ${CAPTURES_DIR}/metrics.jsonl"

# Bring compose down (unless --keep-up)
if [[ $KEEP_UP -eq 0 ]]; then
    echo "docker compose down..."
    docker compose -f "$COMPOSE_FILE" down
fi

# Merge per-node tcpdump text files (time-sorted)
echo "Merging per-node tcpdump files..."
TCPDUMP_OUT="${CAPTURES_DIR}/tcpdump.txt"
if ls "${CAPTURES_DIR}"/node*.txt 2>/dev/null | head -1 | grep -q .; then
    grep -h '^[0-9][0-9][0-9][0-9]-' "${CAPTURES_DIR}"/node*.txt \
        | sort -k1,2 \
        > "$TCPDUMP_OUT"
    echo "Merged tcpdump → $TCPDUMP_OUT"
else
    echo "WARNING: no node*.txt capture files found in $CAPTURES_DIR"
fi

# ---------------------------------------------------------------------------
# Analyse
# ---------------------------------------------------------------------------
if [[ $RUN_ANALYZE -eq 1 ]]; then
    echo ""
    echo "--- [analyze] Running analyze.py ---"
    python3 "${SCRIPT_DIR}/networks/analyze.py" "$CAPTURES_DIR" || true
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "============================================================"
echo "  Throughput test complete"
echo "============================================================"
echo "  Contact plan : $(basename "$CONTACT_PLAN")"
echo "  Run index    : $TEST_CASE_NUMBER"
echo "  Captures dir : $CAPTURES_DIR"
echo ""
echo "  Files:"
echo "    sent.csv      — per-packet send timestamps (sender)"
echo "    recv.csv      — per-packet receive timestamps (receiver)"
echo "    metrics.jsonl — per-second CPU / mem / DB snapshots"
echo "    logs.txt      — docker compose logs (all nodes)"
echo "    tcpdump.txt   — merged per-node traffic (time-sorted)"
echo "    node*.pcap    — per-node binary captures"
echo "    report.txt    — PDR / latency / throughput summary"
echo "    report.json   — machine-readable report"
echo "    report.html   — charts (if matplotlib is installed)"
echo "============================================================"
