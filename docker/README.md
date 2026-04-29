# snxsc_l3 in a Docker container

## HOWTO:

  1. Place snxsc_l3.tar.gz next to this file (the Dockerfile reads it from the build context).
  2. docker compose build
  3. docker compose up -d
  4. telnet <docker-host>


See repo's README for more info

## AI Written overview below:

The userspace half of the SGI L3 controller suite, packaged for Docker.
The container exposes the L2 console of an SGI 3000-series chassis over
**telnet on port 23 with auto-login** as the `l2` user (no credentials),
plus the two SMP ports (8001 and 8003) the `l2` daemon binds at startup.

This is the userspace half only — the `sgil1` kernel driver still has to
live on the host (containers share the host kernel). The companion
`driver/` set is what gets you `/dev/sgil1_cs` and `/dev/sgil1_0..39` on
the host. The container's job is to consume those device nodes, run the
2008 32-bit `l2` daemon against them, and accept telnet connections.

## What's in the box

```
docker/
├── README.md            (this file)
├── Dockerfile           (the image recipe)
├── docker-compose.yml   (one way to run it)
├── run.sh               (the other way to run it)
├── entrypoint.sh        (container PID-1 — runs inetd + l2)
└── files/
    ├── l2-launch        (stdin-fifo wrapper for the l2 daemon)
    ├── l2-autologin     (telnetd's `-E` helper for credential-free login)
    └── l2term-shell     (kiosk-shell wrapper for the l2 user)
```

## Prerequisites

- Docker 20.10+ on the host (any modern version).
- The `sgil1` driver loaded on the host (`driver/setup-driver.sh`, or
  the equivalent on a non-Debian host).
- `/dev/sgil1_cs` visible on the host (`ls /dev/sgil1_*`).
- The `snxsc_l3.tar.gz` application bundle, with the original layout
  (`snxsc_l3/stand/sysco/{bin,lib}/`).

## Build

```sh
cp /path/to/snxsc_l3.tar.gz .
docker build -t snxsc-l3 .
```

The Dockerfile reads `snxsc_l3.tar.gz` straight from the build context
and extracts it into the image. Other tar formats work too — `tar -xf`
auto-detects compression — but the file has to be named
`snxsc_l3.tar.gz` because that's what the `COPY` statement looks for.
Rename your archive if needed.

## Run

Two paths.

### Compose (heavy-handed device passthrough, simpler)

```sh
docker compose up -d
```

This bind-mounts the host's `/dev` into the container and grants a
cgroup rule for major 10 (misc-class char devices, the class
`/dev/sgil1_*` belongs to). No need to enumerate 41 device nodes; the
trade-off is that the container sees the host's whole `/dev`. The
cgroup rule still keeps it from being able to *use* anything outside
major 10, so this is narrower than `--privileged`.

State (`/home/l2/.l2_NVRAM` etc.) persists in a named volume
`l2-state`. Wipe with `docker compose down -v`.

### `run.sh` (per-device passthrough, more surgical)

```sh
./run.sh
```

Builds the `--device=` flag list dynamically (only nodes that actually
exist on the host get passed) and `docker run`s with explicit
passthrough. No bind mount of `/dev`, no cgroup-class grant. State
still persists in the same `l2-state` named volume.

## Connecting

```sh
telnet <docker-host>
```

That's the whole login flow — TCP connection in, L2 console prompt out.
Port 23 → `inetd` → `in.telnetd -E /usr/local/sbin/l2-autologin`
→ `login -f l2` → l2term wrapper → `/opt/snxsc_l3/bin/l2term`
→ UNIX socket to the running daemon.

The two SMP ports are exposed at the same time, point your client at
`<docker-host>:8001` and `:8003` directly.

## Logs

```sh
docker logs -f l2
```

Captures both `inetd` startup and the daemon's stdout/stderr — same
content `journalctl -u l2` would show on a non-containerized install.

## State

The `l2` user's home is `/home/l2/` inside the container, and the
daemon's working directory (set in `entrypoint.sh`) is also `/home/l2`.
That's where `.l2_NVRAM` and any other state files live.

By default both `docker-compose.yml` and `run.sh` mount a named volume
`l2-state` at `/home/l2`. Stop and recreate the container without
losing the L2's saved configuration:

```sh
docker compose down       # leaves the volume intact
docker compose up -d      # picks up the existing /home/l2 contents
```

To wipe state explicitly:

```sh
docker compose down -v    # -v removes named volumes too
# or for run.sh:
docker rm -f l2 && docker volume rm l2-state
```

## Troubleshooting

### Container exits with `/dev/sgil1_cs is not present`

Device passthrough isn't reaching the container. Check the host first:

```sh
ls -l /dev/sgil1_*
lsmod | grep sgil1
```

If the host doesn't have the nodes, run `driver/setup-driver.sh` on
the host. If the host has them but the container doesn't, your run
arguments are missing the `--device=` flags (or the `/dev` mount and
cgroup rule).

### Telnet connects but immediately disconnects

`docker logs l2` will say why. Common causes:

- The l2 daemon couldn't open `/dev/sgil1_cs` (permissions; check the
  cgroup rule or `--device` passthrough).
- The l2 daemon exited with status 2 right after the disconnect —
  expected behavior in some flows; the unit will restart and the next
  `telnet` will work.

### Container restarts every few seconds

The l2 daemon is exiting on startup before tini even gets to delegate.
`docker logs l2 --tail 50` shows the error. If the cause is "no L1
hardware connected" — that's expected; fix the upstream USB
connection, or temporarily `docker stop l2` until the L1 is back.

### Telnet times out instead of refusing

Port 23 isn't published. Check `docker port l2` — should show
`23/tcp -> 0.0.0.0:23`. If not, fix the `ports:` block in your
compose file or the `-p 23:23` flag in `run.sh`.

## Security

Same caveat as the bare-metal deployment: telnet is plaintext and the
`-E /usr/local/sbin/l2-autologin` flag means **anyone reachable on
TCP/23 becomes the `l2` user with no credentials**. Run this only on a
trusted management network. If you need to expose it more broadly,
front it with a VPN or SSH tunnel rather than touching the auto-login
configuration.

## What's missing (vs. the bare-metal `services` set)

Deliberately:

- No SSH (`openssh-server` is not installed).
- No password set on the `l2` user (account is locked; only `login -f`
  via the autologin helper gets in).
- No systemd. `tini` is PID 1, the entrypoint script supervises `inetd`
  and the `l2` daemon. When either dies the container exits and the
  Docker restart policy brings it back.

If you need any of those, use the `services/` set on a VM or bare metal.
