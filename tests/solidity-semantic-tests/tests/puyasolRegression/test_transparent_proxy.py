"""Transparent proxy (proxy.md §2).

The routing rule transparent proxies exist for is moot on the AVM (the admin
surface is a transaction TYPE, not a selector), so the lowering is §1's; the
compile-side gap was that units CONTAINING the OZ proxy contracts died on the
fallback's asm delegatecall. OZ Proxy._delegate now folds to an honest trap,
so every contract in the trio compiles; the implementation runs normally.
"""

from framework import as_int


def test_transparent_proxy_unit_compiles_and_impl_runs(harness):
    """puyasolRegression/contracts/transparent_proxy_unit.sol — flattened
    OZ v5 transparent closure (Proxy/ERC1967Proxy/TransparentUpgradeableProxy/
    ProxyAdmin) + implementation + factory, all in one unit."""
    artifacts = harness.compile(
        "puyasolRegression/contracts/transparent_proxy_unit.sol")
    # Every contract of the trio must have compiled to a deployable artifact.
    for name in ("TranspImpl", "TranspFactory", "ProxyAdmin",
                 "TransparentUpgradeableProxy", "ERC1967Proxy"):
        assert name in artifacts.by_contract, name

    # The implementation is an ordinary contract and runs.
    app = harness.deploy(artifacts, "TranspImpl")
    harness.call(app, "setValue(uint256)", 41)
    assert as_int(harness.call(app, "value()").abi_return) == 41

    # Deploying a proxy at runtime (factory path) fails LOUD with the
    # doctrine trap, not silently: the ctor's ERC1967Utils.upgradeToAndCall
    # fold rejects the in-contract upgrade path.
    factory = harness.deploy(artifacts, "TranspFactory")
    r = harness.call(
        factory, "deployProxy(address,address)",
        harness.localnet.account.address, harness.localnet.account.address,
        extra_fee=20_000, expect_revert=True)
    assert r.reverted
