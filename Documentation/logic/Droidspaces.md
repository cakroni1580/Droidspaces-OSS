# Droidspaces — Internal Architecture & Implementation Reference

---

## Abstract

Droidspaces is a lightweight, zero-virtualization container runtime designed to run full Linux distributions (Ubuntu, Alpine, etc.) with systemd or openrc as PID 1, natively on Android devices. It achieves process isolation through Linux PID, IPC, MNT, and UTS namespaces — the same kernel primitives used by Docker and LXC — but targets the constrained and idiosyncratic Android kernel environment where many standard container tools refuse to operate.

This document is a complete internal architecture reference for **Droidspaces v5.3.1**. Every struct, every syscall, every mount, and every design decision is documented here with the intent that a future implementer could rewrite this project from scratch without ever reading the original source. Where the implementation is elegant, I say so. Where it is broken or fragile, I say so with equal honesty.

The codebase is approximately **12,800 lines of C** across 20 `.c` files and 1 master header, compiled as a single static binary against musl libc.


---

## 1. Project Overview

### 1.1 What It Does

Droidspaces takes a Linux rootfs directory (or ext4 image) and boots it inside a set of Linux namespaces as if it were a tiny virtual machine — except there is no hypervisor, no emulated hardware, and no performance penalty. The host kernel is shared. The container gets:

- Its own PID tree (PID 1 = `/sbin/init`)
- Its own mount table (the rootfs becomes `/`)
- Its own hostname (UTS namespace)
- Its own IPC resources (semaphores, shared memory)
- Its own Cgroup view (Cgroup namespace / Sub-cgroup isolation)
- **Three networking modes**: host (shared stack), NAT (full isolation with internet access via veth/bridge/iptables), and none (air-gapped loopback only)

### 1.2 What It Does NOT Do

- **No user namespace.** The container runs as root from the host kernel's perspective. This is deliberate — Android kernels often lack user namespace support, and Droidspaces requires root anyway.
- **Network namespace is optional.** `--net=host` shares the host stack (default, zero setup cost). `--net=nat` and `--net=none` unshare `CLONE_NEWNET`, giving the container a fully private network stack. See Section 12 for the complete NAT implementation.
- **Limited resource constraints.** Droidspaces implements full cgroup isolation via namespaces and subtree binding (see Section 11), providing a clean root for `systemd`. However, it does not set hard resource limits (CPU/Mem quotas) by default.

### 1.3 Source Structure

```
src/
├── droidspace.h        Master header — all structs, constants, prototypes
├── main.c              CLI parsing, command dispatch
├── config.c            Configuration loading, saving, and validation (ds_config)
├── container.c         start/stop/enter/run/info/show/restart commands
├── boot.c              internal_boot() — the PID 1 boot sequence
├── mount.c             Mount helpers, /dev setup, rootfs.img handling
├── cgroup.c            LXC-style Cgroup setup (v1, v2, hybrid, ns-aware)
├── console.c           epoll-based console I/O monitor loop
├── terminal.c          PTY allocation, /dev/console + /dev/ttyN setup
├── network.c           DNS, host/rootfs networking, veth, route monitor
├── ds_netlink.c        Pure-C RTNETLINK API (link/addr/route/rule ops)
├── ds_iptables.c       Raw socket iptables management (MASQUERADE, DNAT, FORWARD)
├── ds_dhcp.c           Embedded single-lease DHCP server (joinable thread)
├── hardware.c          GPU group auto-detection, X11 socket mounting
├── android.c           Android-specific: SELinux, optimizations, storage
├── android_seccomp.c   Adaptive Seccomp BPF filters for legacy Android (< 5.0)
├── cgroup.c            Cgroup v1/v2 hierarchy mounting and subtree isolation
├── config.c            `ds_config` file loading, metadata syncing, and memory backup
├── environment.c       Environment variables, os-release parsing
├── utils.c             File I/O, UUID generation, firmware path mgmt
├── pid.c               PID file management, workspace, container naming
├── documentation.c     Interactive terminal-based reference viewer
└── check.c             System requirements checker

```

**Key architectural changes from v2:**
- The `struct ds_config` replaces all global variables — every function receives its configuration explicitly
- PTYs are allocated in the **parent** process before fork (LXC model) — no SCM_RIGHTS FD passing
- `enter_namespace()` is factored out as a shared helper for both `enter` and `run` commands
- Thread pool eliminated — all checks and scans run serially (the overhead was larger than the benefit)
- Stop uses `SIGRTMIN+3` for systemd shutdown (not `system("poweroff")`)
- The `[ds-monitor]` daemon uses `waitpid()` instead of `kill(pid, 0)` polling
- **Hostname resolution:** Automatically maps `127.0.1.1` to the container hostname in `/etc/hosts` to fix `apt` and `sudo` resolution issues.
- **PTY Allocation:** `enter` command allocates PTYs natively *inside* the container namespaces for perfect TTY isolation and `ps aux` accuracy.
- **Security & De-duplication:** Environment setup, `os-release` parsing, and path resolution are consolidated into shared helpers with strict buffer bounds (`snprintf` with precision) and hardened I/O (`write_all`).
- **Volatile Overlay Mode:** Leverages Linux OverlayFS to store all container changes in RAM, ensuring an ephemeral environment that is wiped on exit.
- **Custom Bind Mounts:** Allows mapping host directories into the container at arbitrary mount points with automatic destination creation.
- **Security Hardening (v3.2.2+):** Implemented strict path-traversal protection for bind mounts using a `realpath`-based `is_subpath()` helper to prevent container escapes.
- **Strict Naming Architecture:** Enforces mandatory `--name` for image-based containers to ensure host-side infrastructure is predictable.
- **CLI UX Refinement:** Restored flag permutation for the `enter` command and added support for comma-separated bind mounts (`-B src:dest,src2:dest2`).
- **Namespace-Aware Cleanup (v3.3.0+):** Volatile mode cleanup is now namespace-aware. Since OverlayFS is mounted inside the container's private mount namespace, the kernel automatically unmounts it when the namespace dies.
- **Adaptive Seccomp Shield (v3.3.0+):** Implements a kernel-aware BPF filter that resolves Keyring/FBE conflicts and prevents VFS deadlocks on legacy Android kernels (< 5.0).
- **Cgroup Isolation & Session Fix (v4.2.0+):** 
    - Implemented unique host-side cgroup trees per container: `/sys/fs/cgroup/droidspaces/<name>`.
    - Fixed `su` and `login` hangs in entered terminals by physically attaching the entering process to the container's host cgroup *before* joining namespaces. This ensures `systemd-logind` correctly identifies the terminal session.
- **Fast Container Restart (v4.2.2+):**
    - Moves the mount reuse check to the very beginning of the boot sequence, bypassing expensive name resolution and `e2fsck`.
    - Sanitized PID management to remove filesystem side-effects during status checks and name discovery, ensuring tracking state is preserved for the next boot.
- **Robust Reboot & Concurrency (v4.6.0+):**
    - Implemented a rigorous 3-level process hierarchy (Monitor → Intermediate → Init) so that in-container `reboot` syscalls trigger clean internal restarts via deterministic exit codes (249) without ghost processes.
    - Replaced fragile `.restart` coordination markers with a strict `.lock` ownership model where only CLI commands process state transitions, preventing race conditions.
    - Added unified configuration recovery (`ds_metadata_sync`), seamlessly resurrecting lost host-side configurations from a running container's isolated `/run` namespace.
- **Kernel 4.14 Resilience (v4.2.3-v4.2.4):**
    - Implemented a robust 3-attempt retry loop for rootfs image mounting with `sync` and 1-second settle delays.
    - Refined UI logs (v4.2.4) to hide attempt counters on the first try and provide cleaner error reporting.
    - Hardened the unmount sequence with extra `sync` calls to ensure asynchronous loop device teardown completes before the next start.
- **Multi-DNS Support (v4.2.4):**
    - Refactored `ds_get_dns_servers` to support an arbitrary number of DNS servers (comma or space separated).
    - Hardened DNS propagation by switching to a rootfs-relative path (`/.dns_servers`) inside the container after `pivot_root`, ensuring persistence even when host paths are obscured.
    - Added explicit cleanup for the temporary marker file.
- **Sparse Image SELinux Hardening (v4.3.0):**
    - Resolved silent loop mount I/O errors on certain Android devices by applying the `vold_data_file` SELinux context to `.img` files.
    - Context is applied automatically in the C backend before mounting and persistent through the Magisk boot module for all existing containers.
- **DNS Setup Unification & Race Fix (v4.3.2):**
    - Moved host-side networking configuration (`fix_networking_host`) to the pre-fork stage to eliminate race conditions between parent and child.
    - Simplified DNS resolution by removing obsolete `getprop`-based logic and centralizing default DNS servers (1.1.1.1, 8.8.8.8) in the master header.
    - Standardized DNS file writing using shared utility helpers.
- **In-Memory DNS Propagation (v4.4.0):** Resolved a critical bug with volatile rootfs images by replacing file-based DNS marker propagation with a more direct in-memory approach. The host-side process now gathers DNS configuration and stores it in the `ds_config` struct. This is passed to the container process, which then writes the configuration directly to `/run/resolvconf/resolv.conf` after the writable `/run` tmpfs is mounted. This ensures DNS resolution works robustly in all modes.


---

## 2. System Requirements & Assumptions

Droidspaces performs a formal requirements check via the `check` command (implemented in `check.c`). Here's what actually matters:

### Must-Have (Hard Requirements)

| Requirement | How It's Checked | Why |
|---|---|---|
| Root privileges | `getuid() == 0` | Namespace creation and mount operations require CAP_SYS_ADMIN |
| Linux version | `DS_MIN_KERNEL_MAJOR` / `DS_MIN_KERNEL_MINOR` (3.18.0) | Absolute floor for stable namespaces and OverlayFS support |
| PID namespace | `access("/proc/self/ns/pid", F_OK) && is_root` | Container PID isolation |
| Mount namespace | `access("/proc/self/ns/mnt", F_OK) && is_root` | Filesystem isolation |
| UTS namespace | `access("/proc/self/ns/uts", F_OK) && is_root` | Hostname isolation |
| IPC namespace | `access("/proc/self/ns/ipc", F_OK) && is_root` | IPC isolation |
| devtmpfs | `grep_file("/proc/filesystems", "devtmpfs")` | Proper HW device node support |
| Cgroup namespace | `access("/proc/self/ns/cgroup", F_OK)` | Adaptive Cgroup isolation (Modern Path) |
| proc/sys virtual FS | `access()` check | Essential systems for container operation |
| OverlayFS (Optional) | `grep_file("/proc/filesystems", "overlay")` | Required for Volatile Mode (`--volatile`) |

