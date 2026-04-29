"""
AAVE V4 TokenizationSpokeInstance tests.

Note: TokenizationSpokeInstance constructor makes inner app calls to the hub
contract (getAssetCount, getAssetUnderlyingAndDecimals), so it requires a
deployed hub. We verify compilation only — the TEAL was produced successfully.
"""

import pytest
from pathlib import Path


OUT_DIR = Path(__file__).parent.parent / "out"


def test_compiled():
    """Verify TEAL files were produced by the compiler."""
    name = "TokenizationSpokeInstance"
    approval = OUT_DIR / name / f"{name}.approval.teal"
    clear = OUT_DIR / name / f"{name}.clear.teal"
    arc56 = OUT_DIR / name / f"{name}.arc56.json"
    assert approval.exists(), "approval.teal not found"
    assert clear.exists(), "clear.teal not found"
    assert arc56.exists(), "arc56.json not found"
    # Verify non-trivial output
    assert approval.stat().st_size > 1000
