"""Run public listener loopback scenarios without exporting the private raw-peer fixture."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    (root / "target").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="server-tests-", dir=root / "target") as directory:
        project = Path(directory)
        shutil.copytree(root / "src", project / "src")
        for name in ("mcpp.toml", "mcpp.lock"):
            shutil.copy2(root / name, project / name)
        transport = project / "src/core/impl/transport_tests.cppm"
        shutil.copy2(root / "tests/transport/transport_tests.cppm", transport)
        shutil.copy2(root / "tests/support/test_support.hpp", transport.parent / "test_support.hpp")
        for name in (
            "session_tests.cppm", "server_peer.cppm", "server_routes.cppm",
            "server_handlers.cppm", "server_messages.cppm", "server_framing.cppm",
            "server_concurrency.cppm", "server_deadlines.cppm", "server_multipart.cppm",
            "server_proxies.cppm", "server_streams.cppm", "server_lazy.cppm", "server_static.cppm",
            "server_graceful.cppm", "server_tests.cppm",
        ):
            shutil.copy2(root / "tests/server" / name, project / "src/server/impl" / name)
        with (project / "src/mhttp.cppm").open("a", encoding="utf-8") as output:
            output.write("\nexport import :test.transport;\nexport import :test.session;\nexport import :test.server;\n")
        (project / "tests").mkdir()
        (project / "tests/server.cpp").write_text(
            "import mhttp;\n"
            "auto main(int argc, char* argv[]) -> int {\n"
            "    return mhttp::test::RunServerTests(argc, argv);\n}\n",
            encoding="utf-8",
        )
        command = ["mcpp", "test", "--timeout", "30"]
        if len(sys.argv) > 1:
            command += ["--", *sys.argv[1:]]
        fixture = project / "static-fixtures"
        public = fixture / "public"
        (public / "dir").mkdir(parents=True)
        (public / "racing").mkdir()
        (fixture / "outside").mkdir()
        (fixture / "public_other").mkdir()
        (fixture / "outside/secret.txt").write_text("SECRET outside mount", encoding="utf-8")
        (public / "racing/secret.txt").write_text("safe", encoding="utf-8")
        (fixture / "public_other/secret.txt").write_text("SECRET sibling prefix", encoding="utf-8")
        (public / "dir/index.html").write_text("safe directory", encoding="utf-8")
        (public / "letters.txt").write_text("abcdefghijklmnopqrstuvwxyz", encoding="utf-8")
        (public / "%2e%2e.txt").write_text("decoded once", encoding="utf-8")
        with (public / "large.bin").open("wb") as output:
            output.truncate(32 * 1024 * 1024)
        for link, target in (("escape", fixture / "outside"), ("internal-link", public / "dir"),
                             ("prefix-escape", fixture / "public_other"), ("racing-link", fixture / "outside")):
            if os.name == "nt":
                # Junctions exercise reparse-point containment without requiring symlink privileges.
                quote = lambda value: "'" + str(value).replace("'", "''") + "'"
                script = f"New-Item -ItemType Junction -Path {quote(public / link)} -Target {quote(target)} | Out-Null"
                subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                               check=True, capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                (public / link).symlink_to(target, target_is_directory=True)
        try:
            (public / "file-link").symlink_to(fixture / "outside/secret.txt")
        except OSError:
            if os.name != "nt":
                raise
        if os.name != "nt":
            os.mkfifo(public / "fifo")
        return subprocess.run(command, cwd=project, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
