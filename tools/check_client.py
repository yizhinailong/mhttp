"""Run private raw-peer client acceptance tests in an isolated mcpp project."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def install_write_fault(project: Path) -> None:
    """Intercept head completions in the copy, keeping production free of test hooks."""
    connection = project / "src/core/impl/connection.cppm"
    source = connection.read_text(encoding="utf-8")
    import_marker = "import :core.impl.io_error;"
    write_marker = "            if (bytes.empty()) { co_return WriteOutcome{}; }"
    if source.count(import_marker) != 1 or source.count(write_marker) != 1:
        raise RuntimeError("Connection write-fault injection markers changed; review the test seam")
    source = source.replace(import_marker, import_marker + "\nimport :test.client_write_fault;")
    source = source.replace(write_marker, """            if (auto failure = test::ClientWriteFault::Take(bytes)) {
                self->close();
                co_return WriteOutcome{ failure->bytes_transferred, failure->error };
            }
""" + write_marker)
    connection.write_text(source, encoding="utf-8")


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
        for name in ("client_peer.cppm", "client_expect.cppm", "client_tests.cppm"):
            shutil.copy2(root / "tests/client" / name, project / "src/client/impl" / name)
        shutil.copy2(root / "tests/client/client_write_fault.cppm", project / "src/core/impl/client_write_fault.cppm")
        install_write_fault(project)
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
