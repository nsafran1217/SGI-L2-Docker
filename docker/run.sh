#!/bin/bash
# run.sh — bring up the snxsc_l3 telnet container with explicit per-device
# passthrough. Alternative to docker-compose.yml's bind-mount-of-/dev
# approach for users who prefer to grant only the specific device nodes.
#
# Quickstart:
#   1. Place snxsc_l3.tar.gz next to this file.
#   2. docker build -t snxsc-l3 .
#   3. ./run.sh

set -euo pipefail

NAME="${NAME:-l2}"
IMAGE="${IMAGE:-snxsc-l3:latest}"

# Sanity check: the host has to have the sgil1 driver loaded for the device
# nodes to exist. If they don't, the `--device=` flag would fail anyway,
# but a clearer error here is friendlier.
if [[ ! -e /dev/sgil1_cs ]]; then
    echo "ERROR: /dev/sgil1_cs not present on host." >&2
    echo "Has driver/setup-driver.sh been run?  (lsmod | grep sgil1)" >&2
    exit 1
fi

# Build the --device flag list dynamically. /dev/sgil1_cs always exists if
# the module is loaded; sgil1_0..39 are the per-slot nodes — pass through
# only the ones that actually exist (e.g. some kernels may have a smaller
# range, or hot-unplug may have removed some).
DEVICES=( --device=/dev/sgil1_cs )
for i in $(seq 0 39); do
    [[ -e /dev/sgil1_$i ]] && DEVICES+=( --device=/dev/sgil1_$i )
done

# Stop and remove any prior instance so re-runs are clean.
docker rm -f "$NAME" >/dev/null 2>&1 || true

exec docker run -d \
    --name "$NAME" \
    --restart always \
    "${DEVICES[@]}" \
    -v l2-state:/home/l2 \
    -p 23:23 -p 8001:8001 -p 8003:8003 \
    "$IMAGE"
