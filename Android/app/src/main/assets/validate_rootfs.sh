#!/system/bin/sh
# Rootfs Tarball Validator for Droidspaces
# Copyright (c) 2026 ravindu644
# Inspects a container tarball (WITHOUT extracting) to confirm it actually
# contains a Linux rootfs, so arbitrary archives cannot be installed.

# Parameters
TARBALL="$1"
BUSYBOX_PATH="${BUSYBOX_PATH:-/data/local/Droidspaces/bin/busybox}"
BB="$BUSYBOX_PATH"

log() { $BB echo "$1" >&2; }

# Check BusyBox
if [ ! -x "$BB" ]; then
    log "BusyBox not found or not executable at $BB"
    exit 1
fi

# Check tarball
if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ]; then
    log "Tarball not found: $TARBALL"
    exit 1
fi

# Pick a decompressor from the extension (mirrors the extractor).
case "$TARBALL" in
    *.xz) DECOMP="$BB xzcat" ;;
    *)    DECOMP="$BB zcat"  ;;
esac

# Stream entry names and confirm the core top-level directories every Linux
# rootfs has (bin, sbin, etc, usr). awk bails out the moment all four are
# found, so a valid rootfs is confirmed without decompressing the whole
# archive. Merged-usr distros still list bin/sbin as symlink entries, so this
# stays compatible. A tarball wrapped in a single top-level dir (e.g.
# rootfs/bin/...) is intentionally rejected - it would extract to the wrong
# layout anyway.
#
# Exception: NixOS ships none of that layout - /bin, /sbin and /usr are created
# by the activation script on first boot, so a top-level nix/ store is accepted
# on its own (same marker post_extract_fixes.sh uses to detect NixOS).
RESULT=$($DECOMP "$TARBALL" 2>/dev/null | $BB tar -tf - 2>/dev/null | $BB awk '
    {
        name = $0
        sub(/^\.\//, "", name)
        sub(/^\//, "", name)
        split(name, parts, "/")
        top = parts[1]
        if (top == "nix")  ok = 1
        if (top == "bin")  b = 1
        if (top == "sbin") s = 1
        if (top == "etc")  e = 1
        if (top == "usr")  u = 1
        if (ok || (b && s && e && u)) { ok = 1; print "OK"; exit }
    }
    END {
        if (!ok) {
            miss = ""
            if (!b) miss = miss " bin"
            if (!s) miss = miss " sbin"
            if (!e) miss = miss " etc"
            if (!u) miss = miss " usr"
            print "MISSING" miss
        }
    }')

case "$RESULT" in
    OK)
        exit 0
        ;;
    MISSING*)
        log "Not a Linux rootfs - missing top-level dirs:${RESULT#MISSING}"
        exit 2
        ;;
    *)
        log "Could not read archive (corrupt or not a .tar.gz/.tar.xz?)"
        exit 3
        ;;
esac
