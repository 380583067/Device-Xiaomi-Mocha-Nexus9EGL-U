#!/usr/bin/env bash
# Shared helpers for check.sh, install.sh and uninstall.sh.
#
# Sourced, never executed. It owns everything the three entry points used to
# duplicate: locating the Android tree, argument parsing, discovering the patch
# series in apply order, resolving the source project of a patch, and the
# category grouping from patches/categories.

pl_die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 2
}

pl_init() {
    export LC_ALL=C
    PL_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
    PL_TOP="$(cd -- "$PL_ROOT/../../../.." && pwd)"

    if [[ ! -f "$PL_TOP/build/envsetup.sh" || ! -d "$PL_TOP/.repo" ]]; then
        pl_die "cannot locate the Android source root from $PL_ROOT"
    fi

    PL_CATEGORIES="$PL_ROOT/categories"
    PL_ONLY=""
    PL_CATEGORY=""
    PL_LIST=0
}

pl_usage() {
    cat <<EOF
usage: $(basename "$0") [--only <repo path>] [--category <name>] [--list]

  --only <repo path>  restrict the run to one source project,
                      e.g. --only frameworks/av
  --category <name>   restrict the run to one category from patches/categories,
                      e.g. --category camera
  --list              print the series grouped by category, then exit
EOF
}

pl_parse_args() {
    while (( $# )); do
        case "$1" in
            --only)
                [[ -n "${2:-}" ]] || pl_die "--only needs a project path, e.g. --only frameworks/av"
                PL_ONLY="${2%/}"
                shift 2
                ;;
            --category)
                [[ -n "${2:-}" ]] || pl_die "--category needs a name, e.g. --category camera"
                PL_CATEGORY="$2"
                shift 2
                ;;
            --list)
                PL_LIST=1
                shift
                ;;
            -h|--help)
                pl_usage
                exit 0
                ;;
            *)
                pl_die "unknown argument: $1"
                ;;
        esac
    done
}

