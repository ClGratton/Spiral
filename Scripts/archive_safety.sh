#!/usr/bin/env bash

archive_path_is_safe() {
    local value="${1//\\//}"
    [[ -n "$value" && "$value" != /* && ! "$value" =~ ^[A-Za-z]: ]] || return 1
    local depth=0 segment
    IFS='/' read -r -a segments <<< "$value"
    for segment in "${segments[@]}"; do
        [[ -z "$segment" || "$segment" == "." ]] && continue
        if [[ "$segment" == ".." ]]; then
            (( depth > 0 )) || return 1
            depth=$((depth - 1))
        else
            depth=$((depth + 1))
        fi
    done
}

archive_link_is_safe() {
    local member="${1//\\//}"
    local target="${2//\\//}"
    archive_path_is_safe "$member" || return 1
    [[ -n "$target" && "$target" != /* && ! "$target" =~ ^[A-Za-z]: ]] || return 1
    local parent="${member%/*}"
    [[ "$parent" == "$member" ]] && parent=""
    archive_path_is_safe "${parent:+$parent/}$target"
}

assert_safe_zip_archive() {
    local archive="$1" member line marker target
    command -v unzip >/dev/null 2>&1 || { echo "unzip is required to inspect '$archive'." >&2; return 1; }
    local members_file verbose_file
    members_file="$(mktemp "${TMPDIR:-/tmp}/spiral-zip-members-XXXXXX")"
    verbose_file="$(mktemp "${TMPDIR:-/tmp}/spiral-zip-verbose-XXXXXX")"
    unzip -Z1 "$archive" > "$members_file"
    unzip -Z -l "$archive" > "$verbose_file"
    while IFS= read -r member; do
        archive_path_is_safe "$member" || { rm -f "$members_file" "$verbose_file"; echo "Archive '$archive' contains unsafe member '$member'." >&2; return 1; }
        marker=" $member"
        line="$(grep -F -- "$marker" "$verbose_file" | head -n 1 || true)"
        if [[ "$line" == l* ]]; then
            target="$(unzip -p "$archive" "$member")"
            archive_link_is_safe "$member" "$target" || { rm -f "$members_file" "$verbose_file"; echo "Archive '$archive' contains escaping symbolic link '$member' -> '$target'." >&2; return 1; }
        fi
    done < "$members_file"
    rm -f "$members_file" "$verbose_file"
}

assert_safe_tar_archive() {
    local archive="$1" member marker line target
    local members_file verbose_file
    members_file="$(mktemp "${TMPDIR:-/tmp}/spiral-tar-members-XXXXXX")"
    verbose_file="$(mktemp "${TMPDIR:-/tmp}/spiral-tar-verbose-XXXXXX")"
    tar -tzf "$archive" > "$members_file"
    LC_ALL=C tar -tvzf "$archive" > "$verbose_file"
    if grep -q '^h' "$verbose_file"; then
        rm -f "$members_file" "$verbose_file"
        echo "Archive '$archive' contains hard links, which are not admitted by the toolchain extractor." >&2
        return 1
    fi
    while IFS= read -r member; do
        archive_path_is_safe "$member" || { rm -f "$members_file" "$verbose_file"; echo "Archive '$archive' contains unsafe member '$member'." >&2; return 1; }
        marker=" $member -> "
        line="$(grep -F -- "$marker" "$verbose_file" | head -n 1 || true)"
        if [[ "$line" == l* ]]; then
            target="${line#*"$marker"}"
            archive_link_is_safe "$member" "$target" || { rm -f "$members_file" "$verbose_file"; echo "Archive '$archive' contains escaping symbolic link '$member' -> '$target'." >&2; return 1; }
        fi
    done < "$members_file"
    rm -f "$members_file" "$verbose_file"
}