### Recommended

epoll, signalfd, PTY/devpts support, loop device, ext4 — all tested serially in `check_requirements_detailed()`.

### Assumptions

- The binary is compiled statically against musl libc (no glibc dependency)
- The host is Android (detected via `ANDROID_ROOT` env var or `/system/bin/app_process`) or standard Linux
- The rootfs contains `/sbin/init` (systemd or openrc)
- The rootfs contains `/etc/os-release` for auto-naming

---

## 3. Container Lifecycle Overview

Here's the high-level flow from `start` to `stop`, distilled to its essence:

```
                          ┌─────────────────────┐
                          │   droidspaces start  │
                          │     (CLI parsing)    │
                          └──────────┬──────────┘
                                     │
                          ┌──────────▼──────────┐
                          │  check_requirements  │
                          │  fix_networking_host │
                          │  android_optimizations│
                          │  generate UUID       │
                          │  allocate PTYs       │
                          │    (LXC model)       │
                          └──────────┬──────────┘
                                     │
                           fork()    │
                ┌────────────────────┼───────────────────┐
                │ PARENT             │ MONITOR           │
                │                    │ PROCESS           │
                │                    │                   │
                │                    ▼                   │
                │               setsid()                 │
                │          migrate to sub-cgroup         │
                │  unshare(PID|UTS|IPC|CGROUP)           │
                │  [+ CLONE_NEWNET if nat/none]           │
                │                    │                   │
                │               fork()│                   │
                │         ┌──────────┼──────────┐        │
                │         │          │ INTERMEDIATE │        │
                │         │          │ (unshare PID)│        │
                │         │          ▼          │        │
                │         │         fork()      │        │
                │         │    ┌─────┼─────┐    │        │
                │         │    │           ▼    │        │
                │         │    │        PID 1   │        │
                │         │    │   internal_boot() │      │
                │         │    │     │ unshare(MNT) │      │
                │         │    │     │ pivot_root   │      │
                │         │    │     │ execve(init) │      │
                │         │    └──waitpid(init)─┘    │        │
                │         │          │          │        │
                │         │          ▼          │        │
                │         │    Intermediate returns │      │
                │         │    exit status (249 if  │      │
                │         │    reboot, else exit)   │      │
                │         └──────────┬──────────┘        │
                │                    ▼                   │
                │               waitpid(intermediate)    │
                │             check exit status (249?)   │
                │              cleanup_resources()       │
                │                    │                   │
                └────────────────────┼───────────────────┘
                                     │
        ┌───────┴────────┐                               │
        │ FOREGROUND?    │                               │
        ├────YES─────┐   │                               │
        │            ▼   │                               │
        │  console_monitor_loop()                        │
        │  (epoll stdin↔pty_master)                      │
        │                │                               │
        ├────NO──────┐   │                               │
        │            ▼   │                               │
        │  show_info()                                   │
        │  parent exits                                  │
        │  monitor holds PTY FDs via waitpid()           │
        └────────────────┘                               │
```

**Key insight:** The monitor process (`[ds-monitor]`) creates host isolation via `unshare(UTS|IPC)`, then forks an **intermediate process** which unshares the `PID` namespace, then forks the container init (PID 1). This 3-level hierarchy ensures that an internal `reboot()` syscall causes the container init to terminate with a deterministic signal, which the intermediate translates to an exit code. The monitor stays alive via `waitpid()` on the intermediate. When the intermediate exits, the monitor checks the code (reboot vs final shutdown); if final, it relies on an external CLI `.lock` for coordination, handing off or retaining cleanup responsibility.

**The parent** reads the newly created PID file to determine the initial PID 1 in the host namespace, relying on the intermediate or monitor updates, and either enters the foreground console loop or exits.

---

## 4. Start: pivot_root, Namespace Creation, and Mount Setup

### 4.1 Pre-Fork Setup (Parent Process)

Before any forking, `start_rootfs()` in `container.c` performs:

1. **Workspace creation:** `ensure_workspace()` creates `Pids/` directory under the workspace path
2. **SELinux:** If `--selinux-permissive`, set SELinux to permissive mode
3. **Android Storage:** Detect if running on Android and validate storage requirements.
4. **Container Naming (Sync Transition):** 
   - If no `--name` is provided, Droidspaces auto-generates one from `/etc/os-release`.
   - **MANDATORY**: If using a rootfs image (`-i`), the `--name` flag is now mandatory to ensure the host-side infrastructure is predictable.
   - Duplicate names are resolved with a numeric suffix (e.g., `ubuntu-1`) via `find_available_name()`.
   - **Note**: The name is finalized **before** any mounting occurs.
5. **Rootfs image mount:** If `-i` provided, `mount_rootfs_img()` is called:
   - It runs `e2fsck -f -y` to ensure filesystem integrity.
   - It identifies a descriptive mount point at `/mnt/Droidspaces/<name>`.
   - **SELinux Hardening**: Applies the `u:object_r:vold_data_file:s0` context to the image file to avoid silent I/O denials during loop-mount operations.
   - If `--volatile` is active, the image is mounted **Read-Only** (`-o loop,ro`) for maximum safety.
6. **UUID generation:** 32 hex chars from `/dev/urandom`
7. **PTY allocation (LXC model):**
   ```c
   ds_terminal_create(&cfg->console);       // 1 console PTY
   for (int i = 0; i < cfg->tty_count; i++) // 6 TTY PTYs (DS_MAX_TTYS)
       ds_terminal_create(&cfg->ttys[i]);
   ```
   All PTYs are allocated in the **parent** process using `openpty()`. Both master and slave FDs are marked `FD_CLOEXEC`. The slave device paths (e.g., `/dev/pts/3`) are recorded in `tty->name` for later bind-mounting.

### 4.2 Fork Architecture

The parent forks a **monitor process**, not an intermediate throwaway:

```c
pid_t monitor_pid = fork();
if (monitor_pid == 0) {
    /* MONITOR PROCESS */
    setsid();
    prctl(PR_SET_NAME, "[ds-monitor]", 0, 0, 0);
    unshare(CLONE_NEWUTS | CLONE_NEWIPC); // Notice PID is NOT here
    
    pid_t mid_pid = fork();
    if (mid_pid == 0) {
        /* INTERMEDIATE PROCESS */
        unshare(CLONE_NEWPID); // Fresh PID space per boot sequence
        
        pid_t init_pid = fork();
        if (init_pid == 0) {
            /* CONTAINER INIT (PID 1) */
            exit(internal_boot(cfg));
        }
        
        /* Wait for init to exit and capture its status */
        waitpid(init_pid, &status, 0);
        /* If init called reboot(2), kernel sends SIGHUP -> map to 249 */
        exit(WIFSIGNALED(status) && WTERMSIG(status) == SIGHUP ? 249 : ...);
    }
    
    /* Monitor waits for intermediate to exit */
    waitpid(mid_pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 249) {
        goto reboot_loop; // Handle an internal reboot
    }
    
    // Not a reboot, proceed to check locks and cleanup...
}
```

**Namespace allocation split:**
- `CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWCGROUP` — called in the monitor via `unshare()`.
- `CLONE_NEWPID` — called in the *intermediate* process via `unshare()`. This ensures each internal reboot cycle gets a completely clean, fresh PID tree, and avoids Linux constraints limiting `unshare(CLONE_NEWPID)` to once per process.
    
**The Cgroup "Jail" Trick (v4.2.0+):**
To achieve isolation on kernels with cgroup namespace support (4.6+), Droidspaces must be in a non-root cgroup *before* calling `unshare`. 
1.  Creates `/sys/fs/cgroup/droidspaces/<name>`.
2.  Moves itself into that container-specific cgroup (`cgroup.procs`).
3.  Calls `unshare(CLONE_NEWCGROUP)`.
Result: Inside the container, `/sys/fs/cgroup` points to the container's private subtree on the host. This prevents name collisions between concurrent containers and ensures that `systemd` inside the container sees a clean, isolated hierarchy.

This split is deliberate: the monitor retains the host mount namespace so it can perform cleanup (unmounting rootfs images, removing pidfiles, and clearing volatile overlays) after the container exits. The container init gets its own private mount namespace inside `internal_boot()`.

### 4.3 Volatile Mode (OverlayFS Implementation)

When `--volatile` (`-V`) is used, Droidspaces wraps the rootfs in an ephemeral writable layer:

1. **Probe**: `setup_volatile_overlay()` checks `/proc/filesystems` for `overlay` support.
2. **Workspace**: Creates a temporary structure in `/var/lib/Droidspaces/Volatile/<name>/`.
3. **Layering**: 
   - **Lowerdir**: The original rootfs (or RO image mount).
   - **Upperdir/Workdir**: Managed in a dedicated `tmpfs` mounted at the Volatile workspace path.
    - **Merged View**: Mounts the OverlayFS to `<workspace>/merged`.
- **SELinux Fix (Android)**: On Android, the overlay is mounted using the `DS_ANDROID_TMPFS_CONTEXT` macro (default: `u:object_r:tmpfs:s0`) in `droidspace.h` to allow standard write operations within the upperdir.
5. **Redirection**: The configuration's `rootfs_path` is updated to point to this merged view for the duration of the boot.

All file modifications happen in the `tmpfs`-backed `upperdir`. On container exit, the monitor process recursively deletes the workspace. **Key Architectural Insight**: Because the overlay is mounted *after* `unshare(CLONE_NEWNS)` and `MS_PRIVATE` (see `boot.c`), it exists only within the container's mount namespace. When the container dies, the kernel tears down the namespace and the mounts vanish automatically—the host does not need to (and cannot) unmount them.

**Known Limitation — f2fs (Android):**
Most Android devices use f2fs for `/data`. OverlayFS on many Android kernels (4.14, 5.15) does not support f2fs as a `lowerdir`. This means **volatile mode + directory rootfs (`-r`) will fail** when the rootfs lives on f2fs. 

**Pre-flight Check (v3.2.2+)**: Droidspaces performs an early validation of OverlayFS support and f2fs compatibility in the **parent process** before any host-side state (networking, optimizations) is modified. This ensures the runtime fails "cleanly" if volatile mode is unsupported.

Droidspaces detects this at runtime via `statfs()` magic `0xF2F52010` and prints a clear diagnostic. **Workaround**: Use a rootfs image (`-i`) instead — the ext4 loop mount provides a compatible lowerdir.

