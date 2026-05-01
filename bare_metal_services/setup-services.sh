#!/usr/bin/env bash
#
# setup-services.sh — install the snxsc_l3 userspace bundle (l2,
# l2term, ...), create the l2 user + login chain, and stand up the
# systemd service + telnet/SSH access on Debian 13.
#
# This is the userspace half. Pair it with setup-driver.sh for the
# kernel-side half. See setup-driver.sh for the host-vs-VM-vs-Docker
# split (the kernel module lives where the kernel runs; this script
# always runs where the application runs).
#
# Usage:
#   sudo ./setup-services.sh [APP_ARCHIVE]
# Default:
#   APP_ARCHIVE=./snxsc_l3.tar.gz
#
# APP_ARCHIVE may be a .tar or .tar.gz (or any other format `tar -xf`
# auto-detects). Expected layout inside the archive:
#     snxsc_l3/stand/sysco/bin/   (l2, l2term, ...)
#     snxsc_l3/stand/sysco/lib/   (libsnxsc.linux.so)
#     snxsc_l3/usr/man/           (optional man pages)
#
# What it does:
#   1. Enables i386 multiarch and installs:
#        - libc6:i386          (the userspace binaries are 32-bit ELF
#          from 2008; no other libs are needed by l2/l2term — see
#          `objdump -p ... | grep NEEDED`)
#        - openssh-server                  (SSH access)
#        - openbsd-inetd, inetutils-telnetd (telnet access)
#   2. Extracts the application to /opt/snxsc_l3/{bin,lib}.
#   3. Symlinks /usr/tmp -> /tmp (l2 hardcodes /usr/tmp/UNIX_<pid>).
#   4. Installs /usr/local/bin/l2term wrapper, registers in /etc/shells.
#   5. Creates user `l2` with the wrapper as shell, prompts for password.
#   6. Installs and starts l2.service (Restart=always).
#   7. Installs the telnet auto-login helper at
#      /usr/local/sbin/l2-autologin and wires `-E <helper>` into the
#      telnet line of /etc/inetd.conf so telnet drops into the l2
#      console with no prompt.
#   8. Enables openbsd-inetd and ssh.
#
# Pre-req: setup-driver.sh has installed the sgil1 kernel module,
# OR the device nodes /dev/sgil1_cs + /dev/sgil1_<N> are otherwise
# present (e.g. Docker `--device` passthrough).

set -euo pipefail

# ----------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------
APP_ARCHIVE="${1:-./snxsc_l3.tar.gz}"

INSTALL_PREFIX="/opt/snxsc_l3"
L2_USER="l2"
L2_HOME="/home/${L2_USER}"
L2_SHELL="/usr/local/bin/l2term"
AUTOLOGIN="/usr/local/sbin/l2-autologin"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FILES_DIR="${SCRIPT_DIR}/files"

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

APP_ARCHIVE="$(readlink -f -- "$APP_ARCHIVE")"
[[ -r "$APP_ARCHIVE" ]] || die "Application archive not found or unreadable: $APP_ARCHIVE"
[[ -r "$FILES_DIR/l2.service" ]] || die "Missing $FILES_DIR/l2.service"
[[ -r "$FILES_DIR/l2-launch" ]] || die "Missing $FILES_DIR/l2-launch"
[[ -r "$FILES_DIR/l2term-shell" ]] || die "Missing $FILES_DIR/l2term-shell"
[[ -r "$FILES_DIR/l2-autologin" ]] || die "Missing $FILES_DIR/l2-autologin"

log "App archive:    $APP_ARCHIVE"
log "Install prefix: $INSTALL_PREFIX"

# Helpful but non-fatal: warn if the kernel-side prereq looks missing.
if [[ ! -e /dev/sgil1_cs ]]; then
    warn "/dev/sgil1_cs not present — has setup-driver.sh been run, or are the device nodes passed in (Docker --device)? l2.service will fail to talk to the L1 until that's resolved."
fi

# ----------------------------------------------------------------------
# Packages + i386 multiarch
# ----------------------------------------------------------------------
log "Enabling i386 multiarch (the userspace binaries are 32-bit ELF)…"
if ! dpkg --print-foreign-architectures | grep -qx i386; then
    dpkg --add-architecture i386
