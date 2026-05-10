"""Pytest integration for Solidity semantic tests.

Auto-discovers .sol test files, parses their assertions,
compiles with puya-sol, deploys to localnet, and verifies results.
"""
import subprocess
import json
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    PaymentTxn, wait_for_confirmation,
)
import pytest

from parser import parse_test_file, parse_value, SemanticTest, TestCall

ROOT = Path(__file__).parent.parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT / "puya" / ".venv" / "bin" / "puya"
TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"
NO_POPULATE = au.SendParams(populate_app_call_resources=False)


@pytest.fixture(scope="session")
def localnet():
    algod = au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod"))
    kmd = au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd"))
    client = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
    client.set_suggested_params_cache_timeout(0)
    account = client.account.localnet_dispenser()
    client.account.set_signer_from_account(account)
    return client, account


def _split_multi_source(sol_path):
    """Split multi-source test files into temp files.

    Handles both ==== Source: name ==== (inline source) and
    ==== ExternalSource: path ==== (reference to upstream Solidity test data).

    Returns (main_source_path, import_dir) or (sol_path, None) if single-source.
    """
    import tempfile, re, shutil

    content = sol_path.read_text()
    has_source = "==== Source:" in content
    has_external = "==== ExternalSource:" in content

    if not has_source and not has_external:
        return sol_path, None

    tmp_dir = Path(tempfile.mkdtemp(prefix="multisource_"))

    # Upstream Solidity test data directory (for ExternalSource resolution)
    upstream_dir = ROOT / "solidity" / "test" / "libsolidity" / "semanticTests"
    # ExternalSource paths are relative to the test's category directory upstream
    test_category = sol_path.parent.name
    upstream_category_dir = upstream_dir / test_category

    # Handle ExternalSource directives: copy files from upstream
    for m in re.finditer(r'^==== ExternalSource: (.+?) ====$', content, re.MULTILINE):
        spec = m.group(1).strip()
        # Format: "importName=filesystemPath" or just "path" (same for both)
        if "=" in spec:
            # Could have multiple = signs: first = splits import name from path
            # e.g. "a=_external/external.sol=sol" means import "a" from "_external/external.sol=sol"
            # But the Solidity test uses first = as the split
            parts = spec.split("=", 1)
            import_name = parts[0]
            fs_path = parts[1]
        else:
            import_name = spec
            fs_path = spec

        # Resolve the filesystem path from upstream
        src_file = upstream_category_dir / fs_path
        if not src_file.exists():
            # Try normalizing the path
            src_file = upstream_category_dir / Path(fs_path).as_posix()

        # Sanitize import name: strip leading / and ../ to keep within tmp_dir
        safe_name = import_name.lstrip("/")
        while safe_name.startswith("../"):
            safe_name = safe_name[3:]
        dst_file = tmp_dir / safe_name
        dst_file.parent.mkdir(parents=True, exist_ok=True)
        if src_file.exists():
            shutil.copy2(src_file, dst_file)
        else:
            # Create empty placeholder
            dst_file.write_text(f"// ExternalSource not found: {fs_path}\n")

    # Handle inline Source directives
    # Strip ExternalSource lines before splitting on Source
    stripped = re.sub(r'^==== ExternalSource: .+? ====$', '', content, flags=re.MULTILINE)

    if has_source:
        parts = re.split(r'^==== Source: (.+?) ====$', stripped, flags=re.MULTILINE)
        if len(parts) >= 3:
            last_name = None
            for i in range(1, len(parts), 2):
                name = parts[i].strip()
                body = parts[i + 1] if i + 1 < len(parts) else ""
                if "// ----" in body:
                    body = body[:body.index("// ----")]
                dst = tmp_dir / name
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_text(body)
                if not name.endswith(".sol"):
                    (tmp_dir / (name + ".sol")).write_text(body)
                last_name = name

            main = last_name + ".sol" if not last_name.endswith(".sol") else last_name
            return tmp_dir / main, tmp_dir

    # No inline Source directives — the main source is the test file itself
    # (minus the ExternalSource directives and test assertions)
    main_content = stripped
    if "// ----" in main_content:
        main_content = main_content[:main_content.index("// ----")]
    # Also strip any ==== lines
    main_content = re.sub(r'^====.*====$', '', main_content, flags=re.MULTILINE).strip()

    main_file = tmp_dir / sol_path.name
    main_file.write_text(main_content + "\n")
    return main_file, tmp_dir


