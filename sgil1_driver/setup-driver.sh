#!/usr/bin/env bash
#
# setup-driver.sh — install the modernized sgil1 USB kernel driver via
# DKMS on Debian 13 (or any Debian-family system with kernel headers
# and dkms available).
#
# Expected layout (script and source ship together):
#
#   sgil1_driver/
#   ├── setup-driver.sh             ← this script
#   ├── dkms.conf                   ← DKMS metadata
#   ├── sgil1-modules-load.conf     ← /etc/modules-load.d/ entry
#   ├── README.md
#   └── src/
#       ├── sgil1.c
#       ├── sgil1.h
#       ├── Makefile
#       └── 99-sgil1.rules
#
# This is the kernel-side half of the snxsc_l3 deployment. Pair it
# with setup-services.sh (or the docker/ set) for the userspace half.
# On a Proxmox VM both halves run inside the VM. With Docker, only
# setup-driver.sh runs on the host; the container holds the userspace
# half and gets the /dev/sgil1_* nodes via `--device`.
#
# Usage:
#   sudo ./setup-driver.sh
#
# What it does:
#   1. apt: build-essential, kernel headers, dkms.
#   2. Stages src/ under /usr/src/sgil1-5.0/, drops dkms.conf alongside,
#      and registers it with DKMS (so future kernel upgrades rebuild
#      the module automatically).
#   3. Installs the udev rules from src/ to /etc/udev/rules.d/ and
#      sgil1-modules-load.conf to /etc/modules-load.d/.
#   4. modprobe sgil1 right now.

set -euo pipefail

# ----------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------
DRIVER_VERSION="5.0"
DRIVER_SRC_DIR="/usr/src/sgil1-${DRIVER_VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/src"
DKMS_CONF="${SCRIPT_DIR}/dkms.conf"
MODULES_LOAD_CONF="${SCRIPT_DIR}/sgil1-modules-load.conf"

# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------
log()  { printf '\033[1;34m[+]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ----------------------------------------------------------------------
# Sanity
# ----------------------------------------------------------------------
[[ $EUID -eq 0 ]] || die "This script must be run as root (try: sudo $0)"

[[ -d "$SRC_DIR" ]] || die "Source directory not found: $SRC_DIR"
[[ -r "$SRC_DIR/sgil1.c" ]] || die "Missing $SRC_DIR/sgil1.c"
[[ -r "$SRC_DIR/sgil1.h" ]] || die "Missing $SRC_DIR/sgil1.h"
[[ -r "$SRC_DIR/Makefile" ]] || die "Missing $SRC_DIR/Makefile"
[[ -r "$SRC_DIR/99-sgil1.rules" ]] || die "Missing $SRC_DIR/99-sgil1.rules"
[[ -r "$DKMS_CONF" ]] || die "Missing $DKMS_CONF"
[[ -r "$MODULES_LOAD_CONF" ]] || die "Missing $MODULES_LOAD_CONF"

log "Source dir:        $SRC_DIR"
log "DKMS config:       $DKMS_CONF"
log "modules-load conf: $MODULES_LOAD_CONF"

# ----------------------------------------------------------------------
# Packages
# ----------------------------------------------------------------------
export DEBIAN_FRONTEND=noninteractive
log "apt-get update…"
apt-get update -qq

log "Installing build dependencies…"
HEADERS_PKG="linux-headers-$(uname -r)"
if ! apt-cache show "$HEADERS_PKG" >/dev/null 2>&1; then
    warn "$HEADERS_PKG not in apt cache; falling back to linux-headers-amd64"
    HEADERS_PKG="linux-headers-amd64"
fi
apt-get install -y -qq build-essential "$HEADERS_PKG" dkms

# ----------------------------------------------------------------------
# Stage source + DKMS register/build/install
# ----------------------------------------------------------------------
log "Staging driver source in $DRIVER_SRC_DIR …"
mkdir -p "$DRIVER_SRC_DIR"
cp -a "$SRC_DIR/." "$DRIVER_SRC_DIR/"
install -m 0644 "$DKMS_CONF" "$DRIVER_SRC_DIR/dkms.conf"

if dkms status -m sgil1 -v "$DRIVER_VERSION" 2>/dev/null | grep -q .; then
    log "DKMS already has sgil1/${DRIVER_VERSION} registered; removing for a clean rebuild…"
    dkms remove -m sgil1 -v "$DRIVER_VERSION" --all || true
fi

log "DKMS add/build/install for sgil1/${DRIVER_VERSION}…"
dkms add     -m sgil1 -v "$DRIVER_VERSION"
dkms build   -m sgil1 -v "$DRIVER_VERSION"
dkms install -m sgil1 -v "$DRIVER_VERSION"

# ----------------------------------------------------------------------
# udev + modules-load + load now
# ----------------------------------------------------------------------
log "Installing udev rules + modules-load.d entry…"
install -m 0644 "$SRC_DIR/99-sgil1.rules"  /etc/udev/rules.d/99-sgil1.rules
install -m 0644 "$MODULES_LOAD_CONF"       /etc/modules-load.d/sgil1.conf

udevadm control --reload-rules
udevadm trigger || true

log "Loading sgil1 kernel module…"
if ! lsmod | grep -q '^sgil1\b'; then
    modprobe sgil1 || warn "modprobe sgil1 failed — check 'dmesg | tail' (the L1 may not be plugged in yet; the udev rules will load it on plug)."
else
    log "sgil1 already loaded."
fi

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
cat <<EOF

==========================================================================
  Driver installed.

  Module:   sgil1 ${DRIVER_VERSION} via DKMS
            ($(dkms status -m sgil1 -v ${DRIVER_VERSION} 2>/dev/null | head -1))
  Devices:  /dev/sgil1_cs, /dev/sgil1_0..39
            (created by udev once the module is loaded)

  Next:     run setup-services.sh to install the userspace tools and
            create the l2 user + service. (Or build the docker image
            and pass the device nodes through with --device.)

  Useful:
      lsmod | grep sgil1
      dmesg | grep sgil1
      ls -l /dev/sgil1_*
      dkms status -m sgil1
==========================================================================
EOF
