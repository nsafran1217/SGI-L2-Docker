# SGI L2/L3 system controller — Debian 13 deployment

Use this to install the L3 software "bare metal" or on a VM.

Install the driver first.

## How to:

`./setup-serivices ../snxsc_l3.tar.gz`


See the repo's README for more information


### AI Written overview below:

A modern Debian 13 deployment of the SGI L3 user-space controller suite
(`l2`, `l2term`, and friends from the 2008 `snxsc_l3` bundle), driven by
a modernized version of the `sgil1` USB kernel driver. Lets you reach
the L1 console of an SGI 3000-series chassis from a current Linux host
the same way you would have on the original SGI L3 hardware: log in as
the `l2` user via telnet or SSH and you land in the L2 console.

## Distribution layout

This distribution contains:

```
README.md           This file.
driver.tar.gz       Kernel-side: builds the sgil1 USB driver via DKMS.
services.tar.gz     User-side: app files, systemd unit, l2 user, telnet/SSH.
docker.tar.gz       Userspace half packaged for Docker (telnet only, no SSH).
```

Each tarball is self-contained — its own setup script (or Dockerfile)
plus a `files/` subdirectory of templates and helpers.

You also need two **input archives** that aren't shipped here (they're
your originals):

- The driver source: a tarball of the `sgil1_modern/` tree —
  `sgil1.c`, `sgil1.h`, `Makefile`, `99-sgil1.rules`. Default file
  name: `sgil1_modern.tar.gz` (or `sgil1_modern_tar.gz`; either works).
- The application bundle: a tar(.gz) containing
  `snxsc_l3/stand/sysco/{bin,lib}/` and optionally
  `snxsc_l3/usr/man/`. Default file name: `snxsc_l3.tar.gz`.

## Prerequisites

- Debian 13 (other Debian-family kernels in the 5.15→6.12 range likely
  work for the driver; the user-space half is undemanding).
- Root access on the target host.
- The two input archives above, sitting next to the setup scripts (or
  paths to them passed on the command line).
- Either an SGI L1 USB adapter physically attached to the host, or
  device passthrough into a VM, or `--device` mappings into a
  container — the daemon needs `/dev/sgil1_cs` to exist.

## Installation

### 1. Driver

```sh
tar -xzf driver.tar.gz
cd driver
sudo bash setup-driver.sh /path/to/sgil1_modern.tar.gz
```

Or, if the driver source tarball is in the current directory and named
`sgil1_modern_tar.gz` or `sgil1_modern.tar.gz`, just:

```sh
sudo bash setup-driver.sh
```

What it does:

1. `apt`: `build-essential`, `linux-headers-$(uname -r)`, `dkms`.
2. Stages the source under `/usr/src/sgil1-5.0/` and registers it
   with DKMS. Future kernel upgrades will rebuild the module
   automatically; you don't need to re-run this script unless the
   driver source itself changes.
3. Installs the udev rules (`/etc/udev/rules.d/99-sgil1.rules`) and a
   `/etc/modules-load.d/sgil1.conf` so the module loads at boot.
4. `modprobe sgil1` right now.

After it finishes, you should see:

```sh
$ lsmod | grep sgil1
sgil1                  28672  0
$ ls /dev/sgil1_*
/dev/sgil1_0  /dev/sgil1_1  ...  /dev/sgil1_39  /dev/sgil1_cs
```

If you see only `sgil1_cs` (or nothing), the module loaded fine but
no L1 hardware is connected — that's OK, the daemon will pick the L1
up when it's plugged in.

### 2. Services

```sh
tar -xzf services.tar.gz
cd services
sudo bash setup-services.sh /path/to/snxsc_l3.tar.gz
```

Or just `sudo bash setup-services.sh` if the archive is `./snxsc_l3.tar.gz`.

What it does:

1. Enables `i386` multiarch and installs `libc6:i386` (the original
   binaries are 32-bit ELF), `openssh-server`, `openbsd-inetd`, and
   `inetutils-telnetd`.
2. Extracts the application to `/opt/snxsc_l3/{bin,lib}`.
3. Symlinks `/usr/tmp` → `/tmp` (the `l2` binary hardcodes
   `/usr/tmp/UNIX_<pid>` for its IPC socket).
4. Drops a `/usr/local/bin/l2term` shell wrapper that just `exec`s
   the real binary (it's a kiosk-style "shell" for the `l2` user).
5. Adds the wrapper to `/etc/shells` so `login`/telnet accept it.
6. Creates user `l2` with that wrapper as their shell. Prompts for a
   password (used for SSH). If there's no TTY (you piped the script
   in), it generates one and prints it.
7. Installs `/usr/local/sbin/l2-launch`, a tiny stdin-fifo wrapper
   that prevents `l2` from spinning at 100% CPU when run as a
   daemon — see Troubleshooting below.
8. Installs and starts `l2.service` (systemd, `Restart=always`).
9. Enables the telnet entry in `/etc/inetd.conf` (Debian ships it
   `#<off>#`-disabled by default), adds the `-E /usr/local/sbin/l2-autologin`
   flag so connections drop into the `l2` user with no prompt, and
   restarts `openbsd-inetd`.
