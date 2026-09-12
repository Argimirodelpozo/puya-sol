"""Dynamic inputs exercise the shared runtime helper, including empty padding."""

import pytest


# Independent PyCryptodome RIPEMD-160 reference; payload byte i = (37*i+11)%256.
VECTORS = {
    0: "9c1185a5c5e9fc54612808977ee8f548b2258d31",
    1: "e257e7861ba1b82bdcff75ff2bb1613d83b797bd",
    55: "cb9a91b689c2e5a5aff392733f33c8fb75998dbb",
    56: "0fde8285ccc694b28a3205e30e3153c3f2a2dfa2",
    63: "28d9c6a82ecf3196a0c5ab9cd79053118ca43301",
    64: "c9b1871a7c604823d7cc8cc48e4ea37a4f841001",
    119: "3e51052aaeaf2b4b120980b0c3c79f26301ff2f9",
    120: "0268020acd3f91628e14766d7791dbf683bff67d",
    127: "24ba60dff396cf212ad4c3969eb1e281f044a7b0",
    128: "2349a24ad9bd73b09329b1c014c333610548ac55",
}


def payload(length):
    return bytes((37 * i + 11) % 256 for i in range(length))


@pytest.mark.parametrize("optimization", [1, 2])
def test_ripemd_dynamic_boundaries_and_repeated_calls(harness, optimization):
    app = harness.compile_and_deploy(
        "builtinFunctions/contracts/ripemd160_boundaries.sol",
        ensure_budget={"hash": 110000, "twice": 110000},
        extra_args=["--optimization-level", str(optimization)],
    )
    for length, expected in VECTORS.items():
        result = harness.call(app, "hash(bytes)", payload(length), extra_fee=180000)
        assert bytes(result.abi_return).hex() == expected, length
    for a, b in [(0, 55), (56, 1), (63, 64)]:
        result = harness.call(app, "twice(bytes,bytes)", payload(a), payload(b), extra_fee=180000)
        assert tuple(bytes(x).hex() for x in result.abi_return) == (VECTORS[a], VECTORS[b])


def test_ripemd_emitted_padding_respects_avm_byte_limit(tmp_path):
    """Interpret emitted padding/selection for every legal byte length.

    This isolates representation bounds from the full hash's opcode budget.
    The actual hash algorithm is exercised on LocalNet separately above.
    """
    import base64
    import json
    import operator
    import subprocess
    from framework.paths import COMPILER, TESTS_DIR

    result = subprocess.run([str(COMPILER), "--source", str(TESTS_DIR /
                            "builtinFunctions/contracts/ripemd160_boundaries.sol"),
                            "--no-puya", "--output-dir", str(tmp_path)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    roots = json.loads((tmp_path / "awst.json").read_text())
    helper = next(root for root in roots if root.get("id") == "__builtin_ripemd160")
    assert helper["inline"] is False
    body = helper["body"]["body"]
    index = next(i for i, node in enumerate(body) if node["_type"] == "WhileLoop")
    select_chunk = body[index]["loop_body"]["body"][0]
    assert select_chunk["target"]["name"] == "chunk"
    binary = {"+": operator.add, "-": operator.sub, "*": operator.mul, "%": operator.mod,
              "<<": operator.lshift, ">>": operator.rshift, "&": operator.and_}
    compare = {"<": operator.lt, "==": operator.eq}

    def evaluate(node, values):
        kind = node["_type"]
        if kind == "IntegerConstant":
            value = int(node["value"])
        elif kind == "BytesConstant":
            value = base64.b85decode(node["value"])
        elif kind == "VarExpression":
            value = values[node["name"]]
        elif kind == "UInt64BinaryOperation":
            value = binary[node["op"]](evaluate(node["left"], values), evaluate(node["right"], values))
        elif kind == "NumericComparisonExpression":
            value = compare[node["operator"]](evaluate(node["lhs"], values), evaluate(node["rhs"], values))
        elif kind == "ConditionalExpression":
            value = evaluate(node["true_expr"] if evaluate(node["condition"], values) else node["false_expr"], values)
        elif kind == "IntrinsicCall":
            args = [evaluate(arg, values) for arg in node["stack_args"]]
            opcode = node["op_code"]
            if opcode == "len":
                value = len(args[0])
            elif opcode == "concat":
                value = args[0] + args[1]
            elif opcode == "bzero":
                assert 0 <= args[0] <= 4096
                value = bytes(args[0])
            elif opcode == "setbyte":
                data, offset, byte = args
                assert 0 <= offset < len(data) and 0 <= byte <= 255
                value = data[:offset] + bytes([byte]) + data[offset + 1:]
            elif opcode == "extract3":
                data, offset, count = args
                assert 0 <= offset <= offset + count <= len(data)
                value = data[offset:offset + count]
            else:
                raise AssertionError(opcode)
        else:
            raise AssertionError(kind)
        if isinstance(value, bytes):
            assert len(value) <= 4096, (kind, len(value))
        else:
            assert 0 <= value < 2**64, (kind, value)
        return value

    def assign(node, values):
        assert node["_type"] == "AssignmentStatement"
        values[node["target"]["name"]] = evaluate(node["value"], values)

    data = payload(4096)
    for length in range(4097):
        values = {"data": data[:length]}
        for node in body[:index]:
            assign(node, values)
        assert len(values["tail"]) in (64, 128)
        expected = data[:length] + b"\x80" + bytes((55 - length) % 64) + (length * 8).to_bytes(8, "little")
        assert values["padLen"] == len(expected)
        for position in range(0, len(expected), 64):
            values["pos"] = position
            assign(select_chunk, values)
            assert values["chunk"] == expected[position:position + 64], (length, position)
