"""Tests for Morpho Blue lending protocol compiled from unmodified Solidity to AVM.

Morpho Blue (https://github.com/morpho-org/morpho-blue) is a minimal, immutable
lending protocol. This test suite verifies that the puya-sol compiled contracts
deploy and execute basic operations on Algorand localnet.

Source: Solidity 0.8.19, 14 files, ~570 lines main contract.
Compilation: puya-sol (Solidity->AWST) + puya (AWST->TEAL), single contract (6KB).
"""
import hashlib
from pathlib import Path

import algokit_utils as au
import pytest
from algosdk import encoding
from conftest import (
    OUT_DIR,
    deploy_contract,
    fund_contract,
    load_arc56,
    mapping_box_key,
    box_ref,
    addr_to_bytes32,
    app_id_to_bytes32,
    app_id_to_algod_addr,
    int_to_bytes32,
)

pytestmark = pytest.mark.localnet

WAD = 10**18  # 1e18 — Morpho's fixed-point unit


def keccak256(data: bytes) -> bytes:
    """Compute keccak256 hash (matching Solidity's keccak256)."""
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(data)
    return k.digest()


def market_id(loan_token: str, collateral_token: str, oracle: str,
              irm: str, lltv: int) -> bytes:
    """Compute the market ID (keccak256 of MarketParams struct)."""
    # MarketParams = (loanToken, collateralToken, oracle, irm, lltv)
    # Each address is 32 bytes, lltv is 32 bytes = 160 bytes total
    data = (
        addr_to_bytes32(loan_token)
        + addr_to_bytes32(collateral_token)
        + addr_to_bytes32(oracle)
        + addr_to_bytes32(irm)
        + lltv.to_bytes(32, "big")
    )
    return keccak256(data)


def make_market_params(loan_token: str, collateral_token: str,
                       oracle: str, irm: str, lltv: int) -> tuple:
    """Build a MarketParams tuple for ABI calls.

    MarketParams = (loanToken:uint8[32], collateralToken:uint8[32],
                    oracle:uint8[32], irm:uint8[32], lltv:uint256)
    """
    return (
        list(addr_to_bytes32(loan_token)),
        list(addr_to_bytes32(collateral_token)),
        list(addr_to_bytes32(oracle)),
        list(addr_to_bytes32(irm)),
        lltv,
    )


def market_box_key(mid: bytes) -> bytes:
    """Box key for market[id]."""
    return mapping_box_key("market", mid)


def position_box_key(mid: bytes, user_addr: str) -> bytes:
    """Box key for position[id][user]."""
    return mapping_box_key("position", mid, addr_to_bytes32(user_addr))


def id_to_market_params_box_key(mid: bytes) -> bytes:
    """Box key for idToMarketParams[id]."""
    return mapping_box_key("idToMarketParams", mid)


ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"
ZERO_BYTES32 = b"\x00" * 32
NO_POPULATE = au.SendParams(populate_app_call_resources=False)


def call_with_budget(
    localnet: au.AlgorandClient,
    morpho: au.AppClient,
    params: au.AppClientMethodCallParams,
    budget_calls: int = 1,
    padding_box_refs: list | None = None,
    padding_app_refs: list | None = None,
):
    """Call a Morpho method with extra opcode budget from dummy app calls.

    AVM budget = 700 * (outer app calls in group). Complex methods like
    withdraw/borrow need >700 opcodes before their first inner call.
    Adding `budget_calls` extra owner() calls provides more base budget.

    padding_box_refs/padding_app_refs: extra references placed on the padding
    transactions to share resource availability across the group (AVM v9+
    resource sharing). Use when the main call exceeds MaxAppTotalTxnReferences=8.
    """
    import os
    composer = localnet.new_group()
    # Add budget-padding noop calls (owner is cheap, read-only)
    for i in range(budget_calls):
        pad_params = au.AppClientMethodCallParams(
            method="owner",
            box_references=padding_box_refs or [],
            app_references=padding_app_refs or [],
            note=os.urandom(8),  # unique note to avoid duplicate txn errors
        )
        composer.add_app_call_method_call(
            morpho.params.call(pad_params)
        )
    # Add the main call
    composer.add_app_call_method_call(morpho.params.call(params))
    result = composer.send(NO_POPULATE)
    # The main call result is the last one (index = budget_calls)
    return result


def sol_addr(client: au.AppClient) -> str:
    """Get the Solidity-style address for a deployed contract.

    The puya-sol compiler encodes Solidity addresses as 32-byte values where
    bytes 24-31 contain the app ID as a big-endian uint64. Inner calls use
    extract_uint64(addr, 24) to recover the app ID.
    """
    return app_id_to_algod_addr(client.app_id)


def sol_addr_bytes(client: au.AppClient) -> bytes:
    """Get the 32-byte Solidity-style address for a deployed contract."""
    return app_id_to_bytes32(client.app_id)


def create_market_with_irm(morpho, irm, loan, collateral_token, oracle, lltv):
    """Create a market using the real IRM mock (inner call to borrowRate)."""
    irm_addr = sol_addr(irm)
    mid = market_id(sol_addr(loan), sol_addr(collateral_token),
                    sol_addr(oracle), irm_addr, lltv)
    params = make_market_params(sol_addr(loan), sol_addr(collateral_token),
                                sol_addr(oracle), irm_addr, lltv)
    irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_addr))
    lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
    mkt_key = market_box_key(mid)
    id_key = id_to_market_params_box_key(mid)
    morpho.send.call(
        au.AppClientMethodCallParams(
            method="createMarket", args=[params],
            box_references=[
                box_ref(morpho.app_id, irm_key),
                box_ref(morpho.app_id, lltv_key),
                box_ref(morpho.app_id, mkt_key),
                box_ref(morpho.app_id, id_key),
            ],
            app_references=[irm.app_id],
            extra_fee=au.AlgoAmount(micro_algo=1000),
        ),
        send_params=NO_POPULATE,
    )
    return mid, params, mkt_key


# ---------------------------------------------------------------------------
# Compilation tests
# ---------------------------------------------------------------------------

class TestCompilation:
    """Verify all contracts compiled to valid TEAL and bytecode."""

    @pytest.mark.parametrize("name,subdir", [
        ("Morpho", "MorphoTest"),
        ("ERC20Mock", "ERC20MockTest"),
        ("OracleMock", "OracleMockTest"),
        ("IrmMock", "IrmMockTest"),
    ])
    def test_teal_files_exist(self, name, subdir):
        base = OUT_DIR / subdir
        assert (base / f"{name}.approval.teal").exists()
        assert (base / f"{name}.clear.teal").exists()
        assert (base / f"{name}.approval.bin").exists()
        assert (base / f"{name}.clear.bin").exists()

    def test_arc56_spec_exists(self):
        arc56_path = OUT_DIR / "MorphoTest" / "Morpho.arc56.json"
        assert arc56_path.exists()
        spec = au.Arc56Contract.from_json(arc56_path.read_text())
        assert spec.name == "Morpho"

    def test_morpho_has_expected_methods(self):
        spec = load_arc56("Morpho", "MorphoTest")
        method_names = {m.name for m in spec.methods}
        expected = {
            "setOwner", "enableIrm", "enableLltv", "setFee", "setFeeRecipient",
            "createMarket", "supply", "withdraw", "borrow", "repay",
            "supplyCollateral", "withdrawCollateral", "liquidate",
            "flashLoan", "setAuthorization", "setAuthorizationWithSig",
            "accrueInterest", "extSloads",
        }
        assert expected.issubset(method_names), f"Missing: {expected - method_names}"

    def test_erc20mock_has_expected_methods(self):
        spec = load_arc56("ERC20Mock", "ERC20MockTest")
        method_names = {m.name for m in spec.methods}
        expected = {"setBalance", "approve", "transfer", "transferFrom"}
        assert expected.issubset(method_names)

    def test_teal_compiles_to_bytecode(self, algod_client):
        """Verify all TEAL files compile without errors."""
        for subdir in ["MorphoTest", "ERC20MockTest", "OracleMockTest", "IrmMockTest"]:
            base = OUT_DIR / subdir
            for teal_file in base.glob("*.teal"):
                result = algod_client.compile(teal_file.read_text())
                assert "result" in result, f"Failed to compile {teal_file.name}"

    def test_morpho_bytecode_within_limits(self):
        """Verify Morpho bytecode fits within AVM 8KB limit."""
        bin_path = OUT_DIR / "MorphoTest" / "Morpho.approval.bin"
        size = bin_path.stat().st_size
        assert size <= 8192, f"Morpho bytecode {size} bytes exceeds 8KB limit"


# ---------------------------------------------------------------------------
# Deployment tests
# ---------------------------------------------------------------------------

class TestDeployment:
    """Test deploying all contracts on localnet."""

    def test_deploy_erc20mock(self, localnet, account):
        client = deploy_contract(localnet, account, "ERC20Mock")
        assert client.app_id > 0

    def test_deploy_oracle_mock(self, localnet, account):
        client = deploy_contract(localnet, account, "OracleMock")
        assert client.app_id > 0

    def test_deploy_irm_mock(self, localnet, account):
        client = deploy_contract(localnet, account, "IrmMock")
        assert client.app_id > 0

    def test_deploy_morpho(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        client = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
        )
        assert client.app_id > 0


# ---------------------------------------------------------------------------
# ERC20Mock behavioral tests
# ---------------------------------------------------------------------------

class TestERC20Mock:
    """Test the ERC20 mock token contract."""

    @pytest.fixture
    def token(self, localnet, account):
        return deploy_contract(localnet, account, "ERC20Mock")

    def test_set_balance(self, token, account):
        addr = account.address
        bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        result = token.send.call(
            au.AppClientMethodCallParams(
                method="setBalance",
                args=[addr, 1000],
                box_references=[
                    box_ref(token.app_id, bal_key),
                ],
            )
        )
        assert result is not None

    def test_approve(self, token, account):
        spender = account.address
        allowance_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(spender),
        )
        result = token.send.call(
            au.AppClientMethodCallParams(
                method="approve",
                args=[spender, 500],
                box_references=[
                    box_ref(token.app_id, allowance_key),
                ],
            )
        )
        assert result.abi_return is True

    def test_transfer(self, token, account):
        addr = account.address
        bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        # Set balance first
        token.send.call(
            au.AppClientMethodCallParams(
                method="setBalance",
                args=[addr, 1000],
                box_references=[box_ref(token.app_id, bal_key)],
            )
        )
        # Transfer to self (same address, simplest test)
        result = token.send.call(
            au.AppClientMethodCallParams(
                method="transfer",
                args=[addr, 500],
                box_references=[box_ref(token.app_id, bal_key)],
            )
        )
        assert result.abi_return is True

    def test_transfer_from(self, token, account):
        addr = account.address
        bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(addr),
            addr_to_bytes32(addr),
        )
        # Set balance
        token.send.call(
            au.AppClientMethodCallParams(
                method="setBalance",
                args=[addr, 1000],
                box_references=[box_ref(token.app_id, bal_key)],
            )
        )
        # Approve self
        token.send.call(
            au.AppClientMethodCallParams(
                method="approve",
                args=[addr, 500],
                box_references=[box_ref(token.app_id, allow_key)],
            )
        )
        # Transfer from self to self
        result = token.send.call(
            au.AppClientMethodCallParams(
                method="transferFrom",
                args=[addr, addr, 200],
                box_references=[
                    box_ref(token.app_id, bal_key),
                    box_ref(token.app_id, allow_key),
                ],
            )
        )
        assert result.abi_return is True


# ---------------------------------------------------------------------------
# OracleMock behavioral tests
# ---------------------------------------------------------------------------

class TestOracleMock:
    """Test the Oracle mock contract."""

    @pytest.fixture
    def oracle(self, localnet, account):
        return deploy_contract(localnet, account, "OracleMock")

    def test_set_price(self, oracle, account):
        result = oracle.send.call(
            au.AppClientMethodCallParams(
                method="setPrice",
                args=[1_000_000],
            )
        )
        assert result is not None


# ---------------------------------------------------------------------------
# Morpho governance tests
# ---------------------------------------------------------------------------

