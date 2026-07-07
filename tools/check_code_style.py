#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

EXCLUDED_PREFIXES = (
    ".git/",
    ".vs/",
    "bin/",
    "bin-int/",
    "output/",
    "Vendor/",
)

BINARY_EXTENSIONS = {
    ".bmp",
    ".dll",
    ".exe",
    ".icns",
    ".ico",
    ".jpg",
    ".jpeg",
    ".lib",
    ".pdb",
    ".png",
    ".ttf",
}

TEXT_EXTENSIONS = {
    ".bat",
    ".cmd",
    ".cpp",
    ".gitattributes",
    ".gitignore",
    ".h",
    ".hlsl",
    ".hpp",
    ".lua",
    ".md",
    ".ps1",
    ".py",
    ".sh",
    ".txt",
    ".yml",
    ".yaml",
}

SPACE_INDENT_EXTENSIONS = {
    ".cpp",
    ".h",
    ".hlsl",
    ".hpp",
    ".lua",
    ".md",
    ".ps1",
    ".py",
    ".sh",
    ".yml",
    ".yaml",
}

CONFLICT_MARKERS = ("<<<<<<<", "=======", ">>>>>>>")


def repo_path(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def tracked_files() -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            cwd=ROOT,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return [ROOT / item.decode("utf-8") for item in result.stdout.split(b"\0") if item]
    except (FileNotFoundError, subprocess.CalledProcessError):
        return [path for path in ROOT.rglob("*") if path.is_file()]


def should_skip(path: Path) -> bool:
    relative = repo_path(path)
    if any(relative.startswith(prefix) for prefix in EXCLUDED_PREFIXES):
        return True

    if path.suffix in BINARY_EXTENSIONS:
        return True

    return path.suffix not in TEXT_EXTENSIONS and path.name not in {".editorconfig", ".gitattributes", ".gitignore"}


def expected_line_ending(path: Path) -> bytes:
    return b"\r\n" if path.suffix.lower() in {".bat", ".cmd"} else b"\n"


def check_file(path: Path) -> list[str]:
    errors: list[str] = []
    relative = repo_path(path)
    data = path.read_bytes()

    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        return [f"{relative}: not valid UTF-8 text ({error})"]

    if data and not data.endswith(expected_line_ending(path)):
        errors.append(f"{relative}: missing final newline")

    if path.suffix.lower() not in {".bat", ".cmd"} and b"\r\n" in data:
        errors.append(f"{relative}: CRLF line endings are only allowed for .bat/.cmd")

    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines, start=1):
        body = line.rstrip("\r\n")
        stripped = body.strip()

        if body.endswith((" ", "\t")):
            errors.append(f"{relative}:{index}: trailing whitespace")

        if stripped in CONFLICT_MARKERS or any(stripped.startswith(marker + " ") for marker in CONFLICT_MARKERS):
            errors.append(f"{relative}:{index}: merge conflict marker")

        if path.suffix.lower() in SPACE_INDENT_EXTENSIONS and "\t" in body:
            errors.append(f"{relative}:{index}: tab character in space-indented file")

    return errors


def main() -> int:
    errors: list[str] = []

    for path in tracked_files():
        relative = repo_path(path)
        if relative.startswith(("bin/", "bin-int/", "output/")):
            errors.append(f"{relative}: build output must not be tracked")
            continue

        if should_skip(path):
            continue

        errors.extend(check_file(path))

    if errors:
        print("Code style check failed:")
        for error in errors:
            print(f"  {error}")
        return 1

    print("Code style check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
