"""Storage-array push when __postInit never ran.

A proxy-runtime replay deploys the implementation and replays the proxy's
initialize() from history, so the deferred constructor is skipped — and with
it the eager box_create for every top-level dynamic array. The resize has to
materialise its own root box; without that the read asserts "box exists"
(seen on Privacy Pools' Entrypoint.updateRoot).
"""
import pytest

SOURCE = "puyasolRegression/contracts/deferred_ctor_array_push.sol"
CID = "Qm" + "a" * 44          # 46 bytes: inside the real contract's 32..64 bound


@pytest.fixture
def deferred(harness):
    artifacts = harness.compile(SOURCE)
    return harness.deploy(artifacts, "DeferredCtorPush", skip_postinit=True)


def _ok(harness, app, sig, *args):
    r = harness.call(app, sig, *args, extra_fee=20_000)
    assert not r.reverted, f"{sig}{args}: {r.fail_message}"
    return r.abi_return


def test_struct_array_push_creates_its_box(harness, deferred):
    assert _ok(harness, deferred, "updateRoot(uint256,string)", 7, CID) == 0
    assert _ok(harness, deferred, "latestRoot()") == 7


def test_value_array_push_creates_its_box(harness, deferred):
    _ok(harness, deferred, "pushNum(uint256)", 41)
    _ok(harness, deferred, "pushNum(uint256)", 42)
    assert _ok(harness, deferred, "numsLength()") == 2


def test_push_still_works_after_normal_deploy(harness):
    app = harness.compile_and_deploy(SOURCE, "DeferredCtorPush")
    assert _ok(harness, app, "updateRoot(uint256,string)", 9, CID) == 0
    assert _ok(harness, app, "latestRoot()") == 9