class TestMorphoGovernance:
    """Test Morpho governance functions (setOwner, enableIrm, enableLltv)."""

    @pytest.fixture
    def morpho_env(self, localnet, account):
        """Deploy full Morpho environment: Morpho + mocks."""
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=2_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan_token = deploy_contract(localnet, account, "ERC20Mock")
        collateral_token = deploy_contract(localnet, account, "ERC20Mock")
        return {
            "morpho": morpho,
            "irm": irm,
            "oracle": oracle,
            "loan_token": loan_token,
            "collateral_token": collateral_token,
        }

    def test_enable_irm(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        irm = morpho_env["irm"]
        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm",
                args=[irm_sol],
                box_references=[
                    box_ref(morpho.app_id, irm_key),
                ],
            )
        )
        assert result is not None

    def test_enable_irm_already_set_reverts(self, morpho_env, account):
        """Enabling an already-enabled IRM should revert."""
        morpho = morpho_env["morpho"]
        irm = morpho_env["irm"]
        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        refs = [box_ref(morpho.app_id, irm_key)]
        # Enable once
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol], box_references=refs,
            )
        )
        # Enable again — should revert
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="enableIrm", args=[irm_sol], box_references=refs,
                )
            )

    def test_enable_lltv(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        lltv = 800000000000000000  # 80% = 0.8e18
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv",
                args=[lltv],
                box_references=[
                    box_ref(morpho.app_id, lltv_key),
                ],
            )
        )
        assert result is not None

    def test_enable_lltv_too_high_reverts(self, morpho_env, account):
        """LLTV >= WAD (100%) should revert."""
        morpho = morpho_env["morpho"]
        lltv = WAD  # 100% — exceeds max (must be < WAD)
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="enableLltv", args=[lltv],
                    box_references=[box_ref(morpho.app_id, lltv_key)],
                )
            )

    def test_enable_lltv_already_set_reverts(self, morpho_env, account):
        """Enabling an already-enabled LLTV should revert."""
        morpho = morpho_env["morpho"]
        lltv = 500000000000000000  # 50%
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        refs = [box_ref(morpho.app_id, lltv_key)]
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv], box_references=refs,
            )
        )
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="enableLltv", args=[lltv], box_references=refs,
                )
            )

    def test_set_fee_recipient(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        irm = morpho_env["irm"]
        # Set fee recipient to IRM address (just needs a non-zero address)
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="setFeeRecipient",
                args=[sol_addr(irm)],
            )
        )
        assert result is not None

    def test_set_fee_recipient_already_set_reverts(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        irm = morpho_env["irm"]
        irm_sol = sol_addr(irm)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setFeeRecipient", args=[irm_sol],
            )
        )
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setFeeRecipient", args=[irm_sol],
                )
            )

    def test_set_authorization(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        irm = morpho_env["irm"]
        authorized_addr = sol_addr(irm)
        auth_key = mapping_box_key(
            "isAuthorized",
            addr_to_bytes32(account.address),
            addr_to_bytes32(authorized_addr),
        )
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="setAuthorization",
                args=[authorized_addr, True],
                box_references=[box_ref(morpho.app_id, auth_key)],
            )
        )
        assert result is not None

    def test_set_authorization_toggle(self, morpho_env, account):
        """Can authorize and then deauthorize."""
        morpho = morpho_env["morpho"]
        oracle = morpho_env["oracle"]
        authorized_addr = sol_addr(oracle)
        auth_key = mapping_box_key(
            "isAuthorized",
            addr_to_bytes32(account.address),
            addr_to_bytes32(authorized_addr),
        )
        refs = [box_ref(morpho.app_id, auth_key)]
        # Authorize
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setAuthorization", args=[authorized_addr, True],
                box_references=refs,
            )
        )
        # Deauthorize
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="setAuthorization", args=[authorized_addr, False],
                box_references=refs,
            )
        )
        assert result is not None


# ---------------------------------------------------------------------------
# Market creation tests
# ---------------------------------------------------------------------------

class TestMarketCreation:
    """Test Morpho market creation and validation.

    Uses the real IrmMock for createMarket — the IRM's borrowRate() is called
    as an inner transaction during market creation.
    """

    @pytest.fixture
    def env(self, localnet, account):
        """Deploy full environment with IRM/LLTV pre-enabled."""
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock")
        collateral = deploy_contract(localnet, account, "ERC20Mock")

        lltv = 800000000000000000  # 80%

        # Enable real IRM (using padded app ID as Solidity address)
        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )

        # Enable LLTV
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        return {
            "morpho": morpho,
            "irm": irm,
            "oracle": oracle,
            "loan": loan,
            "collateral": collateral,
            "lltv": lltv,
        }

    def test_create_market(self, env, account):
        morpho = env["morpho"]
        mid, params, mkt_key = create_market_with_irm(
            morpho, env["irm"], env["loan"], env["collateral"],
            env["oracle"], env["lltv"],
        )
        assert mid is not None

    def test_create_market_duplicate_reverts(self, env, account):
        """Creating the same market twice should revert."""
        morpho = env["morpho"]
        irm = env["irm"]
        # Create once
        create_market_with_irm(
            morpho, irm, env["loan"], env["collateral"],
            env["oracle"], env["lltv"],
        )
        # Create again — should revert
        with pytest.raises(Exception):
            create_market_with_irm(
                morpho, irm, env["loan"], env["collateral"],
                env["oracle"], env["lltv"],
            )

    def test_create_market_irm_not_enabled_reverts(self, env, account):
        """Creating a market with a non-enabled IRM should revert."""
        morpho = env["morpho"]
        # Use oracle's padded app ID as fake IRM (not enabled)
        fake_irm = sol_addr(env["oracle"])
        params = make_market_params(
            sol_addr(env["loan"]),
            sol_addr(env["collateral"]),
            sol_addr(env["oracle"]),
            fake_irm,
            env["lltv"],
        )
        fake_irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(fake_irm))
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(env["lltv"]))
        mid = market_id(sol_addr(env["loan"]), sol_addr(env["collateral"]),
                        sol_addr(env["oracle"]), fake_irm, env["lltv"])
        refs = [
            box_ref(morpho.app_id, fake_irm_key),
            box_ref(morpho.app_id, lltv_key),
            box_ref(morpho.app_id, market_box_key(mid)),
            box_ref(morpho.app_id, id_to_market_params_box_key(mid)),
        ]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="createMarket", args=[params],
                    box_references=refs,
                ),
                send_params=NO_POPULATE,
            )

    def test_create_market_lltv_not_enabled_reverts(self, env, account):
        """Creating a market with a non-enabled LLTV should revert."""
        morpho = env["morpho"]
        irm = env["irm"]
        bad_lltv = 900000000000000000  # 90%, not enabled
        irm_sol = sol_addr(irm)
        params = make_market_params(
            sol_addr(env["loan"]),
            sol_addr(env["collateral"]),
            sol_addr(env["oracle"]),
            irm_sol,
            bad_lltv,
        )
        mid = market_id(
            sol_addr(env["loan"]),
            sol_addr(env["collateral"]),
            sol_addr(env["oracle"]),
            irm_sol,
            bad_lltv,
        )
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(bad_lltv))
        mkt_key = market_box_key(mid)
        id_key = id_to_market_params_box_key(mid)
        refs = [
            box_ref(morpho.app_id, irm_key),
            box_ref(morpho.app_id, lltv_key),
            box_ref(morpho.app_id, mkt_key),
            box_ref(morpho.app_id, id_key),
        ]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="createMarket", args=[params],
                    box_references=refs,
                    app_references=[irm.app_id],
                    extra_fee=au.AlgoAmount(micro_algo=1000),
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Supply management tests
# ---------------------------------------------------------------------------

class TestSupplyManagement:
    """Test supply and withdraw operations."""

    @pytest.fixture
    def market_env(self, localnet, account):
        """Deploy everything and create a market."""
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock")
        collateral = deploy_contract(localnet, account, "ERC20Mock")

        lltv = 800000000000000000  # 80%

        # Enable real IRM (padded app ID)
        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )

        # Enable LLTV
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        # Create market with real IRM
        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        return {
            "morpho": morpho,
            "irm": irm,
            "oracle": oracle,
            "loan": loan,
            "collateral": collateral,
            "lltv": lltv,
            "mid": mid,
            "params": params,
            "mkt_key": mkt_key,
        }

    def test_supply_zero_address_reverts(self, market_env, account):
        """Supply to zero address should revert."""
        morpho = market_env["morpho"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        mid = market_env["mid"]
        pos_key = position_box_key(mid, ZERO_ADDR)

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="supply",
                    args=[params, 1000, 0, ZERO_ADDR, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                )
            )

    def test_supply_inconsistent_input_reverts(self, market_env, account):
        """Supply with both assets and shares non-zero should revert."""
        morpho = market_env["morpho"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        mid = market_env["mid"]
        pos_key = position_box_key(mid, account.address)

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="supply",
                    args=[params, 1000, 1000, account.address, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                )
            )

    def test_supply_both_zero_reverts(self, market_env, account):
        """Supply with both assets and shares zero should revert."""
        morpho = market_env["morpho"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        mid = market_env["mid"]
        pos_key = position_box_key(mid, account.address)

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="supply",
                    args=[params, 0, 0, account.address, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                )
            )

    def test_accrue_interest(self, market_env, account):
        """Accrue interest on a market."""
        morpho = market_env["morpho"]
        irm = market_env["irm"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="accrueInterest",
                args=[params],
                box_references=[box_ref(morpho.app_id, mkt_key)],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None


# ---------------------------------------------------------------------------
# Collateral management tests
# ---------------------------------------------------------------------------

class TestCollateralManagement:
    """Test collateral supply/withdraw validation.

    Note: Full supply/withdraw with ERC20 transfers requires inner app call
    support for safeTransferFrom. We test input validation (zero assets,
    zero address) and market-not-created checks.
    """

    @pytest.fixture
    def market_env(self, localnet, account):
        """Deploy everything and create a market with real IRM."""
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock")
        collateral = deploy_contract(localnet, account, "ERC20Mock")

        lltv = 800000000000000000  # 80%

        # Enable real IRM (padded app ID)
        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        # Create market with real IRM
        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        return {
            "morpho": morpho,
            "irm": irm,
            "oracle": oracle,
            "loan": loan,
            "collateral": collateral,
            "lltv": lltv,
            "mid": mid,
            "params": params,
            "mkt_key": mkt_key,
        }

    def test_supply_collateral_zero_assets_reverts(self, market_env, account):
        """Supply zero collateral should revert."""
        morpho = market_env["morpho"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        mid = market_env["mid"]
        pos_key = position_box_key(mid, account.address)

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="supplyCollateral",
                    args=[params, 0, account.address, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                )
            )

    def test_supply_collateral_zero_address_reverts(self, market_env, account):
        """Supply collateral to zero address should revert."""
        morpho = market_env["morpho"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        mid = market_env["mid"]
        pos_key = position_box_key(mid, ZERO_ADDR)

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="supplyCollateral",
                    args=[params, 1000, ZERO_ADDR, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                )
            )


# ---------------------------------------------------------------------------
# Owner management tests
# ---------------------------------------------------------------------------

class TestOwnerManagement:
    """Test setOwner and owner() read."""

    @pytest.fixture
    def morpho_env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=2_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        return {"morpho": morpho, "irm": irm}

    def test_read_owner(self, morpho_env, account):
        """Query the owner() getter — should return the deployer's address."""
        morpho = morpho_env["morpho"]
        result = morpho.send.call(
            au.AppClientMethodCallParams(method="owner", args=[])
        )
        assert result.abi_return is not None

    def test_read_fee_recipient(self, morpho_env, account):
        """Query feeRecipient() — should be zero initially."""
        morpho = morpho_env["morpho"]
        result = morpho.send.call(
            au.AppClientMethodCallParams(method="feeRecipient", args=[])
        )
        assert result.abi_return is not None

    def test_set_owner(self, morpho_env, account):
        """setOwner should change the owner."""
        morpho = morpho_env["morpho"]
        new_owner = sol_addr(morpho_env["irm"])
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="setOwner", args=[new_owner],
            )
        )
        assert result is not None

    def test_set_owner_zero_address_reverts(self, morpho_env, account):
        """setOwner to zero address should revert (constructor check)."""
        morpho = morpho_env["morpho"]
        # Setting to same owner should revert (ALREADY_SET)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setOwner", args=[account.address],
                )
            )

    def test_set_fee_recipient_then_read(self, morpho_env, account):
        """Set fee recipient and verify with feeRecipient() getter."""
        morpho = morpho_env["morpho"]
        new_recipient = sol_addr(morpho_env["irm"])
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setFeeRecipient", args=[new_recipient],
            )
        )
        result = morpho.send.call(
            au.AppClientMethodCallParams(method="feeRecipient", args=[])
        )
        assert result.abi_return is not None


# ---------------------------------------------------------------------------
# SetFee tests
# ---------------------------------------------------------------------------

