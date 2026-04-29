"""
M16: Inter-app call tests.
Exercises: calling another deployed contract through an interface,
inner application transactions, return value extraction from LastLog,
chained inter-app calls, bool returns from inner calls.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

# Inner transactions need fee pooling: extra 1000 µALGO per inner txn
INNER_FEE = au.AlgoAmount(micro_algo=1000)
DOUBLE_INNER_FEE = au.AlgoAmount(micro_algo=2000)


@pytest.fixture(scope="module")
def calculator_app_id(
    localnet: au.AlgorandClient, account: SigningAccount
) -> int:
    """Deploy the Calculator contract and return its app ID."""
    client = deploy_contract(localnet, account, "Calculator")
    return client.app_id


@pytest.fixture(scope="module")
def caller_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    calculator_app_id: int,
) -> au.AppClient:
    """Deploy InterAppCallTest with the Calculator's app ID as constructor arg."""
    # The constructor stores ICalculator(_calculator) which is an address.
    # We pass the app ID as a 32-byte big-endian number so that
    # extract(24,8) + btoi gives the uint64 app ID.
    calc_id_bytes = calculator_app_id.to_bytes(32, "big")
    client = deploy_contract(
        localnet, account, "InterAppCallTest",
        app_args=[calc_id_bytes],
    )
    return client


# --- Basic inter-app calls ---


@pytest.mark.localnet
def test_call_add(
    caller_client: au.AppClient, calculator_app_id: int
) -> None:
    """callAdd(3, 5) should return 8 via inter-app call."""
    result = caller_client.send.call(
        au.AppClientMethodCallParams(
            method="callAdd",
            args=[3, 5],
            app_references=[calculator_app_id],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return == 8


@pytest.mark.localnet
def test_call_add_large(
    caller_client: au.AppClient, calculator_app_id: int
) -> None:
    """callAdd with larger numbers."""
    result = caller_client.send.call(
        au.AppClientMethodCallParams(
            method="callAdd",
            args=[1000000, 2000000],
            app_references=[calculator_app_id],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return == 3000000


@pytest.mark.localnet
def test_call_multiply(
    caller_client: au.AppClient, calculator_app_id: int
) -> None:
    """callMultiply(7, 6) should return 42."""
    result = caller_client.send.call(
        au.AppClientMethodCallParams(
            method="callMultiply",
            args=[7, 6],
            app_references=[calculator_app_id],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return == 42


@pytest.mark.localnet
def test_call_is_positive_true(
    caller_client: au.AppClient, calculator_app_id: int
) -> None:
    """callIsPositive(42) should return True."""
    result = caller_client.send.call(
        au.AppClientMethodCallParams(
            method="callIsPositive",
            args=[42],
            app_references=[calculator_app_id],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_call_is_positive_false(
    caller_client: au.AppClient, calculator_app_id: int
) -> None:
    """callIsPositive(0) should return False."""
    result = caller_client.send.call(
        au.AppClientMethodCallParams(
            method="callIsPositive",
            args=[0],
            app_references=[calculator_app_id],
            extra_fee=INNER_FEE,
        )
    )
    assert result.abi_return is False


# --- Chained inter-app calls ---


@pytest.mark.localnet
def test_call_add_then_multiply(
    caller_client: au.AppClient, calculator_app_id: int
) -> None:
    """callAddThenMultiply(2, 3, 4) should return (2+3)*4 = 20."""
    result = caller_client.send.call(
        au.AppClientMethodCallParams(
            method="callAddThenMultiply",
            args=[2, 3, 4],
            app_references=[calculator_app_id],
            extra_fee=DOUBLE_INNER_FEE,  # Two inner calls
        )
    )
    assert result.abi_return == 20


@pytest.mark.localnet
def test_call_add_then_multiply_large(
    caller_client: au.AppClient, calculator_app_id: int
) -> None:
    """callAddThenMultiply(100, 200, 3) should return (100+200)*3 = 900."""
    result = caller_client.send.call(
        au.AppClientMethodCallParams(
            method="callAddThenMultiply",
            args=[100, 200, 3],
            app_references=[calculator_app_id],
            extra_fee=DOUBLE_INNER_FEE,
        )
    )
    assert result.abi_return == 900
