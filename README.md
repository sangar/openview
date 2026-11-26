# ov

Shows who is listening on which port and lets you stop it.

```text
ov [-u]                                  screen view: ports, live
ov :3000                                 who holds port 3000; exits 1 if free
ov ports [-u] [--json]                   list listeners
ov kill [-9] [-g] <pid|:port>            SIGTERM, or SIGKILL with -9; -g signals the process group
ov wait :5432 [--closed] [--timeout 30s] wait until the port opens or closes
ov free                                  print an unused port

-u, --udp   include UDP sockets
```

In the screen view: `↑`/`↓` or `j`/`k` select, `g`/`G` jump to the first and
last, `enter` inspects the process, `x` and `X` send SIGTERM and SIGKILL after
asking (`g` in the question signals the process group), `/` filters, `←`/`→`
and `R` sort, `u` shows UDP, `?` lists every key, `q` quits. The keys match
initctl's.
Addresses reachable from the network are amber, loopback blue. Ports that
Docker containers publish show the container's name.

Ports owned by other users need root to map to a process: `sudo ov`.

## Profile

```text
Language:   C23
Compilers:  clang >= 18 (primary), gcc >= 14
Targets:    linux-x86_64, linux-aarch64, macos-aarch64
            windows-x86_64 excluded: libmc does not support Windows yet
            macos-aarch64 cross builds compile without linking: zig lacks the SDK
Level:      Modern C Level 2
Build:      tools/build.c (./nob build|test|check|cross|ci|install|uninstall)
Deps:       libmc, pinned in deps.lock; zig for cross builds, pinned in mise.toml
```

## Build

```bash
cc tools/build.c -o nob
./nob ci
./nob install    # ~/.local/bin/ov, PREFIX and DESTDIR honoured
```