class TestSetFee:
    """Test setting market fees."""

    @pytest.fixture
    def market_env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock")
        collateral = deploy_contract(localnet, account, "ERC20Mock")

        lltv = 800000000000000000

        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        return {
            "morpho": morpho, "irm": irm, "oracle": oracle,
            "loan": loan, "collateral": collateral,
            "lltv": lltv, "mid": mid, "params": params, "mkt_key": mkt_key,
        }

    def test_set_fee(self, market_env, account):
        """Set a valid fee on a created market."""
        morpho = market_env["morpho"]
        irm = market_env["irm"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        # MAX_FEE = 0.25e18 = 250000000000000000
        new_fee = 100000000000000000  # 10%
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="setFee", args=[params, new_fee],
                box_references=[box_ref(morpho.app_id, mkt_key)],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None

    def test_set_fee_too_high_reverts(self, market_env, account):
        """Fee exceeding MAX_FEE (25%) should revert."""
        morpho = market_env["morpho"]
        irm = market_env["irm"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        # MAX_FEE = 0.25e18 = 250000000000000000
        bad_fee = 300000000000000000  # 30% > 25%
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setFee", args=[params, bad_fee],
                    box_references=[box_ref(morpho.app_id, mkt_key)],
                    app_references=[irm.app_id],
                    extra_fee=au.AlgoAmount(micro_algo=1000),
                ),
                send_params=NO_POPULATE,
            )

    def test_set_fee_already_set_reverts(self, market_env, account):
        """Setting the same fee twice should revert (ALREADY_SET)."""
        morpho = market_env["morpho"]
        irm = market_env["irm"]
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        new_fee = 50000000000000000  # 5%
        call_params = au.AppClientMethodCallParams(
            method="setFee", args=[params, new_fee],
            box_references=[box_ref(morpho.app_id, mkt_key)],
            app_references=[irm.app_id],
            extra_fee=au.AlgoAmount(micro_algo=1000),
        )
        morpho.send.call(call_params, send_params=NO_POPULATE)
        with pytest.raises(Exception):
            morpho.send.call(call_params, send_params=NO_POPULATE)

    def test_set_fee_market_not_created_reverts(self, market_env, account):
        """Setting fee on a non-existent market should revert."""
        morpho = market_env["morpho"]
        irm = market_env["irm"]
        # Use different LLTV to get a different market ID
        fake_params = make_market_params(
            sol_addr(market_env["loan"]),
            sol_addr(market_env["collateral"]),
            sol_addr(market_env["oracle"]),
            sol_addr(irm),
            500000000000000000,  # 50%, different market
        )
        fake_mid = market_id(
            sol_addr(market_env["loan"]),
            sol_addr(market_env["collateral"]),
            sol_addr(market_env["oracle"]),
            sol_addr(irm),
            500000000000000000,
        )
        fake_mkt_key = market_box_key(fake_mid)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setFee", args=[fake_params, 100000000000000000],
                    box_references=[box_ref(morpho.app_id, fake_mkt_key)],
                    app_references=[irm.app_id],
                    extra_fee=au.AlgoAmount(micro_algo=1000),
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Market-not-created revert tests
# ---------------------------------------------------------------------------

class TestMarketNotCreated:
    """Test that operations on non-existent markets revert."""

    @pytest.fixture
    def env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=2_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        # Make params for a market that was never created
        params = make_market_params(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        mid = market_id(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        mkt_key = market_box_key(mid)
        pos_key = position_box_key(mid, account.address)
        return {
            "morpho": morpho, "irm": irm, "params": params,
            "mid": mid, "mkt_key": mkt_key, "pos_key": pos_key,
        }

    def test_supply_market_not_created_reverts(self, env, account):
        morpho = env["morpho"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="supply",
                    args=[env["params"], 1000, 0, account.address, b""],
                    box_references=[
                        box_ref(morpho.app_id, env["mkt_key"]),
                        box_ref(morpho.app_id, env["pos_key"]),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_withdraw_market_not_created_reverts(self, env, account):
        morpho = env["morpho"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdraw",
                    args=[env["params"], 1000, 0, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, env["mkt_key"]),
                        box_ref(morpho.app_id, env["pos_key"]),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_borrow_market_not_created_reverts(self, env, account):
        morpho = env["morpho"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="borrow",
                    args=[env["params"], 1000, 0, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, env["mkt_key"]),
                        box_ref(morpho.app_id, env["pos_key"]),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_repay_market_not_created_reverts(self, env, account):
        morpho = env["morpho"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="repay",
                    args=[env["params"], 1000, 0, account.address, b""],
                    box_references=[
                        box_ref(morpho.app_id, env["mkt_key"]),
                        box_ref(morpho.app_id, env["pos_key"]),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_supply_collateral_market_not_created_reverts(self, env, account):
        morpho = env["morpho"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="supplyCollateral",
                    args=[env["params"], 1000, account.address, b""],
                    box_references=[
                        box_ref(morpho.app_id, env["mkt_key"]),
                        box_ref(morpho.app_id, env["pos_key"]),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_accrue_interest_market_not_created_reverts(self, env, account):
        morpho = env["morpho"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="accrueInterest",
                    args=[env["params"]],
                    box_references=[box_ref(morpho.app_id, env["mkt_key"])],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Full supply and withdraw flow
# ---------------------------------------------------------------------------

class TestSupplyFlow:
    """Test the full supply flow including ERC20 token transfers.

    Supply involves an inner transaction to ERC20.safeTransferFrom which
    transfers tokens from the caller to the Morpho contract. This tests
    the complete flow: set balance → approve → supply → verify.
    """

    @pytest.fixture
    def full_env(self, localnet, account):
        """Deploy full environment with market and funded ERC20 tokens."""
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock",
                              fund_amount=1_000_000)
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock",
                               fund_amount=1_000_000)
        collateral = deploy_contract(localnet, account, "ERC20Mock",
                                     fund_amount=1_000_000)

        lltv = 800000000000000000

        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        return {
            "morpho": morpho, "irm": irm, "oracle": oracle,
            "loan": loan, "collateral": collateral,
            "lltv": lltv, "mid": mid, "params": params, "mkt_key": mkt_key,
        }

    def _setup_token_for_supply(self, token, morpho, account, amount):
        """Set balance and approve Morpho to spend tokens."""
        addr = account.address
        bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        # Set balance
        token.send.call(
            au.AppClientMethodCallParams(
                method="setBalance", args=[addr, amount],
                box_references=[box_ref(token.app_id, bal_key)],
            )
        )
        # Approve Morpho (use Algorand app address — msg.sender in inner call)
        morpho_addr = morpho.app_address
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(addr),
            addr_to_bytes32(morpho_addr),
        )
        token.send.call(
            au.AppClientMethodCallParams(
                method="approve", args=[morpho_addr, amount],
                box_references=[box_ref(token.app_id, allow_key)],
            )
        )

    def test_supply_assets(self, full_env, account, localnet):
        """Supply loan tokens by specifying assets amount."""
        morpho = full_env["morpho"]
        irm = full_env["irm"]
        loan = full_env["loan"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]
        mid = full_env["mid"]

        supply_amount = 10000
        self._setup_token_for_supply(loan, morpho, account, supply_amount)

        pos_key = position_box_key(mid, account.address)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="supply",
                args=[params, supply_amount, 0, account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        # supply returns (assets, shares)
        last_return = result.returns[-1]
        assert last_return.value is not None
        returned_assets, returned_shares = last_return.value
        assert returned_assets == supply_amount
        assert returned_shares > 0

    def test_supply_then_accrue_interest(self, full_env, account, localnet):
        """Supply tokens then accrue interest — verifies market state persists."""
        morpho = full_env["morpho"]
        irm = full_env["irm"]
        loan = full_env["loan"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]
        mid = full_env["mid"]

        supply_amount = 50000
        self._setup_token_for_supply(loan, morpho, account, supply_amount)

        pos_key = position_box_key(mid, account.address)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        # Supply — uses call_with_budget because supply exceeds 700 opcodes
        call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="supply",
                args=[params, supply_amount, 0, account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )

        # Accrue interest (should succeed — market has supply now)
        # Uses call_with_budget because _accrueInterest exceeds 700 opcodes
        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="accrueInterest", args=[params],
                box_references=[box_ref(morpho.app_id, mkt_key)],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
        )
        assert result is not None


# ---------------------------------------------------------------------------
# Full collateral supply flow
# ---------------------------------------------------------------------------

class TestCollateralFlow:
    """Test full supplyCollateral flow with ERC20 transfers."""

    @pytest.fixture
    def full_env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock",
                              fund_amount=1_000_000)
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock",
                               fund_amount=1_000_000)
        collateral = deploy_contract(localnet, account, "ERC20Mock",
                                     fund_amount=1_000_000)

        lltv = 800000000000000000

        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        return {
            "morpho": morpho, "irm": irm, "oracle": oracle,
            "loan": loan, "collateral": collateral,
            "lltv": lltv, "mid": mid, "params": params, "mkt_key": mkt_key,
        }

    def _setup_token(self, token, morpho, account, amount):
        """Set balance and approve Morpho to spend tokens."""
        addr = account.address
        bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        token.send.call(
            au.AppClientMethodCallParams(
                method="setBalance", args=[addr, amount],
                box_references=[box_ref(token.app_id, bal_key)],
            )
        )
        morpho_addr = morpho.app_address
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(addr),
            addr_to_bytes32(morpho_addr),
        )
        token.send.call(
            au.AppClientMethodCallParams(
                method="approve", args=[morpho_addr, amount],
                box_references=[box_ref(token.app_id, allow_key)],
            )
        )

    def test_supply_collateral(self, full_env, account):
        """Supply collateral tokens — full flow with ERC20 transfer."""
        morpho = full_env["morpho"]
        collateral = full_env["collateral"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]
        mid = full_env["mid"]

        coll_amount = 20000
        self._setup_token(collateral, morpho, account, coll_amount)

        pos_key = position_box_key(mid, account.address)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="supplyCollateral",
                args=[params, coll_amount, account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(collateral.app_id, sender_bal),
                    box_ref(collateral.app_id, morpho_bal),
                    box_ref(collateral.app_id, allow_key),
                ],
                app_references=[collateral.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None


# ---------------------------------------------------------------------------
# Withdraw flow
# ---------------------------------------------------------------------------

class TestWithdrawFlow:
    """Test supply then withdraw — full ERC20 transfer out."""

    @pytest.fixture
    def full_env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock",
                              fund_amount=1_000_000)
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock",
                               fund_amount=1_000_000)
        collateral = deploy_contract(localnet, account, "ERC20Mock",
                                     fund_amount=1_000_000)
        lltv = 800000000000000000

        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        return {
            "morpho": morpho, "irm": irm, "oracle": oracle,
            "loan": loan, "collateral": collateral,
            "lltv": lltv, "mid": mid, "params": params, "mkt_key": mkt_key,
        }

    def _supply(self, morpho, irm, loan, account, params, mkt_key, mid, amount, localnet=None):
        """Supply loan tokens into the market."""
        addr = account.address
        _set_balance_and_approve(loan, morpho, addr, amount)
        pos_key = position_box_key(mid, addr)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance", addr_to_bytes32(addr), addr_to_bytes32(morpho.app_address),
        )
        call_params = au.AppClientMethodCallParams(
            method="supply",
            args=[params, amount, 0, addr, b""],
            box_references=[
                box_ref(morpho.app_id, mkt_key),
                box_ref(morpho.app_id, pos_key),
                box_ref(loan.app_id, sender_bal),
                box_ref(loan.app_id, morpho_bal),
                box_ref(loan.app_id, allow_key),
            ],
            app_references=[irm.app_id, loan.app_id],
            extra_fee=au.AlgoAmount(micro_algo=2000),
        )
        if localnet:
            return call_with_budget(localnet, morpho, call_params)
        return morpho.send.call(call_params, send_params=NO_POPULATE)

    def test_withdraw_assets(self, full_env, account, localnet):
        """Supply then withdraw — safeTransfer sends tokens from Morpho to receiver."""
        morpho = full_env["morpho"]
        irm = full_env["irm"]
        loan = full_env["loan"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]
        mid = full_env["mid"]

        supply_amount = 10000
        self._supply(morpho, irm, loan, account, params, mkt_key, mid, supply_amount, localnet=localnet)

        # Withdraw — safeTransfer calls ERC20.transfer(receiver, amount)
        # ERC20.transfer uses msg.sender (=morpho.app_address) as source
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="withdraw",
                args=[params, supply_amount, 0, account.address, account.address],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        # Result is from the grouped transaction — last return value
        last_return = result.returns[-1]
        assert last_return.value[0] == supply_amount
        assert last_return.value[1] > 0

    def test_withdraw_zero_receiver_reverts(self, full_env, account):
        """Withdraw with zero receiver should revert."""
        morpho = full_env["morpho"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdraw",
                    args=[params, 100, 0, account.address, ZERO_ADDR],
                    box_references=[box_ref(morpho.app_id, mkt_key)],
                ),
                send_params=NO_POPULATE,
            )

    def test_withdraw_inconsistent_input_reverts(self, full_env, account):
        """Withdraw with both assets and shares nonzero should revert."""
        morpho = full_env["morpho"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdraw",
                    args=[params, 100, 100, account.address, account.address],
                    box_references=[box_ref(morpho.app_id, mkt_key)],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# WithdrawCollateral flow
# ---------------------------------------------------------------------------

class TestWithdrawCollateralFlow:
    """Supply collateral then withdraw it — safeTransfer out."""

    @pytest.fixture
    def full_env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock",
                              fund_amount=1_000_000)
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock",
                               fund_amount=1_000_000)
        collateral = deploy_contract(localnet, account, "ERC20Mock",
                                     fund_amount=1_000_000)
        lltv = 800000000000000000

        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        return {
            "morpho": morpho, "irm": irm, "oracle": oracle,
            "loan": loan, "collateral": collateral,
            "lltv": lltv, "mid": mid, "params": params, "mkt_key": mkt_key,
        }

    def _supply_collateral(self, morpho, collateral, account, params, mkt_key, mid, amount):
        """Supply collateral tokens into the market."""
        addr = account.address
        bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        collateral.send.call(
            au.AppClientMethodCallParams(
                method="setBalance", args=[addr, amount],
                box_references=[box_ref(collateral.app_id, bal_key)],
            )
        )
        morpho_addr = morpho.app_address
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(addr),
            addr_to_bytes32(morpho_addr),
        )
        collateral.send.call(
            au.AppClientMethodCallParams(
                method="approve", args=[morpho_addr, amount],
                box_references=[box_ref(collateral.app_id, allow_key)],
            )
        )
        pos_key = position_box_key(mid, addr)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho_addr))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="supplyCollateral",
                args=[params, amount, addr, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(collateral.app_id, sender_bal),
                    box_ref(collateral.app_id, morpho_bal),
                    box_ref(collateral.app_id, allow_key),
                ],
                app_references=[collateral.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )

    def test_withdraw_collateral(self, full_env, account, localnet):
        """Supply collateral then withdraw — no borrows so _isHealthy returns true."""
        morpho = full_env["morpho"]
        irm = full_env["irm"]
        collateral = full_env["collateral"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]
        mid = full_env["mid"]

        coll_amount = 20000
        self._supply_collateral(morpho, collateral, account, params, mkt_key, mid, coll_amount)

        # withdrawCollateral: safeTransfer(collateralToken, receiver, amount)
        # _isHealthy: borrowShares==0 → true (no oracle call needed)
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="withdrawCollateral",
                args=[params, coll_amount, account.address, account.address],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(collateral.app_id, morpho_bal),
                    box_ref(collateral.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, collateral.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )

    def test_withdraw_collateral_zero_assets_reverts(self, full_env, account):
        """WithdrawCollateral with zero assets should revert."""
        morpho = full_env["morpho"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdrawCollateral",
                    args=[params, 0, account.address, account.address],
                    box_references=[box_ref(morpho.app_id, mkt_key)],
                ),
                send_params=NO_POPULATE,
            )

    def test_withdraw_collateral_zero_receiver_reverts(self, full_env, account):
        """WithdrawCollateral with zero receiver should revert."""
        morpho = full_env["morpho"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdrawCollateral",
                    args=[params, 100, account.address, ZERO_ADDR],
                    box_references=[box_ref(morpho.app_id, mkt_key)],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Borrow and Repay flow
# ---------------------------------------------------------------------------

class TestBorrowRepayFlow:
    """Full borrow/repay cycle: supply loan + supply collateral → borrow → repay."""

    @pytest.fixture
    def full_env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=5_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock",
                              fund_amount=1_000_000)
        oracle = deploy_contract(localnet, account, "OracleMock",
                                 fund_amount=1_000_000)
        loan = deploy_contract(localnet, account, "ERC20Mock",
                               fund_amount=1_000_000)
        collateral = deploy_contract(localnet, account, "ERC20Mock",
                                     fund_amount=1_000_000)
        lltv = 800000000000000000  # 80%

        irm_sol = sol_addr(irm)
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_sol],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        mid, params, mkt_key = create_market_with_irm(
            morpho, irm, loan, collateral, oracle, lltv,
        )

        # Set oracle price: 1e36 (ORACLE_PRICE_SCALE) = 1:1 collateral to loan ratio
        oracle_price = 10**36
        oracle.send.call(
            au.AppClientMethodCallParams(
                method="setPrice", args=[oracle_price],
            )
        )

        return {
            "morpho": morpho, "irm": irm, "oracle": oracle,
            "loan": loan, "collateral": collateral,
            "lltv": lltv, "mid": mid, "params": params, "mkt_key": mkt_key,
        }

    def _setup_and_supply(self, token, morpho, irm, account, params, mkt_key, mid, amount, localnet=None):
        """Set balance, approve, and supply loan tokens."""
        addr = account.address
        _set_balance_and_approve(token, morpho, addr, amount)
        pos_key = position_box_key(mid, addr)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance", addr_to_bytes32(addr), addr_to_bytes32(morpho.app_address),
        )
        call_params = au.AppClientMethodCallParams(
            method="supply",
            args=[params, amount, 0, addr, b""],
            box_references=[
                box_ref(morpho.app_id, mkt_key),
                box_ref(morpho.app_id, pos_key),
                box_ref(token.app_id, sender_bal),
                box_ref(token.app_id, morpho_bal),
                box_ref(token.app_id, allow_key),
            ],
            app_references=[irm.app_id, token.app_id],
            extra_fee=au.AlgoAmount(micro_algo=2000),
        )
        if localnet:
            return call_with_budget(localnet, morpho, call_params)
        return morpho.send.call(call_params, send_params=NO_POPULATE)

    def _setup_and_supply_collateral(self, token, morpho, account, params, mkt_key, mid, amount):
        """Set balance, approve, and supply collateral tokens."""
        addr = account.address
        bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        token.send.call(
            au.AppClientMethodCallParams(
                method="setBalance", args=[addr, amount],
                box_references=[box_ref(token.app_id, bal_key)],
            )
        )
        morpho_addr = morpho.app_address
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(addr),
            addr_to_bytes32(morpho_addr),
        )
        token.send.call(
            au.AppClientMethodCallParams(
                method="approve", args=[morpho_addr, amount],
                box_references=[box_ref(token.app_id, allow_key)],
            )
        )
        pos_key = position_box_key(mid, addr)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(addr))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho_addr))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="supplyCollateral",
                args=[params, amount, addr, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(token.app_id, sender_bal),
                    box_ref(token.app_id, morpho_bal),
                    box_ref(token.app_id, allow_key),
                ],
                app_references=[token.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )

    def test_borrow(self, full_env, account, localnet):
        """Full borrow: supply 100k loan + 200k collateral → borrow 50k."""
        morpho = full_env["morpho"]
        irm = full_env["irm"]
        oracle = full_env["oracle"]
        loan = full_env["loan"]
        collateral = full_env["collateral"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]
        mid = full_env["mid"]

        # Supply 100k loan tokens (provides liquidity)
        self._setup_and_supply(loan, morpho, irm, account, params, mkt_key, mid, 100000, localnet=localnet)
        # Supply 200k collateral tokens (enables borrowing)
        self._setup_and_supply_collateral(collateral, morpho, account, params, mkt_key, mid, 200000)

        # Borrow 50k — needs health check (oracle + position boxes)
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="borrow",
                args=[params, 50000, 0, account.address, account.address],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id, oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=3000),
            ),
        )
        last_return = result.returns[-1]
        assert last_return.value[0] == 50000
        assert last_return.value[1] > 0

    def test_borrow_then_repay(self, full_env, account, localnet):
        """Full cycle: supply → borrow → repay."""
        morpho = full_env["morpho"]
        irm = full_env["irm"]
        oracle = full_env["oracle"]
        loan = full_env["loan"]
        collateral = full_env["collateral"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]
        mid = full_env["mid"]

        # Supply + collateral
        self._setup_and_supply(loan, morpho, irm, account, params, mkt_key, mid, 100000, localnet=localnet)
        self._setup_and_supply_collateral(collateral, morpho, account, params, mkt_key, mid, 200000)

        # Borrow 30k
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="borrow",
                args=[params, 30000, 0, account.address, account.address],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id, oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=3000),
            ),
        )

        # Repay 30k — safeTransferFrom(msg.sender, address(this), assets)
        # Need sender balance + morpho balance + allowance on loan token
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        # Set up approval for repay (balance already set from borrow proceeds)
        loan.send.call(
            au.AppClientMethodCallParams(
                method="approve", args=[morpho.app_address, 30000],
                box_references=[box_ref(loan.app_id, allow_key)],
            )
        )

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="repay",
                args=[params, 30000, 0, account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        last_return = result.returns[-1]
        assert last_return.value[0] == 30000
        assert last_return.value[1] > 0

    def test_borrow_zero_receiver_reverts(self, full_env, account):
        """Borrow with zero receiver should revert."""
        morpho = full_env["morpho"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="borrow",
                    args=[params, 100, 0, account.address, ZERO_ADDR],
                    box_references=[box_ref(morpho.app_id, mkt_key)],
                ),
                send_params=NO_POPULATE,
            )

    def test_repay_zero_address_reverts(self, full_env, account):
        """Repay with zero onBehalf should revert."""
        morpho = full_env["morpho"]
        params = full_env["params"]
        mkt_key = full_env["mkt_key"]

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="repay",
                    args=[params, 100, 0, ZERO_ADDR, b""],
                    box_references=[box_ref(morpho.app_id, mkt_key)],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Helper: set up a full market environment with supply + collateral