def compile_sol(sol_path, out_dir, via_yul_behavior=False, evm_version=None):
    # Always clean and recompile — no stale cache
    import shutil
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    content = sol_path.read_text()
    has_multi = "==== Source:" in content or "==== ExternalSource:" in content

    if has_multi:
        # Multi-source fixture: pure-Python preprocessor — see
        # multisource_splitter.split_multisource. Materialises each
        # `==== Source: ====` block to a temp file, copies any
        # `ExternalSource:` files in from the upstream Solidity tree.
        from multisource_splitter import split_multisource
        upstream_root = ROOT / "solidity" / "test" / "libsolidity" / "semanticTests"
        source_path, _all, import_dir = split_multisource(
            sol_path, upstream_root if upstream_root.exists() else None
        )
    else:
        source_path = sol_path
        import_dir = None

    cmd = [str(COMPILER), "--source", str(source_path),
           "--output-dir", str(out_dir),
           "--puya-path", str(PUYA)]
    if import_dir:
        cmd += ["--import-path", str(import_dir)]
    if via_yul_behavior:
        cmd += ["--via-yul-behavior"]
    if evm_version:
        cmd += ["--evm-version", evm_version]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)

    # Clean up temp files
    if import_dir:
        import shutil
        shutil.rmtree(import_dir, ignore_errors=True)

    if result.returncode != 0:
        return None
    contracts = {}
    for arc56 in out_dir.glob("*.arc56.json"):
        name = arc56.stem.replace(".arc56", "")
        contracts[name] = {
            "arc56": arc56,
            "approval_teal": out_dir / f"{name}.approval.teal",
            "clear_teal": out_dir / f"{name}.clear.teal",
        }
    return contracts


