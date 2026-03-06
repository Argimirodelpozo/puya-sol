"""
AAVE V4 SignatureGateway tests.
Compilation-only test: constructor parameter passing via inheritance
chain doesn't read from ApplicationArgs (compiler limitation).
"""

import pytest
from pathlib import Path

OUT_DIR = Path(__file__).parent.parent / "out"


def test_compilation():
    """Verify SignatureGateway compiled to TEAL."""
    teal = OUT_DIR / "SignatureGateway" / "SignatureGateway.approval.teal"
    assert teal.exists()
    content = teal.read_text()
    assert len(content.splitlines()) > 100


def test_arc56_spec():
    """Verify ARC56 spec was generated."""
    arc56 = OUT_DIR / "SignatureGateway" / "SignatureGateway.arc56.json"
    assert arc56.exists()
    import json
    spec = json.loads(arc56.read_text())
    methods = [m["name"] for m in spec["methods"]]
    assert "owner" in methods
    assert "supplyWithSig" in methods
