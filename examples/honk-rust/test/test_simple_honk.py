"""
Tests for the SimpleHonkVerifier with Rust test vectors.

Verifies the UltraHonk verifier (40 entities, N=32 circuit) compiled to TEAL
can verify a valid proof from the miquelcabot/ultrahonk_verifier Rust crate.
"""
from pathlib import Path
import json
import base64

from algosdk import encoding, transaction, abi
from algosdk.v2client.algod import AlgodClient
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer,
    AccountTransactionSigner,
)
import algokit_utils as au
import pytest

OUT_DIR = Path(__file__).parent.parent / "out" / "SimpleHonkVerifierTest"


def get_all_contract_names() -> list[str]:
    """Discover all compiled contract names from arc56 files in OUT_DIR."""
    names = []
    for arc56_file in sorted(OUT_DIR.glob("*.arc56.json")):
        name = arc56_file.stem.replace(".arc56", "")
        names.append(name)
    return names


class TestCompilation:
    """Verify TEAL files compile to valid AVM bytecode."""

    def test_teal_files_exist(self):
        """At least one contract's TEAL files should exist."""
        names = get_all_contract_names()
        assert len(names) > 0, f"No compiled contracts found in {OUT_DIR}"

    def test_all_teal_compiles(self, algod_client: AlgodClient):
        """Every TEAL file should compile without errors."""
        for name in get_all_contract_names():
            approval_path = OUT_DIR / f"{name}.approval.teal"
            clear_path = OUT_DIR / f"{name}.clear.teal"
            assert approval_path.exists(), f"Missing {approval_path}"
            assert clear_path.exists(), f"Missing {clear_path}"
            algod_client.compile(approval_path.read_text())
            algod_client.compile(clear_path.read_text())

    def test_arc56_has_verify_method(self):
        """The main contract should expose a verify() method."""
        arc56_path = OUT_DIR / "SimpleHonkVerifierTest.arc56.json"
        assert arc56_path.exists(), f"Missing {arc56_path}"
        data = json.loads(arc56_path.read_text())
        method_names = [m["name"] for m in data["methods"]]
        assert "verify" in method_names, (
            f"Missing verify method. Has: {method_names}"
        )


class TestVerification:
    """Deploy and verify the Honk proof via simulate."""

    @pytest.fixture(scope="class")
    def deployed_app(
        self,
        algod_client: AlgodClient,
        account: au.models.account.SigningAccount,
    ) -> int:
        """Deploy the contract and return app_id."""
        approval_path = OUT_DIR / "SimpleHonkVerifierTest.approval.teal"
        clear_path = OUT_DIR / "SimpleHonkVerifierTest.clear.teal"

        approval_result = algod_client.compile(approval_path.read_text())
        clear_result = algod_client.compile(clear_path.read_text())

        approval_bytes = base64.b64decode(approval_result["result"])
        clear_bytes = base64.b64decode(clear_result["result"])

        # Calculate extra pages needed for large bytecode
        approval_pages = (len(approval_bytes) + 8191) // 8192
        extra_pages = min(3, max(0, approval_pages - 1))

        sp = algod_client.suggested_params()
        txn = transaction.ApplicationCreateTxn(
            sender=account.address,
            sp=sp,
            on_complete=transaction.OnComplete.NoOpOC,
            approval_program=approval_bytes,
            clear_program=clear_bytes,
            global_schema=transaction.StateSchema(0, 0),
            local_schema=transaction.StateSchema(0, 0),
            extra_pages=extra_pages,
        )
        signed_txn = txn.sign(account.private_key)
        tx_id = algod_client.send_transaction(signed_txn)
        result = transaction.wait_for_confirmation(algod_client, tx_id, 4)
        app_id = result["application-index"]

        # Fund the app account
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
        )
        fund_txn = transaction.PaymentTxn(
            sender=account.address,
            sp=sp,
            receiver=app_addr,
            amt=1_000_000,
        )
        signed_fund = fund_txn.sign(account.private_key)
        algod_client.send_transaction(signed_fund)
        transaction.wait_for_confirmation(
            algod_client, signed_fund.get_txid(), 4
        )

        return app_id

    def test_deploy(self, deployed_app: int):
        """Contract deploys successfully."""
        assert deployed_app > 0

    def test_verify_valid_proof(
        self,
        algod_client: AlgodClient,
        account: au.models.account.SigningAccount,
        deployed_app: int,
    ):
        """Valid proof should verify successfully via simulate."""
        method = abi.Method.from_signature("verify()bool")
        sp = algod_client.suggested_params()
        sp.flat_fee = True
        sp.fee = 1000

        atc = AtomicTransactionComposer()
        signer = AccountTransactionSigner(account.private_key)
        atc.add_method_call(
            app_id=deployed_app,
            method=method,
            sender=account.address,
            sp=sp,
            signer=signer,
        )

        # Simulate with extra opcode budget for EC operations
        # ~170k opcodes needed: 70 ecMul + ecAdd + pairing
        sim_request = transaction.SimulateRequest(
            txn_groups=[],
            allow_more_logging=True,
            extra_opcode_budget=320000,
        )
        result = atc.simulate(algod_client, sim_request)

        # Check no failure
        assert not hasattr(result, "failure_message") or result.failure_message is None, (
            f"Simulation failed: {getattr(result, 'failure_message', 'unknown')}"
        )

        # Check return value is true
        method_results = result.abi_results
        assert len(method_results) > 0, "No method results returned"
        assert method_results[0].return_value is True, (
            f"verify() returned {method_results[0].return_value}, expected True"
        )
