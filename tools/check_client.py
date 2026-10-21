"""Run private raw-peer client acceptance tests in an isolated mcpp project."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    (root / "target").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="client-tests-", dir=root / "target") as directory:
        project = Path(directory)
        shutil.copytree(root / "src", project / "src")
        for name in ("mcpp.toml", "mcpp.lock"):
            shutil.copy2(root / name, project / name)
        transport = project / "src/core/impl/transport_tests.cppm"
        shutil.copy2(root / "tests/transport/transport_tests.cppm", transport)
        shutil.copy2(root / "tests/support/test_support.hpp", transport.parent / "test_support.hpp")
        for name in ("client_peer.cppm", "client_tests.cppm"):
            shutil.copy2(root / "tests/client" / name, project / "src/client/impl" / name)
        with (project / "src/mhttp.cppm").open("a", encoding="utf-8") as output:
            output.write("\nexport import :test.transport;\nexport import :test.client;\n")
        (project / "tests").mkdir()
        (project / "tests/client.cpp").write_text(
            "import mhttp;\n"
            "auto main(int argc, char* argv[]) -> int {\n"
            "    return mhttp::test::RunClientTests(argc, argv);\n}\n",
            encoding="utf-8",
        )
        command = ["mcpp", "test", "--timeout", "30"]
        if len(sys.argv) > 1:
            command += ["--", *sys.argv[1:]]
        return subprocess.run(command, cwd=project, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