10. Enables `ssh` and `openbsd-inetd` so they come up at boot.

The order of the two scripts doesn't matter at install time — but the
driver must be loaded before `l2.service` will actually be useful.
`setup-services.sh` warns if `/dev/sgil1_cs` is missing; it'll still
install everything and let you fix the prerequisite later.

## Connecting to the L2 console

Three ways in:

### telnet (zero-credential)

```sh
telnet l2-host
```

Drops straight into the L2 console. No username, no password — `inetd`
calls `in.telnetd` with `-E /usr/local/sbin/l2-autologin`, which `exec`s
`/bin/login -f l2`, which skips authentication and runs the user's
shell (the `l2term` wrapper). Plaintext over the wire — only deploy
on a trusted segment. See the security note below.

### SSH (password-authenticated)

```sh
ssh l2@l2-host
```

Prompts for the password set during install. After auth the user's
shell (the `l2term` wrapper) runs and you're in the L2 console.

### Directly on the host

If you're on the host shell already and `l2.service` is running:

```sh
sudo -u l2 /opt/snxsc_l3/bin/l2term
```

To **leave** the L2 console, type `quit` at the `L2>` prompt, or
disconnect the network connection. `l2term` exits and your session
ends; `l2.service` keeps running so others can still connect.

## Operations

### Service state

```sh
systemctl status l2
systemctl status openbsd-inetd
systemctl status ssh
```

### Logs

```sh
journalctl -u l2 -f          # follow live
journalctl -u l2 --since 1h  # last hour
```

`l2`'s own log lines (`INFO: opened USB control /dev/sgil1_cs`, etc.)
appear here, prefixed with the host name and PID.

### State files

Created by `l2` in its working directory, which the unit sets to
`/home/l2/`. The interesting one is:

```
/home/l2/.l2_NVRAM     persisted L2 settings
```

These are owned by the `l2` user. Back up `/home/l2/` if you care
about preserving L2 configuration across rebuilds.

### Restart and reload

```sh
sudo systemctl restart l2
```

The daemon comes back up within a couple of seconds; the systemd unit
sets `Restart=always` so spontaneous exits are handled automatically.

### Disable bits selectively

```sh
sudo systemctl disable --now openbsd-inetd     # turn off telnet
sudo systemctl disable --now ssh               # turn off SSH (don't unless you have console access!)
sudo systemctl disable --now l2                # stop the daemon entirely
```

## Removing

The two halves uninstall independently.

### Services

```sh
sudo systemctl disable --now l2 openbsd-inetd
sudo rm /etc/systemd/system/l2.service
sudo rm /usr/local/sbin/l2-launch /usr/local/sbin/l2-autologin
sudo rm /usr/local/bin/l2term
sudo userdel -r l2                             # removes /home/l2 too
sudo sed -i '\|/usr/local/bin/l2term|d' /etc/shells
sudo update-inetd --disable telnet
sudo rm -rf /opt/snxsc_l3
sudo systemctl daemon-reload
```

### Driver

```sh
sudo dkms remove -m sgil1 -v 5.0 --all
sudo rm -rf /usr/src/sgil1-5.0
sudo rm /etc/udev/rules.d/99-sgil1.rules /etc/modules-load.d/sgil1.conf
sudo rmmod sgil1
sudo udevadm control --reload-rules
```

## Troubleshooting

### `/dev/sgil1_cs` is missing

The kernel module isn't loaded. Check:

```sh
lsmod | grep sgil1
dmesg | grep sgil1
dkms status -m sgil1
```

If DKMS shows the module as installed but `lsmod` doesn't see it,
`sudo modprobe sgil1` and watch `dmesg`. If DKMS shows the build
failed, see the next item.

### DKMS build fails with `make: *** No targets. Stop.`

The driver's Makefile is the dual-mode kbuild pattern (outer branch
recurses into the kernel build tree, inner branch supplies `obj-m`).
DKMS sets `KERNELRELEASE` in the environment before its first `make`
invocation, which makes the Makefile take the inner branch
immediately and abort with no targets.

The shipped `dkms.conf` works around this by stripping `KERNELRELEASE`
from the env (`env -u KERNELRELEASE make ...`). If you've replaced
that file, restore it from `driver/files/dkms.conf` and re-run
`dkms install`.

### `l2.service` is using 100% CPU

The daemon's main loop does a `select()` that includes `stdin`, and
doesn't handle EOF on it. Under systemd, stdin defaults to
`/dev/null`, which reports as always readable and EOFs immediately —
the loop spins.

The fix lives in `/usr/local/sbin/l2-launch`: it sets up a fifo with
no data and one writer reference, then `exec`s `l2` with that fifo
as stdin. Reads block instead of EOFing; `select()` no longer fires
spuriously. Re-installing the launcher and re-starting the unit
should resolve it.

