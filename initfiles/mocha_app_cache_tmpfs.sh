#!/system/bin/sh
# [HongGuo crash fix 2026-11-26] Mount a tmpfs over the cache directory of
# GL-heavy short-video apps.
#
# Root cause of the SIGSEGV (SEGV_ACCERR) crashes in com.phoenix.read and
# every other short-video app: the NVIDIA GL driver (libglcore.so 343.00)
# JIT-compiles shaders into a cache file (.glXXXXXX) it creates in the app's
# cache dir, mmaps it twice (rw + rx), then UNLINKS it. The backing pages are
# clean file-backed pages of a deleted file, so the kernel can drop them
# freely - and on this 2 GB board a reclaim cycle hits within a minute of
# video playback. A dropped page is unrecoverable (no file to re-read), so
# the next frame jumps into a mapped-out page and the render thread dies:
#   Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR) in TRThread_0
#   #01 libglcore+0x95ae37 (_glDrawArrays)
#
# Moving the cache directory onto a tmpfs turns those pages into shmem pages:
# under pressure the kernel now swaps them out to zram instead of dropping
# them, and the driver's pointer stays valid. Verified on device: 7+ minutes
# of continuous short-drama playback with zero crashes (baseline: crash every
# 60-90 seconds).
#
# The mount is idempotent and survives nothing (tmpfs), so this script runs
# once per boot from init. The uid/gid of the cache dir is taken from the
# directory itself so it keeps matching the app even after reinstalls.

PACKAGES="com.phoenix.read com.mxtech.videoplayer.ad"

for pkg in $PACKAGES; do
    dir="/data/data/$pkg/cache"
    [ -d "$dir" ] || continue
    # Already mounted? /proc/mounts lists the mount point in field 2.
    if grep -q " $dir " /proc/mounts; then
        continue
    fi
    uid=$(stat -c %u "$dir" 2>/dev/null)
    gid=$(stat -c %g "$dir" 2>/dev/null)
    [ -n "$uid" ] || continue
    mount -t tmpfs -o uid=$uid,gid=$gid,mode=0700 tmpfs "$dir"
done

# Watch mode: run as a persistent init service. The user-unlock CE remount
# (/data_mirror/data_ce) clears the tmpfs mount a few minutes after boot, so
# re-apply it whenever it disappears. The loop is nearly free (a grep and a
# stat every 30 seconds).
if [ "$1" = "--watch" ]; then
    while true; do
        sleep 30
        for pkg in $PACKAGES; do
            dir="/data/data/$pkg/cache"
            [ -d "$dir" ] || continue
            grep -q " $dir " /proc/mounts && continue
            uid=$(stat -c %u "$dir" 2>/dev/null)
            gid=$(stat -c %g "$dir" 2>/dev/null)
            [ -n "$uid" ] || continue
            mount -t tmpfs -o uid=$uid,gid=$gid,mode=0700 tmpfs "$dir"
        done
    done
fi
