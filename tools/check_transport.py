"""Run loopback transport tests against implementation partitions in an isolated mcpp project."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    (root / "target").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="transport-tests-", dir=root / "target") as directory:
        project = Path(directory)
        shutil.copytree(root / "src", project / "src")
        for name in ("mcpp.toml", "mcpp.lock"):
            shutil.copy2(root / name, project / name)
        fixture = project / "src/core/impl/transport_tests.cppm"
        shutil.copy2(root / "tests/transport/transport_tests.cppm", fixture)
        shutil.copy2(root / "tests/support/test_support.hpp", fixture.parent / "test_support.hpp")
        primary = project / "src/mhttp.cppm"
        with primary.open("a", encoding="utf-8") as output:
            output.write("\nexport import :test.transport;\n")
        (project / "tests").mkdir()
        (project / "tests/transport.cpp").write_text(
            "import mhttp;\n"
            "auto main(int argc, char* argv[]) -> int {\n"
            "    return mhttp::test::RunTransportTests(argc, argv);\n}\n",
            encoding="utf-8",
        )
        command = ["mcpp", "test", "--timeout", "30"]
        if len(sys.argv) > 1:
            command += ["--", *sys.argv[1:]]
        result = subprocess.run(command, cwd=project, check=False)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
