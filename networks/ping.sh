#!/bin/sh

SLEEP="${1:-0}"
FROM_NODE="${2:-node1}"
TO_NODE="${3:-node4}"
TO_NODE_ADDR="${4:-fd00:4:5::4}"
WAIT="${5:-5}"

echo "--- Waiting ${SLEEP}s for ping ---"
sleep $SLEEP

echo "--- Ping: $FROM_NODE -> $TO_NODE ($TO_NODE_ADDR) ---"
if docker exec "$FROM_NODE" ping6 -c 1 -w "$WAIT" "$TO_NODE_ADDR"; then
    echo "SUCCESS Ping: $FROM_NODE -> $TO_NODE ($TO_NODE_ADDR) ---"
else
    echo "FAILED Ping: $FROM_NODE -> $TO_NODE ($TO_NODE_ADDR) ---"
fi