# Category of a patch. An entry naming the full patch path wins over a project
# prefix, so a single patch can be moved out of its project's default group.
pl_category_of() {
    local repo="$1" rel="${2:-}" category target

    if [[ -f "$PL_CATEGORIES" ]]; then
        # First pass: entries naming one exact patch file.
        while read -r category target; do
            [[ -z "$category" || "$category" == \#* ]] && continue
            [[ "$target" == *.patch ]] || continue
            if [[ -n "$rel" && "$rel" == "$target" ]]; then
                printf '%s\n' "$category"
                return
            fi
        done < "$PL_CATEGORIES"
        # Second pass: project path prefixes.
        while read -r category target; do
            [[ -z "$category" || "$category" == \#* ]] && continue
            [[ "$target" == *.patch ]] && continue
            if [[ "$repo" == "$target" || "$repo" == "$target"/* ]]; then
                printf '%s\n' "$category"
                return
            fi
        done < "$PL_CATEGORIES"
    fi
    printf 'other\n'
}

# Fill PL_PATCHES with the selected patches, sorted by path (apply order).
pl_collect() {
    local patch rel repo

    PL_PATCHES=()
    while IFS= read -r -d '' patch; do
        rel="${patch#"$PL_ROOT"/}"
        repo="${rel%/*}"
        if [[ -n "$PL_ONLY" && "$repo" != "$PL_ONLY" ]]; then
            continue
        fi
        if [[ -n "$PL_CATEGORY" && "$(pl_category_of "$repo" "$rel")" != "$PL_CATEGORY" ]]; then
            continue
        fi
        PL_PATCHES+=("$patch")
    done < <(find "$PL_ROOT" -type f -name '*.patch' -print0 | sort -z)

    if (( ${#PL_PATCHES[@]} == 0 )); then
        local scope=""
        [[ -n "$PL_ONLY" ]] && scope+=" for $PL_ONLY"
        [[ -n "$PL_CATEGORY" ]] && scope+=" in category $PL_CATEGORY"
        pl_die "no .patch files found under $PL_ROOT$scope"
    fi
}

pl_list() {
    local patch rel repo
    for patch in "${PL_PATCHES[@]}"; do
        rel="${patch#"$PL_ROOT"/}"
        repo="${rel%/*}"
        printf '%-8s %s\n' "$(pl_category_of "$repo" "$rel")" "$rel"
    done
}

# Relative path of a patch inside patches/, used in all status messages.
pl_display() {
    printf '%s\n' "${1#"$PL_ROOT"/}"
}

# Absolute path of the source project a patch applies to.
pl_repo_path() {
    local rel="${1#"$PL_ROOT"/}"
    printf '%s/%s\n' "$PL_TOP" "${rel%/*}"
}

# Validate that the patch really lives at the root of its own Git project.
pl_verify_repo() {
    local patch="$1" repo actual expected

    repo="$(pl_repo_path "$patch")"
    if [[ ! -d "$repo" ]]; then
        printf 'ERROR: missing Git project for %s (%s)\n' "$(pl_display "$patch")" "$repo" >&2
        return 1
    fi
    actual="$(git -C "$repo" rev-parse --show-toplevel 2>/dev/null || true)"
    expected="$(cd -- "$repo" && pwd -P)"
    if [[ -z "$actual" || "$actual" != "$expected" ]]; then
        printf 'ERROR: %s is not at a Git project root (%s)\n' "$(pl_display "$patch")" "$repo" >&2
        return 1
    fi
}

# True when the change is already in the worktree.
pl_is_applied() {
    git -C "$(pl_repo_path "$1")" apply --reverse --check "$1" >/dev/null 2>&1
}

# True when the change applies cleanly to the current worktree.
pl_applies() {
    git -C "$(pl_repo_path "$1")" apply --check "$1" >/dev/null 2>&1
}

# Per-category patch counts for the selection, one line each, stable order.
pl_category_summary() {
    local patch rel repo category
    local -A counts=()
    for patch in "${PL_PATCHES[@]}"; do
        rel="${patch#"$PL_ROOT"/}"
        repo="${rel%/*}"
        category="$(pl_category_of "$repo" "$rel")"
        counts[$category]=$(( ${counts[$category]:-0} + 1 ))
    done
    local category_name
    for category_name in $(printf '%s\n' "${!counts[@]}" | sort); do
        printf '  %-8s %d\n' "$category_name" "${counts[$category_name]}"
    done
}

# What has to be rebuilt after touching each category. Only the categories that
# are actually in the current selection are printed, so `--category camera`
# does not tell the user to rebuild the whole tree.
pl_rebuild_hint() {
    local patch rel repo category
    local -A hints=(
        [camera]="m -j22 camera.tegra   # then reinstall /system/lib/hw/camera.tegra.so"
        [media]="m -j22 libstagefright libstagefright_omx"
        [display]="m -j22 libsurfaceflinger libbinder"
        [wifi]="m -j22 libwifi-hal-bcm"
        [power]="m -j22 charger android.hardware.health@2.1-service"
        [system]="m -j22   # framework/base and package changes need a full system image"
        [build]="m -j16 otapackage   # build-system edits only take effect on a full build"
        [other]="m -j16 otapackage"
    )
    local -A wanted=()
    for patch in "${PL_PATCHES[@]}"; do
        rel="${patch#"$PL_ROOT"/}"
        repo="${rel%/*}"
        wanted["$(pl_category_of "$repo" "$rel")"]=1
    done

    local printed=0 category_name
    for category_name in $(printf '%s\n' "${!wanted[@]}" | sort); do
        if (( printed == 0 )); then
            printf '\nRebuild what these categories touch:\n'
            printed=1
        fi
        printf '  %-8s %s\n' "$category_name" "${hints[$category_name]:-${hints[other]}}"
    done
    (( printed )) && printf '\n'
}
