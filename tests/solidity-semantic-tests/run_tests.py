#!/usr/bin/env python3
"""Run Solidity semantic tests against puya-sol compiled contracts on AVM localnet.

Usage:
    python run_tests.py [--category smoke] [--limit 10] [--verbose]

Phases:
  1. Parse .sol test files from tests/<category>/
  2. Compile each with puya-sol
  3. Deploy to localnet
  4. Execute assertion calls and compare results

Results are printed as PASS/FAIL/SKIP/COMPILE_ERROR/DEPLOY_ERROR.
"""
import argparse
import json
import os
import subprocess
import sys
import traceback
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    PaymentTxn, wait_for_confirmation,
)

import re
from parser import parse_test_file, parse_value, SemanticTest


def find_last_contract(sol_path, deployable):
    """Find the last contract defined in the source that has deployable artifacts."""
    content = sol_path.read_text()
    contracts = re.findall(r'(?:contract|library)\s+(\w+)', content)
    for name in reversed(contracts):
        if name in deployable:
            return name
    return list(deployable.keys())[-1]

ROOT = Path(__file__).parent.parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT.parent / "puya" / ".venv" / "bin" / "puya"
TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"

NO_POPULATE = au.SendParams(populate_app_call_resources=False)
POPULATE = au.SendParams(populate_app_call_resources=True)


def setup_localnet():
    """Connect to localnet and return (localnet, account)."""
    algod = au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod")
    )
    kmd = au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd")
    )
    localnet = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
    localnet.set_suggested_params_cache_timeout(0)
    account = localnet.account.localnet_dispenser()
    localnet.account.set_signer_from_account(account)
    return localnet, account


def _split_multi_source(sol_path):
    """Split multi-source test files (==== Source: name ====) into temp files.

    Returns (main_source_path, import_dir) or (sol_path, None) if single-source.
    """
    import tempfile
    content = sol_path.read_text()
    if "==== Source:" not in content:
        return sol_path, None

    parts = re.split(r'^==== Source: (.+?) ====$', content, flags=re.MULTILINE)
    if len(parts) < 3:
        return sol_path, None

    tmp_dir = Path(tempfile.mkdtemp(prefix="multisource_"))
    last_name = None
    for i in range(1, len(parts), 2):
        name = parts[i].strip()
        body = parts[i + 1] if i + 1 < len(parts) else ""
        if "// ----" in body:
            body = body[:body.index("// ----")]
        (tmp_dir / name).write_text(body)
        if not name.endswith(".sol"):
            (tmp_dir / (name + ".sol")).write_text(body)
        last_name = name

    main = last_name + ".sol" if not last_name.endswith(".sol") else last_name
    return tmp_dir / main, tmp_dir


def compile_test(sol_path: Path, out_dir: Path) -> dict | None:
    """Compile a .sol file. Returns dict of contract artifacts or None on failure."""
    out_dir.mkdir(parents=True, exist_ok=True)

    source_path, import_dir = _split_multi_source(sol_path)

    cmd = [str(COMPILER), "--source", str(source_path),
         "--output-dir", str(out_dir),
         "--puya-path", str(PUYA)]
    if import_dir:
        cmd += ["--import-path", str(import_dir)]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        raise Exception(f"compilation timed out after 300s")

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


def _extract_box_refs(teal_path):
    """Extract box references from TEAL — find strings used with box_* ops."""
    teal = teal_path.read_text()
    refs = []
    seen = set()
    lines = teal.split('\n')
    for i, line in enumerate(lines):
        stripped = line.strip()
        if any(op in stripped for op in ['box_create', 'box_get', 'box_put',
                'box_del', 'box_len', 'box_resize', 'box_replace', 'box_extract']):
            for j in range(max(0, i-5), i):
                prev = lines[j].strip()
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
    # Also grab from bytecblock
    bytecblock_match = re.search(r'bytecblock\s+(.*)', teal)
    if bytecblock_match:
        for token in bytecblock_match.group(1).split():
            if token.startswith('"') and token.endswith('"'):
                key = token[1:-1]
                if key not in seen and len(key) <= 64:
                    seen.add(key)
                    refs.append((0, key.encode()))
    return refs


