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
    int_to_bytes64,
)

pytestmark = pytest.mark.localnet

WAD = 10**18  # 1e18 — Morpho's fixed-point unit


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
    return hashlib.sha256(data).digest()


def make_market_params(loan_token: str, collateral_token: str,
                       oracle: str, irm: str, lltv: int) -> tuple:
    """Build a MarketParams tuple for ABI calls.

    MarketParams = (loanToken:uint8[32], collateralToken:uint8[32],
                    oracle:uint8[32], irm:uint8[32], lltv:uint512)
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
        irm_addr = irm.app_address
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_addr))
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm",
                args=[irm_addr],
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
        irm_addr = irm.app_address
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm_addr))
        refs = [box_ref(morpho.app_id, irm_key)]
        # Enable once
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm_addr], box_references=refs,
            )
        )
        # Enable again — should revert
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="enableIrm", args=[irm_addr], box_references=refs,
                )
            )

    def test_enable_lltv(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        lltv = 800000000000000000  # 80% = 0.8e18
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(lltv))
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
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(lltv))
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
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(lltv))
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
                args=[irm.app_address],
            )
        )
        assert result is not None

    def test_set_fee_recipient_already_set_reverts(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        irm = morpho_env["irm"]
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="setFeeRecipient", args=[irm.app_address],
            )
        )
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="setFeeRecipient", args=[irm.app_address],
                )
            )

    def test_set_authorization(self, morpho_env, account):
        morpho = morpho_env["morpho"]
        irm = morpho_env["irm"]
        authorized_addr = irm.app_address
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
        authorized_addr = oracle.app_address
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

    Uses irm=address(0) to skip the IRM.borrowRate() inner transaction call
    during createMarket. Morpho allows this: if irm==address(0), the borrowRate
    call is skipped entirely (Morpho.sol line 163).
    """

    @pytest.fixture
    def env(self, localnet, account):
        """Deploy full environment with IRM(address(0))/LLTV pre-enabled."""
        owner_bytes = addr_to_bytes32(account.address)
        morpho = deploy_contract(
            localnet, account, "Morpho", subdir="MorphoTest",
            constructor_args=[owner_bytes],
            fund_amount=2_000_000,
        )
        irm = deploy_contract(localnet, account, "IrmMock")
        oracle = deploy_contract(localnet, account, "OracleMock")
        loan = deploy_contract(localnet, account, "ERC20Mock")
        collateral = deploy_contract(localnet, account, "ERC20Mock")

        lltv = 800000000000000000  # 80%

        # Enable address(0) as IRM (allows skipping borrowRate inner call)
        zero_irm_key = mapping_box_key("isIrmEnabled", ZERO_BYTES32)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[ZERO_ADDR],
                box_references=[box_ref(morpho.app_id, zero_irm_key)],
            )
        )

        # Also enable real IRM for error-case tests
        irm_key = mapping_box_key("isIrmEnabled", addr_to_bytes32(irm.app_address))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[irm.app_address],
                box_references=[box_ref(morpho.app_id, irm_key)],
            )
        )

        # Enable LLTV
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(lltv))
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

    def _market_refs(self, env, irm_addr=None):
        """Compute all box references needed for createMarket."""
        morpho = env["morpho"]
        if irm_addr is None:
            irm_addr = ZERO_ADDR
        irm_bytes = ZERO_BYTES32 if irm_addr == ZERO_ADDR else addr_to_bytes32(irm_addr)
        mid = market_id(
            env["loan"].app_address,
            env["collateral"].app_address,
            env["oracle"].app_address,
            irm_addr,
            env["lltv"],
        )
        irm_key = mapping_box_key("isIrmEnabled", irm_bytes)
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(env["lltv"]))
        mkt_key = market_box_key(mid)
        id_key = id_to_market_params_box_key(mid)
        return [
            box_ref(morpho.app_id, irm_key),
            box_ref(morpho.app_id, lltv_key),
            box_ref(morpho.app_id, mkt_key),
            box_ref(morpho.app_id, id_key),
        ]

    def test_create_market(self, env, account):
        morpho = env["morpho"]
        params = make_market_params(
            env["loan"].app_address,
            env["collateral"].app_address,
            env["oracle"].app_address,
            ZERO_ADDR,  # zero IRM skips borrowRate inner call
            env["lltv"],
        )
        refs = self._market_refs(env)
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="createMarket", args=[params],
                box_references=refs,
            )
        )
        assert result is not None

    def test_create_market_duplicate_reverts(self, env, account):
        """Creating the same market twice should revert."""
        morpho = env["morpho"]
        params = make_market_params(
            env["loan"].app_address,
            env["collateral"].app_address,
            env["oracle"].app_address,
            ZERO_ADDR,
            env["lltv"],
        )
        refs = self._market_refs(env)
        # Create once
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="createMarket", args=[params],
                box_references=refs,
            )
        )
        # Create again — should revert
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="createMarket", args=[params],
                    box_references=refs,
                )
            )

    def test_create_market_irm_not_enabled_reverts(self, env, account):
        """Creating a market with a non-enabled IRM should revert."""
        morpho = env["morpho"]
        # Use oracle address as fake IRM (not enabled)
        fake_irm = env["oracle"].app_address
        params = make_market_params(
            env["loan"].app_address,
            env["collateral"].app_address,
            env["oracle"].app_address,
            fake_irm,
            env["lltv"],
        )
        refs = self._market_refs(env, irm_addr=fake_irm)
        with pytest.raises(Exception):
            morpho.send.call(
                au.AppClientMethodCallParams(
                    method="createMarket", args=[params],
                    box_references=refs,
                )
            )

    def test_create_market_lltv_not_enabled_reverts(self, env, account):
        """Creating a market with a non-enabled LLTV should revert."""
        morpho = env["morpho"]
        bad_lltv = 900000000000000000  # 90%, not enabled
        params = make_market_params(
            env["loan"].app_address,
            env["collateral"].app_address,
            env["oracle"].app_address,
            ZERO_ADDR,
            bad_lltv,
        )
        mid = market_id(
            env["loan"].app_address,
            env["collateral"].app_address,
            env["oracle"].app_address,
            ZERO_ADDR,
            bad_lltv,
        )
        irm_key = mapping_box_key("isIrmEnabled", ZERO_BYTES32)
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(bad_lltv))
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
                )
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

        # Enable address(0) as IRM (skips borrowRate inner call)
        zero_irm_key = mapping_box_key("isIrmEnabled", ZERO_BYTES32)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[ZERO_ADDR],
                box_references=[box_ref(morpho.app_id, zero_irm_key)],
            )
        )

        # Enable LLTV
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        # Create market with zero IRM
        mid = market_id(
            loan.app_address, collateral.app_address,
            oracle.app_address, ZERO_ADDR, lltv,
        )
        params = make_market_params(
            loan.app_address, collateral.app_address,
            oracle.app_address, ZERO_ADDR, lltv,
        )
        mkt_key = market_box_key(mid)
        id_key = id_to_market_params_box_key(mid)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="createMarket", args=[params],
                box_references=[
                    box_ref(morpho.app_id, zero_irm_key),
                    box_ref(morpho.app_id, lltv_key),
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, id_key),
                ],
            )
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
        params = market_env["params"]
        mkt_key = market_env["mkt_key"]
        result = morpho.send.call(
            au.AppClientMethodCallParams(
                method="accrueInterest",
                args=[params],
                box_references=[box_ref(morpho.app_id, mkt_key)],
            )
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
        """Deploy everything and create a market with zero IRM."""
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

        # Enable address(0) as IRM
        zero_irm_key = mapping_box_key("isIrmEnabled", ZERO_BYTES32)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableIrm", args=[ZERO_ADDR],
                box_references=[box_ref(morpho.app_id, zero_irm_key)],
            )
        )
        lltv_key = mapping_box_key("isLltvEnabled", int_to_bytes64(lltv))
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="enableLltv", args=[lltv],
                box_references=[box_ref(morpho.app_id, lltv_key)],
            )
        )

        # Create market with zero IRM
        mid = market_id(
            loan.app_address, collateral.app_address,
            oracle.app_address, ZERO_ADDR, lltv,
        )
        params = make_market_params(
            loan.app_address, collateral.app_address,
            oracle.app_address, ZERO_ADDR, lltv,
        )
        mkt_key = market_box_key(mid)
        id_key = id_to_market_params_box_key(mid)
        morpho.send.call(
            au.AppClientMethodCallParams(
                method="createMarket", args=[params],
                box_references=[
                    box_ref(morpho.app_id, zero_irm_key),
                    box_ref(morpho.app_id, lltv_key),
                    box_ref(morpho.app_id, mkt_key),
                    box_ref(morpho.app_id, id_key),
                ],
            )
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