**What is NOT used and why:**
- `CLONE_NEWNET` for `--net=host` — the container deliberately shares the host network stack in this mode for zero-setup simplicity. For `--net=nat` and `--net=none`, `CLONE_NEWNET` IS unshared. See Section 12.
- `CLONE_NEWUSER` — Not used because Android kernels often lack support, and the tool requires root anyway.

### 4.4 The internal_boot() Sequence

This is the critical function — it runs as PID 1 inside the new PID namespace. Here is the exact order of operations:

**Step 1 — Unshare mount namespace:**
```c
unshare(CLONE_NEWNS);
```
This gives the container its own private mount table.

**Step 2 — Make root mount private:**
```c
mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
```
This prevents mount events from propagating back to the host. Without this, every mount inside the container would be visible to Android.

**Step 3 — Bind mount rootfs to itself:**
```c
mount(cfg->rootfs_path, cfg->rootfs_path, NULL, MS_BIND | MS_REC, NULL);
```
This is required by `pivot_root(2)` — the new root must be a mount point. For rootfs.img, the ext4 image is already loop-mounted, so this is redundant but harmless.

**Step 4 — Custom Bind Mounts (Optional):**
```c
setup_custom_binds(cfg, cfg->rootfs_path);
```
Iterates through `--bind-mount` entries and performs recursive bind mounts from host to container. It uses `mkdir_p` to automatically create parent directories inside the rootfs if they don't exist. 

**Resilience (v3.2.0+)**: If a host source path is missing or the mount fails, Droidspaces issues a warning and skips the specific entry rather than failing the entire boot sequence ("soft-fail" model). 

**Security Hardening (v3.2.2+)**: To prevent path-traversal escapes, Droidspaces validates both the command-line arguments and the final resolved mount target. Before mounting, it uses `lstat()` to ensure the target inside the rootfs is not a symlink. After mounting, the `is_subpath(rootfs, tgt)` helper uses `realpath()` to ensure that even with a complex malicious hierarchy, a bind mount cannot escape the container's root.
```c
chdir(cfg->rootfs_path);
```

**Step 5 — Create `.old_root` directory:**
```c
mkdir(".old_root", 0755);
```

**Step 6 — Setup /dev:**
```c
setup_dev(".", cfg->hw_access);
```
This mounts either `tmpfs` (isolated mode) or `devtmpfs` (hardware access mode) at `<rootfs>/dev`, then calls `create_devices()` to populate device nodes via `mknod()`. See Section 8 for details.

**Step 7 — Mount /proc:**
```c
domount("proc", "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
```

**Step 8 — Mount /sys:**
- **Without `--hw-access`**: sysfs is mounted RW initially, then a separate sysfs instance is mounted at `sys/devices/virtual/net` for networking tools, and finally the parent `/sys` is remounted read-only.
- **With `--hw-access`**: Core `sysfs` is mounted RW, then **dynamically iterated**. Every subdirectory (devices, bus, class, etc.) is self-bind mounted to "pin" it as an independent RW entry. Finally, the top-level `/sys` is remounted RO.

Aligning with the official **systemd Container Interface**, the top-level `/sys` must be RO for systemd 258+ to correctly identify the environment as a container. The "dynamic hole-punching" ensures hardware access remains RW.

**Step 9 — Mount /run as tmpfs:**
```c
domount("tmpfs", "run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=755");
```

**Step 10 — Bind-mount PTYs (BEFORE pivot_root):**
```c
mount(cfg->console.name, "dev/console", NULL, MS_BIND, NULL);
for (int i = 0; i < cfg->tty_count; i++) {
    snprintf(tty_target, sizeof(tty_target), "dev/tty%d", i + 1);
    mount(cfg->ttys[i].name, tty_target, NULL, MS_BIND, NULL);
}
```
This is the LXC model: PTY slaves allocated in the parent (e.g., `/dev/pts/3`) are bind-mounted to their container targets (e.g., `dev/console`, `dev/tty1`..`dev/tty6`) **before** `pivot_root`. This is critical because after `pivot_root`, the host `/dev/pts/N` paths would no longer be accessible.

**Step 11 — Write internal markers (`/run/droidspaces`):**
```c
write_file("run/droidspaces/<uuid>", "");
write_file("run/droidspaces/version", DS_VERSION);
ds_config_save("run/droidspaces/container.config", cfg);
```
The UUID marker (`run/droidspaces/<uuid>`) is used by the host side to precisely correlate running processes back to the specific container boot session. A normalized copy of the active configuration is also stored inside the volatile `/run` tmpfs. If host-side logs or states (like the config in the workspace) are accidentally deleted, Droidspaces leverages this inner `/run/droidspaces` environment via `/proc` mounts to reconstruct perfectly synchronized configurations back to the host (`ds_metadata_sync`).

**Step 12 — Setup cgroups:**
```c
setup_cgroups();
```
Droidspaces v4.0.0 uses a "data-driven" cgroup strategy ported from LXC:
- **Host Discovery**: Parses `/proc/self/mountinfo` to identify exactly where and how the host has mounted cgroups (v1 comounts, v2 unified, hybrid).
- **Adaptive Strategy**:
    - **Modern (NS active)**: Directly mounts `cgroup`/`cgroup2` FS. The kernel-managed namespace provides the isolation.
    - **Legacy (No NS)**: Manually bind-mounts the process's specific cgroup subtree from the host into the container's hierarchy.
- **Systemd Compatibility**: Replicates comounted v1 controllers (e.g., `cpu,cpuacct`) and creates symlinks for secondary names.
- **Pure V2 Protection**: Detects if the host is pure cgroup v2; if so, it skips the Read-Only remount of `/sys/fs/cgroup` to ensure `systemd` can create its own scopes.

**Step 13 — Optional: Android storage bind mount:**
```c
if (cfg->android_storage) android_setup_storage(".");
```
Bind-mounts `/storage/emulated/0` into the container.