def _encode_ctor_arg(val_str: str) -> bytes:
    """Encode a constructor argument value to ABI bytes (32-byte big-endian for ints)."""
    val = parse_value(val_str)
    if isinstance(val, bool):
        return val.to_bytes(32, 'big') if val else b'\x00' * 32
    if isinstance(val, int):
        return val.to_bytes(32, 'big')
    if isinstance(val, bytes):
        # Left-pad to 32 bytes
        return b'\x00' * (32 - len(val)) + val if len(val) < 32 else val[:32]
    # String or other: encode as UTF-8 bytes
    s = str(val)
    return s.encode('utf-8')


def deploy_contract(localnet, account, artifacts, ctor_args=None, fund_amount=0) -> au.AppClient | None:
    """Deploy a compiled contract. Returns AppClient or None.
    ctor_args: list of raw string args from constructor() call, or None.
    fund_amount: value in wei/microAlgos to fund beyond minimum balance."""
    try:
        app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())
        algod = localnet.client.algod

        approval_bin = encoding.base64.b64decode(
            algod.compile(artifacts["approval_teal"].read_text())["result"]
        )
        clear_bin = encoding.base64.b64decode(
            algod.compile(artifacts["clear_teal"].read_text())["result"]
        )

        max_size = max(len(approval_bin), len(clear_bin))
        extra_pages = max(0, (max_size - 1) // 2048)

        # Encode constructor arguments as application args
        app_args = None
        if ctor_args:
            app_args = [_encode_ctor_arg(a) for a in ctor_args]

        # Extract box references from TEAL
        box_refs = _extract_box_refs(artifacts["approval_teal"])

        sp = algod.suggested_params()
        sp.flat_fee = True
        sp.fee = max(sp.min_fee, 1000) * 4  # cover inner txns + box ops
        txn = ApplicationCreateTxn(
            sender=account.address, sp=sp,
            on_complete=OnComplete.NoOpOC,
            approval_program=approval_bin, clear_program=clear_bin,
            global_schema=StateSchema(num_uints=16, num_byte_slices=16),
            local_schema=StateSchema(num_uints=0, num_byte_slices=0),
            extra_pages=extra_pages,
            app_args=app_args,
            boxes=box_refs if box_refs else None,
        )
        signed = txn.sign(account.private_key)
        txid = algod.send_transaction(signed)
        result = wait_for_confirmation(algod, txid, 4)
        app_id = result["application-index"]

        # Fund
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
        )
        # Fund: min balance + box storage + constructor value
        # Box storage costs 2500 + 400*size per box
        box_cost = sum(2500 + 400 * 1024 for _ in box_refs)  # assume 1KB per box
        min_balance = 100_000 + 28500 * 16 + 50000 * 16 + box_cost
        total_fund = min_balance + fund_amount
        sp2 = algod.suggested_params()
        pay = PaymentTxn(account.address, sp2, app_addr, total_fund)
        txid2 = algod.send_transaction(pay.sign(account.private_key))
        wait_for_confirmation(algod, txid2, 4)

        app_client = au.AppClient(
            au.AppClientParams(
                algorand=localnet, app_spec=app_spec,
                app_id=app_id, default_sender=account.address,
            )
        )

        # Call __postInit if it exists (constructor body that writes to boxes)
        if any(m.name == "__postInit" for m in app_spec.methods):
            from algosdk.transaction import BoxReference as AlgoBoxRef
            try:
                app_client.send.call(
                    au.AppClientMethodCallParams(
                        method="__postInit",
                        args=[],
                        box_references=[AlgoBoxRef(app_index=0, name=ref[1]) for ref in box_refs] if box_refs else None,
                    )
                )
            except Exception as e:
                import sys
                print(f"  [warn] __postInit failed: {str(e)[:100]}", file=sys.stderr)

        # Store box refs on the client for use in subsequent calls
        app_client._box_refs = box_refs
        return app_client
    except Exception:
        return None


SOL_TO_ARC4 = {
    "uint8": "uint8", "uint16": "uint16", "uint32": "uint32", "uint64": "uint64",
    "uint128": "uint128", "uint256": "uint256",
    "int8": "int8", "int16": "int16", "int32": "int32", "int64": "int64",
    "int128": "int128", "int256": "int256",
    "bool": "bool", "address": "address", "string": "string",
    "bytes": "byte[]",
}
for i in range(1, 33):
    SOL_TO_ARC4[f"bytes{i}"] = f"byte[{i}]"


