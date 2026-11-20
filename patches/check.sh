#!/usr/bin/env bash
# Check the whole ordered patch series without changing any source worktree or
# real Git index. Each patch is tested against a temporary index that includes
# earlier patches in the same repository, so dependent patches are checked in
# the order install.sh will use.

set -u

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_TOP="$(cd -- "$SCRIPT_DIR/../../../.." && pwd)"

if [[ ! -f "$ANDROID_TOP/build/envsetup.sh" || ! -d "$ANDROID_TOP/.repo" ]]; then
    echo "ERROR: cannot locate the Android source root from $SCRIPT_DIR" >&2
    exit 2
fi

ONLY=""
while (( $# )); do
    case "$1" in
        --only)
            if [[ -z "${2:-}" ]]; then
                echo "ERROR: --only needs a project path, e.g. --only frameworks/av" >&2
                exit 2
            fi
            ONLY="${2%/}"
            shift 2
            ;;
        -h|--help)
            echo "usage: $(basename "$0") [--only <repo path>]"
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mocha-patches-check.XXXXXX")" || exit 2
trap 'rm -rf -- "$TEMP_DIR"' EXIT

declare -A TEMP_INDEXES=()
index_number=0
errors=0
patch_count=0

while IFS= read -r -d '' patch_file; do
    relative_patch="${patch_file#"$SCRIPT_DIR"/}"
    repo_relative="${relative_patch%/*}"
    repo_path="$ANDROID_TOP/$repo_relative"
    if [[ -n "$ONLY" && "$repo_relative" != "$ONLY" ]]; then
        continue
    fi
    patch_count=$((patch_count + 1))

    if [[ ! -d "$repo_path" ]]; then
        echo "ERROR: missing Git project for $relative_patch ($repo_path)" >&2
        errors=$((errors + 1))
        continue
    fi
    actual_repo="$(git -C "$repo_path" rev-parse --show-toplevel 2>/dev/null || true)"
    expected_repo="$(cd -- "$repo_path" && pwd -P)"
    if [[ -z "$actual_repo" || "$actual_repo" != "$expected_repo" ]]; then
        echo "ERROR: $relative_patch is not at a Git project root ($repo_path)" >&2
        errors=$((errors + 1))
        continue
    fi

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
    # Otherwise, use the temporary index to check/apply it in series order.
    if git -C "$repo_path" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
        printf 'APPLIED  %s\n' "$relative_patch"
    elif GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached --check "$patch_file" >/dev/null 2>&1; then
        if GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached "$patch_file"; then
            printf 'READY    %s\n' "$relative_patch"
        else
            printf 'ERROR    %s (temporary application failed)\n' "$relative_patch" >&2
            errors=$((errors + 1))
        fi
    elif GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached --reverse --check "$patch_file" >/dev/null 2>&1; then
        # This can occur when the exact change is included by an earlier patch.
        printf 'COVERED  %s (change already present in the series)\n' "$relative_patch"
    else
        printf 'CONFLICT %s\n' "$relative_patch" >&2
        GIT_INDEX_FILE="$temp_index" git -C "$repo_path" apply --cached --check "$patch_file" 2>&1 | sed 's/^/         /' >&2
        errors=$((errors + 1))
    fi
done < <(find "$SCRIPT_DIR" -type f -name '*.patch' -print0 | sort -z)

if (( patch_count == 0 )); then
    echo "ERROR: no .patch files found under $SCRIPT_DIR${ONLY:+ for $ONLY}" >&2
    exit 2
fi

if (( errors != 0 )); then
    printf '\nPatch check found %d problem(s). No source files were changed.\n' "$errors" >&2
    exit 1
fi

printf '\nChecked %d patch(es). The source worktrees and real Git indexes were not changed.\n' "$patch_count"
