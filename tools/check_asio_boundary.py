"""Reject Asio references outside implementation directories in library sources."""

from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(__file__).resolve().parent.parent / "src"
    pattern = re.compile(r"\bimport\s+asio\b|\basio\s*::")
    violations = []
    for path in sorted(source.rglob("*")):
        if path.suffix not in {".cppm", ".cpp", ".hpp", ".h"} or "impl" in path.relative_to(source).parts:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if pattern.search(line):
                violations.append(f"{path.relative_to(source.parent)}:{number}: {line.strip()}")
    if violations:
        print("Asio references must stay inside src/**/impl/:", file=sys.stderr)
        print("\n".join(violations), file=sys.stderr)
    return int(bool(violations))


if __name__ == "__main__":
    raise SystemExit(main())
