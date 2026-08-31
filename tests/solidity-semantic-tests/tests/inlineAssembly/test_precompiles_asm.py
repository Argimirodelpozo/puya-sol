"""Raw asm-staticcall precompiles — coverage lane for PrecompileHandlers.cpp.

The high-level builtins (ecrecover, sha256 via Solidity) have their own
lanes; this one drives the ASM staticcall shapes for every implemented
precompile: 0x1 ecRecover, 0x2 SHA-256, 0x4 identity, 0x5 modExp,
0x6 ecAdd, 0x7 ecMul, 0x8 ecPairing.
"""

import hashlib

from framework import as_int

# Upstream's known-good ecrecover vector (ecrecover.sol fixture).
H = bytes.fromhex(
    "18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c")
R = bytes.fromhex(
    "73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f")
S = bytes.fromhex(
    "eeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549")
SIGNER = 0xA94F5374FCE5EDBC8E2A8697C15331677E6EBF0B


def test_precompiles_asm(harness):
    """inlineAssembly/contracts/precompiles_asm.sol"""
    app = harness.compile_and_deploy(
        "inlineAssembly/contracts/precompiles_asm.sol",
        # ec ops and pk-recover exceed the 700/txn budget on their own.
        ensure_budget={"sha": 3_000, "idcopy": 3_000, "modexp": 4_000,
                       "recover": 6_000, "ecAddInverse": 6_000,
                       "ecMulOne": 8_000, "pairEmpty": 12_000},
    )
    fee = {"extra_fee": 20_000}

    payload = b"puya-sol precompile lane"
    got = bytes(harness.call(app, "sha(bytes)", payload, **fee).abi_return)
    assert got == hashlib.sha256(payload).digest()

    word = bytes(range(32))
    got = bytes(harness.call(app, "idcopy(bytes32)", word, **fee).abi_return)
    assert got == word

    assert as_int(harness.call(
        app, "modexp(uint256,uint256,uint256)", 3, 5, 7, **fee
    ).abi_return) == pow(3, 5, 7)

    got = as_int(harness.call(
        app, "recover(bytes32,uint256,bytes32,bytes32)", H, 28, R, S, **fee
    ).abi_return)
    assert got == SIGNER

    x, y = (as_int(v) for v in
            harness.call(app, "ecAddInverse()", **fee).abi_return)
    assert (x, y) == (0, 0)

    x, y = (as_int(v) for v in
            harness.call(app, "ecMulOne()", **fee).abi_return)
    assert (x, y) == (1, 2)

    assert as_int(harness.call(app, "pairEmpty()", **fee).abi_return) == 1
