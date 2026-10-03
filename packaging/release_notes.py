#!/usr/bin/env python3
"""Build release notes and a CHANGELOG entry from the git history.

Usage:
    release_notes.py --version v1.1.0 [--repo owner/name] [--out notes.md]

The previous release is the highest existing v* tag. Commits between it and
HEAD are grouped by conventional-commit prefix so the notes read as a list of
changes rather than a list of hashes.

This deliberately does no semver arithmetic: it compares tags the way a human
does, by asking git what is newer. That avoids a dependency and a wrong answer
from a hand-rolled comparator.
"""

import argparse
import re
import subprocess
import sys
from collections import OrderedDict

TAG_RE = re.compile(r"^v?\d+\.\d+\.\d+")

# Ordered because the output follows this order.
CATEGORIES = OrderedDict(
    [
        ("feat", ("Added", "Features")),
        ("fix", ("Fixed", "Bug fixes")),
        ("perf", ("Changed", "Performance")),
        ("refactor", ("Changed", "Internal")),
        ("docs", ("Changed", "Documentation")),
        ("test", ("Changed", "Tests and CI")),
        ("build", ("Changed", "Build and tooling")),
        ("ci", ("Changed", "Build and tooling")),
        ("chore", ("Changed", "Housekeeping")),
    ]
)

# Plain prefixes a maintainer is likely to type.
SUBJECTS = {
    "add": "feat",
    "added": "feat",
    "new": "feat",
    "fix": "fix",
    "fixed": "fix",
    "bug": "fix",
    "docs": "docs",
    "doc": "docs",
    "test": "test",
    "tests": "test",
    "build": "build",
    "ci": "ci",
    "chore": "chore",
    "refactor": "refactor",
    "perf": "perf",
}


def git(*args):
    return subprocess.run(
        ["git", *args], capture_output=True, text=True, check=True
    ).stdout.strip()


def previous_tag(current):
    """Highest v* tag other than the one being released."""
    tags = [t for t in git("tag", "--sort=-v:refname").splitlines() if TAG_RE.match(t)]
    for tag in tags:
        if tag != current:
            return tag
    return None


def classify(subject):
    """Return (category-key, cleaned subject)."""
    match = re.match(r"^(\w+)(?:\(([^)]*)\))?(!?):\s*(.*)$", subject)
    if match:
        kind, _scope, breaking, rest = match.groups()
        kind = kind.lower()
        if breaking == "!" or kind in ("feat", "fix"):
            return kind, rest
        if kind in SUBJECTS:
            return SUBJECTS[kind], rest
    # Fall back to a leading word, e.g. "Fix the PSRAM detection".
    first = subject.split(" ", 1)[0].strip(",:").lower()
    if first in SUBJECTS:
        rest = subject.split(" ", 1)[1] if " " in subject else ""
        return SUBJECTS[first], rest
    return "chore", subject


def collect(prev, current):
    """Commits between prev..current (or HEAD when there is no previous tag)."""
    span = f"{prev}..HEAD" if prev else "HEAD"
    log = git("log", span, "--no-merges", "--pretty=format:%h%x1f%s%x1f%an")
    commits = []
    for line in log.splitlines():
        if not line.strip():
            continue
        sha, subject, author = line.split("\x1f")
        category, cleaned = classify(subject)
        commits.append((sha, cleaned, author, category))
    return commits


def render(commits, version, previous, repo):
    grouped = OrderedDict()
    for sha, subject, _author, category in commits:
        grouped.setdefault(category, []).append((sha, subject))

    out = []
    out.append(f"## {version}\n")
    if previous:
        out.append(f"Changes since `{previous}`.\n")
    else:
        out.append("First release.\n")

    if not commits:
        out.append("No commits to report.\n")
        return "\n".join(out)

    for category, items in grouped.items():
        heading = CATEGORIES.get(category, ("Changed", "Other"))
        out.append(f"### {heading[1]}\n")
        for sha, subject in items:
            out.append(f"- {subject} (`{sha}`)")
        out.append("")

    if repo:
        out.append(
            f"Full history: https://github.com/{repo}/compare/"
            f"{previous or 'initial'}...{version}\n"
        )
    return "\n".join(out)


def update_changelog(markdown):
    """Insert the new version above the existing ones in CHANGELOG.md."""
    try:
        with open("CHANGELOG.md", encoding="utf-8") as handle:
            existing = handle.read()
    except FileNotFoundError:
        return False

    # Only the section for this version, without our own headings.
    lines = markdown.splitlines()
    kept, skipping = [], False
    for line in lines:
        if line.startswith("### "):
            skipping = True
        elif line.startswith("## "):
            skipping = False
        elif line.startswith("Full history:"):
            skipping = True
        if not skipping:
            kept.append(line)

    body = "\n".join(kept).rstrip() + "\n"

    marker = re.search(r"^## \[", existing, re.M)
    if marker:
        head = existing[: marker.start()].rstrip() + "\n\n"
        tail = existing[marker.start() :]
    else:
        head = existing.rstrip() + "\n\n"
        tail = ""

    with open("CHANGELOG.md", "w", encoding="utf-8") as handle:
        handle.write(head + body + "\n" + tail)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--repo", default="")
    parser.add_argument("--out", default="notes.md")
    parser.add_argument(
        "--no-changelog",
        action="store_true",
        help="print the notes but leave CHANGELOG.md alone",
    )
    args = parser.parse_args()

    if not TAG_RE.match(args.version):
        print(f"error: version must look like v1.0.0, got {args.version!r}",
              file=sys.stderr)
        return 1

    prev = previous_tag(args.version)
    commits = collect(prev, args.version)
    markdown = render(commits, args.version, prev, args.repo)

    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(markdown)

    if not args.no_changelog and update_changelog(markdown):
        print(f"updated CHANGELOG.md (previous release: {prev or 'none'})")
    print(f"wrote {args.out}: {len(commits)} commit(s), previous tag {prev or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())