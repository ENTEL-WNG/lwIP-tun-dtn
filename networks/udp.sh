#!/bin/sh

SLEEP="${1:-0}"
FROM_NODE="${2:-node1}"
TO_NODE="${3:-node3}"
TO_NODE_ADDR="${4:-fd00:2:3::3}"
PORT="${5:-5005}"
WAIT="${6:-5}"
shift 6

# Remaining args are messages; use a default if none provided
if [ $# -eq 0 ]; then
    set -- "udp-test-$$-$(date +%s)"
fi

echo "--- Starting UDP listener on $TO_NODE:$PORT ---"
docker exec -d "$TO_NODE" sh -c "nc -6 -u -k -l $PORT > /tmp/udp_recv.txt 2>&1"
sleep 1

echo "--- Waiting ${SLEEP}s before sending UDP ---"
sleep $SLEEP

echo "--- UDP: $FROM_NODE -> $TO_NODE ($TO_NODE_ADDR:$PORT) [$# message(s)] ---"
for MSG in "$@"; do
    echo "$MSG" | docker exec -i "$FROM_NODE" nc -6 -u -w1 "$TO_NODE_ADDR" "$PORT"
done

echo "--- Waiting ${WAIT}s for delivery ---"
sleep $WAIT

PASS=0
FAIL=0
for MSG in "$@"; do
    if docker exec "$TO_NODE" grep -qF "$MSG" /tmp/udp_recv.txt 2>/dev/null; then
        echo "SUCCESS UDP: $FROM_NODE -> $TO_NODE ($TO_NODE_ADDR:$PORT) msg='$MSG' ---"
        PASS=$((PASS + 1))
    else
        echo "FAILED UDP: $FROM_NODE -> $TO_NODE ($TO_NODE_ADDR:$PORT) msg='$MSG' ---"
        FAIL=$((FAIL + 1))
    fi
done

echo "--- UDP results: $PASS passed, $FAIL failed ---"