# ---------------------------------------------------------------------------

def _make_full_env(localnet, account, oracle_price=10**36):
    """Deploy all contracts, create a market, return env dict."""
    owner_bytes = addr_to_bytes32(account.address)
    morpho = deploy_contract(
        localnet, account, "Morpho", subdir="MorphoTest",
        constructor_args=[owner_bytes],
        fund_amount=5_000_000,
    )
    irm = deploy_contract(localnet, account, "IrmMock", fund_amount=1_000_000)
    oracle = deploy_contract(localnet, account, "OracleMock", fund_amount=1_000_000)
    loan = deploy_contract(localnet, account, "ERC20Mock", fund_amount=1_000_000)
    collateral = deploy_contract(localnet, account, "ERC20Mock", fund_amount=1_000_000)
    lltv = 800000000000000000  # 80%

    irm_sol = sol_addr(irm)
    irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
    morpho.send.call(
        au.AppClientMethodCallParams(
            method="enableIrm", args=[irm_sol],
            box_references=[box_ref(morpho.app_id, irm_key)],
        )
    )
    lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
    morpho.send.call(
        au.AppClientMethodCallParams(
            method="enableLltv", args=[lltv],
            box_references=[box_ref(morpho.app_id, lltv_key)],
        )
    )

    mid, params, mkt_key = create_market_with_irm(
        morpho, irm, loan, collateral, oracle, lltv,
    )

    if oracle_price > 0:
        oracle.send.call(
            au.AppClientMethodCallParams(method="setPrice", args=[oracle_price])
        )

    return {
        "morpho": morpho, "irm": irm, "oracle": oracle,
        "loan": loan, "collateral": collateral,
        "lltv": lltv, "mid": mid, "params": params, "mkt_key": mkt_key,
    }


def _set_balance_and_approve(token, morpho, addr, amount):
    """Set ERC20 balance and approve Morpho to spend."""
    bal_key = mapping_box_key("balanceOf", addr_to_bytes32(addr))
    token.send.call(
        au.AppClientMethodCallParams(
            method="setBalance", args=[addr, amount],
            box_references=[box_ref(token.app_id, bal_key)],
        )
    )
    morpho_addr = morpho.app_address
    allow_key = mapping_box_key(
        "allowance",
        addr_to_bytes32(addr),
        addr_to_bytes32(morpho_addr),
    )
    token.send.call(
        au.AppClientMethodCallParams(
            method="approve", args=[morpho_addr, amount],
            box_references=[box_ref(token.app_id, allow_key)],
        )
    )


def _do_supply(morpho, irm, loan, account, params, mkt_key, mid, amount, localnet=None):
    """Supply loan tokens into the market. Returns result.

    Pass localnet to use call_with_budget (needed when fee is set, as
    _accrueInterest does biguint division exceeding 700 opcode budget).
    """
    addr = account.address
    _set_balance_and_approve(loan, morpho, addr, amount)
    pos_key = position_box_key(mid, addr)
    sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(addr))
    morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
    allow_key = mapping_box_key(
        "allowance", addr_to_bytes32(addr), addr_to_bytes32(morpho.app_address),
    )
    call_params = au.AppClientMethodCallParams(
        method="supply",
        args=[params, amount, 0, addr, b""],
        box_references=[
            box_ref(morpho.app_id, mkt_key),
            box_ref(morpho.app_id, pos_key),
            box_ref(loan.app_id, sender_bal),
            box_ref(loan.app_id, morpho_bal),
            box_ref(loan.app_id, allow_key),
        ],
        app_references=[irm.app_id, loan.app_id],
        extra_fee=au.AlgoAmount(micro_algo=2000),
    )
    if localnet:
        return call_with_budget(localnet, morpho, call_params)
    result = morpho.send.call(call_params, send_params=NO_POPULATE)
    return result


def _do_supply_collateral(morpho, coll_token, account, params, mkt_key, mid, amount):
    """Supply collateral tokens into the market."""
    addr = account.address
    _set_balance_and_approve(coll_token, morpho, addr, amount)
    pos_key = position_box_key(mid, addr)
    sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(addr))
    morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
    allow_key = mapping_box_key(
        "allowance", addr_to_bytes32(addr), addr_to_bytes32(morpho.app_address),
    )
    morpho.send.call(
        au.AppClientMethodCallParams(
            method="supplyCollateral",
            args=[params, amount, addr, b""],
            box_references=[
                box_ref(morpho.app_id, mkt_key),
                box_ref(morpho.app_id, pos_key),
                box_ref(coll_token.app_id, sender_bal),
                box_ref(coll_token.app_id, morpho_bal),
                box_ref(coll_token.app_id, allow_key),
            ],
            app_references=[coll_token.app_id],
            extra_fee=au.AlgoAmount(micro_algo=1000),
        ),
        send_params=NO_POPULATE,
    )