def deploy_app(localnet, account, artifacts):
    try:
        app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())
        algod = localnet.client.algod
        approval_bin = encoding.base64.b64decode(
            algod.compile(artifacts["approval_teal"].read_text())["result"])
        clear_bin = encoding.base64.b64decode(
            algod.compile(artifacts["clear_teal"].read_text())["result"])
        extra_pages = max(0, (max(len(approval_bin), len(clear_bin)) - 1) // 2048)
        # Extract box references from TEAL for constructor box_create calls
        box_refs = _extract_box_refs(artifacts["approval_teal"])
        sp = algod.suggested_params()
        sp.flat_fee = True
        sp.fee = max(sp.min_fee, 1000) * 4  # cover inner txns
        txn = ApplicationCreateTxn(
            sender=account.address, sp=sp,
            on_complete=OnComplete.NoOpOC,
            approval_program=approval_bin, clear_program=clear_bin,
            global_schema=StateSchema(num_uints=16, num_byte_slices=16),
            local_schema=StateSchema(num_uints=0, num_byte_slices=0),
            extra_pages=extra_pages,
            boxes=box_refs if box_refs else None,
        )
        txid = algod.send_transaction(txn.sign(account.private_key))
        result = wait_for_confirmation(algod, txid, 4)
        app_id = result["application-index"]
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big")))
        sp2 = algod.suggested_params()
        # Compute MBR from declared box_create sizes so multi-box state arrays
        # (>32KB total) get funded correctly. Each box's MBR is
        # `2500 + 400 * (key_len + value_len)` microAlgos.
        box_mbr = _compute_box_mbr(artifacts["approval_teal"])
        funding = 1_000_000 + box_mbr
        pay = PaymentTxn(account.address, sp2, app_addr, funding)
        wait_for_confirmation(algod,
            algod.send_transaction(pay.sign(account.private_key)), 4)
        app = au.AppClient(au.AppClientParams(
            algorand=localnet, app_spec=app_spec,
            app_id=app_id, default_sender=account.address))

        # Call __postInit if it exists (constructor body that writes to boxes)
        has_post_init = any(
            m.name == "__postInit" for m in app_spec.methods
        )
        if has_post_init:
            app.send.call(
                au.AppClientMethodCallParams(
                    method="__postInit",
                    args=[],
                ),
            )

        return app
    except Exception:
        return None


def _compute_box_mbr(teal_path):
    """Sum MBR for all box_create calls in the TEAL.

    Pattern: a `pushint <size>` (or `intc_<n>` referencing a known intcblock
    entry) immediately preceding `box_create`, with the box key on the
    line before that. We just sum the value+key MBR per declared box.
    """
    import re
    teal = teal_path.read_text()
    lines = teal.split('\n')

    intc_values: list[int] = []
    intc_match = re.search(r'^\s*intcblock\s+(.*)$', teal, re.MULTILINE)
    if intc_match:
        for tok in intc_match.group(1).split():
            try:
                intc_values.append(int(tok))
            except ValueError:
                pass

    bytec_values: list[bytes] = []
    bytec_match = re.search(r'^\s*bytecblock\s+(.*)$', teal, re.MULTILINE)
    if bytec_match:
        for tok in bytec_match.group(1).split():
            tok = tok.strip()
            if tok.startswith('0x'):
                try:
                    bytec_values.append(bytes.fromhex(tok[2:]))
                except ValueError:
                    bytec_values.append(b'')
            elif tok.startswith('"') and tok.endswith('"'):
                bytec_values.append(tok[1:-1].encode())
            else:
                bytec_values.append(b'')

    def _read_int(line):
        line = line.strip()
        m = re.search(r'pushint\s+(\d+)', line)
        if m: return int(m.group(1))
        m = re.match(r'intc_(\d+)', line)
        if m and int(m.group(1)) < len(intc_values): return intc_values[int(m.group(1))]
        m = re.match(r'intc\s+(\d+)', line)
        if m and int(m.group(1)) < len(intc_values): return intc_values[int(m.group(1))]
        return None

    def _read_bytes_len(line):
        line = line.strip()
        m = re.search(r'pushbytes\s+0x([0-9a-fA-F]+)', line)
        if m: return len(bytes.fromhex(m.group(1)))
        m = re.search(r'pushbytes\s+"([^"]*)"', line)
        if m: return len(m.group(1).encode())
        m = re.match(r'bytec_(\d+)', line)
        if m and int(m.group(1)) < len(bytec_values): return len(bytec_values[int(m.group(1))])
        m = re.match(r'bytec\s+(\d+)', line)
        if m and int(m.group(1)) < len(bytec_values): return len(bytec_values[int(m.group(1))])
        return None

    total = 0
    for i, line in enumerate(lines):
        if line.strip() == 'box_create':
            size = None
            key_len = None
            for j in range(i-1, max(0, i-6), -1):
                if size is None:
                    size = _read_int(lines[j])
                    if size is not None:
                        continue
                if key_len is None:
                    key_len = _read_bytes_len(lines[j])
                    if key_len is not None:
                        break
            if size is None or key_len is None:
                continue
            total += 2500 + 400 * (key_len + size)
    return total


def _extract_box_refs(teal_path):
    """Extract box references from TEAL — find strings used with box_create/box_get/box_resize."""
    import re
    teal = teal_path.read_text()
    refs = []
    seen = set()

    # Find all pushbytes before box_* ops
    lines = teal.split('\n')
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('box_') or 'box_create' in stripped or 'box_get' in stripped or 'box_resize' in stripped:
            # Look backwards for the key pushbytes
            for j in range(max(0, i-5), i):
                prev = lines[j].strip()
                # Match: pushbytes "key" or bytec_N // "key"
                m = re.search(r'// "([^"]+)"', prev)
                if m:
                    key = m.group(1)
                    if key not in seen:
                        seen.add(key)
                        refs.append((0, key.encode()))
                    break
                m2 = re.search(r'pushbytes "([^"]+)"', prev)
                if m2:
                    key = m2.group(1)
                    if key not in seen:
                        seen.add(key)
                        refs.append((0, key.encode()))
                    break

    # Also grab hex constants from bytecblock that *could* be box keys.
    # Filter to ≤64 bytes (AVM box name max) — bigger constants are
    # state-var default-value buffers, not box keys, and submitting them
    # as references makes algod reject the txn.
    bytecblock_match = re.search(r'bytecblock\s+(.*)', teal)
    if bytecblock_match:
        tokens = bytecblock_match.group(1).split()
        for token in tokens:
            if token.startswith('0x') and len(token) > 10:
                key_bytes = bytes.fromhex(token[2:])
                if len(key_bytes) > 64:
                    continue
                if key_bytes not in seen:
                    seen.add(key_bytes)
                    refs.append((0, key_bytes))

    return refs


def compare_values(actual, expected):
    if expected is None:
        return True
    if isinstance(expected, bool):
        return actual is expected
    if isinstance(expected, int):
        if isinstance(actual, bool):
            return (1 if actual else 0) == expected
        if isinstance(actual, int):
            return actual == expected
        # Algorand address string → decode to integer for comparison
        if isinstance(actual, str) and len(actual) == 58:
            try:
                from algosdk import encoding
                addr_bytes = encoding.decode_address(actual)
                addr_int = int.from_bytes(addr_bytes, 'big')
                return addr_int == expected
            except Exception:
                pass
        # Unwrap single-value dicts (ARC4 struct with one field)
        if isinstance(actual, dict) and len(actual) == 1:
            return compare_values(list(actual.values())[0], expected)
        if isinstance(actual, (list, tuple)):
            try:
                actual_bytes = bytes(actual)
                actual_int = int.from_bytes(actual_bytes, 'big')
                if actual_int == expected:
                    return True
                # EVM bytesN returns are left-padded to 32 bytes.
                # ARC4 returns raw N bytes. Left-pad actual to 32 and compare.
                if len(actual_bytes) < 32:
                    padded = actual_bytes + b'\x00' * (32 - len(actual_bytes))
                    padded_int = int.from_bytes(padded, 'big')
                    if padded_int == expected:
                        return True
                return False
            except (ValueError, OverflowError):
                pass
    if isinstance(expected, bytes):
        if isinstance(actual, str):
            actual = actual.encode('utf-8')
        if isinstance(actual, bytes):
            if actual == expected:
                return True
            if actual.rstrip(b'\x00') == expected.rstrip(b'\x00'):
                return True
            return False
        if isinstance(actual, (list, tuple)):
            actual_bytes = bytes(actual)
            if actual_bytes == expected:
                return True
            if actual_bytes.rstrip(b'\x00') == expected.rstrip(b'\x00'):
                return True
            return False
    return str(actual) == str(expected)
