#!/usr/bin/env bash
# Packet capture helper for CS3530 Assignment-1  (cs23btech11047)
#
#   ./analysis/capture.sh <iface> <label> [seconds] [port]
#
# Examples used in the report:
#   Single client, captured at the client host NIC:
#       sudo ./analysis/capture.sh eth0 parta_single_client 30
#   Ten clients, captured at the server interface facing the clients:
#       sudo ./analysis/capture.sh eth0 partb_N10_serverside 30
#
# The raw command this wraps is
#   tcpdump -i <iface> -s 0 -w <file>.pcap 'port 11047'
set -euo pipefail

IFACE=${1:?usage: capture.sh <iface> <label> [seconds] [port]}
LABEL=${2:?usage: capture.sh <iface> <label> [seconds] [port]}
SECONDS_TO_RUN=${3:-30}
PORT=${4:-11047}

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/pcaps"
mkdir -p "$DIR"
OUT="$DIR/${LABEL}.pcap"

echo "interface : $IFACE"
echo "filter    : port $PORT"
echo "duration  : ${SECONDS_TO_RUN}s"
echo "output    : $OUT"
echo
echo "tcpdump -i $IFACE -s 0 -w $OUT 'port $PORT'"

timeout "${SECONDS_TO_RUN}" tcpdump -i "$IFACE" -s 0 -w "$OUT" "port $PORT" || true

echo
echo "captured $(stat -c%s "$OUT" 2>/dev/null || echo 0) bytes"
echo "analyse with: python3 analysis/analyze_pcap.py $OUT --port $PORT"
