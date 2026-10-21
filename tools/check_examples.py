"""Build and run the companion REST executables on an actual dynamic loopback port."""

import json
import os
from pathlib import Path
import queue
import re
import subprocess
import threading


def executable(root: Path, name: str) -> Path:
    """Use the current compilation database rather than a cached toolchain-specific path."""
    entry = (root / "examples" / name / "main.cpp").resolve()
    commands = json.loads((root / "compile_commands.json").read_text(encoding="utf-8"))
    directories = {Path(item["directory"]) for item in commands if Path(item["file"]).resolve() == entry}
    if len(directories) != 1:
        raise RuntimeError(f"Cannot identify the current build directory for {name}")
    suffix = ".exe" if os.name == "nt" else ""
    artifact = directories.pop() / "bin" / (name + suffix)
    if not artifact.is_file():
        raise RuntimeError(f"Missing built example: {artifact}")
    return artifact


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    subprocess.run(["mcpp", "build", "--locked"], cwd=root, check=True)
    server = executable(root, "rest_server")
    client = executable(root, "rest_client")
    invalid = subprocess.run([str(server), "invalid"], capture_output=True, text=True, timeout=5)
    if invalid.returncode != 1 or "usage:" not in invalid.stderr:
        raise RuntimeError("The server must reject an invalid port before starting")
    process = subprocess.Popen(
        [str(server), "0"], cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8",
    )
    lines: queue.Queue[str | None] = queue.Queue()

    def read_output() -> None:
        for line in process.stdout:
            lines.put(line)
        lines.put(None)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    try:
        line = lines.get(timeout=10)
        if line is None:
            raise RuntimeError("REST server exited before readiness")
        print(line, end="", flush=True)
        ready = re.fullmatch(r"Listening on (http://127\.0\.0\.1:[1-9][0-9]*)\n", line)
        if not ready:
            raise RuntimeError("REST server did not publish its bound dynamic port")
        subprocess.run([str(client), ready[1]], cwd=root, timeout=30, check=True)
        process.stdin.write("\n")
        process.stdin.flush()
        if process.wait(timeout=10) != 0:
            raise RuntimeError("REST server did not stop cleanly")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        reader.join(timeout=5)
        process.stdin.close()
        process.stdout.close()
    print("[pass] REST examples: CRUD, validation, dynamic port, and console shutdown", flush=True)


if __name__ == "__main__":
    main()
