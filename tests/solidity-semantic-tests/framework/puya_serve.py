"""Session-persistent `puya serve` backend client.

The puya backend is a Python process whose interpreter+import startup
(several seconds; networkx/structlog/ir.optimize imports dominate) dwarfs
the actual per-contract compile work. The harness pays that startup once
per contract on a backend-cache (L2) miss. puya ships a service mode
(`puya serve`): a pygls JsonRPCServer speaking JSON-RPC 2.0 with LSP
Content-Length framing over stdio, whose `compile` method runs the same
`awst_to_teal` + artifact writing as the CLI. This module keeps ONE such
server alive per test process and routes backend compiles through it, so
the startup tax is paid once per session instead of once per miss.

Trust model: the server is used for the HAPPY PATH only. Any
non-success — spawn failure, transport error, timeout, a JSON-RPC error
response, or error/critical logs in the result — makes `compile_via_server`
return False and the caller falls back to the classic one-shot subprocess,
which stays the source of truth for failures (including the handful of
expect-compile-error tests). Artifacts produced via serve are
byte-identical to subprocess output (verified: .teal/.arc56.json/.bin),
so backend-cache entries are interchangeable between the two paths.

The one post-backend step the subprocess path performs that the server
does not is `writeChildDeployTemplates` (puya-sol main.cpp): hexing each
`new C()` child contract's compiled .bin into deploy.tmpl.json. That is
ported here (`_write_child_deploy_templates`), keyed off the
`APPROVAL_<Child>` stub entries puya-sol writes into
options.json's cli_template_definitions.

Set PUYA_SOL_NO_SERVE=1 to disable the server and force the subprocess
path for every compile.

xdist note: each worker is a separate process with its own module state,
hence its own server — no cross-worker coordination needed.
"""
from __future__ import annotations

import atexit
import json
import os
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .paths import CACHE_DIR, PUYA

_DISABLED = os.environ.get("PUYA_SOL_NO_SERVE") == "1"