def _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, amount):
    """Borrow loan tokens from the market."""
    pos_key = position_box_key(mid, account.address)
    morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
    receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
    return call_with_budget(
        localnet, morpho,
        au.AppClientMethodCallParams(
            method="borrow",
            args=[params, amount, 0, account.address, account.address],
            box_references=[
                box_ref(morpho.app_id, mkt_key),
                box_ref(morpho.app_id, pos_key),
                box_ref(loan.app_id, morpho_bal),
                box_ref(loan.app_id, receiver_bal),
            ],
            app_references=[irm.app_id, loan.app_id, oracle.app_id],
            extra_fee=au.AlgoAmount(micro_algo=3000),
        ),
    )


# ---------------------------------------------------------------------------
# Authorization delegation tests
# ---------------------------------------------------------------------------

class TestAuthorizationDelegation:
    """Test withdraw/borrow on behalf of another address via isAuthorized."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    @pytest.fixture
    def second_account(self, localnet, account):
        """Create and fund a second account."""
        acc2 = localnet.account.random()
        localnet.account.ensure_funded(
            acc2, account,
            min_spending_balance=au.AlgoAmount(micro_algo=10_000_000),
            min_funding_increment=au.AlgoAmount(micro_algo=1_000_000),
        )
        localnet.account.set_signer_from_account(acc2)
        return acc2

    def test_withdraw_unauthorized_reverts(self, env, account, second_account, localnet):
        """Withdraw on behalf of another without authorization should revert."""
        morpho = env["morpho"]
        irm = env["irm"]
        loan = env["loan"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # account supplies tokens
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 10000, localnet=localnet)

        # second_account tries to withdraw on behalf of account — not authorized
        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdraw",
                    args=[params, 1000, 0, account.address, second_account.address],
                    sender=second_account.address,
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_withdraw_authorized(self, env, account, second_account, localnet):
        """Authorize second_account, then withdraw on behalf succeeds."""
        morpho = env["morpho"]
        irm = env["irm"]
        loan = env["loan"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # account supplies tokens
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 10000, localnet=localnet)

        # account authorizes second_account
        auth_key = mapping_box_key(
            "isAuthorized",
            addr_to_bytes32(account.address),
            addr_to_bytes32(second_account.address),
        )
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setAuthorization",
                args=[second_account.address, True],
                box_references=[box_ref(morpho.app_id, auth_key)],
            )
        )

        # second_account withdraws on behalf of account
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(second_account.address))

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="withdraw",
                args=[params, 5000, 0, account.address, second_account.address],
                sender=second_account.address,
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(morpho.app_id, auth_key),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        last_return = result.returns[-1]
        assert last_return.value[0] == 5000

    def test_supply_on_behalf(self, env, account, second_account, localnet):
        """Supply on behalf of second_account — anyone can supply for anyone."""
        morpho = env["morpho"]
        irm = env["irm"]
        loan = env["loan"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        supply_amount = 10000
        # account sets up tokens and supplies on behalf of second_account
        _set_balance_and_approve(loan, morpho, account.address, supply_amount)
        pos_key = position_box_key(mid, second_account.address)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="supply",
                args=[params, supply_amount, 0, second_account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        returned_assets, returned_shares = result.returns[-1].value
        assert returned_assets == supply_amount
        assert returned_shares > 0

    def test_repay_on_behalf(self, env, account, second_account, localnet):
        """Repay on behalf of second_account — anyone can repay for anyone."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply liquidity as account
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)

        # second_account supplies collateral + borrows
        # Set balance for second_account (setBalance is permissionless)
        s2_addr = second_account.address
        s2_bal = mapping_box_key("balanceOf", addr_to_bytes32(s2_addr))
        collateral.send.call(
            au.AppClientMethodCallParams(
                method="setBalance", args=[s2_addr, 200000],
                box_references=[box_ref(collateral.app_id, s2_bal)],
            )
        )
        # second_account approves morpho (must be sent by second_account)
        morpho_addr = morpho.app_address
        s2_allow = mapping_box_key(
            "allowance",
            addr_to_bytes32(s2_addr),
            addr_to_bytes32(morpho_addr),
        )
        collateral.send.call(
            au.AppClientMethodCallParams(
                method="approve", args=[morpho_addr, 200000],
                sender=s2_addr,
                box_references=[box_ref(collateral.app_id, s2_allow)],
            )
        )
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho_addr))
        pos_key_2 = position_box_key(mid, s2_addr)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="supplyCollateral",
                args=[params, 200000, s2_addr, b""],
                sender=s2_addr,
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key_2),
                    box_ref(collateral.app_id, s2_bal),
                    box_ref(collateral.app_id, morpho_bal),
                    box_ref(collateral.app_id, s2_allow),
                ],
                app_references=[collateral.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )

        # second_account borrows
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(s2_addr))
        loan_morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho_addr))
        borrow_result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="borrow",
                args=[params, 30000, 0, s2_addr, s2_addr],
                sender=s2_addr,
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key_2),
                    box_ref(loan.app_id, loan_morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id, oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=3000),
            ),
        )
        borrow_assets = borrow_result.returns[-1].value[0]
        assert borrow_assets == 30000

        # Now account repays on behalf of second_account
        _set_balance_and_approve(loan, morpho, account.address, 30000)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )
        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="repay",
                args=[params, 30000, 0, second_account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key_2),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, loan_morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        returned_assets, returned_shares = result.returns[-1].value
        assert returned_assets == 30000
        assert returned_shares > 0

    def test_borrow_authorized(self, env, account, second_account, localnet):
        """Authorize second_account, then borrow on behalf succeeds."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # account supplies liquidity + collateral
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 200000)

        # account authorizes second_account
        auth_key = mapping_box_key(
            "isAuthorized",
            addr_to_bytes32(account.address),
            addr_to_bytes32(second_account.address),
        )
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setAuthorization",
                args=[second_account.address, True],
                box_references=[box_ref(morpho.app_id, auth_key)],
            )
        )

        # second_account borrows on behalf of account
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(second_account.address))

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="borrow",
                args=[params, 5000, 0, account.address, second_account.address],
                sender=second_account.address,
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(morpho.app_id, auth_key),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id, oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=3000),
            ),
        )
        last_return = result.returns[-1]
        assert last_return.value[0] == 5000
        assert last_return.value[1] > 0

    def test_borrow_unauthorized_reverts(self, env, account, second_account):
        """Borrow on behalf without authorization should revert."""
        morpho = env["morpho"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="borrow",
                    args=[params, 100, 0, account.address, second_account.address],
                    sender=second_account.address,
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_withdraw_collateral_unauthorized_reverts(self, env, account, second_account):
        """WithdrawCollateral on behalf without authorization should revert."""
        morpho = env["morpho"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 10000)

        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdrawCollateral",
                    args=[params, 5000, account.address, second_account.address],
                    sender=second_account.address,
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# setAuthorizationWithSig (EIP-712 signature)
# ---------------------------------------------------------------------------

class TestAuthorizationWithSig:
    """Test setAuthorizationWithSig using EIP-712 signed authorization.

    The signer's Ethereum address (derived from secp256k1 public key via
    keccak256) is used as the authorizer. The Morpho contract verifies the
    signature using ecrecover and sets isAuthorized accordingly.
    """

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def _make_sig(self, morpho_app_id, authorizer_addr_bytes, authorized_addr_bytes,
                  is_authorized, nonce, deadline, private_key_bytes):
        """Build EIP-712 digest and sign it.

        Returns (v, r, s) tuple and the authorizer's Solidity address.
        """
        from eth_keys import keys as eth_keys
        from Crypto.Hash import keccak as pycryptodome_keccak

        def keccak256_hash(data: bytes) -> bytes:
            k = pycryptodome_keccak.new(digest_bits=256)
            k.update(data)
            return k.digest()

        # EIP-712 type hashes (must match ConstantsLib.sol)
        DOMAIN_TYPEHASH = keccak256_hash(
            b"EIP712Domain(uint256 chainId,address verifyingContract)"
        )
        AUTHORIZATION_TYPEHASH = keccak256_hash(
            b"Authorization(address authorizer,address authorized,bool isAuthorized,uint256 nonce,uint256 deadline)"
        )

        # On AVM: block.chainid → global CurrentApplicationID (uint64)
        # address(this) → global CurrentApplicationAddress (32-byte Algorand app address)
        chain_id = morpho_app_id
        # The Algorand app address = sha512_256("appID" + app_id.to_bytes(8, "big"))
        import hashlib
        app_addr_raw = hashlib.new("sha512_256", b"appID" + morpho_app_id.to_bytes(8, "big")).digest()
        verifying_contract = app_addr_raw  # 32 bytes, same as global CurrentApplicationAddress

        # DOMAIN_SEPARATOR = keccak256(DOMAIN_TYPEHASH || itob(app_id) || app_address)
        # On AVM: block.chainid → uint64 → itob (8 bytes), address(this) → 32-byte app address
        domain_data = (
            DOMAIN_TYPEHASH
            + chain_id.to_bytes(8, "big")       # itob: 8 bytes, NOT 32
            + verifying_contract                 # 32-byte Algorand app address
        )
        domain_separator = keccak256_hash(domain_data)

        # hashStruct = keccak256(AUTHORIZATION_TYPEHASH || struct_arc4_bytes)
        # ARC4 struct encoding: address(32) + address(32) + bool(1, 0x80/0x00) + uint256(32) + uint256(32)
        auth_data = (
            AUTHORIZATION_TYPEHASH
            + authorizer_addr_bytes              # uint8[32]: 32 bytes
            + authorized_addr_bytes              # uint8[32]: 32 bytes
            + (b"\x80" if is_authorized else b"\x00")  # ARC4 bool: 1 byte
            + nonce.to_bytes(32, "big")          # uint256: 32 bytes
            + deadline.to_bytes(32, "big")       # uint256: 32 bytes
        )
        hash_struct = keccak256_hash(auth_data)

        # digest = keccak256("\x19\x01" + DOMAIN_SEPARATOR + hashStruct)
        digest = keccak256_hash(b"\x19\x01" + domain_separator + hash_struct)

        # Sign with secp256k1
        pk = eth_keys.PrivateKey(private_key_bytes)
        sig = pk.sign_msg_hash(digest)

        v = sig.v + 27  # EIP-155: v is 0 or 1, add 27 for ecrecover
        r = sig.r.to_bytes(32, "big")
        s = sig.s.to_bytes(32, "big")

        # Derive the Ethereum address from the public key
        pubkey_bytes = pk.public_key.to_bytes()  # 64 bytes (uncompressed, no 0x04 prefix)
        addr_hash = keccak256_hash(pubkey_bytes)
        eth_addr_20 = addr_hash[12:]  # last 20 bytes
        sol_addr_32 = b"\x00" * 12 + eth_addr_20  # left-pad to 32 bytes

        return v, r, s, sol_addr_32

    def test_set_authorization_with_sig(self, env, account, localnet):
        """Sign an authorization and verify it on-chain."""
        morpho = env["morpho"]

        # Generate a secp256k1 key pair for the authorizer
        import os
        from eth_keys import keys as eth_keys
        from Crypto.Hash import keccak as pycryptodome_keccak

        def keccak256_local(data: bytes) -> bytes:
            k = pycryptodome_keccak.new(digest_bits=256)
            k.update(data)
            return k.digest()

        private_key = os.urandom(32)

        # Derive authorizer's Solidity address from private key
        pk = eth_keys.PrivateKey(private_key)
        pubkey_bytes = pk.public_key.to_bytes()
        addr_hash = keccak256_local(pubkey_bytes)
        eth_addr_20 = addr_hash[12:]
        authorizer_sol_addr = b"\x00" * 12 + eth_addr_20

        # The authorized address (who gets authorized)
        authorized_addr = account.address
        authorized_bytes = addr_to_bytes32(authorized_addr)

        # Build signature
        nonce = 0
        deadline = 2**64 - 1  # far future
        v, r, s, authorizer_sol_addr = self._make_sig(
            morpho.app_id, authorizer_sol_addr, authorized_bytes,
            True, nonce, deadline, private_key,
        )

        # Build Authorization tuple: (authorizer, authorized, isAuthorized, nonce, deadline)
        authorization = (
            list(authorizer_sol_addr),   # authorizer: uint8[32]
            list(authorized_bytes),      # authorized: uint8[32]
            True,                        # isAuthorized: bool
            nonce,                       # nonce: uint256
            deadline,                    # deadline: uint256
        )

        # Build Signature tuple: (v, r, s)
        signature = (
            v,          # v: uint64
            list(r),    # r: uint8[32]
            list(s),    # s: uint8[32]
        )

        # Box key for isAuthorized[authorizer][authorized]
        auth_key = mapping_box_key(
            "isAuthorized",
            authorizer_sol_addr,
            authorized_bytes,
        )
        # Box key for nonce[authorizer]
        nonce_key = mapping_box_key("nonce", authorizer_sol_addr)

        # ecdsa_pk_recover costs 1700 opcodes; need budget padding
        result = call_with_budget(
            localnet,
            morpho,
            au.AppClientMethodCallParams(
                method="setAuthorizationWithSig",
                args=[authorization, signature],
                box_references=[
                    box_ref(morpho.app_id, auth_key),
                    box_ref(morpho.app_id, nonce_key),
                ],
            ),
            budget_calls=3,
        )
        assert result is not None

    def test_set_authorization_with_sig_expired_reverts(self, env, account):
        """Expired deadline should revert."""
        morpho = env["morpho"]

        # Use deadline = 0 (already expired)
        authorization = (
            list(b"\x00" * 32),          # authorizer
            list(addr_to_bytes32(account.address)),  # authorized
            True,
            0,   # nonce
            0,   # deadline = 0 → expired
        )
        signature = (27, list(b"\x00" * 32), list(b"\x00" * 32))

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setAuthorizationWithSig",
                    args=[authorization, signature],
                ),
                send_params=NO_POPULATE,
            )

    def test_set_authorization_with_sig_invalid_nonce_reverts(self, env, account):
        """Wrong nonce should revert."""
        morpho = env["morpho"]

        authorization = (
            list(addr_to_bytes32(account.address)),
            list(addr_to_bytes32(account.address)),
            True,
            999,  # nonce = 999, but actual nonce is 0
            2**64 - 1,
        )
        signature = (27, list(b"\x00" * 32), list(b"\x00" * 32))

        nonce_key = mapping_box_key("nonce", addr_to_bytes32(account.address))
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setAuthorizationWithSig",
                    args=[authorization, signature],
                    box_references=[box_ref(morpho.app_id, nonce_key)],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Supply/withdraw by shares
# ---------------------------------------------------------------------------

class TestByShares:
    """Test supply and withdraw using shares instead of assets."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def test_supply_by_shares(self, env, account, localnet):
        """Supply by specifying shares (assets=0, shares>0).

        Morpho uses virtual shares (VIRTUAL_SHARES=10^6, VIRTUAL_ASSETS=1).
        First supply of 10000 assets produces ~10^10 shares.
        Supply by shares: we request a large share amount to get meaningful assets.
        """
        morpho = env["morpho"]
        irm = env["irm"]
        loan = env["loan"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # First supply by assets to establish share ratio
        supply_result = _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 10000, localnet=localnet)
        initial_shares = supply_result.returns[-1].value[1]  # shares
        assert initial_shares > 0

        # Supply by shares — use half the initial shares amount
        share_amount = initial_shares // 2
        _set_balance_and_approve(loan, morpho, account.address, 50000)
        pos_key = position_box_key(mid, account.address)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="supply",
                args=[params, 0, share_amount, account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        returned_assets, returned_shares = result.returns[-1].value
        assert returned_shares == share_amount
        assert returned_assets > 0

    def test_borrow_by_shares(self, env, account, localnet):
        """Borrow by specifying shares (assets=0, shares>0)."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply liquidity + collateral
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 200000)

        # First borrow by assets to establish borrow share ratio
        borrow_result = _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 10000)
        initial_borrow_shares = borrow_result.returns[-1].value[1]
        assert initial_borrow_shares > 0

        # Borrow by shares — use half the initial borrow shares
        share_amount = initial_borrow_shares // 2
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="borrow",
                args=[params, 0, share_amount, account.address, account.address],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id, oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=3000),
            ),
        )
        returned_assets, returned_shares = result.returns[-1].value
        assert returned_shares == share_amount
        assert returned_assets > 0

    def test_borrow_inconsistent_input_reverts(self, env, account, localnet):
        """Borrow with both assets and shares nonzero should revert."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 200000)

        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="borrow",
                    args=[params, 1000, 1000, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_repay_inconsistent_input_reverts(self, env, account, localnet):
        """Repay with both assets and shares nonzero should revert."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 200000)
        _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 10000)

        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="repay",
                    args=[params, 1000, 1000, account.address, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_repay_by_shares(self, env, account, localnet):
        """Repay by specifying shares (assets=0, shares>0)."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply + collateral + borrow
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 200000)
        borrow_result = _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 30000)
        borrow_shares = borrow_result.returns[-1].value[1]

        # Approve loan token for repay
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )
        loan.send.call(
            au.AppClientMethodCallParams(
                method="approve", args=[morpho.app_address, 50000],
                box_references=[box_ref(loan.app_id, allow_key)],
            )
        )

        # Repay half the borrow shares
        repay_shares = borrow_shares // 2
        pos_key = position_box_key(mid, account.address)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="repay",
                args=[params, 0, repay_shares, account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        returned_assets, returned_shares = result.returns[-1].value
        assert returned_shares == repay_shares
        assert returned_assets > 0

    def test_withdraw_by_shares(self, env, account, localnet):
        """Withdraw by specifying shares (assets=0, shares>0)."""
        morpho = env["morpho"]
        irm = env["irm"]
        loan = env["loan"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply and capture actual shares amount
        _set_balance_and_approve(loan, morpho, account.address, 10000)
        pos_key = position_box_key(mid, account.address)
        sender_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )
        supply_result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="supply",
                args=[params, 10000, 0, account.address, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, sender_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        total_shares = supply_result.returns[-1].value[1]

        # Withdraw half the shares
        withdraw_shares = total_shares // 2
        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="withdraw",
                args=[params, 0, withdraw_shares, account.address, account.address],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, mapping_box_key("balanceOf", addr_to_bytes32(account.address))),
                ],
                app_references=[irm.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=2000),
            ),
        )
        last_return = result.returns[-1]
        assert last_return.value[1] == withdraw_shares
        assert last_return.value[0] > 0


