"""Check the harness's failure paths in separate, bounded test processes."""

from pathlib import Path
import subprocess


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    checks = [
        ("public", True, ["[pass] public module smoke", "1 case(s), 0 failure(s)"]),
        ("--self-test-failure", False, [
            "intentional worker failure", "intentional exception",
            "[pass] execution continues after exceptions", "3 case(s), 2 failure(s)",
        ]),
        ("no-case-has-this-name", False, ["no test cases matched"]),
        ("--self-test-deadline", False, ["deadline exceeded: intentional timeout"]),
    ]
    for argument, success, diagnostics in checks:
        result = subprocess.run(
            ["mcpp", "test", "smoke", "--timeout", "30", "--", argument], cwd=root,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8",
            errors="replace", timeout=60, check=False,
        )
        if (result.returncode == 0) != success or not all(
            diagnostic in result.stdout for diagnostic in diagnostics
        ):
            raise RuntimeError(f"{argument} failed:\n{result.stdout}")
        print(f"[pass] harness {argument}", flush=True)


if __name__ == "__main__":
    main()