class _ServeClient:
    """One `puya serve` child process + LSP-framed JSON-RPC over its stdio."""

    def __init__(self) -> None:
        self._proc: subprocess.Popen | None = None
        self._dead = False  # permanent for the session once tripped
        self._next_id = 0
        self._lock = threading.Lock()
        # Single reader thread so a request can be awaited with a timeout;
        # if the deadline passes we kill the server and the blocked read
        # unblocks with EOF.
        self._reader = ThreadPoolExecutor(max_workers=1)

    # ── process lifecycle ────────────────────────────────────────────────
    def _ensure_started(self) -> bool:
        if self._proc is not None and self._proc.poll() is None:
            return True
        if self._dead:
            return False
        try:
            CACHE_DIR.mkdir(parents=True, exist_ok=True)
            stderr_log = open(CACHE_DIR / "puya_serve.stderr.log", "ab")  # noqa: SIM115
            # Strip PYTHONPATH for the same reason compile.py does for the
            # subprocess path: a user-site puya would shadow puya/.venv.
            env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
            self._proc = subprocess.Popen(
                [str(PUYA), "serve", "--log-level", "warning"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=stderr_log,
                env=env,
            )
        except Exception:
            self._dead = True
            return False
        atexit.register(self._shutdown)
        return True

    def _kill(self) -> None:
        self._dead = True
        if self._proc is None:
            return
        try:
            self._proc.kill()
            self._proc.wait(timeout=5)
        except Exception:
            pass

    def _shutdown(self) -> None:
        if self._proc is None or self._proc.poll() is not None:
            return
        try:
            self._proc.stdin.close()  # pygls exits on stdin EOF
            self._proc.wait(timeout=2)
        except Exception:
            try:
                self._proc.kill()
            except Exception:
                pass

    # ── framing ──────────────────────────────────────────────────────────
    def _read_message_matching(self, want_id: int) -> dict:
        """Blocking-read messages until the response with `want_id` arrives.

        Skips server-initiated notifications (no matching id). Runs on the
        reader thread; raises on EOF/garbage, which the caller maps to
        'server unavailable'.
        """
        stdout = self._proc.stdout
        while True:
            headers: dict[bytes, bytes] = {}
            while True:
                line = stdout.readline()
                if not line:
                    raise EOFError("puya serve closed stdout")
                line = line.strip()
                if not line:
                    break
                key, _, value = line.partition(b":")
                headers[key.strip().lower()] = value.strip()
            body = stdout.read(int(headers[b"content-length"]))
            msg = json.loads(body)
            if msg.get("id") == want_id:
                return msg

    def request(self, method: str, params: dict, timeout: float) -> dict | None:
        """One JSON-RPC round-trip. Returns the response message, or None if
        the server is/became unavailable (spawn failure, transport error,
        timeout). A JSON-RPC *error response* is returned as-is — that is an
        answer, not a transport failure."""
        with self._lock:
            if not self._ensure_started():
                return None
            self._next_id += 1
            req_id = self._next_id
            body = json.dumps(
                {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params}
            ).encode()
            try:
                self._proc.stdin.write(
                    f"Content-Length: {len(body)}\r\n\r\n".encode() + body
                )
                self._proc.stdin.flush()
                future = self._reader.submit(self._read_message_matching, req_id)
                return future.result(timeout=timeout)
            except Exception:
                # Covers BrokenPipe on write, EOF/garbage on read, and the
                # future timeout. Kill so a read blocked past its deadline
                # unblocks via EOF.
                self._kill()
                return None


_CLIENT: _ServeClient | None = None


def _client() -> _ServeClient:
    global _CLIENT
    if _CLIENT is None:
        _CLIENT = _ServeClient()
    return _CLIENT


def _write_child_deploy_templates(out_dir: Path, options: dict) -> None:
    """Python port of puya-sol's writeChildDeployTemplates (main.cpp).

    For contracts using `new C()`, puya-sol stubs APPROVAL_<Child>/
    CLEAR_<Child> template definitions into options.json and, after the
    backend run, hexes each child's compiled .bin into deploy.tmpl.json
    for the deploy harness to substitute. Mirror that after a serve
    compile. No children → no file, matching the C++ early return.
    """
    defs = options.get("cli_template_definitions") or {}
    # APPROVAL_ keys carry _P0/_P1 page suffixes; CLEAR_ is one per child.
    children = sorted(
        k[len("CLEAR_"):] for k in defs
        if isinstance(k, str) and k.startswith("CLEAR_")
    )
    if not children:
        return
    tmpl: dict[str, str] = {}
    for child in children:
        approval = out_dir / f"{child}.approval.bin"
        if approval.exists():
            # Two ≤4096-byte pages matching the ApprovalProgramPages template
            # pair emitted by SolNewExpression; page 1 empty if unused.
            hex_val = approval.read_bytes().hex()
            page_hex = 4096 * 2
            tmpl[f"TMPL_APPROVAL_{child}_P0"] = hex_val[:page_hex]
            tmpl[f"TMPL_APPROVAL_{child}_P1"] = hex_val[page_hex:]
        clear = out_dir / f"{child}.clear.bin"
        if clear.exists():
            tmpl[f"TMPL_CLEAR_{child}"] = clear.read_bytes().hex()
    # sort_keys matches nlohmann::json's std::map ordering so the file is
    # byte-identical to the C++ writer's (these land in committed out/ dirs).
    (out_dir / "deploy.tmpl.json").write_text(json.dumps(tmpl, indent=2, sort_keys=True))


def compile_via_server(out_dir: Path, timeout: float) -> bool:
    """Compile the AWST already sitting in `out_dir` via the persistent server.

    Reads the stage-1 frontend outputs (awst.json/options.json), sends a
    `compile` RPC, and lets the server write the artifacts (it runs the same
    awst_to_teal artifact-writing as the CLI, into the absolute paths in
    options.json's compilation_set — which already point at `out_dir`).

    Returns True only on verified success (no error/critical logs AND at
    least one .arc56.json present). Returns False for *anything* else; the
    caller then falls back to the one-shot subprocess, which is the source
    of truth for failures.
    """
    if _DISABLED:
        return False
    awst_path = out_dir / "awst.json"
    options_path = out_dir / "options.json"
    if not awst_path.is_file() or not options_path.is_file():
        return False
    try:
        awst = json.loads(awst_path.read_text())
        options = json.loads(options_path.read_text())
    except (OSError, json.JSONDecodeError):
        return False

    resp = _client().request(
        "compile",
        {
            "awst": awst,
            "options": options,
            # The subprocess backend inherits the harness cwd; base_path is
            # pushd()'d by the server for the compile, so this keeps any
            # cwd-relative rendering identical between the two paths.
            "base_path": str(Path.cwd()),
            "log_level": "error",
            "source_annotations": {},
        },
        timeout=timeout,
    )
    if resp is None or "result" not in resp:
        return False  # transport failure or JSON-RPC error → subprocess
    logs = resp["result"].get("logs") or []
    if any(log.get("level") in ("error", "critical") for log in logs):
        return False  # compile problem → let the subprocess produce it
    if not any(out_dir.glob("*.arc56.json")):
        return False  # server claimed success but wrote nothing usable
    _write_child_deploy_templates(out_dir, options)
    return True
