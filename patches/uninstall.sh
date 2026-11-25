#!/usr/bin/env bash
# Reverse only this patch series, nothing else.
#
# Descending path order removes later patches first, so a patch that depends on
# an earlier one is undone before its base. It never runs git checkout or git
# clean, so local work outside the series is preserved.

set -u

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/patchlib.sh
. "$SCRIPT_DIR/lib/patchlib.sh"

pl_init
pl_parse_args "$@"
pl_collect

if (( PL_LIST )); then
    pl_list
    exit 0
fi

undone=()
uninstall_failed=0

for ((i=${#PL_PATCHES[@]} - 1; i >= 0; i--)); do
    patch="${PL_PATCHES[$i]}"
    relative_patch="$(pl_display "$patch")"

    if ! pl_verify_repo "$patch"; then
        uninstall_failed=1
        break
    fi

    if pl_is_applied "$patch"; then
        if git -C "$(pl_repo_path "$patch")" apply --reverse "$patch"; then
            printf 'REVERSED %s\n' "$relative_patch"
            undone+=("$patch")
        else
            printf 'ERROR: failed to reverse %s\n' "$relative_patch" >&2
            uninstall_failed=1
            break
        fi
    elif pl_applies "$patch"; then
        printf 'SKIP     %s (not applied)\n' "$relative_patch"
    else
        printf 'ERROR: cannot safely determine or reverse patch state: %s\n' "$relative_patch" >&2
        uninstall_failed=1
        break
    fi
done

if (( uninstall_failed != 0 )); then
    # Restore only what this invocation reversed, preserving unrelated staged
    # and unstaged work in every source repository.
    for ((i=${#undone[@]} - 1; i >= 0; i--)); do
        patch="${undone[$i]}"
        if ! git -C "$(pl_repo_path "$patch")" apply "$patch"; then
            printf 'ERROR: rollback failed for %s; inspect that repository before continuing.\n' \
                "$(pl_display "$patch")" >&2
        fi
    done
    exit 1
fi

printf '\nPatch removal complete. Unrelated source changes were left untouched.\n'
pl_rebuild_hint
