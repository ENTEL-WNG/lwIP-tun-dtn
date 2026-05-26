#!/bin/bash
# run_test.sh — Orchestrate a full test run for a contact plan.
#
# Usage: bash run_test.sh [contact-plan.toml]
#   Default contact plan: contact-plan-2.toml
#
# Steps:
#   1. Generate per-node configs and docker-compose from the contact plan
#   2. docker compose up (detached, with build)
#   3. Run networks/<plan>/test.sh
#   4. Collect docker compose logs → networks/<plan>/captures/<run>/logs
#   5. docker compose down
#   6. Merge node*.txt tcpdump files into a single time-sorted tcpdump file
#   7. Print completion message

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONTACT_PLAN="${1:-${SCRIPT_DIR}/networks/contact-plan-2.toml}"

if [ ! -f "$CONTACT_PLAN" ]; then
    echo "ERROR: Contact plan not found: $CONTACT_PLAN" >&2
    exit 1
fi

echo "=== Contact plan: $CONTACT_PLAN ==="

# --- Step 1: Generate network configs ---
echo "--- Generating network configs ---"
GEN_OUTPUT=$(python3 generate-network.py "$CONTACT_PLAN")
echo "$GEN_OUTPUT"

# Extract the output directory from the last OUTPUT_DIR=... line
COMPOSE_DIR=$(echo "$GEN_OUTPUT" | grep '^OUTPUT_DIR=' | tail -1 | cut -d= -f2)

if [ -z "$COMPOSE_DIR" ]; then
    echo "ERROR: generate-network.py did not output OUTPUT_DIR" >&2
    exit 1
fi

COMPOSE_FILE="${COMPOSE_DIR}/docker-compose.yml"

if [ ! -f "$COMPOSE_FILE" ]; then
    echo "ERROR: docker-compose.yml not found at $COMPOSE_FILE" >&2
    exit 1
fi

# Derive PLAN_NAME from the compose directory basename for capture path
PLAN_NAME=$(basename "$COMPOSE_DIR")

# Read TEST_CASE_NUMBER from the generated .env file
ENV_FILE="${COMPOSE_DIR}/.env"
TEST_CASE_NUMBER=0
if [ -f "$ENV_FILE" ]; then
    TEST_CASE_NUMBER=$(grep '^TEST_CASE_NUMBER=' "$ENV_FILE" | cut -d= -f2)
fi

CAPTURES_DIR="${COMPOSE_DIR}/captures/${TEST_CASE_NUMBER}"
mkdir -p "$CAPTURES_DIR"

echo "--- Run index: $TEST_CASE_NUMBER (output: $CAPTURES_DIR) ---"

# --- Step 1b: Clean dtn_storage directories ---
# echo "--- Cleaning dtn_storage directories ---"
# rm -rf "${SCRIPT_DIR}/dtn_storage"
# echo "dtn_storage cleaned."

# --- Step 2: docker compose up ---
echo "--- Starting docker compose ---"
docker compose -f "$COMPOSE_FILE" up -d --build
# docker compose -f "$COMPOSE_FILE" up --build

# Wait until all containers are running
echo "--- Waiting for all containers to be running ---"
TIMEOUT=60
ELAPSED=0
while true; do
    TOTAL=$(docker compose -f "$COMPOSE_FILE" ps -q | wc -l | tr -d ' ')
    RUNNING=$(docker compose -f "$COMPOSE_FILE" ps --status running -q | wc -l | tr -d ' ')
    if [ "$TOTAL" -gt 0 ] && [ "$RUNNING" -eq "$TOTAL" ]; then
        echo "All $TOTAL container(s) running."
        break
    fi
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        echo "ERROR: Timed out waiting for containers to start (${RUNNING}/${TOTAL} running)" >&2
        docker compose -f "$COMPOSE_FILE" ps >&2
        docker compose -f "$COMPOSE_FILE" down
        exit 1
    fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

# --- Step 3: Run test.sh ---
TEST_SCRIPT="${COMPOSE_DIR}/test.sh"
if [ -f "$TEST_SCRIPT" ]; then
    echo "--- Running test script: $TEST_SCRIPT ---"
    bash "$TEST_SCRIPT"
else
    echo "WARNING: No test.sh found at $TEST_SCRIPT, skipping."
fi

# --- Step 4: Collect logs (before down, containers still exist) ---
echo "--- Waiting 3s for docker compose logs---"
sleep 3

echo "--- Collecting docker compose logs ---"
rm -f "${CAPTURES_DIR}/logs.txt"
docker compose -f "$COMPOSE_FILE" logs --no-color --timestamps 2>&1 \
    | sed 's/^\([^ ]*\) *|/\1 |/' \
    | sort -k3 \
    > "${CAPTURES_DIR}/logs.txt"
echo "Logs written to ${CAPTURES_DIR}/logs.txt"

# --- Step 5: docker compose down ---
echo "--- Stopping docker compose ---"
docker compose -f "$COMPOSE_FILE" down

# --- Step 6: Merge node*.txt tcpdump files ---
echo "--- Merging per-node tcpdump files ---"
TCPDUMP_OUT="${CAPTURES_DIR}/tcpdump.txt"
# Combine lines that start with a timestamp (YYYY-MM-DD) from all node txt files,
# then sort by timestamp field.
if ls "${CAPTURES_DIR}"/node*.txt 2>/dev/null | head -1 | grep -q .; then
    rm -f "$TCPDUMP_OUT"
    grep -h '^[0-9][0-9][0-9][0-9]-' "${CAPTURES_DIR}"/node*.txt \
        | sort -k1,2 \
        > "$TCPDUMP_OUT"
    echo "Merged tcpdump written to $TCPDUMP_OUT"
else
    echo "WARNING: No node*.txt files found in $CAPTURES_DIR"
fi

# --- Step 7: Done ---
echo ""
echo "=== Test completed ==="
echo "  Plan:         $PLAN_NAME"
echo "  Run index:    $TEST_CASE_NUMBER"
echo "  Captures dir: $CAPTURES_DIR"
echo "    logs        — docker compose logs"
echo "    tcpdump     — merged per-node traffic (time-sorted)"
echo "    capture.*   — host-level pcap/txt"
echo "    node*.pcap  — per-node binary pcap"
echo "    node*.txt   — per-node human-readable traffic"
echo "    node*_trace.txt — per-node netfilter trace"
