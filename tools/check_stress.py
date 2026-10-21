"""Verify bounded stress-client behavior against a dynamic loopback HTTP/1.1 peer."""

from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time

# Reuse artifact discovery without creating generated files in the source directory.
sys.dont_write_bytecode = True
from check_examples import executable


class Peer(ThreadingHTTPServer):
    """Count actual wire requests, simultaneous handlers, and accepted TCP connections."""

    def __init__(self):
        self.lock = threading.Lock()
        self.requests = Counter()
        self.connections = 0
        self.active = 0
        self.maximum_active = 0
        super().__init__(("127.0.0.1", 0), Handler)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)
        with self.lock:
            self.connections += 1
        return connection, address


class Handler(BaseHTTPRequestHandler):
    """Return bounded complete representations and a deliberately slow response."""

    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # A drained client may reset its otherwise idle persistent connection.

    def respond(self, head=False, echo=False):
        peer = self.server
        with peer.lock:
            peer.requests[self.command, self.path] += 1
            peer.active += 1
            peer.maximum_active = max(peer.maximum_active, peer.active)
        try:
            status, body = 200, b"ready"
            if self.path.startswith("/work"):
                time.sleep(0.01)
            elif self.path == "/slow":
                time.sleep(0.15)
            elif self.path == "/fail":
                status = 503
            if echo:
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                if (body != b"hello" or self.headers.get_all("X-Probe") != ["one", "two"]
                        or self.headers.get("Content-Type") != "application/example"):
                    status = 400
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head:
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # The timeout scenario intentionally closes before the response is sent.
        finally:
            with peer.lock:
                peer.active -= 1

    def do_GET(self):
        self.respond()

    def do_HEAD(self):
        self.respond(head=True)

    def do_POST(self):
        self.respond(echo=True)


def run(binary, arguments, expected=0):
    """Keep every execution bounded and surface full diagnostics on a mismatched exit."""
    result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=20)
    if result.returncode != expected:
        raise RuntimeError(f"stress_test exited {result.returncode}, expected {expected}:\n"
                           f"{result.stdout}\n{result.stderr}")
    return result.stdout


def number(output, label):
    """Extract one report field, requiring a complete nonnegative decimal count."""
    found = re.search(rf"^{re.escape(label)}: ([0-9]+)$", output, re.MULTILINE)
    if not found:
        raise RuntimeError(f"Missing report count {label}:\n{output}")
    return int(found[1])


def require_counts(output, attempts, responses, http=0, transport=0, body=None):
    """Compare reported accounting against externally controlled wire outcomes."""
    expected = {"Requests": attempts, "Responses": responses, "HTTP failures": http,
                "Transport failures": transport}
    if body is not None:
        expected["Body bytes"] = body
    for label, value in expected.items():
        if number(output, label) != value:
            raise RuntimeError(f"Expected {label}={value}:\n{output}")
    if not re.search(r"^Latency .*p50<=.*p95<=.*p99<=", output, re.MULTILINE):
        raise RuntimeError(f"Missing bounded latency report:\n{output}")


