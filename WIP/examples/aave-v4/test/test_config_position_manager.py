"""
AAVE V4 ConfigPositionManager tests.
Compilation-only test: constructor parameter passing via inheritance
chain doesn't read from ApplicationArgs (compiler limitation).
"""

import pytest
from pathlib import Path

OUT_DIR = Path(__file__).parent.parent / "out"


def test_compilation():
    """Verify ConfigPositionManager compiled to TEAL."""
    teal = OUT_DIR / "ConfigPositionManager" / "ConfigPositionManager.approval.teal"
    assert teal.exists(), "TEAL file not found"
    content = teal.read_text()
    assert len(content.splitlines()) > 100, "TEAL too short"


def test_arc56_spec():
    """Verify ARC56 spec was generated."""
    arc56 = OUT_DIR / "ConfigPositionManager" / "ConfigPositionManager.arc56.json"
    assert arc56.exists()
    import json
    spec = json.loads(arc56.read_text())
    methods = [m["name"] for m in spec["methods"]]
    assert "owner" in methods
    assert "setGlobalPermission" in methods