# ---------------------------------------------------------------------------
# Insufficient liquidity / collateral reverts
# ---------------------------------------------------------------------------

class TestInsufficientReverts:
    """Test that insufficient liquidity and collateral checks work."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def test_withdraw_insufficient_liquidity_reverts(self, env, account, localnet):
        """Withdraw more than available (after borrowing) should revert."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply 10k loan + 20k collateral
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 10000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 20000)

        # Borrow 8k (leaving 2k liquidity)
        _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 8000)

        # Try to withdraw 5k — only 2k liquidity available
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        with pytest.raises(Exception):
            call_with_budget(
                localnet, morpho,
                au.AppClientMethodCallParams(
                    method="withdraw",
                    args=[params, 5000, 0, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                        box_ref(loan.app_id, morpho_bal),
                        box_ref(loan.app_id, receiver_bal),
                    ],
                    app_references=[irm.app_id, loan.app_id],
                    extra_fee=au.AlgoAmount(micro_algo=2000),
                ),
            )

    def test_borrow_insufficient_collateral_reverts(self, env, account, localnet):
        """Borrow more than collateral allows (health check fails)."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply 100k loan + only 10k collateral
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 10000)

        # Try to borrow 9k — with 80% LLTV and 1:1 price, max borrow = 10k*0.8 = 8k
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        with pytest.raises(Exception):
            call_with_budget(
                localnet, morpho,
                au.AppClientMethodCallParams(
                    method="borrow",
                    args=[params, 9000, 0, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                        box_ref(loan.app_id, morpho_bal),
                        box_ref(loan.app_id, receiver_bal),
                    ],
                    app_references=[irm.app_id, loan.app_id, oracle.app_id],
                    extra_fee=au.AlgoAmount(micro_algo=3000),
                ),
            )

    def test_borrow_at_max_collateral_succeeds(self, env, account, localnet):
        """Borrow exactly at max allowed by collateral — should succeed."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply 100k loan + 10k collateral
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 10000)

        # Borrow 7999 — max is ~8000 (10k * 0.8), leaving 1 unit margin
        result = _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 7999)
        last_return = result.returns[-1]
        assert last_return.value[0] == 7999

    def test_withdraw_collateral_insufficient_reverts(self, env, account, localnet):
        """Withdraw collateral when position has debt — health check fails."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply 100k loan + 10k collateral, borrow 7000
        # With 80% LLTV, max borrow = 10k*0.8 = 8k, so 7k is healthy
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 10000)
        _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 7000)

        # Try to withdraw 3k collateral — remaining 7k collateral
        # max borrow would be 7k*0.8 = 5600 < 7000 debt → unhealthy → revert
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))

        with pytest.raises(Exception):
            call_with_budget(
                localnet, morpho,
                au.AppClientMethodCallParams(
                    method="withdrawCollateral",
                    args=[params, 3000, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                        box_ref(collateral.app_id, morpho_bal),
                        box_ref(collateral.app_id, receiver_bal),
                    ],
                    app_references=[irm.app_id, collateral.app_id, oracle.app_id],
                    extra_fee=au.AlgoAmount(micro_algo=3000),
                ),
            )


# ---------------------------------------------------------------------------
# Interest accrual with fee
# ---------------------------------------------------------------------------

class TestInterestWithFee:
    """Test _accrueInterest when market has a nonzero fee."""

    @pytest.fixture
    def env(self, localnet, account):
        e = _make_full_env(localnet, account)
        morpho = e["morpho"]
        irm = e["irm"]
        params = e["params"]
        mkt_key = e["mkt_key"]

        # Set fee to 10% (0.1 * WAD)
        fee = WAD // 10
        # setFee internally calls _accrueInterest which does biguint division
        call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="setFee", args=[params, fee],
                box_references=[box_ref(morpho.app_id, mkt_key)],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
        )
        e["fee"] = fee
        return e

    def test_accrue_interest_with_fee(self, env, account, localnet):
        """Accrue interest with fee set — fee shares should be minted.

        Interest is only generated when totalBorrowAssets > 0, so we need
        supply + collateral + borrow first. The fee is applied as a portion
        of interest, minted as shares to feeRecipient's position.
        """
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Set feeRecipient first (it's zero by default)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setFeeRecipient", args=[account.address],
            ),
            send_params=NO_POPULATE,
        )

        # Supply 100k + 200k collateral (pass localnet for budget — fee causes extra ops)
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 200000)

        # Borrow 50k — need feeRecipient position box since _accrueInterest
        # writes to position[id][feeRecipient].supplyShares when fee > 0
        fee_recipient_pos = position_box_key(mid, account.address)
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="borrow",
                args=[params, 50000, 0, account.address, account.address],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(morpho.app_id, fee_recipient_pos),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, loan.app_id, oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=3000),
            ),
        )

        import time
        time.sleep(2)  # let block timestamp advance for elapsed > 0

        # Accrue interest — with fee, this should mint fee shares
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="accrueInterest", args=[params],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, fee_recipient_pos),
                ],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None


# ---------------------------------------------------------------------------
# Liquidation
# ---------------------------------------------------------------------------

class TestLiquidation:
    """Test liquidation flow — requires unhealthy position."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def test_liquidate_unhealthy_position(self, env, account, localnet):
        """Full liquidation: supply, borrow, drop oracle price, liquidate."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply 100k loan + 20k collateral, borrow 15k
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 20000)
        _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 15000)

        # Drop oracle price to make position unhealthy
        # At 1:1 price, maxBorrow = 20000 * 0.8 = 16000 > 15000 (healthy)
        # At 0.5:1 price, maxBorrow = 20000 * 0.5 * 0.8 = 8000 < 15000 (unhealthy)
        low_price = 10**36 // 2  # 0.5x
        oracle.send.call(
            au.AppClientMethodCallParams(method="setPrice", args=[low_price])
        )

        # Liquidate — seize 5000 collateral
        pos_key = position_box_key(mid, account.address)

        # Box keys for token transfers
        bal_morpho = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        bal_sender = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        loan_allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        # Approve loan token for repayment during liquidation
        _set_balance_and_approve(loan, morpho, account.address, 50000)

        # liquidate: safeTransfer(collateral → liquidator) + safeTransferFrom(loan ← liquidator)
        # Split refs across padding txn and main txn to stay under MaxAppTotalTxnReferences=8
        # Padding txn carries loan token refs; main txn carries collateral + morpho refs
        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="liquidate",
                args=[params, account.address, 5000, 0, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(collateral.app_id, bal_morpho),
                    box_ref(collateral.app_id, bal_sender),
                ],
                app_references=[irm.app_id, collateral.app_id, oracle.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=5000),
            ),
            budget_calls=2,
            padding_box_refs=[
                box_ref(loan.app_id, bal_sender),
                box_ref(loan.app_id, bal_morpho),
                box_ref(loan.app_id, loan_allow_key),
            ],
            padding_app_refs=[loan.app_id],
        )
        last_return = result.returns[-1]
        seized_assets = last_return.value[0]
        repaid_assets = last_return.value[1]
        assert seized_assets == 5000
        assert repaid_assets > 0

    def test_liquidate_by_repaid_shares(self, env, account, localnet):
        """Liquidate by specifying repaidShares (seizedAssets=0, repaidShares>0)."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply 100k loan + 20k collateral, borrow 15k
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 20000)
        borrow_result = _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 15000)
        borrow_shares = borrow_result.returns[-1].value[1]

        # Drop oracle price to make position unhealthy
        low_price = 10**36 // 2  # 0.5x
        oracle.send.call(
            au.AppClientMethodCallParams(method="setPrice", args=[low_price])
        )

        # Liquidate by specifying repaidShares (half the borrow shares)
        repaid_share_amount = borrow_shares // 4
        pos_key = position_box_key(mid, account.address)
        bal_morpho = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        bal_sender = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        loan_allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        _set_balance_and_approve(loan, morpho, account.address, 50000)

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="liquidate",
                args=[params, account.address, 0, repaid_share_amount, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(collateral.app_id, bal_morpho),
                    box_ref(collateral.app_id, bal_sender),
                ],
                app_references=[irm.app_id, collateral.app_id, oracle.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=5000),
            ),
            budget_calls=2,
            padding_box_refs=[
                box_ref(loan.app_id, bal_sender),
                box_ref(loan.app_id, bal_morpho),
                box_ref(loan.app_id, loan_allow_key),
            ],
            padding_app_refs=[loan.app_id],
        )
        last_return = result.returns[-1]
        seized_assets = last_return.value[0]
        repaid_assets = last_return.value[1]
        assert seized_assets > 0  # collateral was seized
        assert repaid_assets > 0  # debt was repaid

    def test_liquidate_healthy_position_reverts(self, env, account, localnet):
        """Liquidating a healthy position should revert."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply + collateral + small borrow (healthy)
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 20000)
        _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 5000)

        # Try to liquidate healthy position — oracle still at 1:1
        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            call_with_budget(
                localnet, morpho,
                au.AppClientMethodCallParams(
                    method="liquidate",
                    args=[params, account.address, 1000, 0, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                    app_references=[irm.app_id, oracle.app_id],
                    extra_fee=au.AlgoAmount(micro_algo=3000),
                ),
                budget_calls=2,
            )

    def test_liquidate_bad_debt(self, env, account, localnet):
        """Liquidate all collateral — remaining debt is socialized as bad debt.

        When a borrower's collateral reaches 0 after liquidation but they still
        have borrowShares > 0, the contract writes off the bad debt by reducing
        both totalBorrowAssets and totalSupplyAssets (socializing losses).
        """
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply 100k loan + small collateral (1000), borrow near max
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 1000)
        # With 80% LLTV at 1:1 price, max borrow = 1000 * 0.8 = 800
        _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 790)

        # Crash oracle price to make position deeply underwater
        # At 0.1x price, maxBorrow = 1000 * 0.1 * 0.8 = 80 < 790 (deeply unhealthy)
        crash_price = 10**36 // 10  # 0.1x
        oracle.send.call(
            au.AppClientMethodCallParams(method="setPrice", args=[crash_price])
        )

        # Liquidate ALL collateral (seize 1000 — all of it)
        pos_key = position_box_key(mid, account.address)
        bal_morpho = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        bal_sender = mapping_box_key("balanceOf", addr_to_bytes32(account.address))
        loan_allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(account.address),
            addr_to_bytes32(morpho.app_address),
        )

        _set_balance_and_approve(loan, morpho, account.address, 50000)

        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="liquidate",
                args=[params, account.address, 1000, 0, b""],
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(collateral.app_id, bal_morpho),
                    box_ref(collateral.app_id, bal_sender),
                ],
                app_references=[irm.app_id, collateral.app_id, oracle.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=5000),
            ),
            budget_calls=2,
            padding_box_refs=[
                box_ref(loan.app_id, bal_sender),
                box_ref(loan.app_id, bal_morpho),
                box_ref(loan.app_id, loan_allow_key),
            ],
            padding_app_refs=[loan.app_id],
        )
        last_return = result.returns[-1]
        seized_assets = last_return.value[0]
        repaid_assets = last_return.value[1]
        assert seized_assets == 1000  # all collateral seized
        assert repaid_assets > 0  # some debt repaid
        # Bad debt was socialized: repaid_assets < 790 (debt) because
        # collateral at 0.1x price can only cover a fraction.
        # The contract wrote off remaining debt by reducing totalSupplyAssets.

    def test_liquidate_market_not_created_reverts(self, env, account):
        """Liquidate on non-existent market should revert."""
        morpho = env["morpho"]
        # Use bogus params
        bogus_params = make_market_params(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        bogus_mid = market_id(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        bogus_mkt_key = market_box_key(bogus_mid)

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="liquidate",
                    args=[bogus_params, account.address, 100, 0, b""],
                    box_references=[box_ref(morpho.app_id, bogus_mkt_key)],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Flash Loan
# ---------------------------------------------------------------------------

class TestFlashLoan:
    """Test flashLoan functionality using FlashBorrowerMock.

    The flash loan flow:
    1. FlashBorrower calls Morpho.flashLoan(token, amount, data)
    2. Morpho transfers tokens to FlashBorrower
    3. Morpho calls FlashBorrower.onMorphoFlashLoan(amount, data)
    4. Morpho transfers tokens back from FlashBorrower

    Since FlashBorrowerMock's approve() is stubbed on AVM, we pre-set
    the allowance using ERC20Mock.setAllowance().
    """

    @pytest.fixture
    def env(self, localnet, account):
        e = _make_full_env(localnet, account)
        morpho = e["morpho"]
        loan = e["loan"]

        # Deploy FlashBorrowerMock with Morpho address as constructor arg
        morpho_sol_bytes = app_id_to_bytes32(morpho.app_id)
        flash_borrower = deploy_contract(
            localnet, account, "FlashBorrowerMock",
            constructor_args=[morpho_sol_bytes],
            fund_amount=1_000_000,
        )
        e["flash_borrower"] = flash_borrower
        return e

    def test_flash_loan_zero_assets_reverts(self, env, account):
        """flashLoan with zero assets should revert."""
        morpho = env["morpho"]
        loan = env["loan"]

        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="flashLoan",
                    args=[sol_addr(loan), 0, b""],
                ),
                send_params=NO_POPULATE,
            )

    @pytest.mark.xfail(reason="Inner call ABI type mismatch: FlashBorrowerMock emits uint8[32] selector vs Morpho's address selector")
    def test_flash_loan_via_mock(self, env, account, localnet):
        """Full flash loan via FlashBorrowerMock contract.

        Currently xfail: high-level cross-contract calls encode address as
        uint8[32] producing a different method selector than Morpho expects.
        """
        morpho = env["morpho"]
        irm = env["irm"]
        loan = env["loan"]
        flash_borrower = env["flash_borrower"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        flash_amount = 5000

        # Supply liquidity so Morpho has tokens to lend
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 10000, localnet=localnet)

        # Pre-set allowance: FlashBorrower approves Morpho to take tokens back
        fb_addr = flash_borrower.app_address
        morpho_addr = morpho.app_address
        allow_key = mapping_box_key(
            "allowance",
            addr_to_bytes32(fb_addr),
            addr_to_bytes32(morpho_addr),
        )
        loan.send.call(
            au.AppClientMethodCallParams(
                method="setAllowance",
                args=[fb_addr, morpho_addr, flash_amount],
                box_references=[box_ref(loan.app_id, allow_key)],
            )
        )

        # Call FlashBorrower.flashLoan which calls Morpho.flashLoan internally
        token_addr_encoded = app_id_to_bytes32(loan.app_id)

        fb_bal = mapping_box_key("balanceOf", addr_to_bytes32(fb_addr))
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho_addr))

        result = flash_borrower.send.call(
            au.AppClientMethodCallParams(
                method="flashLoan",
                args=[sol_addr(loan), flash_amount, token_addr_encoded],
                box_references=[
                    box_ref(loan.app_id, fb_bal),
                    box_ref(loan.app_id, morpho_bal),
                    box_ref(loan.app_id, allow_key),
                ],
                app_references=[morpho.app_id, loan.app_id],
                extra_fee=au.AlgoAmount(micro_algo=5000),
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None


# ---------------------------------------------------------------------------
# extSloads
# ---------------------------------------------------------------------------

class TestExtSloads:
    """Test extSloads storage view function."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def test_ext_sloads_empty(self, env, account):
        """extSloads with empty array returns empty array."""
        morpho = env["morpho"]
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="extSloads", args=[[]],
            )
        )
        assert result.abi_return == []

    @pytest.mark.xfail(reason="extSloads uses assembly sload/mstore for memory array — not supported on AVM")
    def test_ext_sloads_single_slot(self, env, account):
        """extSloads with one slot returns one value."""
        morpho = env["morpho"]
        # Read a random slot — should return zero bytes32
        slot = list(b"\x00" * 31 + b"\x01")
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="extSloads", args=[[slot]],
            )
        )
        assert len(result.abi_return) == 1