**Step 14 — pivot_root:**
```c
syscall(SYS_pivot_root, ".", ".old_root");
chdir("/");
```
The current directory (`.`, which is the rootfs path) becomes the new root filesystem. The old root (`/` from Android's perspective) is moved to `.old_root` under the new root.

**Step 15 — Setup devpts:**
```c
setup_devpts(cfg->hw_access);
```
This MUST happen after `pivot_root` because devpts `newinstance` needs to be mounted inside the new root. Mounts at `/dev/pts` with options `gid=5,newinstance,ptmxmode=0666,mode=0620`. Then virtualizes `/dev/ptmx` by bind-mounting `/dev/pts/ptmx` over it.

**Step 16 — Network configuration (rootfs side):**
```c
fix_networking_rootfs(cfg);
```
Sets hostname (from `--hostname`), writes `/etc/hostname`, and generates `/etc/hosts`.

**The apt/sudo hostname fix:**
To prevent `apt` warnings and `sudo` resolution delays, Droidspaces explicitly maps the container's hostname to `127.0.1.1` in `/etc/hosts`. This is a critical fix for many Linux distributions where the loopback alias is expected.

It then writes the DNS configuration directly from the in-memory `ds_config` struct to `/run/resolvconf/resolv.conf` (ensuring proper null-termination for system stability). Finally, it symlinks `/etc/resolv.conf` and appends Android network groups (`aid_inet`, `aid_net_raw`, `aid_net_admin`) to `/etc/group`.


**Step 17 — Unmount old root:**
```c
umount2("/.old_root", MNT_DETACH);
rmdir("/.old_root");
```

**Step 18 — Write container marker:**
```c
write_file("/run/systemd/container", "droidspaces");
```
This is how systemd detects it's running in a container, and how Droidspaces validates a PID belongs to one of its containers (`is_valid_container_pid()` checks this file).

**Step 19 — Setup environment:**
```c
clearenv();
setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
setenv("TERM", "xterm-256color", 1);
setenv("HOME", "/root", 1);
setenv("container", "droidspaces", 1);
setenv("container_ttys", "/dev/pts/0 /dev/pts/1 ...", 1);
```
The `container_ttys` string is built from the PTY slave names recorded during allocation.

**Step 20 — Redirect stdio to console:**
```c
int console_fd = open("/dev/console", O_RDWR);
ds_terminal_set_stdfds(console_fd);     // dup2 to 0, 1, 2
ds_terminal_make_controlling(console_fd); // setsid() + TIOCSCTTY
```

**Step 21 — Exec init:**
```c
execve("/sbin/init", argv, environ);
// Fallback:
execve("/bin/sh", argv, environ);
```

After `execve`, the container is running. PID 1 is now systemd (or openrc), its stdio is connected to the console PTY, and the monitor process holds the master side of all PTYs.

---

## 5. UUID-Based PID Discovery and Multi-Container Support

### 5.1 The Problem

After `fork() + unshare() + fork()`, the parent process needs to know the _global_ PID of the container's init process. The monitor process knows the init PID in its local view, but due to PID namespace translation, this may differ from the host view.

### 5.2 The Sync Pipe Approach

In v3, the PID discovery has been simplified. The monitor process sends the init's PID directly to the parent via a sync pipe:

```c
/* Monitor side */
pid_t init_pid = fork();
write(sync_pipe[1], &init_pid, sizeof(pid_t));

/* Parent side */
read(sync_pipe[0], &cfg->container_pid, sizeof(pid_t));
```

Since the monitor has not entered a PID namespace itself (only its children do), the `init_pid` it observes is the host-global PID.

### 5.3 The UUID Marker (Boot Confirmation)

The UUID marker file at `/run/<uuid>` still serves an important purpose: boot confirmation. After saving the PID, the parent polls for the marker via `/proc/<pid>/root/run/droidspaces` to confirm the container has completed `pivot_root` before displaying status information:

```c
char marker[PATH_MAX];
snprintf(marker, sizeof(marker), "/proc/%d/root/run/droidspaces", cfg->container_pid);
for (int i = 0; i < 50; i++) {    /* 5 seconds max */
    if (access(marker, F_OK) == 0) break;
    usleep(100000);                /* 100ms */
}
```

### 5.4 UUID Scanning for Container Recovery

The `find_container_init_pid()` function retains the full UUID scan for container recovery and the `scan` command. It iterates over all PIDs in `/proc` and checks `build_proc_root_path(pid, "/run/<uuid>", ...)` with retry logic (20 retries, 200ms delay).

### 5.5 Why This Works for Multi-Container

Each container gets a unique UUID. The marker file exists only inside that specific container's mount namespace (in its `/run` tmpfs). The `/proc/<pid>/root` path traverses into the container's root filesystem from the host. So two containers with different rootfs paths will have different UUIDs, and the scan will find the correct PID for each.

### 5.6 PID Persistence and Strict Naming

The discovered PID is written to a pidfile at `<workspace>/Pids/<name>.pid`:
- Android: `/data/local/Droidspaces/Pids/ubuntu-24.04.pid`
- Linux: `/var/lib/Droidspaces/Pids/ubuntu-24.04.pid`

**Strict Naming Rules (v3.2.0+):**
- **Image Mode**: `--name` is now **mandatory** for containers starting from an image (`-i`). This ensures host-side infrastructure (like `/mnt/Droidspaces/<name>`) is perfectly descriptive.
- **Rootfs Mode**: If no name is provided, it is auto-generated from `/etc/os-release`.
- **Synchronization**: The final container name (resolved via `find_available_name()`) is used consistently for the PID file, the hostname, and the host-side mount point.

---

## 6. Init Execution and Terminal I/O Forwarding

### 6.1 The exec Call

```c
char *argv[] = {"/sbin/init", NULL};
execve("/sbin/init", argv, environ);
```

No arguments are passed to init. The environment is clean. The `container=droidspaces` environment variable tells systemd it's in a container. Fallback to `/bin/sh` if `/sbin/init` is not found.

### 6.2 Foreground Mode

When `--foreground` is specified:

1. The parent process puts the terminal into raw mode via `ds_setup_tios()`:
   ```c
   new_tios.c_iflag |= IGNPAR;
   new_tios.c_iflag &= ~(ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF);
   new_tios.c_lflag &= ~(TOSTOP | ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL);
   /* Keep host's ONLCR enabled to avoid staircase output if the container side
    * stops sending \r (e.g. during shutdown or sudo execution). Duplicate \r
    * are harmless. */
   // new_tios.c_oflag &= ~ONLCR;
   new_tios.c_oflag |= OPOST;
   ```

2. Then it enters `console_monitor_loop()`, which uses epoll to shuttle data between:
   - `STDIN_FILENO` (user input) → `console_master` (container input)
   - `console_master` (container output) → `STDOUT_FILENO` (user display)

3. The epoll loop also monitors a `signalfd` for:
   - `SIGCHLD` — detects when the intermediate or container process exits
   - `SIGINT` / `SIGTERM` — forwarded to the container's init PID
   - `SIGWINCH` — terminal resize is forwarded via `ioctl(TIOCSWINSZ)`

4. The loop terminates on `EPOLLHUP`/`EPOLLERR` on the master fd, or when `SIGCHLD` indicates the container has exited.

### 6.3 Background Mode

When foreground is not requested:

1. The **monitor process** (forked during start) is already running. It calls `waitpid(init_pid, ...)` and blocks until the container exits. The monitor inherits all PTY master FDs from the parent, keeping them alive for the container's lifetime.

2. The parent polls for the boot marker, calls `show_info()` to display container details, logs connection instructions, and exits.

3. When the container exits, the monitor process calls `cleanup_container_resources()` to remove pidfiles, unmount images, restore firmware paths, and restore Android optimizations.

### 6.4 How Getty/Login Works

In the current implementation, the login process (getty) attaches directly to `/dev/console`. Even after a user logs in, the session remains attached to the console.

**Note:** This is a stable but primitive implementation. A future improvement (TODO) is to properly spawn `agetty` on individual `/dev/ttyN` nodes. Pull Requests are welcome to refine this behavior!

---

## 7. TTY/PTY/Console Setup

### 7.1 The LXC Model

Droidspaces v3 allocates all PTYs **in the parent process** before forking, following the LXC approach. This is a fundamental improvement over v2, which allocated PTYs inside the container and sent master FDs back via `SCM_RIGHTS`.

```c
// terminal.c: ds_terminal_create()
int ds_terminal_create(struct ds_tty_info *tty) {
    openpty(&tty->master, &tty->slave, tty->name, NULL, NULL);
    fcntl(tty->master, F_SETFD, FD_CLOEXEC);
    fcntl(tty->slave, F_SETFD, FD_CLOEXEC);
    return 0;
}
```

Key points:
- `openpty()` allocates a master/slave pair from the host's `/dev/ptmx`
- Both FDs are marked `FD_CLOEXEC` so they don't leak to the container's init via `execve()`
- The slave name (e.g., `/dev/pts/3`) is recorded in `tty->name`
- **Initial Winsize Propagation**: Immediately after allocation, Droidspaces propagates the host terminal's window size to the master PTY. This ensures the slave side has correct dimensions from the very first line of boot output, preventing "staircase" alignment issues in programs like `sudo` that query the terminal size during startup.

### 7.2 Console PTY

One dedicated PTY is allocated for `/dev/console`. Its slave path is bind-mounted to `dev/console` inside the rootfs before `pivot_root`:

```c
mount(cfg->console.name, "dev/console", NULL, MS_BIND, NULL);
```

After pivot_root, `internal_boot()` opens `/dev/console`, redirects stdin/stdout/stderr to it, and makes it the controlling terminal via `TIOCSCTTY`.

**The Fallback Winsize:** If the console PTY reports uninitialized `{0, 0}` dimensions, PID 1 applies a sane default of 24×80. This acts as a safety net until the host-side `console_monitor_loop` performs the first real synchronization.

### 7.3 TTY PTYs (Placeholders)

Up to 6 PTYs are allocated for `/dev/tty1` through `/dev/tty6` and bind-mounted into the container. However, in the current version, these are **non-functional placeholders**. 

They exist primarily to satisfy Init systems like OpenRC, which expect these devices to exist and would otherwise flood the log with "not found" errors. They are currently "nuking errors" rather than providing multiple login seats.

### 7.4 devpts newinstance

After `pivot_root`, a **new** devpts instance is mounted at `/dev/pts` with the `newinstance` flag. This gives the container its own PTY numbering space, independent of the host. The container's `/dev/ptmx` is virtualized by bind-mounting `/dev/pts/ptmx` over it (or symlink as fallback).

The mount options tried (in fallback order):
1. `gid=5,newinstance,ptmxmode=0666,mode=0620`
2. `newinstance,ptmxmode=0666,mode=0620`
3. `gid=5,newinstance,mode=0620`
4. `newinstance,ptmxmode=0666`
5. `newinstance`

### 7.5 Why Parent-Side Allocation is Better

The v2 approach of allocating PTYs inside the container had several problems:
- Required `SCM_RIGHTS` FD passing over Unix sockets (complex and fragile)
- Required a socketpair to be created before fork and managed across process boundaries
- The `[ds-monitor]` daemon in v2 used a `while(kill(pid, 0) == 0) sleep(60)` polling loop just to hold FDs

The v3 approach is simpler: PTYs are allocated before `fork()`, inherited by the monitor process, and bind-mounted before `pivot_root`. No FD passing is needed. The monitor holds the master FDs alive naturally via `waitpid()`.

---

## 8. Hardware Access Mode

### 8.1 The Default: Isolated /dev (tmpfs)

Without `--hw-access`, the container gets a minimal `/dev` on a private tmpfs:

```c
domount("none", dev_path, "tmpfs", MS_NOSUID | MS_NOEXEC, "size=8M,mode=755");
```

Followed by `mknod()` for: `null`, `zero`, `full`, `random`, `urandom`, `tty`, `console`, `ptmx`, plus `net/tun`, `fuse`, and `tty1`-`tty6` mount targets.

### 8.2 Hardware Access: devtmpfs

With `--hw-access`, the container mounts the kernel's `devtmpfs`:

```c
domount("devtmpfs", dev_path, "devtmpfs", MS_NOSUID | MS_NOEXEC, "mode=755");
```

**CRITICAL:** `devtmpfs` is a shared singleton in the kernel. All instances share the same backing store. This means:
- The container can see **all** host devices (Binder, GPU, USB, etc.)
- Changes to the devtmpfs (unlink, mknod) affect **all** mountpoints

### 8.3 The Conflict Resolution Strategy

After mounting `devtmpfs`, certain nodes conflict with the container's needs (the container needs its own `console`, `ptmx`, etc. for PTY isolation). The solution:

```c
const char *conflicts[] = {"console", "tty", "full", "null", "zero",
                           "random", "urandom", "ptmx", NULL};
for (int i = 0; conflicts[i]; i++) {
    umount2(path, MNT_DETACH);   // Unmount any bind-mounts
    unlink(path);                 // Remove the node
}
```

Then `create_devices()` immediately recreates them as **real character devices** using `mknod()`:

```c
mknod(path, S_IFCHR | 0666, makedev(1, 3));  // null = 1:3
mknod(path, S_IFCHR | 0666, makedev(5, 2));  // ptmx = 5:2
// etc.
```

This is the legacy Droidspaces v2 strategy — proven safe because:
1. The `unlink` + `mknod` is atomic per-node (the host sees a brief gap, but the node is immediately restored)
2. The new nodes have the correct major:minor numbers, so they work identically
3. The `console` node (5:1) created by `mknod` serves as a mount target for the PTY bind-mount

### 8.4 devpts in HW Access Mode

When setting up `/dev/ptmx` after devpts mount, the HW access path differs:

```c
if (hw_access) {
    /* /dev is devtmpfs (shared). Do NOT unlink ptmx. Bind-mount over the
     * node that create_devices() already created. */
    mount("/dev/pts/ptmx", "/dev/ptmx", NULL, MS_BIND, NULL);
} else {
    /* /dev is private tmpfs. Safe to unlink and replace. */
    unlink("/dev/ptmx");
    write_file("/dev/ptmx", "");
    mount("/dev/pts/ptmx", "/dev/ptmx", NULL, MS_BIND, NULL);
}
```

### 8.5 /sys in HW Access Mode (The systemd 258+ Getty Fix)

Starting with version **258**, systemd changed its container detection logic. It now uses the Read-Only status of the top-level `/sys` mount as the primary indicator of a virtualized environment. 

**The Problem:**
In standard `--hw-access` mode, `/sys` was previously Read-Write. Systemd 258+ would misidentify the container as a "Physical Host" and trigger a hardware console resolution logic (`resolve_dev_console`). This logic reads `/sys/class/tty/console/active` and attempts to open the host's actual console (e.g., `/dev/tty0`). Since this device node is either missing or an invalid directory in the container, the startup of `console-getty.service` would fail with an **`EISDIR`** (Is a directory) error, causing an infinite restart loop.

**The Fix (Dynamic Hole-Punching):**
Droidspaces solves this by strictly adhering to the systemd Container Interface while preserving hardware transparency:
1. **Dynamic Iteration**: Droidspaces opens the `/sys` directory and iterates through all sub-entries using `readdir`.
2. **RW Pinning**: Every subdirectory (e.g., `/sys/devices`, `/sys/bus`, `/sys/class`, `/sys/block`) is **self-bind mounted** onto itself (`MS_BIND | MS_REC`). This "pins" them as independent mount points that preserve their Read-Write state.
3. **Container Signaling**: The top-level `/sys` mount is then remounted as **Read-Only** (`MS_RDONLY`).
4. **Result**: Systemd sees a RO `/sys` and correctly identifies the container environment, while the container processes retain full RW access to the hardware subsystems via the pinned sub-mounts.

Additionally, `/dev/null` is bind-mounted over `/sys/class/tty/console/active` to mask host TTY discovery entirely.

### 8.6 Hardware and X11 Support (v4.5.0+)

When either `--hw-access` or the dedicated `--termux-x11` flag is enabled, Droidspaces performs environment-aware configuration for graphics support. This is implemented in `hardware.c` and integrated into `internal_boot()` at two critical points:

**Pre-pivot_root — GPU GID Scanning:**
If `--hw-access` is enabled, while the host's `/dev` is still accessible, `scan_host_gpu_gids()` probes ~40 known GPU device paths across all major GPU families (DRI, NVIDIA, Mali, Adreno, KFD, PowerVR, Tegra, DMA Heaps). For each device that exists, it collects the file's group ID via `stat()`. GID 0 (root) is skipped since root already has full access. Duplicate GIDs are de-duplicated.

**Post-pivot_root — Group Creation & X11:**
`setup_hardware_access()` is called after `pivot_root` and networking setup. It coordinates the hardware exposure:

1. **GPU Groups**: If `--hw-access` is enabled, `setup_gpu_groups()` reads the container's `/etc/group`, checks for existing GIDs (idempotent on restart), and appends new entries like `gpu_<GID>:x:<GID>:root`. This ensures the container's root user has the correct group membership to access GPU devices without manual intervention.

2. **X11 Socket**: If either `--hw-access` or `--termux-x11` is enabled, `setup_x11_socket()` performs platform-aware X11 socket mounting:
   - **Android**: Checks for Termux X11 at `/data/data/com.termux/files/usr/tmp/.X11-unix`
   - **Desktop Linux**: Uses `/proc/1/root/tmp/.X11-unix` to reach the host filesystem after `pivot_root`
   - Only the `.X11-unix` subdirectory is mounted (never `/tmp`) to avoid FBE keyring conflicts on encrypted Android devices.
   - The mount target is created with `0777` permissions to support Termux's non-root UID.

All operations are non-fatal: failures produce warnings but don't prevent the container from booting.

---

## 9. Entering a Running Container

### 9.1 The enter_namespace() Helper

Droidspaces v3 factored out namespace entry into a shared function used by both `enter` and `run`:

```c
int enter_namespace(pid_t pid) {
    const char *ns_names[] = {"mnt", "uts", "ipc", "pid"};
    int ns_fds[4];
    
    /* 1. Open ALL namespace FDs first (before any setns) */
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, ns_names[i]);
        ns_fds[i] = open(path, O_RDONLY);
    }
    
    /* 2. Enter namespaces */
    for (int i = 0; i < 4; i++) {
        if (ns_fds[i] >= 0) {
            setns(ns_fds[i], 0);
            close(ns_fds[i]);
        }
    }
    return 0;
}
```

**Critical detail:** All namespace FDs are opened **before** any `setns()` call. This is because entering one namespace (e.g., mount) changes the view of `/proc`, which could make subsequent namespace FD opens fail.

The mount namespace is mandatory; others are optional with warnings.

### 9.2 The "Native PTY" Attachment Model

In v3.1.2, the `enter` and `run` commands use a sophisticated PTY attachment model to bridge the host and container:

1. **Namespace Entry:** The intermediate process joins the container namespaces (`mnt`, `pid`, `ipc`, `uts`) using `enter_namespace()`.
2. **Native PTY Allocation:** While *inside* the container's private mount and PID namespaces, it allocates a new PTY pair using `openpty()`. This ensures the PTY is part of the container's private `devpts` instance.
3. **FD Passing:** The master side of this native PTY is passed back to the host process via `SCM_RIGHTS` over a Unix domain socket.
4. **Proxy Loop & Eager Sync:** The host process remains on the host and runs an `epoll` proxy loop. Crucially, the parent **eagerly synchronizes** the host terminal's window size to the received PTY master *before* anything else. This eliminates race conditions where the child shell execs interactive programs (like `htop` or `nano`) before the proxy loop has synchronized the terminal size.
5. **Precise Controlling TTY:** Inside the container, following the double-fork for PID namespace (see 9.3), the **final child process** calls `setsid()` and `ioctl(TIOCSCTTY)` on the native PTY slave *immediately* before binary execution. 

**Why This Matters**: Calling `setsid()` in the final process (and not an intermediate parent) is critical for compatibility with tools like the `login` command. Since `login` itself calls `setsid()`, it must be able to re-acquire the controlling terminal. If an intermediate process holds the session lock, `login` will hang. Droidspaces matches the LXC/Docker attachment model here for perfect POSIX behavior.

This fixes the "not a tty" errors and ensures that `tty` and `ps aux` show the correct `/dev/pts/N` device *relative to the container*.

### 9.7 Cgroup Attachment (The `su` Fix)
A critical architectural requirement for Systemd-based containers is that any process entering the container must be accounted for in the container's cgroup hierarchy.
- **The Issue**: If a process enters namespaces but remains in the host's root cgroup (or the Droidspaces manager cgroup), `systemd-logind` and `sd-pam` inside the container will fail to map the process to a valid session. This results in the `su` command hanging indefinitely or failing to initialize the session.
- **The Fix**: Before joining any namespaces, the `enter` command identifies the container's host-side cgroup path (`/sys/fs/cgroup/droidspaces/<name>`) and **physically attaches** the process to it by writing to `cgroup.procs`.
- **Result**: Since the process is in the correct cgroup *before* it enters the PID/MNT namespaces, it is instantly recognized as "part of the container" by the container's init system, allowing `su`, `sudo`, and `login` to function natively without PAM workarounds.

### 9.3 The Double-Fork for PID Namespace

After `setns(CLONE_NEWPID)`, a `fork()` is required because the calling process remains in its original PID namespace:

```c
pid_t child = fork();           // First fork: joins PID namespace
if (child == 0) {
    enter_namespace(pid);
    pid_t shell_pid = fork();   // Second fork: actually in the container PID tree
    if (shell_pid == 0) {
        /* In PID 1 namespace view */
        execve("/bin/bash", argv, environ);
    }
    waitpid(shell_pid, NULL, 0);
}
waitpid(child, NULL, 0);
```


### 9.4 Shell Selection

The `enter` command tries shells in order: `/bin/bash` → `/bin/ash` → `/bin/sh`. If a user argument is provided, it uses `su -l <user>` instead.

### 9.5 Environment Setup

The `enter` and `run` commands set a clean environment:
- Standard `PATH`, `TERM=xterm-256color`, `HOME=/root`, `container=droidspaces`, `LANG=C.UTF-8`
- Sources `/etc/environment` if present (with proper KEY=VALUE and quote handling)

### 9.6 Run Command

`run_in_rootfs()` follows the same namespace entry pattern but executes the user-specified command instead of a shell. If the command string contains spaces and is a single argument, it's wrapped with `/bin/sh -c`.

---

## 10. Stop/Restart

### 10.1 Stop Sequence

`stop_rootfs()` in `container.c`:

**Step 1 — Resolve PID:**
Read pidfile, validate with `kill(pid, 0)`. Capture rootfs path for firmware cleanup while the process is still alive.

**Step 2 — Graceful shutdown:**
```c
kill(pid, SIGRTMIN + 3);
```
`SIGRTMIN+3` is the documented systemd signal for "halt immediately, don't shut down services". This is faster than running `poweroff` inside the container.

**Step 3 — Wait with escalation:**
```c
for (int i = 0; i < DS_STOP_TIMEOUT * 5; i++) {
    if (kill(pid, 0) < 0 && errno == ESRCH) { stopped = 1; break; }
    usleep(200000);  /* 200ms */
    if (i == 10) {
        kill(pid, SIGTERM);  /* Fallback after 2s */
    }
}
```

**Step 4 — Force kill:**
```c
if (!stopped) {
    kill(pid, SIGKILL);
}
```

`SIGKILL` to the container's init PID causes the kernel to tear down the entire PID namespace and kill all processes within it.

**Step 5 — Cleanup:**
```c
cleanup_container_resources(cfg, 0, skip_unmount);
```
- Restore Android optimizations (`android_optimizations(0)`)
- **Robust Cleanup (v3.2.1+)**: Adds a settle-time for lazy unmounts (`MNT_DETACH`) before attempting to remove host-side mount directories. This prevents leftover empty directories in `/mnt/Droidspaces/` when the kernel is slow to release loop devices.
- Remove firmware path entry
- **Cleanup Volatile Overlay (v3.3.0+)**: Calls `cleanup_volatile_overlay()`. This is a **namespace-aware passive cleanup**. It checks `/proc/self/mountinfo`; if the overlay is already unmounted by the kernel (due to namespace teardown), it skips straight to directory removal. If the mounts are still visible (e.g., during a manual `stop` of a live container), it performs a standard `umount`. All over-engineered retry loops and process-killing logic from v3.2.x have been removed for a leaner runtime.
- Unmount rootfs.img if applicable (unless `skip_unmount` for restart)
- Remove `.mount` sidecar file
- Remove pidfile

- **Restart Coordination (v4.2.2+)**: If `skip_unmount` is true, `stop_rootfs()` creates a `.restart` marker file in the PIDs directory. The monitor process uses this to skip its automatic resource cleanup, preserving the loop mount and pidfile for the subsequent `start` command.

### 10.2 Restart (Fast Path)

Restart is an orchestrated `stop` + `start` that leverages the coordination marker for a "fast-path" boot:

1. **Stop (Orchestration)**: Calls `stop_rootfs(cfg, 1)`.
   - Creates the `.restart` marker.
   - Kills the container init.
2. **Monitor (Preservation)**: Detects the marker after `waitpid()` returns and exits *without* calling `cleanup_container_resources()`. This avoids unmounting the rootfs.
3. **Start (Reuse)**: Calls `start_rootfs(cfg)`.
   - **Early Detection**: Checks for the marker as the first operation.
   - **Mount Reuse**: If the marker exists, it resolves the pidfile, reads the `.mount` sidecar, and identifies the existing host-side mount point.
   - **Fast Boot**: It adopts the existing mount as `cfg->rootfs_path` and skips name generation, image mounting, and `e2fsck`.

This ensures restarts are near-instant (under 200ms) and robust against race conditions between the CLI and background monitor.

### 10.3 Multi-Container Stop

The `--name` flag accepts comma-separated names:
```
droidspaces --name=web,db,cache stop
```

Each name is parsed with `strtok()`, a subcfg is created, and `stop_rootfs()` is called individually.
---

## 11. Android Compatibility & Seccomp Shielding

Droidspaces v4.3.2 implements a refined **Adaptive Seccomp Shield** in `src/android_seccomp.c`. This logic provides deep compatibility for legacy Android kernels while maximizing features on newer systems.

### 11.1 The Problems Being Solved

#### A. The Keyring / FBE Conflict (ENOKEY)
On Android hosts with File-Based Encryption (FBE), the filesystem encryption keys are stored in the kernel's **session keyring**. When `systemd` (specifically `journald` or `logind`) attempts to create a new session keyring using `keyctl(KEYCTL_JOIN_SESSION_KEYRING)`, the process effectively loses access to the host-provided keys.
- **Symptoms**: The container suddenly reports "Required key not available" (ENOKEY) when accessing encrypted files, leading to a system crash.
- **Fix**: The Seccomp filter intercepts `keyctl`, `add_key`, and `request_key` and returns `ENOSYS`. This tricks systemd into falling back to the existing session keyring.

#### B. The Namespace Deadlock (grab_super)
On legacy Android kernels (notably 4.14, 4.9, and below), systemd's use of mount namespaces for service sandboxing (`PrivateTmp=yes`, `ProtectSystem=yes`) triggers a race condition in the kernel's `grab_super` path.
- **Symptoms**: The Entire system hangs in a "D" state deadlock during the boot of services like `systemd-resolved` or `systemd-networkd`.
- **Fix**: The filter monitors `unshare` and `clone`. If flags such as `CLONE_NEWNS` or `CLONE_NEWPID` are requested, it returns `EPERM`. Systemd gracefully fallbacks to running the service in the main container namespace, bypassing the deadlock.

### 11.2 Conditional Shield Architecture (v4.3.2)

The shield is applied surgically based on both the host kernel and the guest OS type to maximize freedom for non-systemd containers:

1. **Host Discovery**: At boot, Droidspaces probes `uname()` and registers the kernel version.
2. **Guest Discovery**: `internal_boot()` calls `is_systemd_rootfs()` to check for `systemd` binary locations or symlinks in the rootfs.
3. **Application Matrix**:
    - **Kernel 5.0+**: The filter is **skipped entirely**. Modern kernels have resolved the `grab_super` races and handle keyrings robustly.
    - **Legacy Kernel (< 5.0)**:
        - **Keyring/FBE Fix**: **Always Applied**. Syscalls like `keyctl` return `ENOSYS` to ensure container stability on Android hosts.
        - **Namespace/Deadlock Fix**: **Conditional**. Only applied if `systemd` is detected. Non-systemd containers (Alpine) are granted full namespace freedom (Docker/LXC).

### 11.3 Filter Logic Implementation (BPF)

The filter uses the following logic flow in `src/android_seccomp.c`:
1. **Keyring Check**: If syscall is `keyctl`, `add_key`, or `request_key`, return `RET_ERRNO(ENOSYS)`. This part is always active on legacy kernels.
2. **Conditional Jump**: If the container is NOT using systemd, the next section is skipped.
3. **Namespace Check**: 
   - If syscall is `unshare` or `clone`.
   - Load arguments and check against `ns_mask` (`0x7E020000`).
   - If a namespace flag match is found, return `RET_ERRNO(EPERM)`.
4. **Default**: `SECCOMP_RET_ALLOW`.

This "Shield" acts as a compatibility layer that makes legacy hardware feel like modern hardware to the container's init system.

---

## 12. Network Isolation — NAT Mode

This section documents the most complex subsystem in Droidspaces: full network isolation with internet access on Android. It took longer to get right than everything else combined.

### 12.1 Why Docker and LXC Fail on Android

Before explaining what Droidspaces does, it's worth understanding why every other container runtime fails at the network layer on Android — even when `lxc-checkconfig` or `docker info` shows all kernel features as "enabled".

**`CONFIG_ANDROID_PARANOID_NETWORK`** is the primary killer. Even with every netfilter config compiled in, Android restricts socket creation to processes in specific GID groups (`inet`, `net_raw`, `net_admin`). Docker and LXC daemons run as root but outside those GIDs — `socket(AF_INET, ...)` silently fails with `EPERM`. The daemon tries to create `docker0` or `virbr0`, the kernel denies it, and the daemon gives up. `lxc-checkconfig` only reads `/proc/config.gz` — it has zero awareness of this GID restriction.

**Android SELinux policy** denies `create` on `tun_socket`, `netlink_route_socket`, and `rawip_socket` for anything that isn't a labeled Android service. The Docker daemon gets no label — every netlink operation is blocked before it reaches the kernel's netfilter code.

**`/proc/sys/net` lockdown** — Android init writes-protects several sysctl knobs early in boot. `ip_forward` and `conf/all/forwarding` — Docker tries to enable these and silently fails.

**No `netd` cooperation** — On stock Android, all network interface creation goes through `netd` (the network daemon). Creating `docker0` bypasses `netd` entirely; Android's routing rules then ignore those interfaces because they were never registered with `netd`'s routing table management.

Droidspaces sidesteps all of this by:
1. Requiring a custom kernel with `CONFIG_ANDROID_PARANOID_NETWORK=n`
2. Running under SELinux permissive mode (user-controlled)
3. Using pure RTNETLINK and raw socket iptables — no external binaries, no `netd`
4. Implementing its own DHCP server and route monitor

### 12.2 The Three Network Modes

| Mode | `CLONE_NEWNET` | Internet | Use Case |
|---|---|---|---|
| `--net=host` (default) | No | Yes (shared) | Simplicity, no isolation needed |
| `--net=nat` | Yes | Yes (via NAT) | Isolation with internet access |
| `--net=none` | Yes | No | Air-gapped, security-critical |

For `nat` and `none`, `CLONE_NEWNET` is added to the `unshare()` call in the intermediate process. The container gets a completely private network stack — no host interfaces are visible inside.

### 12.3 The Three-Pipe Handshake

Creating the veth pair requires a precise coordination sequence between the monitor and the container init. A race condition here would mean trying to `open("/proc/<pid>/ns/net")` before the process exists, or the container's DHCP client running before the host has set up the interface.

Three pipes solve this:

```
mid_sync_pipe:   intermediate ──► monitor   (sends init_pid)
net_ready_pipe:  init          ──► monitor   (signals "I'm alive in my netns")
net_done_pipe:   monitor       ──► init      (sends ds_net_handshake)
```

**Sequence:**

1. Intermediate process forks init, writes `init_pid` to `mid_sync_pipe`, then waits.
2. Init signals "READY" on `net_ready_pipe` — it is alive in its private netns.
3. Monitor reads `init_pid`, then reads the READY byte — this is the synchronization point that ensures `/proc/<init_pid>/ns/net` is valid.
4. Monitor calls `setup_veth_host_side(cfg, init_pid)` — creates bridge, veth pair, iptables rules, moves peer veth into container's netns.
5. Monitor derives `ds_net_handshake` (peer interface name + IP) and writes it to `net_done_pipe`.
6. Init reads the handshake, calls `setup_veth_child_side_named()` — renames the interface to `eth0`, brings it up.
7. Container's DHCP client (`dhclient`/`udhcpc`) sends DISCOVER; the embedded DHCP server responds with the pre-computed offer.

For `--net=none`, the same pipe exchange happens (monitor sends an empty handshake), and init just brings up loopback — no veth, no DHCP.

### 12.4 Subnet and Deterministic IP Assignment

The NAT network uses `172.28.0.0/16` (`DS_DEFAULT_SUBNET`). Gateway is always `172.28.0.1` (`DS_NAT_GW_IP`).

Container IPs are derived deterministically from the init PID using a multiplicative hash:

```c
uint32_t hash = (uint32_t)pid;
hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
int octet3 = (int)(((hash >> 8) % 254) + 1);  // 1–254, skips 0 (infrastructure row)
int octet4 = (int)((hash % 254) + 1);          // 1–254, skips 0 and 255
// Result: 172.28.<octet3>.<octet4>/16
```

This spreads sequential PIDs across the entire /16 space, preventing address collisions between concurrent containers without requiring any state or allocation table. The DHCP server is pre-loaded with this IP as the only offer — it never negotiates.

### 12.5 veth Pair and Bridge Topology

**Interface naming** is also PID-derived:
- Host side: `ds-v<pid % 100000>` (e.g. `ds-v11388`)
- Container peer: `ds-p<pid % 100000>` (e.g. `ds-p11388`)

**Bridge mode** (default): The host veth is attached to `ds-br0`. The bridge holds `172.28.0.1`. DHCP server binds to `ds-br0` because the kernel delivers frames from the container to the bridge interface, not the slave — a socket bound to the slave would never see DHCP DISCOVERs.

**Bridgeless fallback**: On kernels where bridge creation fails, `ds-br0` is skipped. `172.28.0.1/32` is assigned directly to the host veth, and a host route for the container IP is installed. The DHCP server binds to the veth directly.

**Samsung/MTK TX checksum workaround**: After creating the veth, `ds_net_disable_tx_checksum()` clears `NETIF_F_ALL_CSUM` on the host veth via `ETHTOOL_SFEATURES`. This fixes silent packet drops on certain Qualcomm and MediaTek SoCs where the virtual NIC's checksum offload interacts badly with the software bridge.

### 12.6 iptables Rules

All iptables operations use the raw socket API via `ds_iptables.c` — no `iptables` binary is ever executed. Rules are installed in the correct order:

```
POSTROUTING MASQUERADE  — on all upstream interfaces, src 172.28.0.0/16
FORWARD ACCEPT          — on ds-br0 (or ds-vXXXXX in bridgeless)
INPUT ACCEPT            — on ds-br0 (for DHCP and DNS)
MSS clamp               — TCPMSS --clamp-mss-to-pmtu (fixes MTU issues)
bridge-nf-call-iptables — set to 0 to stop bridge frames hitting iptables twice
rp_filter               — set to 0 on ds-br0 (Android strict mode breaks NAT)
```

### 12.7 Port Forwarding and the xt_addrtype Fallback

Port forwarding is specified as `--port HOST:CONTAINER/PROTO` (e.g. `--port 22:22/tcp`).

For each mapping, a PREROUTING DNAT rule is inserted:
```
-t nat -A PREROUTING -p tcp --dport 22 -m addrtype --dst-type LOCAL -j DNAT --to-destination 172.28.x.x:22
```

The `--dst-type LOCAL` match restricts the rule to packets destined for the phone's own IP — preventing the rule from accidentally matching transit traffic.

**The 4.14 problem**: `xt_addrtype` (`CONFIG_NETFILTER_XT_MATCH_ADDRTYPE`) is not compiled into stock Android 4.14 kernels. If the module is absent, the DNAT insert fails silently.

**The fix**: Before installing any port forward rules, `addrtype_available()` reads `/proc/net/ip_tables_matches` for the `addrtype` string. If absent, the rules are installed without `--dst-type LOCAL` — slightly less precise but functional. Both paths use `run_command_log()` which pipes stderr through `ds_log("[IPT] ...")` so failures are always visible in the log.

### 12.8 The Embedded DHCP Server

`ds_dhcp.c` implements a minimal RFC 2131 DHCP server as a **joinable thread** inside the monitor process. It serves exactly one lease — the deterministic IP pre-computed by `veth_peer_ip()`.

**Why embedded instead of `dnsmasq`?**
- No external binary dependency — Droidspaces is a single static binary
- `dnsmasq` isn't available on most Android systems
- One container = one lease = trivial implementation (no lease table needed)
- The server can be MAC-targeted: it reads the peer veth's MAC before moving it into the container netns, and only responds to DHCP packets from that MAC

**Protocol flow**:
1. Container's DHCP client broadcasts DISCOVER
2. Server responds with OFFER (`yiaddr` = pre-computed IP, `siaddr` = gateway, lease time = 24h)
3. Client sends REQUEST
4. Server sends ACK
5. Client handles renewals (server responds to every REQUEST with the same ACK)

**Stop/join safety**: `ds_dhcp_server_stop()` sets `stop=1`, calls `shutdown()` on the socket to unblock the `recvfrom()`, saves the `tid`, then calls `pthread_join(tid)` **outside the mutex**. This guarantees the thread has fully exited before `start()` can call `memset(&g_dhcp)` on a restart cycle. Without the join, two DHCP threads could run simultaneously with a corrupted shared context.

### 12.9 Android Policy Routing — The `--upstream` Flag

Android does not use a single default route. Every interface (`wlan0`, `rmnet_data0`, etc.) has its own routing table. The kernel uses policy rules (`ip rule`) to decide which table to consult based on source address, interface, or UID. There is no single "active" interface — both WiFi and mobile data can have simultaneous default routes in their own tables.

Old approach (auto-detection): Parse `ip rule` output to guess which table was "active". Completely unreliable on MTK/Qualcomm where both interfaces have rules of equal priority.

**Current approach**: The user explicitly declares which interfaces can carry upstream traffic via `--upstream wlan0,rmnet_data0`. This list is mandatory for `--net=nat`.

On Android, after moving the peer veth into the container netns, `ds_net_setup_android_routing()` runs:

```
ip rule add from 172.28.0.0/16 lookup <active_table> priority 100
ip rule add to 172.28.0.0/16 lookup <active_table> priority 200
```

`<active_table>` is the routing table number of the first declared upstream interface that is currently `IFF_UP | IFF_RUNNING` and has a default route. `ds_nl_get_iface_table()` reads this via RTNETLINK — no `ip` binary needed.

This injects low-priority rules that direct container traffic through whichever upstream table is active, without disturbing Android's own routing rules.

### 12.10 The Upstream Route Monitor

The user can switch from WiFi to mobile data (or vice versa) while a container is running. Without handling this, the container would lose internet when the active interface changes.

The route monitor is a `pthread` running `route_monitor_loop()` inside the monitor process. It:

1. Opens a `NETLINK_ROUTE` socket subscribed to `RTMGRP_LINK | RTMGRP_IPV4_IFADDR`
2. Calls `do_upstream_reprobe()` on any link state or address change event, but only for events on the declared upstream interfaces
3. Runs a **30-second heartbeat** via `poll()` timeout to catch devices with broken netlink notifications (common on some MTK SoCs) and to re-assert `ip_forward=1` which Android's `netd` can reset at any time
4. On reprobe, calls `find_active_upstream()` — iterates the declared upstream list in priority order, returns the first interface that is `IFF_UP | IFF_RUNNING` with a routing table
5. If the active table changed, atomically swaps the `ip rule` (delete old, add new) and logs `[NET] Route monitor: upstream switch table X → Y`

The monitor is started by `ds_net_start_route_monitor()` immediately after `setup_veth_host_side()` succeeds, and stopped by `ds_net_stop_route_monitor()` in `ds_net_cleanup()` before any teardown.

### 12.11 Multi-Container Cleanup Safety

Several iptables rules are **shared** across all containers — `MASQUERADE`, `FORWARD ACCEPT`, and the Android policy `ip rule` entries. Removing them while other containers are still running would immediately kill their networking.

`ds_net_cleanup()` handles this safely:

1. Deletes the container's own veth (`ds-vXXXXX`)
2. Calls `ds_nl_count_ifaces_with_prefix(ctx, "ds-v")` — counts remaining `ds-v*` interfaces via RTM_GETLINK dump
3. If count > 0: other containers are still running → only remove per-container rules (port forwards, bridgeless iface rules) → return early
4. If count == 0: last container → remove shared rules (MASQUERADE, FORWARD, Android policy rules, bridge rules)

This is the correct check because veth deletion is atomic — if `ds-vXXXXX` is gone but other `ds-v*` interfaces exist, those other containers are definitely still alive.

---

## 13. Status Reporting

### 12.1 Status Command

`check_status()` resolves the pidfile (auto-detect if only one container running), reads the PID, validates with `kill(pid, 0)`, and checks `is_valid_container_pid()` (which reads `/proc/<pid>/root/run/systemd/container` for "droidspaces").

### 12.2 Info Command

`show_info()` provides detailed introspection:

1. **Auto-Resolution:** If no container name is specified, it auto-detects the running container if only one is found. If multiple are running, it lists them.
2. **Feature Introspection:** It detects active features by probing the container's `/proc/<pid>/root`:
   - **SELinux:** Reads `/sys/fs/selinux/enforce`.
   - **IPv6:** Checks `/proc/sys/net/ipv6/conf/all/disable_ipv6` inside the container.
   - **Android Storage:** Verifies the mount at `/storage/emulated/0`.
   - **HW Access:** Checks the `/dev` filesystem type.
3. **Guest OS Info:** Reads `/etc/os-release` from the container's rootfs (even if stopped).

### 12.3 Show Command

`show_containers()` lists all running containers in a table:

```
┌──────────────┬──────────┐
│ NAME         │ PID      │
├──────────────┼──────────┤
│ ubuntu-24.04 │ 12345    │
│ alpine-3.19  │ 23456    │
└──────────────┴──────────┘
```

It reads all `.pid` files from the Pids directory, validates each PID, removes stale pidfiles, and renders a Unicode box-drawing table with dynamic column widths.

### 12.4 Scan Command

`scan_containers()` scans *all* PIDs in `/proc` for processes that:
1. Pass `is_valid_container_pid()` (have `/run/systemd/container` containing "droidspaces")
2. Are init processes in their namespace (checked via `/proc/<pid>/status` NSpid field — last value is 1)
3. Are not already tracked in the Pids directory

Found containers are automatically registered with auto-generated names from `/proc/<pid>/root/etc/os-release`.

---

## 14. Build System Notes

### Compiler and Libc

The Makefile compiles with musl libc for maximum portability across Android devices:

```makefile
CFLAGS  = -Wall -Wextra -Wpedantic -Werror -O2 -flto -std=gnu99 -Isrc -no-pie -pthread
CFLAGS += -Wformat=2 -Wformat-security -Wformat-overflow=2 -Wformat-truncation=2
CFLAGS += -Wnull-dereference -Wcast-qual -Wlogical-op
CFLAGS += -Wduplicated-cond -Wduplicated-branches -Wimplicit-fallthrough=3
LDFLAGS = -static -no-pie -flto -pthread
LIBS    = -lutil
```

Key flags:
- **`-static`**: Statically linked binary. No shared library dependencies.
- **`-no-pie`**: Non-position-independent executable. Some Android kernels have issues with PIE static binaries.
- **`-flto`**: Link-time optimization for smaller binary size.
- **`-std=gnu99`**: GNU C99 dialect (needed for `_GNU_SOURCE` features).
- **`-lutil`**: Links `libutil` for `openpty()` support.

### Cross-Compilation

Four architectures are supported:
- `x86_64`: `x86_64-linux-musl-gcc`
- `x86`: `i686-linux-musl-gcc`
- `aarch64`: `aarch64-linux-musl-gcc`
- `armhf`: `arm-linux-musleabihf-gcc`

Cross-compiler search order: `$MUSL_CROSS`, `~/toolchains/<triple>-cross/bin/`, `PATH`, `/opt/cross/bin/`.

### Output

All binaries go to `output/droidspaces`. An `all-build` target creates architecture-specific binaries (`droidspaces-x86_64`, `droidspaces-aarch64`, etc.). The `all-tarball` target creates a unified distribution with all architectures in a single archive.

---

## 15. Data Structures

### 14.1 struct ds_config

The central configuration struct, passed to every function (replaces all global variables from v2):

```c
struct ds_config {
    /* Paths */
    char rootfs_path[PATH_MAX];       /* --rootfs=   */
    char rootfs_img_path[PATH_MAX];   /* --rootfs-img= */
    char pidfile[PATH_MAX];           /* --pidfile= or auto-resolved */
    char container_name[256];         /* --name= or auto-generated */
    char hostname[256];               /* --hostname= or container_name */
    char dns_servers[1024];         /* --dns= (comma/space separated) */
    char dns_server_content[1024];  /* In-memory DNS config for boot */
    char uuid[DS_UUID_LEN + 1];       /* UUID for PID discovery */

    /* Flags */
    int foreground;                   /* --foreground */
    int hw_access;                    /* --hw-access */
    int enable_ipv6;                  /* --enable-ipv6 */
    int android_storage;              /* --enable-android-storage */
    int selinux_permissive;           /* --selinux-permissive */
    char prog_name[64];               /* argv[0] for logging */

    /* Runtime state */
    pid_t container_pid;              /* PID 1 of the container (host view) */
    pid_t intermediate_pid;           /* intermediate fork pid */
    int is_img_mount;                 /* 1 if rootfs was loop-mounted from .img */
    char img_mount_point[PATH_MAX];   /* where the .img was mounted */

    /* Terminal (console + ttys) */
    struct ds_tty_info console;
    struct ds_tty_info ttys[DS_MAX_TTYS];
    int tty_count;                    /* how many TTYs are active */
};
```

### 14.2 struct ds_tty_info

```c
struct ds_tty_info {
    int master;            /* master fd (stays in parent/monitor) */
    int slave;             /* slave fd (bind-mounted into container) */
    char name[PATH_MAX];   /* slave device path (e.g. /dev/pts/3) */
};
```

### 14.3 Constants

| Constant | Value | Purpose |
|---|---|---|
| `DS_MAX_TTYS` | 6 | Maximum TTY devices per container |
| `DS_UUID_LEN` | 32 | UUID hex string length |
| `DS_MAX_CONTAINERS` | 1024 | Maximum auto-naming suffix |
| `DS_STOP_TIMEOUT` | 8 | Seconds before SIGKILL |
| `DS_PID_SCAN_RETRIES` | 20 | UUID scan retry count |
| `DS_PID_SCAN_DELAY_US` | 200000 | 200ms between retries |

---

## 16. Recommended Reimplementation Approach

If you're rewriting Droidspaces from scratch, here is the checklist of every decision and every syscall, in order.

### Phase 1: Configuration

1. Parse CLI arguments or `.config` files into a `struct ds_config`:
   - `rootfs_path` or `rootfs_img_path`
   - `container_name` (auto-generate from os-release if not provided)
   - `hostname`
   - `foreground`, `hw_access`, `enable_ipv6`, `android_storage`, `selinux_permissive`
   - Capture unrecognized lines for recovery preservation.

2. **Early Restart Detection & Concurrency Lock**: 
   - Check if an external command lock (`.lock`) is active via `is_external_lock_active()`.
   - If active, the container is being restarted. Resolve pidfile, and read `.mount` sidecar to identify the existing mount.
   - If mount active, set `cfg->rootfs_path` and `restart_reuse = 1`.
   - Acquire lock via `acquire_external_lock()` to establish single-writer ownership over the state transition.

3. Validate rootfs path exists and contains `/sbin/init`.

4. Resolve pidfile path: `<workspace>/Pids/<name>.pid`.

5. Check if already running: `kill(saved_pid, 0)` + validate.

6. Run requirements check: namespaces, devtmpfs, pivot_root

### Phase 2: Pre-Fork Setup (in parent)

6. Generate UUID (32 hex chars from `/dev/urandom`)

7. **Rootfs image mount**: If `-i` provided:
   - Identify host-side mount point at `/mnt/Droidspaces/<name>`.
   - `e2fsck -f -y <img>`
   - **SELinux Hardening**: Apply `u:object_r:vold_data_file:s0` to the image file via `chcon` before mounting to prevent silent I/O failures.
   - **Mount with Retry (4.14 resilience)**: Try `mount -o loop,ro <img> <mount_point>`. If it fails (common on Kernel 4.14 due to stale loop state), the system calls `sync()`, waits 1 second, and retries up to 3 times. Log output only shows the "Attempt X/Y" counter if the first try fails.

8. **Allocate PTYs in the parent** (LXC model):
   - `openpty()` × (1 console + N TTYs)
   - Record slave names (e.g., `/dev/pts/0`, `/dev/pts/1`, ...)
   - Mark all FDs `FD_CLOEXEC`

9. Create sync pipe for PID communication

10. Android optimizations: `max_phantom_processes`, disable `deviceidle`

### Phase 3: Fork Monitor + Fork Intermediate + Fork Init

11. `fork()` → monitor process

12. In monitor: `setsid()`, `prctl(PR_SET_NAME, "[ds-monitor]")`.
    - **Sub-Cgroup Jailing**: Create `/sys/fs/cgroup/droidspaces` and move the monitor PID into it.
    - **Host Isolation**: `unshare(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWCGROUP)` (Note: No PID namespace here).

13. `fork()` again → intermediate process.
    - **Namespace Creation**: `unshare(CLONE_NEWPID)` — Creates a fresh PID tree for the new boot session.

14. `fork()` again inside intermediate → PID 1 child (this is the process that becomes init). Intermediate begins `waitpid()` blocking on the init process.

15. Monitor sends `init_pid` to parent via sync pipe, then `waitpid()` on the intermediate process.
    - If intermediate exits with code 249 (indicating init called `reboot(2)` and generated `SIGHUP`), the monitor loops back to step 13 to spawn a fresh intermediate/init hierarchy.
    - If intermediate exits normally, the monitor checks `is_external_lock_active()`.
    - If lock absent, the monitor calls `cleanup_container_resources()`. Otherwise, it hands off cleanup to the CLI.
    - Monitor exits.

### Phase 4: internal_boot() (runs as PID 1)

16. `unshare(CLONE_NEWNS)` — own mount namespace

17. `mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)` — prevent mount propagation

18. **Optional: Setup Volatile Overlay** (`setup_volatile_overlay()`):
    - Verify OverlayFS support.
    - Setup `upperdir`/`workdir` in a private `tmpfs`.
    - Mount OverlayFS over the rootfs and update `cfg->rootfs_path`.

19. Bind mount rootfs to itself: `mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL)`

20. **Optional: Custom Bind Mounts** (`setup_custom_binds()`):
    - Iterate entries and bind-mount host paths to container targets.
    - Use `mkdir_p` for automatic destination creation.

21. `chdir(rootfs)`

22. `mkdir(".old_root", 0755)`

23. Setup `/dev`:
    -   Without `--hw-access`: `mount("none", "<rootfs>/dev", "tmpfs", ...)` + `mknod()` for null, zero, full, random, urandom, tty, console, ptmx, net/tun, fuse
    -   With `--hw-access`: `mount("devtmpfs", "<rootfs>/dev", "devtmpfs", ...)`, unlink conflicting nodes, then recreate with `mknod()`

24. `mount("proc", "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)`

25. Mount sysfs:
    -   Without `--hw-access`: mount RW → mount separate sysfs instance at `sys/devices/virtual/net` → remount parent RO
    -   With `--hw-access`: mount RW → **dynamic self-bind mount** all subdirectories → remount parent RO

26. `mount("tmpfs", "run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=755")`

27. **Bind-mount PTY slaves** from parent:
    -   `mount(cfg->console.name, "dev/console", NULL, MS_BIND, NULL)`
    -   `mount(cfg->ttys[i].name, "dev/tty<N>", NULL, MS_BIND, NULL)` for each TTY

28. `write_file("run/<uuid>", "init")` — UUID marker for PID discovery/boot confirmation

29. **Setup cgroups** (`setup_cgroups()`): 
    - Discover host hierarchies via `mountinfo`.
    - If in cgroup namespace, direct-mount `cgroup`/`cgroup2`.
    - If legacy, bind-mount subtrees.
    - Handle v1 comounts and create symlinks.

30. Optional: bind mount Android storage

31. **`syscall(SYS_pivot_root, ".", ".old_root")`**

32. `chdir("/")`

33. `mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,newinstance,ptmxmode=0666,mode=0620")`

34. Setup `/dev/ptmx`: bind mount `/dev/pts/ptmx` → `/dev/ptmx`

35. Configure networking (rootfs side): hostname, multi-DNS, resolv.conf, Android groups

36. `umount2("/.old_root", MNT_DETACH)`, `rmdir("/.old_root")`

37. `write_file("/run/systemd/container", "droidspaces")`

38. `clearenv()`, set `PATH`, `TERM`, `HOME`, `container`, `container_ttys`

39. Redirect stdio:
    ```c
    int fd = open("/dev/console", O_RDWR);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    setsid();
    ioctl(0, TIOCSCTTY, 0);
    ```

40. `execve("/sbin/init", {"init", NULL}, environ)`

### Phase 5: Parent — Post-Fork

41. Read `init_pid` from sync pipe

42. Configure host-side networking (ip_forward, IPv6, multi-DNS file, iptables)

43. Save PID to pidfile

44. If foreground: enter console monitor loop (epoll: stdin↔pty_master, signalfd for SIGCHLD/SIGINT/SIGTERM/SIGWINCH)

45. If background: show info and exit (monitor process handles cleanup)

### Phase 6: Stop

46. Read PID from pidfile, validate.

47. Acquire the external command lock (`.lock`) to prevent race conditions with the background monitor process.

48. `kill(pid, SIGRTMIN + 3)` for systemd

49. Wait up to 8 seconds with 200ms polling

50. Escalate to `SIGTERM` after 2 seconds

51. If still alive: `kill(pid, SIGKILL)`

52. Cleanup: remove pidfile, unmount rootfs.img, restore firmware path, restore Android settings, cleanup volatile overlay (namespace-aware).
    - If part of a `restart`, do NOT release the lock so the subsequent `start` command can inherit the active mount state. Otherwise, release the lock.

### Phase 7: Enter

52. Read PID from pidfile
53. **Cgroup Attachment**: Attach the process to the container's host-side cgroup (`/sys/fs/cgroup/droidspaces/<name>`).
54. Open `/proc/<pid>/ns/{mnt,uts,ipc,pid}` — all FDs at once
55. `setns()` for each
56. `fork()` (required after `setns(CLONE_NEWPID)`)
57. In child: `fork()` again to actually be in the new PID namespace
58. `clearenv()`, set environment, source `/etc/environment`
59. `execve()` shell: try `/bin/bash`, `/bin/ash`, `/bin/sh`

---

## 17. Project Licensing & SPDX

The Droidspaces source code is licensed under the **GNU General Public License v3.0 or later (GPL-3.0-or-later)**. 

Every source file in the `src/` directory and the `Makefile` contain standard SPDX-License-Identifier tags and copyright headers. A full copy of the license is provided in the `LICENSE` file at the root of the project.

**Copyright (C) 2026 Ravindu <ravindu644>**

---

## 18. Document License

This document is licensed under the Creative Commons Attribution 4.0 International (CC BY 4.0) License.
You are free to share and adapt the material for any purpose, even commercially, provided you give appropriate credit,
provide a link to the license, and indicate if changes were made.

---

**End of Document**

*This document was updated by analyzing v4.7.3 of the Droidspaces source code — approximately 9,200 lines of C across 18 files. Every syscall, every mount, and every design decision described here was verified against the actual implementation.*
