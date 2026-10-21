"""Verify mcpp's implementation partitions and diagnose any public Asio/name leakage."""

from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    (root / "target").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="module-boundary-", dir=root / "target") as directory:
        probe = Path(directory)
        (probe / "src/impl").mkdir(parents=True)
        (probe / "tests").mkdir()
        (probe / "mcpp.toml").write_bytes((root / "mcpp.toml").read_bytes())
        (probe / "src/mhttp.cppm").write_text(
            "export module mhttp;\nexport import :probe;\n", encoding="utf-8"
        )
        (probe / "src/impl/probe.cppm").write_text(
            "module mhttp:probe.impl;\n"
            "import std;\nimport asio;\n"
            "namespace mhttp::impl {\n"
            "class ProbeImpl {\n"
            "    asio::io_context m_context;\n"
            "public:\n    auto Ready() const -> bool { return !m_context.stopped(); }\n"
            "};\n}\n", encoding="utf-8"
        )
        (probe / "src/probe.cppm").write_text(
            "export module mhttp:probe;\nimport std;\nimport :probe.impl;\n"
            "export namespace mhttp {\n"
            "class Probe {\n"
            "    std::shared_ptr<impl::ProbeImpl> m_impl;\n"
            "public:\n    Probe() : m_impl{std::make_shared<impl::ProbeImpl>()} {}\n"
            "    auto Ready() const -> bool { return m_impl->Ready(); }\n"
            "};\n}\n", encoding="utf-8"
        )
        source = probe / "tests/probe.cpp"
        prefix = "import std;\nimport mhttp;\n"
        checks = [
            ("public PIMPL", "mhttp::Probe probe; return probe.Ready() ? 0 : 1;", None),
            ("Asio namespace hidden", "asio::io_context context;", "asio"),
            ("implementation names hidden", "mhttp::impl::ProbeImpl impl;", "ProbeImpl"),
            ("explicit Asio import", "asio::io_context context;", None),
        ]
        for name, body, rejected_symbol in checks:
            imports = prefix + ("import asio;\n" if name == "explicit Asio import" else "")
            source.write_text(imports + "auto main() -> int { " + body + " }\n", encoding="utf-8")
            result = subprocess.run(
                ["mcpp", "test", "--timeout", "30"], cwd=probe,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8",
                errors="replace", check=False,
            )
            if rejected_symbol is None:
                valid = result.returncode == 0
            else:
                valid = result.returncode != 0 and rejected_symbol in result.stdout
            if not valid:
                raise RuntimeError(f"{name} failed:\n{result.stdout}")
            print(f"[pass] {name}", flush=True)


if __name__ == "__main__":
    main()
