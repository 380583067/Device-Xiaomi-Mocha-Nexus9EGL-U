#!/usr/bin/env bash
# Apply the patch series after a repo sync.
#
# The series is preflighted with check.sh first; if anything conflicts nothing
# is installed. Patches are applied in sorted path order so a dependent patch
# sees the changes it builds on. If one fails mid-way, only the patches applied
# by this run are reversed, so pre-existing local work is preserved.

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

preflight_args=()
[[ -n "$PL_ONLY" ]] && preflight_args+=(--only "$PL_ONLY")
[[ -n "$PL_CATEGORY" ]] && preflight_args+=(--category "$PL_CATEGORY")

if ! "$SCRIPT_DIR/check.sh" "${preflight_args[@]}"; then
    echo "ERROR: patch preflight failed; no patches were installed." >&2
    exit 1
fi

applied=()
install_failed=0

for patch in "${PL_PATCHES[@]}"; do
    relative_patch="$(pl_display "$patch")"

    if ! pl_verify_repo "$patch"; then
        install_failed=1
        break
    fi

    if pl_is_applied "$patch"; then
        printf 'SKIP     %s (already applied)\n' "$relative_patch"
        continue
    fi

    if ! pl_applies "$patch"; then
        printf 'ERROR: patch no longer applies: %s\n' "$relative_patch" >&2
        install_failed=1
        break
    fi

    if git -C "$(pl_repo_path "$patch")" apply "$patch"; then
        printf 'APPLIED  %s\n' "$relative_patch"
        applied+=("$patch")
    else
        printf 'ERROR: failed to apply: %s\n' "$relative_patch" >&2
        install_failed=1
        break
    fi
done

if (( install_failed != 0 )); then
    # Roll back only what this invocation installed, in reverse order. This
    # preserves all pre-existing edits in the source repositories.
    for ((i=${#applied[@]} - 1; i >= 0; i--)); do
        patch="${applied[$i]}"
        if ! git -C "$(pl_repo_path "$patch")" apply --reverse "$patch"; then
            printf 'ERROR: rollback failed for %s; inspect that repository before continuing.\n' \
                "$(pl_display "$patch")" >&2
        fi
    done
    exit 1
fi

printf '\nPatch installation complete.\n'