fi

export DEBIAN_FRONTEND=noninteractive
log "apt-get update…"
apt-get update -qq

log "Installing packages…"
apt-get install -y -qq \
    libc6:i386 \
    openssh-server \
    openbsd-inetd \
    inetutils-telnetd

# ----------------------------------------------------------------------
# Application: extract and flatten to /opt/snxsc_l3/{bin,lib}
# ----------------------------------------------------------------------
# tar -xf auto-detects compression (.tar / .tar.gz / .tar.xz / etc.).
log "Installing snxsc_l3 application to $INSTALL_PREFIX …"
mkdir -p "$INSTALL_PREFIX"
tar -xf "$APP_ARCHIVE" -C "$INSTALL_PREFIX"

# Expected layout from the original snxsc_l3 bundle: snxsc_l3/stand/sysco/{bin,lib}
# and snxsc_l3/usr/man. Flatten to $INSTALL_PREFIX/{bin,lib,share/man}.
if [[ -d "$INSTALL_PREFIX/snxsc_l3/stand/sysco/bin" ]]; then
    rm -rf "$INSTALL_PREFIX/bin" "$INSTALL_PREFIX/lib"
    mv "$INSTALL_PREFIX/snxsc_l3/stand/sysco/bin" "$INSTALL_PREFIX/bin"
    mv "$INSTALL_PREFIX/snxsc_l3/stand/sysco/lib" "$INSTALL_PREFIX/lib"
    if [[ -d "$INSTALL_PREFIX/snxsc_l3/usr/man" ]]; then
        mkdir -p "$INSTALL_PREFIX/share"
        rm -rf "$INSTALL_PREFIX/share/man"
        mv "$INSTALL_PREFIX/snxsc_l3/usr/man" "$INSTALL_PREFIX/share/man"
    fi
    rm -rf "$INSTALL_PREFIX/snxsc_l3"
elif [[ ! -x "$INSTALL_PREFIX/bin/l2" ]]; then
    die "Archive extracted but $INSTALL_PREFIX/bin/l2 is not present.
Expected the archive to contain snxsc_l3/stand/sysco/{bin,lib}/. Got:
$(ls -la "$INSTALL_PREFIX")"
fi

# The binaries shipped non-executable in the original bundle.
chmod 0755 "$INSTALL_PREFIX/bin/"*
chmod 0644 "$INSTALL_PREFIX/lib/"*.so 2>/dev/null || true

# ----------------------------------------------------------------------
# /usr/tmp must exist (l2 creates UNIX socket there)
# ----------------------------------------------------------------------
if [[ ! -e /usr/tmp ]]; then
    log "Creating /usr/tmp -> /tmp symlink (l2 hardcodes /usr/tmp/UNIX_<pid>)…"
    ln -s /tmp /usr/tmp
elif [[ -L /usr/tmp ]]; then
    log "/usr/tmp is already a symlink to $(readlink /usr/tmp); leaving alone."
else
    log "/usr/tmp exists as a real directory; leaving alone."
fi

# ----------------------------------------------------------------------
# l2term login-shell wrapper + /etc/shells
# ----------------------------------------------------------------------
log "Installing $L2_SHELL wrapper…"
install -m 0755 "$FILES_DIR/l2term-shell" "$L2_SHELL"

if ! grep -qxF "$L2_SHELL" /etc/shells; then
    log "Adding $L2_SHELL to /etc/shells (so login/telnet accept it)…"
    printf '%s\n' "$L2_SHELL" >> /etc/shells
fi

# ----------------------------------------------------------------------
# User
# ----------------------------------------------------------------------
if id -u "$L2_USER" >/dev/null 2>&1; then
    log "User '$L2_USER' already exists; updating shell to $L2_SHELL …"
    usermod -s "$L2_SHELL" "$L2_USER"
else
    log "Creating user '$L2_USER' (home=$L2_HOME, shell=$L2_SHELL)…"
    useradd --create-home --home-dir "$L2_HOME" --shell "$L2_SHELL" "$L2_USER"
fi

