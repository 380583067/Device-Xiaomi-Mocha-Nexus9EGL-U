#!/usr/bin/env bash
# Validate the whole patch series without touching any source worktree or real
# Git index.
#
# Each project's patches are checked against a scratch copy of only the files
# the series touches, built from the current worktree. Working on copies keeps
# the check strict (git apply, no fuzz) and lets a later patch see the earlier
# ones already applied, so dependent patches are validated in install order.
# Cost is proportional to the patch set instead of the size of the tree: the
# whole series checks in a couple of seconds.
#
# Status per patch:
#   APPLIED   the change is already in the worktree
#   READY     applies cleanly on top of the earlier patches
#   COVERED   an earlier patch in the series already brings this change
#   CONFLICT  does not apply; the git error is printed and nothing is changed

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

TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/mocha-patches-check.XXXXXX")" || exit 2
trap 'rm -rf -- "$TEMP_ROOT"' EXIT

# Repository-relative paths a patch modifies, in file order.
pl_patch_files() {
    sed -n 's|^diff --git a/.* b/\(.*\)$|\1|p' "$1"
}

declare -A SCRATCH=()
errors=0
patch_count=0
ready=0
applied=0

for patch in "${PL_PATCHES[@]}"; do
    relative_patch="$(pl_display "$patch")"
    repo_relative="${relative_patch%/*}"
    repo_path="$(pl_repo_path "$patch")"
    patch_count=$((patch_count + 1))

    pl_verify_repo "$patch" || { errors=$((errors + 1)); continue; }

    # Scratch tree for this project: only the files the series touches, taken
    # from the worktree so unrelated local edits are part of the picture. Files
    # a patch creates are simply absent here and get created on application.
    if [[ -z "${SCRATCH[$repo_relative]+present}" ]]; then
        scratch="$(mktemp -d "$TEMP_ROOT/p.XXXXXX")"
        SCRATCH[$repo_relative]="$scratch"
        while IFS= read -r file; do
            [[ -n "$file" ]] || continue
            [[ -e "$scratch/$file" ]] && continue
            [[ -f "$repo_path/$file" ]] || continue
            mkdir -p -- "$scratch/$(dirname -- "$file")"
            cp -p -- "$repo_path/$file" "$scratch/$file"
        done < <(
            for sibling in "${PL_PATCHES[@]}"; do
                [[ "$(pl_display "$sibling")" == "$repo_relative"/* ]] || continue
                pl_patch_files "$sibling"
            done | sort -u
        )
    fi
    scratch="${SCRATCH[$repo_relative]}"

    if pl_is_applied "$patch"; then
        printf 'APPLIED  %s\n' "$relative_patch"
        applied=$((applied + 1))
    elif git -C "$scratch" apply --check "$patch" >/dev/null 2>&1; then
        if git -C "$scratch" apply "$patch"; then
            printf 'READY    %s\n' "$relative_patch"
            ready=$((ready + 1))
        else
            printf 'ERROR    %s (scratch application failed)\n' "$relative_patch" >&2
            errors=$((errors + 1))
        fi
    elif git -C "$scratch" apply --reverse --check "$patch" >/dev/null 2>&1; then
        # Can happen when an earlier patch of the series already brings it.
        printf 'COVERED  %s (change already present in the series)\n' "$relative_patch"
    else
        printf 'CONFLICT %s\n' "$relative_patch" >&2
        git -C "$scratch" apply --check "$patch" 2>&1 | sed 's/^/         /' >&2
        errors=$((errors + 1))
    fi
done

if (( errors != 0 )); then
    printf '\nPatch check found %d problem(s). No source files were changed.\n' "$errors" >&2
    exit 1
fi

printf '\nChecked %d patch(es): %d already applied, %d ready to apply, %d covered.\n' \
    "$patch_count" "$applied" "$ready" "$((patch_count - applied - ready))"
printf 'Across these categories:\n'
pl_category_summary
printf '\nThe source worktrees and real Git indexes were not changed.\n'
printf 'Install them with: %s/install.sh [--category <name>]\n' "$SCRIPT_DIR"
