#!/usr/bin/env bash
# Reverse only this patch series. Descending lexical order removes later patches
# first (for example, frameworks/av/0041 before 0034, which it depends on).

set -u

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_TOP="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"

if [[ ! -f "$ANDROID_TOP/build/envsetup.sh" || ! -d "$ANDROID_TOP/.repo" ]]; then
    echo "ERROR: cannot locate the Android source root from $SCRIPT_DIR" >&2
    exit 2
fi

patches=()
while IFS= read -r -d '' patch_file; do
    patches+=("$patch_file")
done < <(find "$SCRIPT_DIR" -type f -name '*.patch' -print0 | sort -z)

undone=()
uninstall_failed=0

for ((i=${#patches[@]} - 1; i >= 0; i--)); do
    patch_file="${patches[$i]}"
    relative_patch="${patch_file#"$SCRIPT_DIR"/}"
    repo_relative="${relative_patch%/*}"
    repo_path="$ANDROID_TOP/$repo_relative"

    if [[ ! -d "$repo_path" ]]; then
        printf 'ERROR: missing Git project for %s (%s)\n' "$relative_patch" "$repo_path" >&2
        uninstall_failed=1
        break
    fi
    actual_repo="$(git -C "$repo_path" rev-parse --show-toplevel 2>/dev/null || true)"
    expected_repo="$(cd -- "$repo_path" && pwd -P)"
    if [[ -z "$actual_repo" || "$actual_repo" != "$expected_repo" ]]; then
        printf 'ERROR: %s is not at a Git project root (%s)\n' "$relative_patch" "$repo_path" >&2
        uninstall_failed=1
        break
    fi

    if git -C "$repo_path" apply --reverse --check "$patch_file"; then
        if git -C "$repo_path" apply --reverse "$patch_file"; then
            printf 'REVERSED %s\n' "$relative_patch"
            undone+=("$patch_file")
        else
            printf 'ERROR: failed to reverse %s\n' "$relative_patch" >&2
            uninstall_failed=1
            break
        fi
    elif git -C "$repo_path" apply --check "$patch_file" >/dev/null 2>&1; then
        printf 'SKIP     %s (not applied)\n' "$relative_patch"
    else
        printf 'ERROR: cannot safely determine or reverse patch state: %s\n' "$relative_patch" >&2
        uninstall_failed=1
        break
    fi
done

if (( uninstall_failed != 0 )); then
    # Restore only patches reversed by this invocation, preserving unrelated
    # staged and unstaged work in every source repository.
    for ((i=${#undone[@]} - 1; i >= 0; i--)); do
        patch_file="${undone[$i]}"
        relative_patch="${patch_file#"$SCRIPT_DIR"/}"
        repo_relative="${relative_patch%/*}"
        repo_path="$ANDROID_TOP/$repo_relative"
        if ! git -C "$repo_path" apply "$patch_file"; then
            printf 'ERROR: rollback failed for %s; inspect that repository before continuing.\n' "$relative_patch" >&2
        fi
    done
    exit 1
fi

printf '\nPatch removal complete. Unrelated source changes were left untouched.\n'
