"""Regression tests for abstract contracts, virtual/override, inheritance with constructor args."""
from pathlib import Path
import pytest
import algokit_utils as au
from conftest import compile_sol, deploy

FEATURE_DIR = Path(__file__).parent
SOL_FILE = FEATURE_DIR / "contracts" / "AbstractContracts.sol"
OUT_DIR = FEATURE_DIR / "out"
pytestmark = pytest.mark.localnet


@pytest.fixture(scope="module")
def compiled():
    return compile_sol(SOL_FILE, OUT_DIR)


@pytest.fixture(scope="module")
def dog(compiled, localnet, account):
    c = compiled["Dog"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


@pytest.fixture(scope="module")
def named_dog(compiled, localnet, account):
    c = compiled["NamedDog"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


@pytest.fixture(scope="module")
def vault(compiled, localnet, account):
    c = compiled["OwnedVault"]
    return deploy(localnet, account, c["arc56"], c["approval_teal"], c["clear_teal"])


class TestSingleAbstract:
    def test_abstract_not_compiled(self, compiled):
        """Abstract contracts should not produce deployable artifacts."""
        assert "Animal" not in compiled

    def test_concrete_compiled(self, compiled):
        assert "Dog" in compiled

    def test_inherited_state(self, dog):
        """Dog inherits species/legs from Animal constructor."""
        assert dog.send.call(au.AppClientMethodCallParams(method="describe")).abi_return == "Canine"
        assert dog.send.call(au.AppClientMethodCallParams(method="legCount")).abi_return == 4

    def test_override_method(self, dog):
        assert dog.send.call(au.AppClientMethodCallParams(method="sound")).abi_return == "Woof"


class TestMultipleAbstract:
    def test_compiled(self, compiled):
        assert "NamedDog" in compiled
        assert "Named" not in compiled  # abstract

    def test_animal_state(self, named_dog):
        assert named_dog.send.call(au.AppClientMethodCallParams(method="legCount")).abi_return == 4

    def test_override_sound(self, named_dog):
        assert named_dog.send.call(au.AppClientMethodCallParams(method="sound")).abi_return == "Bark"

    def test_override_greet(self, named_dog):
        assert named_dog.send.call(au.AppClientMethodCallParams(method="greet")).abi_return == "Hello, I am a dog"

    def test_named_set_name(self, named_dog):
        named_dog.send.call(au.AppClientMethodCallParams(method="setName", args=["Rex"]))
        assert named_dog.send.call(au.AppClientMethodCallParams(method="name")).abi_return == "Rex"


class TestOwnablePattern:
    def test_owner_set(self, vault, account):
        assert vault.send.call(au.AppClientMethodCallParams(method="owner")).abi_return == account.address

    def test_deposit(self, vault):
        vault.send.call(au.AppClientMethodCallParams(method="deposit", args=[100]))
        assert vault.send.call(au.AppClientMethodCallParams(method="balance")).abi_return == 100

    def test_withdraw(self, vault):
        vault.send.call(au.AppClientMethodCallParams(method="deposit", args=[200]))
        vault.send.call(au.AppClientMethodCallParams(method="withdraw", args=[50]))
        # balance = 100 (from test_deposit) + 200 - 50 = 250
        assert vault.send.call(au.AppClientMethodCallParams(method="balance")).abi_return == 250

    def test_withdraw_insufficient_reverts(self, vault):
        with pytest.raises(Exception, match="insufficient|assert"):
            vault.send.call(
                au.AppClientMethodCallParams(method="withdraw", args=[99999]),
                send_params=au.SendParams(populate_app_call_resources=False),
            )
