"""
AAVE V4 UnitPriceFeed tests.
Translated from UnitPriceFeed.t.sol (Foundry).
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def feed(localnet, account):
    # constructor(uint8 decimals_, string memory description_)
    # decimals=8 means UNITS = 10^8 (answer = 1e8 = 1.0 with 8 decimals)
    decimals = 8
    description = b"Unit price feed"
    return deploy_contract(
        localnet, account, "UnitPriceFeed",
        app_args=[decimals.to_bytes(8, "big"), description],
    )


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


def test_deploy(feed):
    assert feed.app_id > 0


def test_version(feed):
    # UnitPriceFeed always returns version 1
    assert _call(feed, "version") == 1


def test_decimals(feed):
    assert _call(feed, "decimals") == 8


def test_description(feed):
    result = _call(feed, "description")
    assert result == "Unit price feed"


def test_latestRoundData(feed):
    """latestRoundData: roundId=block.timestamp, answer=10^decimals, answeredInRound=roundId."""
    result = _call(feed, "latestRoundData")
    vals = list(result.values()) if isinstance(result, dict) else list(result)
    roundId = vals[0]
    answer = vals[1]
    answeredInRound = vals[4]
    assert roundId > 0  # block.timestamp
    assert answer == 10**8  # 1e8 with 8 decimals
    assert answeredInRound == roundId


def test_getRoundData(feed):
    """getRoundData should return same answer for any roundId."""
    result = _call(feed, "getRoundData", 1)
    vals = list(result.values()) if isinstance(result, dict) else list(result)
    assert vals[1] == 10**8  # answer

    # Any other round ID should return the same
    result2 = _call(feed, "getRoundData", 42)
    vals2 = list(result2.values()) if isinstance(result2, dict) else list(result2)
    assert vals2[1] == 10**8
