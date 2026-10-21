"""Run optional JSON consumer tests in an isolated project with the json feature enabled."""

from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import tomllib


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update-lock", action="store_true", help="resolve dependencies and update the JSON consumer lock")
    parser.add_argument("filters", nargs="*")
    args = parser.parse_args()
    (root / "target").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="json-tests-", dir=root / "target") as directory:
        project = Path(directory)
        shutil.copy2(root / "tests/json/mcpp.toml", project / "mcpp.toml")
        if not args.update_lock:
            shutil.copy2(root / "tests/json/mcpp.lock", project / "mcpp.lock")
        (project / "tests").mkdir()
        (project / "support").mkdir()
        shutil.copy2(root / "tests/json/json_test.cpp", project / "tests/json.cpp")
        shutil.copy2(root / "tests/support/test_support.hpp", project / "support/test_support.hpp")
        config = tomllib.loads((root / "mcpp.toml").read_text(encoding="utf-8"))
        command = ["mcpp", "test", "--toolchain", config["toolchain"]["default"], "--timeout", "30"]
        if not args.update_lock:
            command.append("--locked")
        if args.filters:
            command += ["--", *args.filters]
        result = subprocess.run(command, cwd=project, check=False).returncode
        if result == 0 and args.update_lock:
            shutil.copy2(project / "mcpp.lock", root / "tests/json/mcpp.lock")
        return result


if __name__ == "__main__":
    raise SystemExit(main())
