#!/bin/sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# UDP: after 5s, send multiple messages
sh "$SCRIPT_DIR/../udp.sh" 5 node1 node4 fd00:4:5::4 5005 5 "msg-one" "msg-two" "msg-three"

sh "$SCRIPT_DIR/../udp.sh" 5 node1 node4 fd00:4:5::4 5005 5 "msg-four" "msg-five" "msg-six"