# ov roadmap

## Vision

Someone's dev server won't start because "address already in use". They type
`ov :3000`, see that a stale `node` from yesterday holds it, and kill it, or
open `ov` and watch every listener on the machine live, see which ones are
reachable from the network, and stop any of them with one key.

`ov` is not a firewall tool, packet capture, a remote monitor, a service
manager or a resource monitor. It covers this repository only.

Checked on 2026-11-26

## Focus

Everything in the plan is built and runs on macOS; the tests pass on macOS
and Linux. ov is at proof of concept: the author has not used it day to day
yet. What takes it to MVP:

1. **libmc merged:** the `procinfo` module is in
   [sangar/libmc#77](https://github.com/sangar/libmc/pull/77) and ov pins
   its branch commit. See [Port to process](#port-to-process).
2. **Daily use:** a week of using `ov` instead of `lsof -i`, fixing what
   gets in the way. See [Screen view](#screen-view).

## Principles

- **The port is the handle.** Every command accepts `:port` where it takes a
  PID; that's how people think about the problem.
- **No new dependencies for side features.** Docker is read through
  `docker ps`, not its API.
- **Degrade, don't fail.** Rows ov can't map without root are marked, and the
  container column hides when Docker isn't there.
- **Never hold the port you look at.** Probes connect to a port, never bind
  it, so `ov wait` cannot make a starting server fail.
- **TCP by default.** UDP sockets show with `-u`, or `u` in the screen view.

## Where we are

`ov` builds, its tests pass under the sanitizers on macOS and in a `gcc:14`
container on Linux, and `./nob ci` covers check, tests, the release build and
the cross builds. libmc's `procinfo` module passes libmc's full suite on both
systems.

On the way:

- A libmc test killed its own shell: `pty_close` zeroes the pid, and
  `killpg(0)` signals the caller's group. `process_signal` and
  `process_signal_group` now refuse pids and groups below 1.
- The current Docker context was OrbStack while Docker Desktop held the
  ports, so ov reads `docker ps` from every context.
- Containers are matched on address and port, not port alone: a `ruby` on
  127.0.0.1:3000 was labelled with the container on 0.0.0.0:3000.
- A review by a second agent found six defects; five are fixed. A kill
  confirmed after the process exited now checks the listener again, so a
  reused pid is never signalled. Pid 1 and group 1 are refused. macOS
  reports a dual-stack listener as `::`, as Linux does. `kill -g` signals
  each group once. Container names no longer point into freed memory. The
  sixth, Docker blocking the screen, is under [Screen view](#screen-view).

## Port to process

Now. Bar: the same answers as `lsof -iTCP -sTCP:LISTEN` and `ss -ltnp`,
without needing them installed.

- [x] **Project builds and checks locally.** `./nob ci` runs the modern-c
      check, the tests, the release build and zig cross builds for
      linux-x86_64 and linux-aarch64; macos-aarch64 compiles without
      linking, as zig lacks the SDK.
- [ ] **libmc: sockets with their processes, process details, kill
      signals.** `mc/platform/procinfo.h`, `SIGNAL_KILL` and
      `process_signal_group`, on libmc's `ports` branch in
      `../libmc-ports`, tested on macOS and Linux.
      1. [x] **Open the pull request** to libmc and pin its branch commit:
             [sangar/libmc#77](https://github.com/sangar/libmc/pull/77),
             opened on 26 November.
      2. [ ] **Re-pin** `deps.lock` to the merge commit on `master`.

## Commands

Done on 26 November.

- [x] **Answer "who holds this port".** `ov :3000` and `ov ports
      [-u] [--json]`; when nothing readable holds the port but it accepts
      connections, it says so and suggests sudo.
- [x] **Stop it.** `ov kill [-9] [-g] <pid|:port>`; `-g` refuses ov's own
      process group.
- [x] **Wait for a port in scripts.** `ov wait :5432 [--closed]
      [--timeout 30s]`.
- [x] **Hand out a free port.** `ov free`.
- [x] **Show which container owns a port.** From `docker ps` in every
      Docker context.

## Screen view

Built; now in daily use. Bar: the look and feel of `btop`, scoped to ports:
rounded boxes with the title and key hints drawn in the border, colour by
meaning, mouse and keyboard alike. Built on libmc's `mc/term/*`.

- [x] **Live ports list** with filter, sort by column (`←`/`→`, `r`, or a
      click on the header), click and wheel to select.
- [x] **Flag ports reachable from the network:** exposed addresses in amber,
      loopback in blue.
- [x] **Kill from the list** with `k`, `K` and `g`, each confirmed first, as
      btop asks.
- [x] **Inspect a process** in a panel above the list.
- [x] **Help and themes.** `?` overlay; light or dark from `COLORFGBG`.
- [ ] **Keep the screen smooth while Docker answers.** Reading every Docker
      context takes about half a second on the author's machine and blocks
      the screen every five seconds; move it to a thread that sends its
      result through a queue.
- [ ] **Check the mouse by hand.** Clicks and the wheel are untested; tmux
      cannot send them.

## Feature ideas

**Later**

- Detect the terminal's background with an OSC 11 query, for terminals that
  do not set `COLORFGBG`, such as Terminal.app; a libmc `terminal.h`
  addition.

**Not doing**

- Service management (systemd, launchd): a different job; `systemctl` and
  `launchctl` do it.
- CPU and memory figures: `htop` and Activity Monitor do it.
- Firewall rules, packet capture, remote hosts: other tools.
- Windows: libmc doesn't support it yet.

## Open questions

None.
