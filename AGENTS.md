# AGENTS.md

Instructions for AI coding agents working in this repository. Humans should read
[CONTRIBUTING.md](./CONTRIBUTING.md), which carries the full inventory of what already
exists here.

## What this is

Droidspaces is a container runtime. Two halves:

- `src/` is the C backend, statically linked against musl, one binary for Android and Linux.
- `Android/` is the Compose app that drives that binary over a root shell.

The compiled backend ships inside the APK at `Android/app/src/main/assets/binaries`.

## Build

C backend:

```
make native            # build for the host arch, this is the real build command
make aarch64           # cross builds: aarch64, x86_64, armhf, x86, riscv64
make debug-hardened    # ASan, UBSan, LSan
make all-build         # every arch, then syncs into the APK assets
make format            # clang-format over src/
```

Bare `make` only prints help. It does not build anything.

Android app, from `Android/`:

```
./build.sh             # debug
./build.sh release     # signed release
```

Use the script, not gradle directly. CI runs `make all-tarball` then gradle, and does not
check formatting, so `make format` is on you.

Adding a new `.c` file means adding it to `SRCS` in the Makefile. There is no wildcard.

## Commits

- Run `make format` before committing any change to a `.c` or `.h` file.
- Sign off every commit: `git commit -s`.
- Never add a `Co-Authored-By:` trailer for an AI agent. Human co-authors are fine.
- Prefixes, matching the existing history:
  - `app:` for the Android app, with `app: fix:` and `app: refactor:` for those cases
  - `fix:`, `refactor:`, `feat:`, `docs:` or a subsystem name (`net:`, `mount:`, `seccomp:`,
    `daemon:`, `socketd:`) for the backend
  - `fix(security):` for anything security related, either half

## Style

Four rules. They apply to code, comments, commit messages, and documentation.

**No em-dashes.** Use a comma, a full stop, or rewrite the sentence.

**No ASCII banner comments.** No rows of `-----`, no `=====`, no boxed section headers. The
SPDX licence block at the top of each file stays. Existing banners in `src/` predate this
rule and are being removed separately, do not add more.

**Comments sound like a person.** Say why the code does something, or what breaks if it
does not. Skip comments that restate the line below them.

```c
/* Bad: increment the counter */
count++;

/* Good: netd resets our rules on restart, so re-assert them every cycle */
install_policy_rules(cfg);
```

**Ten lines that work beat a hundred that do the same thing.** Delete before you add. A
smaller diff in the right place is the goal, not a smaller diff anywhere.

## Reuse before you write

The largest cleanup this project ever needed was caused by writing new code beside existing
code instead of extending it: a 700-line duplicate of the container config form, three
copies of the init system screen, one bottom bar pasted seven times. Grep before you write.
`CONTRIBUTING.md` lists the shared components and helpers in both halves.

These are choke points. Bypassing one is a bug, not a shortcut.

- `ContainerCommandBuilder.quote()` in `Android/app/src/main/java/com/droidspaces/app/util/ContainerCommandBuilder.kt`
  wraps every dynamic value that reaches a root shell.
- `ServiceManagerBase.isSafeServiceName()` allow-lists service names that came from inside a
  container. It fails closed. So does `ValidationUtils`.
- `ds_peer_authorized()` in `src/utils.c` is the only authorization gate in the tree, and
  `ds_peer_in_pidns()` backs it. Both must fail closed. A "cannot determine, so allow"
  branch there was a root container escape once already.
- `src/include/droidspace.h` is the catch-all header and every `.c` file includes only it.
  Grep it before writing a helper.
- Logging is `ds_log`, `ds_warn`, `ds_error`, `ds_die`. Never a bare `fprintf` or `exit`.
- `run_command`, `run_command_quiet`, `run_command_log` are the only sanctioned way to run
  an external binary. There is no `system()` in this tree and there must not be one.
- Workspace paths come from `get_workspace_dir`, `get_pids_dir`, `get_net_dir`,
  `get_logs_dir`. They switch between the Android and Linux roots. Never hardcode either.

## Hard constraints

- Minimum kernel is 3.10. No `openat2`, no `clone3`, no `pidfd_*`, no cgroup v2 only paths.
- Anything platform specific is guarded with `is_android()`, both directions.
- The app targets `minSdk = 26`. Test on Android 8 behaviour before assuming an API exists.
- Autoboot scripts under `init/android-service/vendor/bin/` are strict POSIX sh. No bash
  arrays, no bashisms.
- No new dependency when an installed one covers it. The app already has libsu, Compose,
  navigation, lifecycle, and the Termux terminal.
