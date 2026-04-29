#!/bin/bash
# /usr/local/bin/entrypoint.sh — the container's "init script".
#
# Run by tini as the foreground process. Two jobs:
#
#   1. inetd  — listens on port 23, dispatches incoming telnet connections
#               to in.telnetd (which `-E`s into the l2-autologin helper,
#               which `login -f l2`s straight into the l2 console).
#   2. l2     — the system-controller daemon itself, as user `l2`.
#               Wrapped via /usr/local/sbin/l2-launch to give it a
#               blocking stdin (otherwise it spins at 100% CPU on
#               /dev/null EOFs — see the launcher's comments).
#
# When l2 exits, this script exits, tini exits, the container exits.
# `--restart=always` (or compose's `restart: always`) brings it right back.

set -euo pipefail

# Pre-flight: the L1 device nodes need to be visible inside the container.
# They're created by the host's sgil1 kernel module and passed through via
# `--device=` flags or a `/dev` bind mount.
if [[ ! -e /dev/sgil1_cs ]]; then
    cat >&2 <<'EOF'
[!] /dev/sgil1_cs is not present inside the container.

The L1 device nodes are created by the sgil1 kernel module on the HOST,
and need to be passed through to the container. Two ways:

  1. Per-device (more surgical):
       docker run \
         --device=/dev/sgil1_cs \
         --device=/dev/sgil1_0 ... --device=/dev/sgil1_39 \
         ...

  2. Bulk (heavier-handed):
       docker run \
         -v /dev:/dev \
         --device-cgroup-rule='c 10:* rmw' \
         ...

If /dev/sgil1_cs is missing on the host too, run setup-driver.sh on the
host first (the kernel module lives where the kernel runs).
EOF
    exit 1
fi

echo "[+] Starting openbsd-inetd (foreground)…"
# -i = do not daemonize; stay in the foreground so we (and tini) can supervise.
/usr/sbin/inetd -i &

# Brief grace period so inetd binds before we hand off the foreground.
sleep 0.3

echo "[+] Starting l2 daemon as user 'l2'…"
# Switch UID/GID via setpriv (no PAM, unlike runuser/su) and pin cwd +
# HOME to /home/l2 so the daemon's NVRAM file lands in the right place.
cd /home/l2
export HOME=/home/l2 USER=l2 LOGNAME=l2
exec setpriv --reuid=l2 --regid=l2 --init-groups \
     /usr/local/sbin/l2-launch
