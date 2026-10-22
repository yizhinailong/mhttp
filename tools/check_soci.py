"""Run optional SOCI consumer tests in an isolated project with the soci feature enabled."""

from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import tomllib


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update-lock", action="store_true", help="re-resolve dependencies and replace the local SOCI consumer lock")
    parser.add_argument("filters", nargs="*")
    args = parser.parse_args()
    # The lock is generated locally and never committed; CI resolves from the manifest.
    lock = root / "tests/soci/mcpp.lock"
    locked = lock.is_file() and not args.update_lock
    (root / "target").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="soci-tests-", dir=root / "target") as directory:
        project = Path(directory)
        shutil.copy2(root / "tests/soci/mcpp.toml", project / "mcpp.toml")
        if locked:
            shutil.copy2(lock, project / "mcpp.lock")
        (project / "tests").mkdir()
        (project / "support").mkdir()
        for source in (root / "tests/soci").glob("*.cpp"):
            shutil.copy2(source, project / "tests" / source.name)
        shutil.copy2(root / "tests/support/test_support.hpp", project / "support/test_support.hpp")
        config = tomllib.loads((root / "mcpp.toml").read_text(encoding="utf-8"))
        command = ["mcpp", "test", "--toolchain", config["toolchain"]["default"], "--timeout", "30"]
        if locked:
            command.append("--locked")
        if args.filters:
            command += ["--", *args.filters]
        result = subprocess.run(command, cwd=project, check=False).returncode
        if result == 0 and not locked:
            shutil.copy2(project / "mcpp.lock", lock)
        return result


if __name__ == "__main__":
    raise SystemExit(main())