def resolve_method(app_spec, sol_sig):
    """Resolve a Solidity method signature to an ARC56 method."""
    m = re.match(r'(\w+)\((.*)\)', sol_sig)
    if not m:
        return sol_sig
    name, params_str = m.group(1), m.group(2)
    methods = [method for method in app_spec.methods if method.name == name]
    if len(methods) == 0:
        return sol_sig
    if len(methods) == 1:
        method = methods[0]
        args_part = ",".join(a.type for a in method.args)
        ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
        return f"{name}({args_part}){ret_part}"
    sol_params = [p.strip() for p in params_str.split(",")] if params_str else []
    arc4_params = [SOL_TO_ARC4.get(p, p) for p in sol_params]
    for method in methods:
        method_types = [a.type for a in method.args]
        if method_types == arc4_params:
            args_part = ",".join(method_types)
            ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
            return f"{name}({args_part}){ret_part}"
    for method in methods:
        if len(method.args) == len(sol_params):
            args_part = ",".join(a.type for a in method.args)
            ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
            return f"{name}({args_part}){ret_part}"
    return sol_sig


_budget_helper_app_id = None

def _get_budget_helper(algod, sender, signer):
    """Deploy (once) a tiny app that just approves — used for opcode budget pooling.

    With 15 helper calls in a group, total budget is 16 × 700 = 11,200 opcodes.
    """
    global _budget_helper_app_id
    if _budget_helper_app_id is not None:
        return _budget_helper_app_id
    from algosdk.transaction import ApplicationCreateTxn, OnComplete, StateSchema, wait_for_confirmation
    from algosdk import encoding
    approval = encoding.base64.b64decode(
        algod.compile("#pragma version 10\nint 1")["result"])
    clear = approval
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender, sp=sp, on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=0, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
    )
    txid = algod.send_transaction(txn.sign(signer.private_key if hasattr(signer, 'private_key') else signer))
    result = wait_for_confirmation(algod, txid, 4)
    _budget_helper_app_id = result["application-index"]
    return _budget_helper_app_id


def _call_with_extra_budget(app, method, args, extra_calls=15):
    """Call an app method with extra opcode budget by pooling dummy app calls."""
    from algosdk.transaction import ApplicationCallTxn, OnComplete
    from algosdk.atomic_transaction_composer import (
        AtomicTransactionComposer, TransactionWithSigner,
    )
    from algosdk.abi import Method
    import os

    algod = app.algorand.client.algod
    sender = app._default_sender
    signer = app._default_signer or app.algorand.account.get_signer(sender)
    app_id = app.app_id

    # Get or deploy the budget helper app
    helper_id = _get_budget_helper(algod, sender, signer)

    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 1000 * (extra_calls + 1)

    atc = AtomicTransactionComposer()

    # The real method call — include box refs if available
    from algosdk.transaction import BoxReference as AlgoBoxRef
    box_refs = []
    if hasattr(app, '_box_refs') and app._box_refs:
        box_refs = [AlgoBoxRef(app_index=0, name=ref[1]) for ref in app._box_refs]

    abi_method = Method.from_signature(method)
    atc.add_method_call(
        app_id=app_id, method=abi_method, sender=sender,
        sp=sp, signer=signer, method_args=args if args else [],
        note=os.urandom(8),
        boxes=box_refs if box_refs else None,
    )

    # Dummy budget-pooling calls to the helper app (cheap, always succeeds)
    sp_dummy = algod.suggested_params()
    sp_dummy.flat_fee = True
    sp_dummy.fee = 0
    for _ in range(extra_calls):
        txn = ApplicationCallTxn(
            sender=sender, sp=sp_dummy, index=helper_id,
            on_complete=OnComplete.NoOpOC, note=os.urandom(8),
        )
        atc.add_transaction(TransactionWithSigner(txn, signer))

    result = atc.execute(algod, wait_rounds=4)

    class FakeResult:
        pass
    r = FakeResult()
    r.abi_return = result.abi_results[0].return_value if result.abi_results else None
    return r