Confirm the diagnosis without touching systemd:

```sh
/opt/snxsc_l3/bin/l2 < /dev/null     # spins
/opt/snxsc_l3/bin/l2                  # works (TTY is the stdin)
```

### `l2.service` exits immediately

Check the journal:

```sh
journalctl -u l2 -n 50
```

Common causes:

- `Permission denied` writing `./.l2_NVRAM` — the unit's
  `WorkingDirectory=` should be `/home/l2`. If it's something the
  `l2` user can't write to, fix it and restart.
- `can't open /dev/sgil1_cs` — driver isn't loaded. See above.
- Status code 2 with no error in the log — usually the previous
  client disconnected. The unit is set to `Restart=always` and will
  come back; if it's flapping every second, that's a different
  problem and you'll see it in `systemctl status`.

### `telnet: Unable to connect to remote host: Connection refused`

Two possibilities:

1. `inetd` isn't listening on port 23. Check
   `ss -tlnp | grep ':23 '`. If empty,
   `sudo systemctl restart openbsd-inetd`.
2. The telnet line in `/etc/inetd.conf` is disabled. Check
   `grep telnet /etc/inetd.conf`. If you see `#<off># telnet ...`,
   re-run `setup-services.sh` (idempotent), or manually:
   ```sh
   sudo update-inetd --enable telnet
   sudo systemctl restart openbsd-inetd
   ```

### Telnet connects, but you get a login prompt instead of dropping in

The `-E` flag is missing from the telnet line in `/etc/inetd.conf`.
Check:

```sh
grep telnet /etc/inetd.conf
```

The line should end with `... /usr/sbin/telnetd -E /usr/local/sbin/l2-autologin`.
Re-run `setup-services.sh` to wire it up correctly.

### `l2term` shows nothing or hangs

`l2term` connects to the daemon's UNIX socket at `/usr/tmp/UNIX_<pid>`.
If the daemon isn't running or the symlink is wrong, `l2term`
silently waits.

```sh
systemctl is-active l2     # should print 'active'
ls -l /usr/tmp             # should be a symlink to /tmp
ls -l /tmp/UNIX_*          # should show a socket owned by user 'l2'
```

## Container and VM deployments

The driver lives wherever the kernel runs; the application lives
wherever it's executed. Match the file sets accordingly.

| Where it lives                                     | Driver runs on …            | Application runs on …                |
| -------------------------------------------------- | --------------------------- | ------------------------------------ |
| Bare-metal Debian 13                               | the host (`driver/`)        | the host (`services/`)               |
| Proxmox VM with USB passthrough into the VM        | inside the VM (`driver/`)   | inside the VM (`services/`)          |
| Docker container                                   | the Docker host (`driver/`) | inside the container (`docker/`)     |

### Docker

`docker.tar.gz` is a turnkey container build. Run `driver/setup-driver.sh`
on the **host** so `/dev/sgil1_cs` and `/dev/sgil1_0..39` exist with the
right permissions. Then:

```sh
tar -xzf docker.tar.gz
cd docker
cp /path/to/snxsc_l3.tar.gz .       # the build context expects this name
docker build -t snxsc-l3 .
docker compose up -d                # or: ./run.sh
```

Telnet to the Docker host's port 23 and you're in. SMP is on 8001 and
8003 (the two ports the daemon binds at startup). See
`docker/README.md` inside the tarball for full details — including the
two device-passthrough strategies (bind-mount `/dev` + cgroup rule, vs
explicit `--device=` per node) and how to persist `/home/l2` across
container recreations.

The container is **telnet-only by design** — no SSH, no password.
Trade-offs documented in `docker/README.md`.

### Proxmox

Forward the L1 USB adapter to the VM in the hardware tab
(`Add → USB Device`, pick by vendor:product). Inside the VM, run
`driver/setup-driver.sh` and `services/setup-services.sh` exactly as on
bare metal.

**Do not** install the driver on the Proxmox host — if both the host
and the VM have `sgil1` available they'll race for the device on plug.
If you've already installed it on the host,
`echo blacklist sgil1 > /etc/modprobe.d/blacklist-sgil1.conf` keeps it
out of the way.

## Security

This deployment is configured for a **trusted management network**.
Two specific things to be aware of:

- **Telnet is plaintext.** The auto-login means every TCP/23
  connection becomes the `l2` user with no credentials. Anyone who
  can route a packet to port 23 has the L2 console. Firewall it,
  put it on a private VLAN, or disable telnet entirely
  (`systemctl disable --now openbsd-inetd`) and use SSH only.
- **The `l2` user has shell access via SSH** with the password set
  at install time. The shell is the kiosk-style `l2term` wrapper —
  `ssh l2@host whoami` doesn't work because the wrapper ignores
  arguments — but it's still a real account with a real password.
  Use a strong password, or replace password auth with key-based
  auth in `sshd_config`.

If you're putting this on the open internet (don't), at minimum:
disable telnet, set `PasswordAuthentication no` in `sshd_config`,
and require key auth for the `l2` user.