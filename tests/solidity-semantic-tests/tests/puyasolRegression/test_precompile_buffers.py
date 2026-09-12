"""Precompile buffers are checked against solc/PyEVM, separately from scratch memory."""
import hashlib
import pytest
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_precompile_buffers(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/precompile_buffers.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    def check(signature, args=(), count=2):
        return invoke(harness, app, profile, signature, args, ["uint256"] * count)
    for size in (0, 1, 31, 32, 33):
        for window in (0, 1, 31, 32, 64):
            copied = b"\xff" * min(size, window, 32)
            complete = b"\xff" * min(size, 32)
            assert check("identity(uint256,uint256)", [size, window], 3) == (
                size, int.from_bytes(copied.ljust(32, b"\0"), "big"),
                int.from_bytes(complete.ljust(32, b"\0"), "big"))
    for size in (0, 3, 32):
        digest = hashlib.sha256(b"\xff" * size).digest()
        for window in (0, 1, 31, 32, 64):
            assert check("sha(uint256,uint256)", [size, window], 3) == (
                32, int.from_bytes(digest[:window].ljust(32, b"\xff"), "big"), int.from_bytes(digest, "big"))
    for off, size in ((1024, 0), (1024, 31), (4090, 128)):
        assert check("invalidRecovery(uint256,uint256)", [off, size]) == (0, 123)
    for size in (0, 192, 384):
        assert check("pairing(uint256)", [size]) == (32, 1)
    invoke(harness, app, profile, "pairing(uint256)", [191], reverts=True)
    for base, exponent, modulus in ((7, 0, 0), (7, 0, 1), (7, 0, 13), (7, 3, 13)):
        assert check("modexp(uint256,uint256,uint256)", [base, exponent, modulus]) == (
            32, pow(base, exponent, modulus) if modulus else 0)
    assert check("callOrder()", count=3) == (65431, 32, 0)
