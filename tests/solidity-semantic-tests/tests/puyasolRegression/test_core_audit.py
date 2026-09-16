"""Constructor trust boundaries and scalar adapters; solc is the value oracle."""

import pytest

from framework.deploy import DeployError
from framework.paths import TESTS_DIR
from test_ast_audit import compile_app
from test_call_operands import invoke


def word(value, width=32):
    return (value % (1 << (8 * width))).to_bytes(width, "big")


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("width", [8, 32])
def test_constructor_create_scalars(harness, via_ir, profile, width):
    artifacts = compile_app(harness, "core_constructor_ingress", via_ir, profile, False)

    def deploy(name, values):
        return harness.deploy(artifacts, name, ctor_args=values if profile == "arc4" else [b"".join(values)])

    carrier = width if profile == "arc4" else 32
    app = deploy("ScalarIngress", [word(v, carrier) for v in (7, 1, 1)])
    assert invoke(harness, app, profile, "seen()") == (1107,)
    for values in ((256, 0, 0), (0, 2, 0), (0, 0, 2), (1 << 63, 0, 0)):
        with pytest.raises(DeployError, match="create txn failed"):
            deploy("ScalarIngress", [word(v, carrier) for v in values])
    if carrier == 32:
        with pytest.raises(DeployError, match="create txn failed"):
            deploy("ScalarIngress", [word((1 << 64) + 7), word(0), word(0)])
    for small, wide in ((-128, -(1 << 95)), (-1, -1), (127, (1 << 95) - 1)):
        app = deploy("SignedIngress", [word(small, carrier), word(wide)])
        # ARC4 signed return values use uint256 two's-complement carriers.
        expected = (small, wide) if profile == "evm" else (small % (1 << 256), wide % (1 << 256))
        assert invoke(harness, app, profile, "small()", returns=["int256"]) == (expected[0],)
        assert invoke(harness, app, profile, "wide()", returns=["int256"]) == (expected[1],)
    for small, wide in ((128, 0), (255, 0), (0, 1 << 95), (0, (1 << 96) - 1)):
        with pytest.raises(DeployError, match="create txn failed"):
            deploy("SignedIngress", [word(small, carrier), word(wide)])
    app = deploy("UnsignedIngress", [word((1 << 96) - 1, 12 if profile == "arc4" else 32)])
    assert invoke(harness, app, profile, "seen()") == ((1 << 96) - 1,)
    with pytest.raises(DeployError, match="create txn failed"):
        deploy("UnsignedIngress", [word(1 << 96)])


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_constructor_deferred_scalars(harness, via_ir, profile):
    artifacts = compile_app(harness, "core_constructor_ingress", via_ir, profile, True)
    app = harness.deploy(artifacts, "ScalarIngress", skip_postinit=True)
    method = next(m for m in app.app_spec.methods if m.name == "__postInit").to_abi_method()
    for number, choice in ((256, 0), (0, 2), (1 << 63, 0)):
        result = harness.call_raw(app, method.get_selector(),
                                  extra_args=[word(number, 8), b"\x00", word(choice, 8)],
                                  expect_revert=True, budget_pool=8)
        assert result.reverted
    assert not harness.call(app, "__postInit", 7, True, 1).reverted
    assert invoke(harness, app, profile, "seen()") == (1107,)

    app = harness.deploy(artifacts, "SignedIngress", skip_postinit=True)
    method = next(m for m in app.app_spec.methods if m.name == "__postInit").to_abi_method()
    for small, wide in ((128, 0), (0, 1 << 95), (0, 1 << 256), (0, (1 << 512) - 1)):
        result = harness.call_raw(app, method.get_selector(),
                                  extra_args=[word(small, 8), word(wide, 64)],
                                  expect_revert=True, budget_pool=8)
        assert result.reverted
    assert not harness.call(app, "__postInit", (1 << 64) - 1, (1 << 256) - 1).reverted
    expected = -1 if profile == "evm" else (1 << 256) - 1
    assert invoke(harness, app, profile, "small()", returns=["int256"]) == (expected,)
    assert invoke(harness, app, profile, "wide()", returns=["int256"]) == (expected,)

    app = harness.deploy(artifacts, "UnsignedIngress", skip_postinit=True)
    method = next(m for m in app.app_spec.methods if m.name == "__postInit").to_abi_method()
    for dirty in (word(1 << 96), word(1 << 256, 64)):
        result = harness.call_raw(app, method.get_selector(), extra_args=[dirty],
                                  expect_revert=True, budget_pool=8)
        assert result.reverted
    assert not harness.call(app, "__postInit", (1 << 96) - 1).reverted
    assert invoke(harness, app, profile, "seen()") == ((1 << 96) - 1,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_native_constructor_uses_solc_abi_coder(harness, tmp_path, via_ir, slot):
    source = tmp_path / "V1.sol"
    source.write_text((TESTS_DIR / "puyasolRegression/contracts/core_constructor_ingress.sol").read_text()
                      .replace("pragma solidity ^0.8.20;", "pragma solidity ^0.8.20; pragma abicoder v1;"))
    artifacts = harness.compile(source, via_yul_behavior=via_ir,
                                extra_args=["--evm-storage-layout"] if slot else [])
    if slot:
        app = harness.deploy(artifacts, "ScalarIngress", skip_postinit=True)
        method = next(m for m in app.app_spec.methods if m.name == "__postInit").to_abi_method()
        result = harness.call_raw(app, method.get_selector(), extra_args=[word(263, 8), b"\x80", word(1, 8)],
                                  expect_revert=via_ir, budget_pool=8)
        assert result.reverted == via_ir
        if not via_ir:
            assert invoke(harness, app, "arc4", "seen()") == (1107,)
    elif via_ir:
        with pytest.raises(DeployError, match="create txn failed"):
            harness.deploy(artifacts, "ScalarIngress", ctor_args=[word(263), word(2), word(1)])
    else:
        app = harness.deploy(artifacts, "ScalarIngress", ctor_args=[word(263), word(2), word(1)])
        assert invoke(harness, app, "arc4", "seen()") == (1107,)
        app = harness.deploy(artifacts, "SignedIngress", ctor_args=[word(255), word((1 << 96) - 1)])
        for getter in ("small()", "wide()"):
            assert invoke(harness, app, "arc4", getter, returns=["int256"]) == ((1 << 256) - 1,)
