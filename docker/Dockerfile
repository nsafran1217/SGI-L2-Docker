# syntax=docker/dockerfile:1
#
# snxsc_l3 telnet container
#
# Exposes the L2 console of an SGI 3000-series chassis over TCP/23 (telnet,
# auto-login as the `l2` user, no credentials), plus the two SMP ports
# (8001 and 8003) the l2 daemon binds at startup.
#
# Build context expectation: snxsc_l3.tar.gz (or .tar / .tar.xz / etc. —
# `tar -xf` autodetects compression) sitting next to this Dockerfile,
# containing snxsc_l3/stand/sysco/{bin,lib}/.
#
# This image is the userspace half. The kernel-side half (sgil1 driver via
# DKMS) lives on the *host*; the container needs the resulting device nodes
# (/dev/sgil1_cs and /dev/sgil1_0..39) passed through at run time.

FROM debian:13-slim

# Avoid interactive apt prompts (no tzdata config dialog, etc.)
ENV DEBIAN_FRONTEND=noninteractive

# Runtime dependencies. Specifically NOT installed:
#   - openssh-server: the container is telnet-only by design.
#   - dkms / build-essential / kernel headers: the driver is the host's job.
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        libc6:i386 \
        openbsd-inetd \
        inetutils-telnetd \
        tini \
        util-linux \
 && rm -rf /var/lib/apt/lists/*

# Application: extract and flatten to /opt/snxsc_l3/{bin,lib}.
COPY snxsc_l3.tar.gz /tmp/snxsc_l3.tar.gz
RUN mkdir -p /opt/snxsc_l3 \
 && tar -xf /tmp/snxsc_l3.tar.gz -C /opt/snxsc_l3 \
 && mv /opt/snxsc_l3/snxsc_l3/stand/sysco/bin /opt/snxsc_l3/bin \
 && mv /opt/snxsc_l3/snxsc_l3/stand/sysco/lib /opt/snxsc_l3/lib \
 && rm -rf /opt/snxsc_l3/snxsc_l3 /tmp/snxsc_l3.tar.gz \
 && chmod 0755 /opt/snxsc_l3/bin/*

# /usr/tmp -> /tmp (the l2 binary hardcodes /usr/tmp/UNIX_<pid> for IPC).
RUN ln -sf /tmp /usr/tmp

# Helper scripts: kiosk-shell wrapper, telnet auto-login, stdin-fifo
# launcher. See files/*.sh and the main README for the full story.
COPY files/l2term-shell    /usr/local/bin/l2term
COPY files/l2-autologin    /usr/local/sbin/l2-autologin
COPY files/l2-launch       /usr/local/sbin/l2-launch
RUN chmod 0755 /usr/local/bin/l2term \
                /usr/local/sbin/l2-autologin \
                /usr/local/sbin/l2-launch \
 && echo /usr/local/bin/l2term >> /etc/shells

# l2 user — locked password, kiosk shell. `login -f` (used by the autologin
# helper, invoked from in.telnetd via the -E flag) bypasses authentication,
# so a usable password isn't needed.
RUN useradd --create-home --home-dir /home/l2 \
            --shell /usr/local/bin/l2term l2

# Wire the telnet line in /etc/inetd.conf:
#   - Enable it (Debian's inetutils-telnetd registers it #<off># by default).
#   - Strip any leading whitespace (Debian 13 packaging quirk).
#   - Append `-E /usr/local/sbin/l2-autologin` so connections drop straight
#     into the l2 user with no prompt.
RUN update-inetd --enable telnet 2>/dev/null || true \
 && sed -i -E 's/^[[:space:]]+(telnet[[:space:]])/\1/' /etc/inetd.conf \
 && sed -i -E '/^telnet[[:space:]]/ { /-E[[:space:]]/!s|$| -E /usr/local/sbin/l2-autologin| }' /etc/inetd.conf

# Entrypoint: launches inetd (foreground) + the l2 daemon under the l2 user.
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 0755 /usr/local/bin/entrypoint.sh

# Telnet, plus the two SMP ports the l2 daemon binds at startup
# ("INFO: SMP listening on port: 8001" / "8003").
EXPOSE 23 8001 8003

# tini handles signal forwarding and zombie reaping. -g delivers signals to
# the entire process group, so SIGTERM from `docker stop` reaches both inetd
# and l2 (rather than just whichever happens to be the foreground child).
ENTRYPOINT ["/usr/bin/tini", "-g", "--"]
CMD ["/usr/local/bin/entrypoint.sh"]