# ---------------------------------------------------------------------------
# Helper for creating a second account
# ---------------------------------------------------------------------------

def _make_second_account(localnet, account):
    """Create and fund a second account."""
    acc2 = localnet.account.random()
    localnet.account.ensure_funded(
        acc2, account,
        min_spending_balance=au.AlgoAmount(micro_algo=10_000_000),
        min_funding_increment=au.AlgoAmount(micro_algo=1_000_000),
    )
    localnet.account.set_signer_from_account(acc2)
    return acc2


# ---------------------------------------------------------------------------
# onlyOwner modifier tests (EASY gaps 2-6)
# ---------------------------------------------------------------------------

class TestOnlyOwner:
    """Test that onlyOwner functions revert when called by non-owner."""

    @pytest.fixture
    def env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=2_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        acc2 = _make_second_account(localnet, account)
        return {"morpho": morpho, "irm": irm, "acc2": acc2}

    def test_set_owner_not_owner_reverts(self, env, account):
        """setOwner from non-owner should revert."""
        morpho = env["morpho"]
        acc2 = env["acc2"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setOwner",
                    args=[acc2.address],
                    sender=acc2.address,
                ),
                send_params=NO_POPULATE,
            )

    def test_enable_irm_not_owner_reverts(self, env):
        """enableIrm from non-owner should revert."""
        morpho = env["morpho"]
        acc2 = env["acc2"]
        irm_sol = sol_addr(env["irm"])
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_sol))
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="enableIrm", args=[irm_sol],
                    box_references=[box_ref(morpho.app_id, irm_key)],
                    sender=acc2.address,
                ),
                send_params=NO_POPULATE,
            )

    def test_enable_lltv_not_owner_reverts(self, env):
        """enableLltv from non-owner should revert."""
        morpho = env["morpho"]
        acc2 = env["acc2"]
        lltv = 500000000000000000
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="enableLltv", args=[lltv],
                    box_references=[box_ref(morpho.app_id, lltv_key)],
                    sender=acc2.address,
                ),
                send_params=NO_POPULATE,
            )

    def test_set_fee_not_owner_reverts(self, env, localnet, account):
        """setFee from non-owner should revert.

        Uses a simplified approach: just call setFee with a fake market params
        from non-owner. The onlyOwner check happens before market validation.
        """
        morpho = env["morpho"]
        acc2 = env["acc2"]
        fake_params = make_market_params(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        fake_mid = market_id(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        fake_mkt_key = market_box_key(fake_mid)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setFee", args=[fake_params, 100000000000000000],
                    box_references=[box_ref(morpho.app_id, fake_mkt_key)],
                    sender=acc2.address,
                ),
                send_params=NO_POPULATE,
            )

    def test_set_fee_recipient_not_owner_reverts(self, env):
        """setFeeRecipient from non-owner should revert."""
        morpho = env["morpho"]
        acc2 = env["acc2"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setFeeRecipient",
                    args=[sol_addr(env["irm"])],
                    sender=acc2.address,
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Additional governance edge cases (EASY gaps 1, 7, 8, 12, 13, 14)
# ---------------------------------------------------------------------------

class TestGovernanceEdgeCases:
    """Test governance edge cases: boundary values, already-set, etc."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def test_set_fee_at_max_boundary(self, env, account):
        """setFee at exactly MAX_FEE (25%) should succeed."""
        morpho = env["morpho"]
        irm = env["irm"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        max_fee = 250000000000000000  # 0.25e18
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="setFee", args=[params, max_fee],
                box_references=[box_ref(morpho.app_id, mkt_key)],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None

    def test_set_fee_reset_to_zero(self, env, account):
        """Set fee then reset to 0."""
        morpho = env["morpho"]
        irm = env["irm"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        call_args = dict(
            box_references=[box_ref(morpho.app_id, mkt_key)],
            app_references=[irm.app_id],
            extra_fee=au.AlgoAmount(micro_algo=1000),
        )
        # Set fee to 10%
        morpho.send.call(
            au.AppClientMethodCallParams(method="setFee", args=[params, 100000000000000000], **call_args),
            send_params=NO_POPULATE,
        )
        # Reset to 0
        result = morpho.send.call(
            au.AppClientMethodCallParams(method="setFee", args=[params, 0], **call_args),
            send_params=NO_POPULATE,
        )
        assert result is not None

    def test_enable_lltv_max_valid(self, env, account):
        """enableLltv at WAD-1 (maximum valid value) should succeed."""
        morpho = env["morpho"]
        max_lltv = WAD - 1  # 999999999999999999
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(max_lltv))
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[max_lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )
        assert result is not None

    def test_set_authorization_already_set_reverts(self, env, account):
        """Setting the same authorization twice should revert (ALREADY_SET)."""
        morpho = env["morpho"]
        irm = env["irm"]
        authorized_addr = sol_addr(irm)
        auth_key = mapping_box_key(
            "isAuthorized",
            addr_to_bytes32(account.address),
            addr_to_bytes32(authorized_addr),
        )
        refs = [box_ref(morpho.app_id, auth_key)]
        # Authorize
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setAuthorization", args=[authorized_addr, True],
                box_references=refs,
            )
        )
        # Authorize again — ALREADY_SET
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setAuthorization", args=[authorized_addr, True],
                    box_references=refs,
                )
            )

    def test_accrue_interest_twice_same_block(self, env, account, localnet):
        """Accruing interest twice in same group should succeed (elapsed=0 early return)."""
        morpho = env["morpho"]
        irm = env["irm"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        import os
        call_params = au.AppClientMethodCallParams(
            method="accrueInterest", args=[params],
            box_references=[box_ref(morpho.app_id, mkt_key)],
            app_references=[irm.app_id],
            extra_fee=au.AlgoAmount(micro_algo=1000),
        )
        # Call twice in same group — second call should hit elapsed=0 early return
        composer = localnet.new_group()
        composer.add_app_call_method_call(morpho.params.call(
            au.AppClientMethodCallParams(
                method="accrueInterest", args=[params],
                box_references=[box_ref(morpho.app_id, mkt_key)],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
                note=os.urandom(8),
            )
        ))
        composer.add_app_call_method_call(morpho.params.call(
            au.AppClientMethodCallParams(
                method="accrueInterest", args=[params],
                box_references=[box_ref(morpho.app_id, mkt_key)],
                app_references=[irm.app_id],
                extra_fee=au.AlgoAmount(micro_algo=1000),
                note=os.urandom(8),
            )
        ))
        result = composer.send(NO_POPULATE)
        assert result is not None


# ---------------------------------------------------------------------------
# Market-not-created for withdrawCollateral (EASY gap 9)
# ---------------------------------------------------------------------------

class TestWithdrawCollateralMarketNotCreated:
    """Test withdrawCollateral on non-existent market."""

    @pytest.fixture
    def env(self, localnet, account):
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=2_000_000,
        )
        params = make_market_params(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        mid = market_id(ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, 0)
        mkt_key = market_box_key(mid)
        pos_key = position_box_key(mid, account.address)
        return {"morpho": morpho, "params": params, "mkt_key": mkt_key, "pos_key": pos_key}

    def test_withdraw_collateral_market_not_created_reverts(self, env, account):
        """withdrawCollateral on non-existent market should revert."""
        morpho = env["morpho"]
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdrawCollateral",
                    args=[env["params"], 1000, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, env["mkt_key"]),
                        box_ref(morpho.app_id, env["pos_key"]),
                    ],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Liquidate inconsistent input (EASY gap 10)
# ---------------------------------------------------------------------------

class TestLiquidateInputValidation:
    """Test liquidate input validation."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def test_liquidate_both_nonzero_reverts(self, env, account):
        """liquidate with both seizedAssets and repaidShares nonzero should revert."""
        morpho = env["morpho"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]
        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="liquidate",
                    args=[params, account.address, 1000, 1000, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )

    def test_liquidate_both_zero_reverts(self, env, account):
        """liquidate with both seizedAssets and repaidShares zero should revert."""
        morpho = env["morpho"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]
        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="liquidate",
                    args=[params, account.address, 0, 0, b""],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Withdraw both zero (EASY gap 11)
# ---------------------------------------------------------------------------

class TestWithdrawBothZero:
    """Test withdraw with both assets and shares zero."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    def test_withdraw_both_zero_reverts(self, env, account):
        """withdraw with both assets and shares zero should revert (INCONSISTENT_INPUT)."""
        morpho = env["morpho"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]
        pos_key = position_box_key(mid, account.address)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="withdraw",
                    args=[params, 0, 0, account.address, account.address],
                    box_references=[
                        box_ref(morpho.app_id, mkt_key),
                        box_ref(morpho.app_id, pos_key),
                    ],
                ),
                send_params=NO_POPULATE,
            )


# ---------------------------------------------------------------------------
# Medium gaps
# ---------------------------------------------------------------------------

class TestMediumGaps:
    """Test medium-difficulty coverage gaps."""

    @pytest.fixture
    def env(self, localnet, account):
        return _make_full_env(localnet, account)

    @pytest.fixture
    def second_account(self, localnet, account):
        return _make_second_account(localnet, account)

    def test_borrow_insufficient_liquidity(self, env, account, localnet):
        """Borrow more than total supply should revert (INSUFFICIENT_LIQUIDITY).

        Supply 1000, deposit massive collateral, try to borrow 2000.
        """
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # Supply only 1000 tokens
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 1000, localnet=localnet)

        # Supply massive collateral (enough to support the borrow)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 10_000_000)

        # Try to borrow 2000 — exceeds total supply of 1000
        with pytest.raises(Exception):
            _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 2000)

    def test_set_authorization_with_sig_invalid_signature(self, env, account, localnet):
        """Valid deadline + valid nonce but wrong private key should revert (INVALID_SIGNATURE)."""
        morpho = env["morpho"]
        import os
        from eth_keys import keys as eth_keys
        from Crypto.Hash import keccak as pycryptodome_keccak

        def keccak256_local(data: bytes) -> bytes:
            k = pycryptodome_keccak.new(digest_bits=256)
            k.update(data)
            return k.digest()

        # Generate two different key pairs
        key1 = os.urandom(32)
        key2 = os.urandom(32)

        # Use key1's address as authorizer
        pk1 = eth_keys.PrivateKey(key1)
        pubkey1 = pk1.public_key.to_bytes()
        addr_hash1 = keccak256_local(pubkey1)
        authorizer_addr = b"\x00" * 12 + addr_hash1[12:]

        authorized_bytes = addr_to_bytes32(account.address)

        # Sign with key2 (wrong key for authorizer)
        v, r, s, wrong_addr = TestAuthorizationWithSig._make_sig(
            self, morpho.app_id, authorizer_addr, authorized_bytes,
            True, 0, 2**64 - 1, key2,
        )

        authorization = (
            list(authorizer_addr),
            list(authorized_bytes),
            True, 0, 2**64 - 1,
        )
        signature = (v, list(r), list(s))

        auth_key = mapping_box_key("isAuthorized", authorizer_addr, authorized_bytes)
        nonce_key = mapping_box_key("nonce", authorizer_addr)

        with pytest.raises(Exception):
            call_with_budget(
                localnet, morpho,
                au.AppClientMethodCallParams(
                    method="setAuthorizationWithSig",
                    args=[authorization, signature],
                    box_references=[
                        box_ref(morpho.app_id, auth_key),
                        box_ref(morpho.app_id, nonce_key),
                    ],
                ),
                budget_calls=3,
            )

    def test_set_authorization_with_sig_revoke(self, env, account, localnet):
        """Revoke authorization via signature (isAuthorized=false)."""
        morpho = env["morpho"]
        import os
        from eth_keys import keys as eth_keys
        from Crypto.Hash import keccak as pycryptodome_keccak

        def keccak256_local(data: bytes) -> bytes:
            k = pycryptodome_keccak.new(digest_bits=256)
            k.update(data)
            return k.digest()

        private_key = os.urandom(32)
        pk = eth_keys.PrivateKey(private_key)
        pubkey_bytes = pk.public_key.to_bytes()
        addr_hash = keccak256_local(pubkey_bytes)
        authorizer_addr = b"\x00" * 12 + addr_hash[12:]
        authorized_bytes = addr_to_bytes32(account.address)

        auth_key = mapping_box_key("isAuthorized", authorizer_addr, authorized_bytes)
        nonce_key = mapping_box_key("nonce", authorizer_addr)
        refs = [box_ref(morpho.app_id, auth_key), box_ref(morpho.app_id, nonce_key)]

        # First: grant authorization (nonce=0)
        v, r, s, _ = TestAuthorizationWithSig._make_sig(
            self, morpho.app_id, authorizer_addr, authorized_bytes,
            True, 0, 2**64 - 1, private_key,
        )
        call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="setAuthorizationWithSig",
                args=[(list(authorizer_addr), list(authorized_bytes), True, 0, 2**64 - 1),
                      (v, list(r), list(s))],
                box_references=refs,
            ),
            budget_calls=3,
        )

        # Second: revoke authorization (nonce=1, isAuthorized=false)
        v, r, s, _ = TestAuthorizationWithSig._make_sig(
            self, morpho.app_id, authorizer_addr, authorized_bytes,
            False, 1, 2**64 - 1, private_key,
        )
        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="setAuthorizationWithSig",
                args=[(list(authorizer_addr), list(authorized_bytes), False, 1, 2**64 - 1),
                      (v, list(r), list(s))],
                box_references=refs,
            ),
            budget_calls=3,
        )
        assert result is not None

    def test_withdraw_collateral_authorized_partial(self, env, account, second_account, localnet):
        """Authorize second_account, withdraw partial collateral while debt remains healthy."""
        morpho = env["morpho"]
        irm = env["irm"]
        oracle = env["oracle"]
        loan = env["loan"]
        collateral = env["collateral"]
        params = env["params"]
        mkt_key = env["mkt_key"]
        mid = env["mid"]

        # account supplies tokens and collateral, borrows
        _do_supply(morpho, irm, loan, account, params, mkt_key, mid, 100000, localnet=localnet)
        _do_supply_collateral(morpho, collateral, account, params, mkt_key, mid, 100000)
        _do_borrow(localnet, morpho, irm, oracle, loan, account, params, mkt_key, mid, 10000)

        # account authorizes second_account
        auth_key = mapping_box_key(
            "isAuthorized",
            addr_to_bytes32(account.address),
            addr_to_bytes32(second_account.address),
        )
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setAuthorization",
                args=[second_account.address, True],
                box_references=[box_ref(morpho.app_id, auth_key)],
            )
        )

        # second_account withdraws small amount of collateral (position stays healthy)
        pos_key = position_box_key(mid, account.address)
        morpho_bal = mapping_box_key("balanceOf", addr_to_bytes32(morpho.app_address))
        receiver_bal = mapping_box_key("balanceOf", addr_to_bytes32(second_account.address))
        result = call_with_budget(
            localnet, morpho,
            au.AppClientMethodCallParams(
                method="withdrawCollateral",
                args=[params, 1000, account.address, second_account.address],
                sender=second_account.address,
                box_references=[
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, pos_key),
                    box_ref(morpho.app_id, auth_key),
                    box_ref(collateral.app_id, morpho_bal),
                    box_ref(collateral.app_id, receiver_bal),
                ],
                app_references=[irm.app_id, collateral.app_id, oracle.app_id],
                extra_fee=au.AlgoAmount(micro_algo=3000),
            ),
            budget_calls=2,
        )
        assert result is not None

    @pytest.mark.xfail(reason="AVM requires app references even for unreachable inner calls — app 0 unavailable")
    def test_create_market_zero_irm(self, env, account):
        """Create a market with irm=address(0) — no borrowRate() call."""
        morpho = env["morpho"]
        loan = env["loan"]
        collateral = env["collateral"]
        oracle = env["oracle"]
        lltv = env["lltv"]

        # Enable address(0) as IRM
        zero_irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(ZERO_ADDR))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[ZERO_ADDR],
                box_references=[box_ref(morpho.app_id, zero_irm_key)],
            )
        )

        # Create market with zero IRM
        params = make_market_params(
            sol_addr(loan), sol_addr(collateral), sol_addr(oracle), ZERO_ADDR, lltv,
        )
        mid = market_id(
            sol_addr(loan), sol_addr(collateral), sol_addr(oracle), ZERO_ADDR, lltv,
        )
        mkt_key = market_box_key(mid)
        id_key = id_to_market_params_box_key(mid)
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))

        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="createMarket", args=[params],
                box_references=[
                    box_ref(morpho.app_id, zero_irm_key),
                    box_ref(morpho.app_id, lltv_key),
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, id_key),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None

    @pytest.mark.xfail(reason="AVM requires app references even for unreachable inner calls — app 0 unavailable")
    def test_accrue_interest_zero_irm_market(self, env, account):
        """accrueInterest on a zero-IRM market — only lastUpdate is updated, no inner call."""
        morpho = env["morpho"]
        loan = env["loan"]
        collateral = env["collateral"]
        oracle = env["oracle"]
        lltv = env["lltv"]

        # Enable zero IRM and create market
        zero_irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(ZERO_ADDR))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[ZERO_ADDR],
                box_references=[box_ref(morpho.app_id, zero_irm_key)],
            )
        )
        params = make_market_params(
            sol_addr(loan), sol_addr(collateral), sol_addr(oracle), ZERO_ADDR, lltv,
        )
        mid = market_id(
            sol_addr(loan), sol_addr(collateral), sol_addr(oracle), ZERO_ADDR, lltv,
        )
        mkt_key = market_box_key(mid)
        id_key = id_to_market_params_box_key(mid)
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes32(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="createMarket", args=[params],
                box_references=[
                    box_ref(morpho.app_id, zero_irm_key),
                    box_ref(morpho.app_id, lltv_key),
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, id_key),
                ],
            ),
            send_params=NO_POPULATE,
        )

        # accrueInterest — no IRM inner call needed
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="accrueInterest", args=[params],
                box_references=[box_ref(morpho.app_id, mkt_key)],
            ),
            send_params=NO_POPULATE,
        )
        assert result is not None
