"""Regression tests for inner app calls (cross-contract calls via interfaces)."""
import os
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy, app_id_to_algod_addr, NO_POPULATE

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "InnerCalls.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture
def counter(compiled, localnet, account):
    c = compiled["Counter"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


@pytest.fixture
def oracle(compiled, localnet, account):
    c = compiled["Oracle"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


@pytest.fixture
def caller(compiled, localnet, account, counter, oracle):
    c = compiled["Caller"]
    app = deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])
    # Wire up addresses
    app.send.call(au.AppClientMethodCallParams(
        method="setCounter", args=[app_id_to_algod_addr(counter.app_id)],
    ))
    app.send.call(au.AppClientMethodCallParams(
        method="setOracle", args=[app_id_to_algod_addr(oracle.app_id)],
    ))
    return app


class _Result:
    """Wrapper to extract abi_return from grouped transaction result."""
    def __init__(self, raw):
        self._raw = raw
    @property
    def abi_return(self):
        # The main call is the last in the group
        returns = self._raw.returns
        if returns:
            return returns[-1].value
        return None

def call_with_budget(localnet, app, params, budget_calls=2):
    composer = localnet.new_group()
    for _ in range(budget_calls):
        composer.add_app_call_method_call(app.params.call(
            au.AppClientMethodCallParams(method="counterAddr", note=os.urandom(8))
        ))
    composer.add_app_call_method_call(app.params.call(params))
    return _Result(composer.send(NO_POPULATE))


class TestReadInnerCall:
    def test_read_counter_initial(self, caller, counter, localnet):
        """Read uint256 from Counter via interface call."""
        r = call_with_budget(localnet, caller,
            au.AppClientMethodCallParams(
                method="readCounter",
                app_references=[counter.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        assert r.abi_return == 0

    def test_read_oracle_price(self, caller, oracle, localnet):
        """Read price from Oracle after setting it."""
        oracle.send.call(au.AppClientMethodCallParams(method="setPrice", args=[42000]))
        r = call_with_budget(localnet, caller,
            au.AppClientMethodCallParams(
                method="readPrice",
                app_references=[oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        assert r.abi_return == 42000

    def test_read_bool_return(self, caller, oracle, localnet):
        """Inner call returning bool."""
        r = call_with_budget(localnet, caller,
            au.AppClientMethodCallParams(
                method="checkIsOracle",
                app_references=[oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        assert r.abi_return is True


class TestWriteInnerCall:
    def test_increment(self, caller, counter, localnet):
        """Write to Counter via interface — increment."""
        call_with_budget(localnet, caller,
            au.AppClientMethodCallParams(
                method="callIncrement",
                app_references=[counter.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        # Verify directly on Counter
        r = counter.send.call(au.AppClientMethodCallParams(method="getValue"))
        assert r.abi_return == 1

    def test_add_with_argument(self, caller, counter, localnet):
        """Write with uint256 argument."""
        call_with_budget(localnet, caller,
            au.AppClientMethodCallParams(
                method="callAdd", args=[10],
                app_references=[counter.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        r = counter.send.call(au.AppClientMethodCallParams(method="getValue"))
        assert r.abi_return == 10


class TestMultipleInnerCalls:
    def test_increment_twice(self, caller, counter, localnet):
        """Two inner calls in one transaction."""
        call_with_budget(localnet, caller,
            au.AppClientMethodCallParams(
                method="incrementTwice",
                app_references=[counter.app_id],
                extra_fee=au.AlgoAmount(micro_algo=4000),
            ),
            budget_calls=3,
        )
        r = counter.send.call(au.AppClientMethodCallParams(method="getValue"))
        assert r.abi_return == 2


class TestInnerCallWithComputation:
    def test_double_price(self, caller, oracle, localnet):
        """Inner call + local computation."""
        oracle.send.call(au.AppClientMethodCallParams(method="setPrice", args=[500]))
        r = call_with_budget(localnet, caller,
            au.AppClientMethodCallParams(
                method="doublePrice",
                app_references=[oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        assert r.abi_return == 1000