def main():
    root = Path(__file__).resolve().parent.parent
    subprocess.run(["mcpp", "build", "--locked"], cwd=root, check=True)
    binary = executable(root, "stress_test")
    if "usage: stress_test" not in run(binary, ["--help"]):
        raise RuntimeError("Missing stress example usage")
    invalid_options = [[], ["https://127.0.0.1/"], ["relative"],
                       ["http://127.0.0.1:1", "--connections", "0"],
                       ["http://127.0.0.1:1", "--connections", "257"],
                       ["http://127.0.0.1:1", "--threads", "2x"],
                       ["http://127.0.0.1:1", "--requests", "-1"],
                       ["http://127.0.0.1:1", "--requests", "18446744073709551616"],
                       ["http://127.0.0.1:1", "--requests", "1", "--duration", "1"],
                       ["http://127.0.0.1:1", "--timeout-ms", "0"],
                       ["http://127.0.0.1:1", "--method", "CONNECT"],
                       ["http://127.0.0.1:1", "--header", "invalid"],
                       ["http://127.0.0.1:1", "--header", "Bad Name: value"],
                       ["http://127.0.0.1:1", "--unknown", "1"],
                       ["http://127.0.0.1:1", "--requests"]]
    for arguments in invalid_options:
        run(binary, arguments, expected=1)
    print("[pass] stress CLI bounds, syntax, methods, and plain-HTTP validation", flush=True)

    with Peer() as peer:
        thread = threading.Thread(target=peer.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        thread.start()
        origin = f"http://127.0.0.1:{peer.server_port}"
        try:
            output = run(binary, [origin + "/work?batch=count", "--requests", "31",
                                  "--connections", "4", "--threads", "1"])
            require_counts(output, 31, 31, body=155)
            latency = re.search(r"Latency .*avg=([0-9.]+) min=([0-9.]+) p50<=([0-9.]+) "
                                r"p95<=([0-9.]+) p99<=([0-9.]+) max=([0-9.]+)", output)
            if not latency:
                raise RuntimeError("Missing latency measurements")
            average, minimum, p50, p95, p99, maximum = map(float, latency.groups())
            if not minimum <= average <= maximum or not 9 <= p50 <= p95 <= p99:
                raise RuntimeError(f"Latency must account for the peer's 10ms wait:\n{output}")
            with peer.lock:
                if peer.requests["GET", "/work?batch=count"] != 31 or not 2 <= peer.connections <= 4:
                    raise RuntimeError("Count mode must issue exactly 31 requests and reuse at most four connections")
                if peer.maximum_active < 2:
                    raise RuntimeError("One I/O thread must sustain overlapping asynchronous clients")
            print("[pass] exact global count, query, keep-alive reuse, and single-thread async overlap", flush=True)

            output = run(binary, [origin + "/few", "--requests", "2", "--connections", "5"])
            require_counts(output, 2, 2, body=10)
            with peer.lock:
                if peer.requests["GET", "/few"] != 2:
                    raise RuntimeError("More workers than attempts must not overshoot the global budget")

            output = run(binary, [origin + "/echo?key=value", "--requests", "7", "--connections", "2",
                                  "--method", "POST", "--body", "hello", "--content-type", "application/example",
                                  "--header", "X-Probe: one", "--header", "X-Probe: two"])
            require_counts(output, 7, 7, body=35)
            output = run(binary, [origin + "/head", "--requests", "3", "--method", "HEAD"])
            require_counts(output, 3, 3, body=0)
            print("[pass] workers above request budget, POST body/duplicate headers, and HEAD framing", flush=True)

            output = run(binary, [origin + "/fail", "--requests", "5", "--connections", "2"], expected=2)
            require_counts(output, 5, 5, http=5, body=25)
            if number(output, "Status 503") != 5:
                raise RuntimeError("HTTP errors must preserve their status frequencies")
            output = run(binary, [origin + "/slow", "--requests", "4", "--connections", "2",
                                  "--timeout-ms", "30"], expected=2)
            require_counts(output, 4, 0, transport=4, body=0)
            print("[pass] HTTP failures, deadline cancellation, and failure exit codes", flush=True)

            output = run(binary, [origin + "/work?batch=time", "--duration", "1", "--connections", "2"])
            attempts = number(output, "Requests")
            require_counts(output, attempts, attempts, body=attempts * 5)
            elapsed = re.search(r"^Elapsed: ([0-9.]+) s$", output, re.MULTILINE)
            with peer.lock:
                received = peer.requests["GET", "/work?batch=time"]
            if attempts < 2 or received != attempts or not elapsed or not 0.9 <= float(elapsed[1]) <= 3:
                raise RuntimeError(f"Duration mode must stop submissions and drain its final attempts:\n{output}")
            print("[pass] bounded duration submissions and final-request drain", flush=True)
        finally:
            peer.shutdown()
            thread.join(timeout=5)

    with socket.socket() as unused:
        unused.bind(("127.0.0.1", 0))
        closed_port = unused.getsockname()[1]
    output = run(binary, [f"http://127.0.0.1:{closed_port}/", "--requests", "2",
                          "--connections", "2", "--timeout-ms", "100"], expected=2)
    require_counts(output, 2, 0, transport=2, body=0)
    print("[pass] failed connections remain bounded and count as transport failures", flush=True)


if __name__ == "__main__":
    main()
