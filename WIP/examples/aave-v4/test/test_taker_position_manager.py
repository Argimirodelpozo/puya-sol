"""
AAVE V4 TakerPositionManager tests.
Compilation-only test: constructor parameter passing via inheritance
chain doesn't read from ApplicationArgs (compiler limitation).
"""

import pytest
from pathlib import Path

OUT_DIR = Path(__file__).parent.parent / "out"


def test_compilation():
    """Verify TakerPositionManager compiled to TEAL."""
    teal = OUT_DIR / "TakerPositionManager" / "TakerPositionManager.approval.teal"
    assert teal.exists()
    content = teal.read_text()
    assert len(content.splitlines()) > 100


def test_arc56_spec():
    """Verify ARC56 spec was generated."""
    arc56 = OUT_DIR / "TakerPositionManager" / "TakerPositionManager.arc56.json"
    assert arc56.exists()
    import json
    spec = json.loads(arc56.read_text())
    methods = [m["name"] for m in spec["methods"]]
    assert "owner" in methods
    assert "withdrawOnBehalfOf" in methods