if passwd --status "$L2_USER" 2>/dev/null | awk '{print $2}' | grep -qE '^(L|NP)$'; then
    if [[ -t 0 && -t 1 ]]; then
        log "Set a password for the '$L2_USER' user (used for SSH login):"
        passwd "$L2_USER"
    else
        GEN_PW="$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16)"
        echo "${L2_USER}:${GEN_PW}" | chpasswd
        warn "No TTY available — generated random password for '$L2_USER': $GEN_PW"
        warn "Change it now with: passwd $L2_USER"
    fi
else
    log "User '$L2_USER' already has a password set; leaving it unchanged."
fi
# quiet the login
touch /home/$L2_USER/.hushlogin
chown $L2_USER:$L2_USER /home/$L2_USER/.hushlogin

# ----------------------------------------------------------------------
# l2.service (with the l2-launch stdin-fifo wrapper it ExecStart=s)
# ----------------------------------------------------------------------
log "Installing l2 stdin-fifo launcher /usr/local/sbin/l2-launch …"
install -m 0755 "$FILES_DIR/l2-launch" /usr/local/sbin/l2-launch

log "Installing systemd unit l2.service …"
install -m 0644 "$FILES_DIR/l2.service" /etc/systemd/system/l2.service
systemctl daemon-reload
systemctl enable l2.service

log "Starting l2.service …"
if ! systemctl restart l2.service; then
    warn "l2.service failed to start. Inspect with: journalctl -u l2.service -n 50"
fi

# ----------------------------------------------------------------------
# Telnet: enable line, install autologin helper, wire -E flag
# ----------------------------------------------------------------------
# inetutils-telnetd's postinst registers the telnet line in
# /etc/inetd.conf but it can land disabled in two different ways:
#   - openbsd-inetd convention: line prefixed with `#<off># `
#   - inetutils-inetd convention: line has a leading space (col 1 is
#     reserved for active entries)
# Handle both.
log "Enabling telnet entry in /etc/inetd.conf …"
update-inetd --enable telnet 2>/dev/null || true
if [[ -f /etc/inetd.conf ]]; then
    sed -i -E 's/^[[:space:]]+(telnet[[:space:]])/\1/' /etc/inetd.conf
    
fi

log "Installing telnet auto-login helper $AUTOLOGIN …"
install -m 0755 "$FILES_DIR/l2-autologin" "$AUTOLOGIN"

# Append `-E <helper>` to the telnetd command in /etc/inetd.conf so
# in.telnetd skips the username/password prompt and exec's our helper,
# which `login -f l2`'s straight into the L2 console. Idempotent —
# only edits the line if -E isn't already present.
log "Wiring telnetd -E $AUTOLOGIN in /etc/inetd.conf …"
sed -i -E "/^telnet[[:space:]]/ { /-E[[:space:]]/!s|\$| -h -E ${AUTOLOGIN}| }" /etc/inetd.conf

log "Enabling openbsd-inetd (for telnet)…"
systemctl enable openbsd-inetd
systemctl restart openbsd-inetd || warn "openbsd-inetd failed to (re)start; check 'systemctl status openbsd-inetd'."

# ----------------------------------------------------------------------
# SSH
# ----------------------------------------------------------------------
log "Enabling SSH (ssh.service) …"
systemctl enable ssh
systemctl start ssh || true

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
HOST_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
[[ -z "$HOST_IP" ]] && HOST_IP="<this-host>"

cat <<EOF

==========================================================================
  Services installed.

  App:      $INSTALL_PREFIX/{bin,lib}
  Service:  l2.service ($(systemctl is-active l2.service 2>/dev/null || echo inactive))
  User:     $L2_USER (shell: $L2_SHELL)

  Connect to the L2 console:
      telnet $HOST_IP            (auto-login as $L2_USER, no prompt)
      ssh    $L2_USER@$HOST_IP   (password required)

  Useful:
      systemctl status l2
      journalctl -u l2 -f
      ls -l /dev/sgil1_*

  NOTE: telnet auto-login means anyone reachable on TCP/23 becomes the
  '$L2_USER' user with no credentials. Firewall accordingly. Disable later:
      systemctl disable --now openbsd-inetd

  If l2.service is failing and /dev/sgil1_cs is missing, run
  setup-driver.sh first (or check your container/VM device passthrough).
==========================================================================
EOF
