"""
M37: ARC56 tuple return struct name collision (Gap 11 verification).
Tests that methods returning different named tuples get unique struct names
and that the contract functions work correctly.
"""

import json
from pathlib import Path

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, OUT_DIR


@pytest.fixture(scope="module")
def client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "TupleReturnsTest")


def test_arc56_unique_struct_names() -> None:
    """Each named-tuple return type should have a unique struct name."""
    arc56_path = OUT_DIR / "TupleReturnsTest" / "TupleReturnsTest.arc56.json"
    spec = json.loads(arc56_path.read_text())

    structs = spec.get("structs", {})

    # getPoint and getInfo should have distinct struct names
    assert "getPointReturn" in structs
    assert "getInfoReturn" in structs

    # getPointReturn has 2 fields
    assert len(structs["getPointReturn"]) == 2
    assert structs["getPointReturn"][0]["name"] == "px"
    assert structs["getPointReturn"][1]["name"] == "py"

    # getInfoReturn has 3 fields
    assert len(structs["getInfoReturn"]) == 3
    assert structs["getInfoReturn"][0]["name"] == "value"
    assert structs["getInfoReturn"][1]["name"] == "count"
    assert structs["getInfoReturn"][2]["name"] == "active"

    # Methods reference correct struct names
    methods = {m["name"]: m for m in spec["methods"]}
    assert methods["getPoint"]["returns"].get("struct") == "getPointReturn"
    assert methods["getInfo"]["returns"].get("struct") == "getInfoReturn"

    # Unnamed tuple should not have struct
    assert "struct" not in methods["getPair"]["returns"] or methods["getPair"]["returns"]["struct"] == ""


@pytest.mark.localnet
def test_get_point(client: au.AppClient) -> None:
    result = client.send.call(
        au.AppClientMethodCallParams(method="getPoint", args=[10, 20])
    )
    # Named tuple returns come back as dicts with field names
    assert result.abi_return == {"px": 10, "py": 20}


@pytest.mark.localnet
def test_get_info(client: au.AppClient) -> None:
    result = client.send.call(
        au.AppClientMethodCallParams(method="getInfo", args=[5])
    )
    # value=5*2=10, count=5+1=6, active=5>0=true
    assert result.abi_return == {"value": 10, "count": 6, "active": True}


@pytest.mark.localnet
def test_get_info_zero(client: au.AppClient) -> None:
    result = client.send.call(
        au.AppClientMethodCallParams(method="getInfo", args=[0])
    )
    # value=0, count=1, active=false (0 > 0 is false)
    assert result.abi_return == {"value": 0, "count": 1, "active": False}


@pytest.mark.localnet
def test_get_pair(client: au.AppClient) -> None:
    result = client.send.call(
        au.AppClientMethodCallParams(method="getPair", args=[3, 4])
    )
    assert result.abi_return == [7, 12]


@pytest.mark.localnet
def test_get_sum(client: au.AppClient) -> None:
    result = client.send.call(
        au.AppClientMethodCallParams(method="getSum", args=[100, 200])
    )
    assert result.abi_return == 300
