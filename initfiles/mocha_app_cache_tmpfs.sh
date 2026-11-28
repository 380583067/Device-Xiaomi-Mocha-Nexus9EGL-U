#!/system/bin/sh
# [2026-11-28 universal crash fix] Mount a tmpfs over EVERY app's cache
# directory, with a per-directory size cap.
#
# Root cause of the SIGSEGV (SEGV_ACCERR) crashes in com.phoenix.read,
# com.mxtech.videoplayer.ad and any other GL-heavy app: the NVIDIA GL driver
# (libglcore.so 343.00) JIT-compiles shaders into a cache file (.glXXXXXX) it
# creates in the app's cache dir, mmaps it twice (rw + rx), then UNLINKS it.
# The backing pages are clean file-backed pages of a deleted file, so the
# kernel can drop them freely - and on this 2 GB board a reclaim cycle hits
# within a minute of video playback. A dropped page is unrecoverable (no file
# to re-read), so the next GL call jumps into a mapped-out page and the
# render thread dies:
#   Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), pc = fault addr
#   #01 libglcore+0x95ae37 (_glDrawArrays)   <- same offset in every victim
#
# Moving the cache directory onto a tmpfs turns those pages into shmem pages:
# under pressure the kernel now swaps them out to zram instead of dropping
# them, and the driver's pointer stays valid. Verified on device: 7+ minutes
# of continuous short-drama playback with zero crashes (baseline: crash every
# 60-90 seconds). The 2026-11-28 MX Player loop crash (identical backtrace)
# showed the per-package allowlist approach does not scale - any GL-heavy app
# not on the list keeps crashing - so this version covers every app by
# default, with a size cap per mount to bound the aggregate RAM cost:
# tmpfs only consumes pages for populated files, and populated pages swap to
# the 1.5 GB zram instead of being lost.
#
# Per-directory cap: CACHE_TMPFS_SIZE (default 96m - the libglcore JIT caches
# are a few MB; 96m leaves headroom for regular cache files while capping the
# worst case per app). EXCLUDE_PACKAGES lists uids/apps to skip.
#
# The mounts are idempotent and survive nothing (tmpfs), so this script runs
# once per boot from init and re-applies from the --watch service after the
# user-unlock CE remount clears them. The uid/gid of each cache dir is taken
# from the directory itself so it keeps matching the app even after
# reinstalls.

CACHE_TMPFS_SIZE=96m
EXCLUDE_PACKAGES=""

do_mounts()
{
    # " $dir " membership test against one read of /proc/mounts.
    mounts=$(cat /proc/mounts)
    for dir in /data/data/*/cache; do
        [ -d "$dir" ] || continue
        pkg=${dir#/data/data/}; pkg=${pkg%/cache}
        case " $EXCLUDE_PACKAGES " in
            *" $pkg "*) continue ;;
        esac
        case "$mounts" in
            *" $dir "*|*" $dir/"*) continue ;;
        esac
        uid=$(stat -c %u "$dir" 2>/dev/null)
        gid=$(stat -c %g "$dir" 2>/dev/null)
        [ -n "$uid" ] || continue
        mount -t tmpfs -o uid=$uid,gid=$gid,mode=0700,size=$CACHE_TMPFS_SIZE tmpfs "$dir"
    done
    # Uninstall cleanup: a tmpfs mount whose cache dir no longer belongs to a
    # live app (dir deleted) is orphaned - drop it so the mount table does not
    # grow stale entries for uninstalled packages.
    mounts=$(cat /proc/mounts)
    echo "$mounts" | grep "tmpfs /data/data/" | while read -r _ _ mnt _; do
        pkg=$(echo "$mnt" | sed -E 's#^/data/data/([^/]+)/cache$#\1#')
        [ "$pkg" = "$mnt" ] && continue
        [ -d "/data/data/$pkg/cache" ] || umount "$mnt" 2>/dev/null
    done
}

do_mounts

# Watch mode: run as a persistent init service. The user-unlock CE remount
# (/data_mirror/data_ce) clears the tmpfs mounts a few minutes after boot, so
# re-apply them whenever they disappear. The loop is a single /proc/mounts
# read + a stat per app every 30 seconds.
if [ "$1" = "--watch" ]; then
    while true; do
        sleep 30
        do_mounts
    done
fi
