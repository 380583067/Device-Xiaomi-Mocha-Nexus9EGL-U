#!/usr/bin/env bash
# Validate the whole patch series without touching any source worktree or real
# Git index.
#
# Each patch is tested against a throwaway index that already contains the
# earlier patches of the same project, so dependent patches are checked in the
# order install.sh will apply them.

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

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mocha-patches-check.XXXXXX")" || exit 2
trap 'rm -rf -- "$TEMP_DIR"' EXIT

declare -A TEMP_INDEXES=()
index_number=0
errors=0
patch_count=0

for patch in "${PL_PATCHES[@]}"; do
    relative_patch="$(pl_display "$patch")"
    repo_relative="${relative_patch%/*}"
    repo_path="$(pl_repo_path "$patch")"
    patch_count=$((patch_count + 1))

    pl_verify_repo "$patch" || { errors=$((errors + 1)); continue; }

    # One throwaway index per source project lets later patches see earlier
    # patches while keeping the user's actual index and files untouched.
    if [[ -z "${TEMP_INDEXES[$repo_relative]+present}" ]]; then
        temp_index="$TEMP_DIR/index-$index_number"
        index_number=$((index_number + 1))
        if ! GIT_INDEX_FILE="$temp_index" git -C "$repo_path" read-tree HEAD ||
           ! GIT_INDEX_FILE="$temp_index" git -C "$repo_path" add -A; then
            echo "ERROR: cannot snapshot the current worktree for $repo_relative" >&2
            errors=$((errors + 1))
            continue
        fi
        TEMP_INDEXES[$repo_relative]="$temp_index"
    fi
    temp_index="${TEMP_INDEXES[$repo_relative]}"

    # A patch already present in the real worktree is reported as installed.
    # Otherwise use the temporary index to check/apply it in series order.
    if pl_is_applied "$patch"; then
        printf 'APPLIED  %s\n' "$relative_patch"
    elif GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached --check "$patch" >/dev/null 2>&1; then
        if GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached "$patch"; then
            printf 'READY    %s\n' "$relative_patch"
        else
            printf 'ERROR    %s (temporary application failed)\n' "$relative_patch" >&2
            errors=$((errors + 1))
        fi
    elif GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached --reverse --check "$patch" >/dev/null 2>&1; then
        # This can happen when an earlier patch already brings the same change.
        printf 'COVERED  %s (change already present in the series)\n' "$relative_patch"
    else
        printf 'CONFLICT %s\n' "$relative_patch" >&2
        GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached --check "$patch" 2>&1 | sed 's/^/         /' >&2
        errors=$((errors + 1))
    fi
done

if (( errors != 0 )); then
    printf '\nPatch check found %d problem(s). No source files were changed.\n' "$errors" >&2
    exit 1
fi

printf '\nChecked %d patch(es). The source worktrees and real Git indexes were not changed.\n' "$patch_count"
