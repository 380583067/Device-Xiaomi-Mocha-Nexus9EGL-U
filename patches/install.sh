#!/usr/bin/env bash
# Apply the categorized patch series from any working directory. The check is
# non-destructive; changes are then applied in sorted order so dependent patches
# see the changes they build on.

set -u

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_TOP="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"

if [[ ! -f "$ANDROID_TOP/build/envsetup.sh" || ! -d "$ANDROID_TOP/.repo" ]]; then
    echo "ERROR: cannot locate the Android source root from $SCRIPT_DIR" >&2
    exit 2
fi

if ! "$SCRIPT_DIR/check.sh"; then
    echo "ERROR: patch preflight failed; no patches were installed." >&2
    exit 1
fi

patches=()
while IFS= read -r -d '' patch_file; do
    patches+=("$patch_file")
done < <(find "$SCRIPT_DIR" -type f -name '*.patch' -print0 | sort -z)

applied=()
install_failed=0

for patch_file in "${patches[@]}"; do
    relative_patch="${patch_file#"$SCRIPT_DIR"/}"
    repo_relative="${relative_patch%/*}"
    repo_path="$ANDROID_TOP/$repo_relative"
    actual_repo="$(git -C "$repo_path" rev-parse --show-toplevel 2>/dev/null || true)"
    expected_repo="$(cd -- "$repo_path" && pwd -P 2>/dev/null || true)"

    if [[ -z "$actual_repo" || "$actual_repo" != "$expected_repo" ]]; then
        printf 'ERROR: missing Git project root for %s (%s)\n' "$relative_patch" "$repo_path" >&2
        install_failed=1
        break
    fi

    if git -C "$repo_path" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
        printf 'SKIP     %s (already applied)\n' "$relative_patch"
        continue
    fi

    if ! git -C "$repo_path" apply --check "$patch_file"; then
        printf 'ERROR: patch no longer applies: %s\n' "$relative_patch" >&2
        install_failed=1
        break
    fi

    if git -C "$repo_path" apply "$patch_file"; then
        printf 'APPLIED  %s\n' "$relative_patch"
        applied+=("$patch_file")
    else
        printf 'ERROR: failed to apply: %s\n' "$relative_patch" >&2
        install_failed=1
        break
    fi
done

if (( install_failed != 0 )); then
    # Roll back only patches installed by this invocation, in reverse order.
    # This preserves all pre-existing edits in the source repositories.
    for ((i=${#applied[@]} - 1; i >= 0; i--)); do
        patch_file="${applied[$i]}"
        relative_patch="${patch_file#"$SCRIPT_DIR"/}"
        repo_relative="${relative_patch%/*}"
        repo_path="$ANDROID_TOP/$repo_relative"
        if ! git -C "$repo_path" apply --reverse "$patch_file"; then
            printf 'ERROR: rollback failed for %s; inspect that repository before continuing.\n' "$relative_patch" >&2
        fi
    done
    exit 1
fi

printf '\nPatch installation complete.\n'
