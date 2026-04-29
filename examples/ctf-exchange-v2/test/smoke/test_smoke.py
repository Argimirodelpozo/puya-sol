"""Smoke tests: each v2 contract deploys, runs __postInit, exposes ABI calls."""
import pytest


def test_collateral_onramp_deploys(collateral_onramp):
    assert collateral_onramp.app_id > 0


def test_collateral_offramp_deploys(collateral_offramp):
    assert collateral_offramp.app_id > 0


def test_permissioned_ramp_deploys(permissioned_ramp):
    assert permissioned_ramp.app_id > 0


def test_ctf_adapter_deploys(ctf_adapter):
    assert ctf_adapter.app_id > 0


def test_collateral_token_deploys(collateral_token):
    assert collateral_token.app_id > 0


@pytest.mark.skip(
    reason="NegRiskAdapter constructor calls `INegRiskAdapter(_negRiskAdapter).wcol()` "
           "which fires an inner-tx whose ApplicationID is computed from the wrong "
           "stack slot — same `dig N` stack-depth bug as the with-callback wrap/unwrap "
           "path in CollateralToken. SafeTransferLib was already AVM-ported on these "
           "adapters, but this constructor pattern hits a separate puya-sol lowering "
           "bug for inner-tx target resolution that would need a compiler-side fix.")
def test_negrisk_adapter_deploys(negrisk_adapter):
    assert negrisk_adapter.app_id > 0
