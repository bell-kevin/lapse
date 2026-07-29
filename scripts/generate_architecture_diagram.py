#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate a Mermaid architecture diagram from repository contents."""
from __future__ import annotations

import argparse
import difflib
import hashlib
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "architecture.mmd"

SKIP_DIRS = {
    ".git",
    ".github",
    ".venv",
    "venv",
    "node_modules",
    "dist",
    "build",
    "coverage",
    ".next",
    ".idea",
    ".vscode",
    "__pycache__",
}

CODE_EXTENSIONS = {
    ".py": "Python",
    ".ts": "TypeScript",
    ".tsx": "TypeScript",
    ".js": "JavaScript",
    ".jsx": "JavaScript",
    ".go": "Go",
    ".rs": "Rust",
    ".java": "Java",
    ".kt": "Kotlin",
    ".swift": "Swift",
    ".rb": "Ruby",
    ".php": "PHP",
    ".c": "C",
    ".h": "C/C++",
    ".hh": "C/C++",
    ".hpp": "C/C++",
    ".hxx": "C/C++",
    ".cc": "C++",
    ".cpp": "C++",
    ".cxx": "C++",
    ".cs": "C#",
    ".sh": "Shell",
    ".sql": "SQL",
}

CODE_FILENAMES = {
    "CMakeLists.txt": "CMake",
    "Makefile": "Make",
}

IMPORT_PATTERNS = [
    re.compile(r"^\s*import\s+([a-zA-Z0-9_\.\-/]+)", re.MULTILINE),
    re.compile(r"^\s*from\s+([a-zA-Z0-9_\.\-/]+)\s+import", re.MULTILINE),
    re.compile(r"require\(['\"]([a-zA-Z0-9_\.\-/@]+)['\"]\)"),
    re.compile(r"^\s*use\s+([a-zA-Z0-9_\\:]+)", re.MULTILINE),
    re.compile(r"#include\s+[<\"]([a-zA-Z0-9_\.\-/]+)[>\"]"),
]


@dataclass
class FileInfo:
    path: Path
    component: str
    language: str


def should_skip(path: Path) -> bool:
    return any(part in SKIP_DIRS for part in path.parts)


def safe_read(path: Path) -> str:
    try:
        if path.stat().st_size > 200_000:
            return ""
        return path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def iter_repo_files(root: Path) -> Iterable[Path]:
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        if path.is_file() and not should_skip(path.relative_to(root)):
            yield path


def component_for(path: Path) -> str:
    parts = path.parts
    if len(parts) <= 1:
        return "root"
    return parts[0]


def sanitize_node_id(name: str) -> str:
    slug = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    digest = hashlib.sha256(name.encode("utf-8")).hexdigest()[:10]
    return f"n_{slug}_{digest}"


def detect_dependencies(files: list[FileInfo]) -> Counter[tuple[str, str]]:
    by_component = defaultdict(set)
    for f in files:
        stem = f.path.stem.lower()
        by_component[f.component].add(stem)
        by_component[f.component].add(f.component.lower())

    deps: Counter[tuple[str, str]] = Counter()
    for f in files:
        text = safe_read(ROOT / f.path)
        if not text:
            continue

        tokens: set[str] = set()
        for pattern in IMPORT_PATTERNS:
            for match in pattern.findall(text):
                token = str(match).replace("\\", "/").split("/")[0].split(".")[0].lower()
                if token:
                    tokens.add(token)

        for component, names in by_component.items():
            if component == f.component:
                continue
            if tokens & names:
                deps[(f.component, component)] += 1
    return deps


def build_diagram(files: list[FileInfo], docs: list[Path], deps: Counter[tuple[str, str]]) -> str:
    by_component: dict[str, list[FileInfo]] = defaultdict(list)
    for info in files:
        by_component[info.component].append(info)

    lines: list[str] = []
    lines.append("---")
    lines.append("title: Repository Architecture")
    lines.append("---")
    lines.append("graph TD")
    lines.append("    repo[(Repository)]")

    for component in sorted(by_component):
        node = sanitize_node_id(component)
        component_files = by_component[component]
        lang_counts = Counter(f.language for f in component_files)
        top_languages = sorted(lang_counts.items(), key=lambda item: (-item[1], item[0]))[:2]
        summary = ", ".join(f"{lang}:{count}" for lang, count in top_languages)
        file_count = len(component_files)
        noun = "file" if file_count == 1 else "files"
        label = f"{component}\\n{file_count} {noun}\\n{summary or 'mixed'}"
        lines.append(f"    {node}[\"{label}\"]")
        lines.append(f"    repo --> {node}")

    if docs:
        lines.append("    docs[(Documentation)]")
        lines.append("    repo --> docs")
        for doc in sorted(docs, key=lambda item: item.as_posix())[:8]:
            doc_path = doc.as_posix()
            doc_id = sanitize_node_id("doc_" + doc_path)
            lines.append(f"    {doc_id}[\"{doc_path}\"]")
            lines.append(f"    docs --> {doc_id}")

    sorted_deps = sorted(
        deps.items(),
        key=lambda item: (-item[1], item[0][0], item[0][1]),
    )
    for (src, dst), weight in sorted_deps[:30]:
        src_id = sanitize_node_id(src)
        dst_id = sanitize_node_id(dst)
        lines.append(f"    {src_id} -. {weight} refs .-> {dst_id}")

    lines.append("")
    lines.append("%% Generated by scripts/generate_architecture_diagram.py")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Fail if diagram is out of date.")
    args = parser.parse_args()

    file_infos: list[FileInfo] = []
    docs: list[Path] = []

    for abs_path in iter_repo_files(ROOT):
        rel_path = abs_path.relative_to(ROOT)
        ext = rel_path.suffix.lower()
        if ext in {".md", ".rst", ".adoc"}:
            docs.append(rel_path)
        language = CODE_FILENAMES.get(rel_path.name, CODE_EXTENSIONS.get(ext))
        if language:
            file_infos.append(
                FileInfo(path=rel_path, component=component_for(rel_path), language=language)
            )

    deps = detect_dependencies(file_infos)
    diagram = build_diagram(file_infos, docs, deps)

    expected = diagram + "\n"
    existing = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
    if args.check:
        if existing != expected:
            print("architecture.mmd is out of date. Run scripts/generate_architecture_diagram.py")
            print(
                "".join(
                    difflib.unified_diff(
                        existing.splitlines(keepends=True),
                        expected.splitlines(keepends=True),
                        fromfile="docs/architecture.mmd",
                        tofile="generated architecture.mmd",
                    )
                ),
                end="",
            )
            return 1
        print("architecture.mmd is up to date.")
        return 0

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("w", encoding="utf-8", newline="\n") as output:
        output.write(expected)
    print(f"Wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