def _regroup_args(raw_args, method_sig):
    """Regroup flat EVM ABI-encoded args into structured ARC4 args.

    EVM ABI encoding places static values and offsets for dynamic types in
    the "head" region (one word per param), then dynamic data in the "tail".
    This function decodes that layout into individual ARC4-compatible args.
    """
    import re as _re
    m = _re.match(r'\w+\(([^)]*)\)', method_sig)
    if not m or not m.group(1):
        return raw_args

    # Parse parameter types
    param_types = []
    depth = 0
    current = ""
    for c in m.group(1):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        if c == ',' and depth == 0:
            param_types.append(current.strip())
            current = ""
        else:
            current += c
    if current.strip():
        param_types.append(current.strip())

    # If arg count matches param count, no regrouping needed
    if len(raw_args) == len(param_types):
        return raw_args

    # Check if any params are complex (arrays/strings/bytes)
    dynamic_types = {'string', 'bytes'}
    has_complex = any('[' in p or p in dynamic_types for p in param_types)
    if not has_complex:
        return raw_args

    def _is_dynamic(pt):
        return pt in dynamic_types or _re.match(r'.*\[\]$', pt)

    def _static_array_dims(pt):
        """Parse static array dimensions. Returns list of sizes or None.
        e.g. 'uint256[3]' -> [3], 'uint256[3][2]' -> [3, 2], 'uint256' -> None
        """
        dims = _re.findall(r'\[(\d+)\]', pt)
        return [int(d) for d in dims] if dims else None

    def _static_array_size(pt):
        dims = _static_array_dims(pt)
        if not dims:
            return None
        total = 1
        for d in dims:
            total *= d
        return total

    # Two-pass ABI decode: head (one word per param) then tail (dynamic data)
    # Head region: N words where N = number of params (each static param is
    # its value, each dynamic param is an offset into the tail)
    n_params = len(param_types)

    def _build_nested_array(flat, offset, dims):
        """Build nested list from flat args. Returns (nested_list, new_offset)."""
        if len(dims) == 1:
            arr = flat[offset:offset + dims[0]]
            return arr, offset + dims[0]
        outer_size = dims[-1]  # outermost dimension
        inner_dims = dims[:-1]
        result = []
        for _ in range(outer_size):
            inner, offset = _build_nested_array(flat, offset, inner_dims)
            result.append(inner)
        return result, offset

    # If no dynamic types, use simple sequential consumption
    if not any(_is_dynamic(pt) for pt in param_types):
        result = []
        idx = 0
        for pt in param_types:
            dims = _static_array_dims(pt)
            if dims is not None:
                total = _static_array_size(pt)
                if idx + total <= len(raw_args):
                    if len(dims) == 1:
                        result.append(raw_args[idx:idx + total])
                    else:
                        nested, _ = _build_nested_array(raw_args, idx, dims)
                        result.append(nested)
                    idx += total
                else:
                    result.append(raw_args[idx] if idx < len(raw_args) else 0)
                    idx += 1
            else:
                result.append(raw_args[idx] if idx < len(raw_args) else 0)
                idx += 1
        return result

    # Has dynamic types: proper ABI head/tail decode
    # Head: one word per param. For static types, the value is inline.
    # For dynamic types, the word is an offset (in bytes) to the tail data.
    result = []
    head_idx = 0
    for pt in param_types:
        dims = _static_array_dims(pt)
        sz = _static_array_size(pt)
        if sz is not None and not _is_dynamic(pt):
            # Static array: consumes total elements inline in the head
            if head_idx + sz <= len(raw_args):
                if dims and len(dims) > 1:
                    nested, _ = _build_nested_array(raw_args, head_idx, dims)
                    result.append(nested)
                else:
                    result.append(raw_args[head_idx:head_idx + sz])
                head_idx += sz
            else:
                result.append(raw_args[head_idx] if head_idx < len(raw_args) else 0)
                head_idx += 1
        elif _is_dynamic(pt):
            # Dynamic type: head word is offset (in bytes)
            offset = raw_args[head_idx] if head_idx < len(raw_args) else 0
            head_idx += 1
            # Decode data at offset
            if isinstance(offset, int):
                word_idx = offset // 32  # convert byte offset to word index
                if word_idx < len(raw_args):
                    length = raw_args[word_idx]
                    if isinstance(length, int):
                        data_start = word_idx + 1
                        if pt in dynamic_types:
                            # string/bytes: collect data words and concatenate
                            if length == 0:
                                result.append("" if pt == 'string' else b"")
                            elif data_start < len(raw_args) and length <= 10000:
                                val = raw_args[data_start]
                                if isinstance(val, bytes):
                                    result.append(val[:length])
                                elif isinstance(val, int):
                                    # Data as int words
                                    data = b""
                                    n_words = min((length + 31) // 32, len(raw_args) - data_start)
                                    for w in range(n_words):
                                        if data_start + w < len(raw_args):
                                            wv = raw_args[data_start + w]
                                            if isinstance(wv, bytes):
                                                data += wv
                                            elif isinstance(wv, int):
                                                data += wv.to_bytes(32, 'big')
                                    result.append(data[:length])
                                elif val is None:
                                    result.append("" if pt == 'string' else b"")
                                else:
                                    result.append(val)
                            else:
                                result.append("" if pt == 'string' else b"")
                        else:
                            # Dynamic array: collect elements (cap to available args)
                            n_avail = len(raw_args) - data_start
                            n_elems = min(int(length), n_avail) if length <= 10000 else n_avail
                            arr = []
                            for j in range(n_elems):
                                arr.append(raw_args[data_start + j])
                            result.append(arr)
                    else:
                        result.append(raw_args[word_idx])
                else:
                    result.append("" if pt in dynamic_types else [])
            else:
                result.append(offset)
        else:
            # Simple scalar
            result.append(raw_args[head_idx] if head_idx < len(raw_args) else 0)
            head_idx += 1

    return result


def execute_call(app, call, app_spec=None, verbose=False):
    """Execute a test call and return (passed, detail)."""
    try:
        # Build args
        raw_args = []
        for arg_str in call.args:
            val = parse_value(arg_str)
            raw_args.append(val)

        args = []
        # Determine expected parameter sizes from ARC4 method spec
        resolved = resolve_method(app_spec, call.method_signature) if app_spec else call.method_signature
        param_sizes = {}  # index → expected byte count for byte[N] params
        param_types = {}  # index → ARC4 type string
        if app_spec:
            import re as _re
            pm = _re.match(r'\w+\(([^)]*)\)', resolved)
            if pm and pm.group(1):
                ptypes = pm.group(1).split(',')
                for idx, pt in enumerate(ptypes):
                    param_types[idx] = pt.strip()
                    bm = _re.match(r'byte\[(\d+)\]', pt)
                    if bm:
                        param_sizes[idx] = int(bm.group(1))
        for i, a in enumerate(raw_args):
            if a is None:
                args.append(0)
            elif isinstance(a, bytes):
                if i in param_sizes:
                    # byte[N] static array param: pad/truncate and convert to list
                    expected_size = param_sizes[i]
                    if len(a) < expected_size:
                        a = a + b'\x00' * (expected_size - len(a))
                    elif len(a) > expected_size:
                        a = a[:expected_size]
                    args.append(list(a))
                else:
                    # string/bytes/other: pass as bytes directly
                    args.append(a)
            elif isinstance(a, int) and i in param_sizes:
                # int → byte[N]: convert large int (EVM left-padded) to N-byte list
                expected_size = param_sizes[i]
                byte_len = max(expected_size, (a.bit_length() + 7) // 8) if a else expected_size
                a_bytes = a.to_bytes(byte_len, 'big')[-expected_size:] if a else b'\x00' * expected_size
                if len(a_bytes) < expected_size:
                    a_bytes = b'\x00' * (expected_size - len(a_bytes)) + a_bytes
                args.append(list(a_bytes))
            elif isinstance(a, int) and param_types.get(i) == 'address':
                # int → address: encode as 32-byte Algorand address
                from algosdk import encoding
                a_bytes = a.to_bytes(32, 'big')
                args.append(encoding.encode_address(a_bytes))
            else:
                args.append(a)

        # Skip payable calls (AVM has no native currency in app calls)
        if call.value_wei > 0:
            return None, "payable skipped"

        method = resolve_method(app_spec, call.method_signature) if app_spec else call.method_signature

        # Regroup flat args for static/dynamic array parameters
        args = _regroup_args(args, call.method_signature)

        # Trim excess args if method expects fewer (extra calldata in EVM tests)
        if app_spec:
            import re as _re2
            pm2 = _re2.match(r'\w+\(([^)]*)\)', method)
            if pm2:
                n_expected = len([p for p in pm2.group(1).split(',') if p.strip()]) if pm2.group(1).strip() else 0
                if len(args) > n_expected:
                    args = args[:n_expected]

        # Convert bytes to str for string params (algokit expects str)
        if app_spec:
            resolved_m = resolve_method(app_spec, call.method_signature)
            import re as _re
            pm2 = _re.match(r'\w+\(([^)]*)\)', resolved_m)
            if pm2 and pm2.group(1):
                rptypes = pm2.group(1).split(',')
                for i2, pt2 in enumerate(rptypes):
                    if pt2.strip() == 'string' and i2 < len(args) and isinstance(args[i2], bytes):
                        args[i2] = args[i2].decode('utf-8', errors='replace')

        params = au.AppClientMethodCallParams(method=method, args=args if args else None)

        if call.expect_failure:
            try:
                app.send.call(params, send_params=NO_POPULATE)
                return False, "expected FAILURE but succeeded"
            except Exception:
                return True, "correctly reverted"
        else:
            try:
                result = app.send.call(params, send_params=POPULATE)
            except Exception as ex1:
                err_str = str(ex1)
                is_budget = "cost budget exceeded" in err_str or "dynamic cost budget" in err_str
                is_fee = "fee too small" in err_str
                is_simulate = "simulate" in err_str.lower() or "resolving execution info" in err_str.lower()
                if is_budget or is_fee or is_simulate:
                    try:
                        result = _call_with_extra_budget(
                            app, method, args,
                            extra_calls=3 if is_fee and not is_budget else 15,
                        )
                    except Exception:
                        raise ex1
                else:
                    raise
            actual = result.abi_return

            # Parse expected values
            if len(call.expected) == 0 or (len(call.expected) == 1 and call.expected[0] == ""):
                # Void return
                return True, "void ok"

            # If ABI return is None but we expect values, check transaction logs
            # for structured return data (emitted by assembly return() in void functions)
            if actual is None and len(call.expected) > 0:
                try:
                    # Get transaction logs
                    tx_id = result.tx_ids[0] if hasattr(result, 'tx_ids') and result.tx_ids else None
                    logs = None
                    if hasattr(result, 'confirmations') and result.confirmations:
                        logs = result.confirmations[0].get('logs', [])
                    elif tx_id:
                        tx_info = app.algorand.client.algod.pending_transaction_info(tx_id)
                        logs = tx_info.get('logs', [])

                    if logs:
                        import base64
                        # Use the last log entry (structured return data)
                        raw = base64.b64decode(logs[-1])
                        # Skip ARC4 return prefix if present
                        if raw[:4] == b'\x15\x1f\x7c\x75':
                            raw = raw[4:]
                        # Decode as N × 32-byte uint256 values
                        n_values = len(raw) // 32
                        if n_values > 0:
                            actual = [int.from_bytes(raw[i*32:(i+1)*32], 'big') for i in range(n_values)]
                            if len(actual) == 1:
                                actual = actual[0]
                except Exception:
                    pass  # Fall through to normal comparison

            if len(call.expected) == 1:
                expected = parse_value(call.expected[0])
                if _compare_values(actual, expected):
                    return True, f"{actual}"
                return False, f"expected {expected}, got {actual}"

            # Multiple return values
            if isinstance(actual, (list, tuple)):
                actual_list = list(actual)
            elif isinstance(actual, dict):
                actual_list = list(actual.values())
            else:
                actual_list = [actual]

            expected_list = [parse_value(e) for e in call.expected]

            # Struct return: EVM wraps in offset (0x20) then fields inline/dynamic.
            # If actual is a dict and expected starts with 0x20, skip the offset
            # and try matching the struct fields against the remaining expected values.
            if (isinstance(actual, dict)
                and len(expected_list) >= 2
                and isinstance(expected_list[0], int) and expected_list[0] == 32):
                struct_expected = expected_list[1:]
                struct_vals = list(actual.values())
                decoded = _try_decode_evm_returns(struct_expected, struct_vals)
                if decoded is not None:
                    return True, f"{actual}"

            # Handle EVM ABI-encoded dynamic return values.
            # EVM returns strings/bytes as [offset, length, data_word1, data_word2, ...]
            # Multi-return with strings: [off1, off2, ..., len1, data1..., len2, data2...]
            # ARC4 returns values directly. Try to decode EVM format.
            # Also try when lengths coincidentally match (e.g., byte[] "abc" → 3 elements
            # vs EVM [0x20, 3, padded_data] → also 3 elements).
            if (len(actual_list) != len(expected_list)
                or (len(expected_list) >= 2
                    and isinstance(expected_list[0], int) and expected_list[0] == 32)):
                decoded = _try_decode_evm_returns(expected_list, actual_list)
                if decoded is not None:
                    return True, f"{actual}"
                # Simple single dynamic return: [0x20, len, data...]
                if (len(expected_list) >= 2
                    and isinstance(expected_list[0], int) and expected_list[0] == 32
                    and isinstance(expected_list[1], int)):
                    exp_len = expected_list[1]
                    collapsed = expected_list[2:]
                    if not collapsed and exp_len == 0:
                        if isinstance(actual, str) and actual == "":
                            return True, f"{actual}"
                        if isinstance(actual, bytes) and actual == b"":
                            return True, f"{actual}"
                        if isinstance(actual, (list, tuple)) and len(actual) == 0:
                            return True, f"{actual}"
                    elif len(collapsed) >= 1:
                        # Concatenate multi-word data
                        data = b""
                        for c in collapsed:
                            if isinstance(c, bytes):
                                data += c
                            elif isinstance(c, int):
                                data += c.to_bytes(32, 'big')
                        data = data[:exp_len]  # trim to declared length
                        # Compare with original actual (not split actual_list).
                        # ARC4 byte[] returns as list of ints — convert to bytes.
                        actual_for_cmp = actual
                        if isinstance(actual, (list, tuple)) and all(isinstance(x, int) for x in actual):
                            actual_for_cmp = bytes(actual)
                        if _compare_values(actual_for_cmp, data):
                            return True, f"{actual}"
                        return False, f"expected {data}, got {actual_for_cmp}"
                    if collapsed:
                        expected_list = collapsed

            if len(actual_list) != len(expected_list):
                return False, f"expected {len(expected_list)} values, got {len(actual_list)}"

            for i, (a, e) in enumerate(zip(actual_list, expected_list)):
                if not _compare_values(a, e):
                    return False, f"value[{i}]: expected {e}, got {a}"

            return True, f"{actual_list}"

    except Exception as ex:
        if call.expect_failure:
            return True, "correctly reverted"
        return False, f"exception: {str(ex)[:100]}"


def _try_decode_evm_returns(expected_list, actual_list):
    """Try to match EVM ABI-encoded multi-return values against ARC4 actual values.

    EVM head/tail encoding: the head has N words (one per return value).
    Dynamic types (string, bytes) have an offset in the head pointing to tail data.
    Static types have their value inline in the head.

    We detect which head slots are offsets (pointing into tail) vs inline values,
    decode the dynamic data, and compare with the ARC4 actual values.

    Returns True if they match, None if pattern not recognized.
    """
    n_actual = len(actual_list)
    if n_actual < 1 or n_actual > len(expected_list):
        return None

    head = expected_list[:n_actual]
    tail_start = n_actual  # word index where tail begins

    # Determine which head slots are dynamic offsets vs static values.
    # A head word is likely an offset if: it's an int, multiple of 32, and >= tail_start*32
    decoded = []
    for i in range(n_actual):
        v = head[i]
        is_offset = (isinstance(v, int) and v >= tail_start * 32 and v % 32 == 0
                     and v // 32 < len(expected_list))
        if is_offset:
            # Decode dynamic value at offset
            slot_idx = v // 32
            if slot_idx >= len(expected_list):
                return None
            length = expected_list[slot_idx]
            if not isinstance(length, int):
                return None
            data_start = slot_idx + 1
            if length == 0:
                decoded.append("")
            else:
                data = b""
                n_words = (length + 31) // 32
                for w in range(n_words):
                    if data_start + w >= len(expected_list):
                        break
                    val = expected_list[data_start + w]
                    if isinstance(val, bytes):
                        data += val
                    elif isinstance(val, int):
                        data += val.to_bytes(32, 'big')
                data = data[:length]
                try:
                    decoded.append(data.decode('utf-8', errors='replace'))
                except Exception:
                    decoded.append(data)
        else:
            # Static value — use as-is
            decoded.append(v)

    if len(decoded) != len(actual_list):
        return None
    for a, d in zip(actual_list, decoded):
        if not _compare_values(a, d):
            return None
    return True


def _compare_values(actual, expected):
    """Compare an actual AVM return value with an expected semantic test value."""
    if expected is None:
        return True  # void or FAILURE (already handled)
    if isinstance(expected, bool):
        return actual is expected
    if isinstance(expected, int):
        if isinstance(actual, bool):
            return (1 if actual else 0) == expected
        if isinstance(actual, int):
            return actual == expected
        if isinstance(actual, (list, tuple)):
            try:
                actual_bytes = bytes(actual)
                actual_int = int.from_bytes(actual_bytes, 'big')
                if actual_int == expected:
                    return True
                # EVM bytesN returns are right-padded to 32 bytes.
                # ARC4 returns raw N bytes. Right-pad actual to 32 and compare.
                if len(actual_bytes) < 32:
                    padded = actual_bytes + b'\x00' * (32 - len(actual_bytes))
                    if int.from_bytes(padded, 'big') == expected:
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
    if isinstance(expected, int) and isinstance(actual, str):
        # Address return: Algorand address string → decode to 32 bytes → int
        try:
            addr_bytes = encoding.decode_address(actual)
            addr_int = int.from_bytes(addr_bytes, 'big')
            if addr_int == expected:
                return True
        except Exception:
            pass
        return False
    if isinstance(actual, str) and isinstance(expected, str):
        return actual == expected
    return str(actual) == str(expected)


def run_test(test: SemanticTest, localnet, account, verbose=False):
    """Run a single semantic test. Returns (status, details)."""
    if test.skipped:
        return "SKIP", test.skip_reason

    # Compile
    out_dir = OUT_DIR / test.category / test.name
    contracts = compile_test(test.source_path, out_dir)
    if not contracts:
        return "COMPILE_ERROR", "compilation failed"

    # Find the main contract (usually the last/largest one)
    # Filter out non-deployable ones
    deployable = {k: v for k, v in contracts.items()
                  if v["approval_teal"].exists()}
    if not deployable:
        return "COMPILE_ERROR", "no deployable contracts"

    # Use the last contract defined in source (Solidity convention)
    contract_name = find_last_contract(test.source_path, deployable)
    artifacts = deployable[contract_name]

    # Load app spec for method resolution
    app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())

    # Extract constructor args and value if present
    ctor_args = None
    ctor_value = 0
    for call in test.calls:
        if call.method_name == "constructor":
            if call.args:
                ctor_args = call.args
            ctor_value = call.value_wei
            break

    # Deploy
    app = deploy_contract(localnet, account, artifacts,
                          ctor_args=ctor_args, fund_amount=ctor_value)
    if not app:
        return "DEPLOY_ERROR", "deployment failed"

    # Execute calls
    passed = 0
    failed = 0
    skipped = 0
    details = []

    for call in test.calls:
        # Skip constructor calls — handled during deployment
        if call.method_name == "constructor":
            continue
        ok, detail = execute_call(app, call, app_spec, verbose)
        if ok is None:
            skipped += 1
            if verbose:
                details.append(f"  SKIP: {call.raw_line} ({detail})")
        elif ok:
            passed += 1
            if verbose:
                details.append(f"  PASS: {call.raw_line}")
        else:
            failed += 1
            details.append(f"  FAIL: {call.raw_line} — {detail}")

    if failed > 0:
        return "FAIL", f"{passed}p/{failed}f/{skipped}s" + "\n".join([""] + details)
    return "PASS", f"{passed}p/{skipped}s"


def main():
    parser = argparse.ArgumentParser(description="Run Solidity semantic tests")
    parser.add_argument("--category", default=None, help="Specific category to run")
    parser.add_argument("--limit", type=int, default=None, help="Max tests per category")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--file", default=None, help="Run a specific .sol file")
    args = parser.parse_args()

    localnet, account = setup_localnet()

    if args.file:
        test = parse_test_file(Path(args.file))
        status, detail = run_test(test, localnet, account, args.verbose)
        print(f"{status}: {test.name} — {detail}")
        return

    # Discover categories
    if args.category:
        categories = [args.category]
    else:
        categories = sorted(d.name for d in TESTS_DIR.iterdir() if d.is_dir())

    results = {"PASS": 0, "FAIL": 0, "SKIP": 0, "COMPILE_ERROR": 0, "DEPLOY_ERROR": 0}

    for cat in categories:
        cat_dir = TESTS_DIR / cat
        if not cat_dir.exists():
            continue

        tests = sorted(cat_dir.glob("*.sol"))
        if args.limit:
            tests = tests[:args.limit]

        print(f"\n=== {cat} ({len(tests)} tests) ===")

        for sol_file in tests:
            test = parse_test_file(sol_file)
            try:
                status, detail = run_test(test, localnet, account, args.verbose)
            except Exception as e:
                status, detail = "COMPILE_ERROR", str(e)
            results[status] += 1

            icon = {"PASS": "✓", "FAIL": "✗", "SKIP": "○",
                    "COMPILE_ERROR": "⚠", "DEPLOY_ERROR": "⚠"}.get(status, "?")
            short_detail = detail.split("\n")[0][:60]
            print(f"  {icon} {test.name}: {short_detail}")

    print(f"\n{'='*50}")
    total = sum(results.values())
    print(f"Total: {total} tests")
    for status, count in sorted(results.items()):
        if count > 0:
            print(f"  {status}: {count}")


if __name__ == "__main__":
    main()
