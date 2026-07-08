"""LocalNet client wrapper. One client per pytest session."""
from __future__ import annotations

import algokit_utils as au


class LocalNet:
    """Lightweight wrapper exposing (client, account) plus the raw algod.

    Also lazy-deploys a tiny "budget helper" app whose only purpose is to
    return immediately; dummy calls to it in a transaction group give the
    real method call extra opcode budget (each inner txn at min-fee adds
    700 opcodes). Exposed as `localnet.budget_helper_id`.
    """

    def __init__(self) -> None:
        algod = au.ClientManager.get_algod_client(
            au.ClientManager.get_default_localnet_config("algod")
        )
        kmd = au.ClientManager.get_kmd_client(
            au.ClientManager.get_default_localnet_config("kmd")
        )
        self.client = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
        # Cache suggested params for 60s: every fee field is overridden manually
        # (flat_fee + explicit fee at all call/deploy sites) and the validity
        # window is ~1000 rounds, so a 60s-stale first-round is harmless. At 0
        # the suite made ~10k redundant GET /params round-trips (6-8 per test).
        # Duplicate-txid collisions from identical cached params are prevented
        # by random notes on every txn (payments got theirs with this change).
        self.client.set_suggested_params_cache_timeout(60)
        self.account = self.client.account.localnet_dispenser()
        self.client.account.set_signer_from_account(self.account)
        self._budget_helper_id: int | None = None

    @property
    def algod(self):
        return self.client.client.algod

    @property
    def budget_helper_id(self) -> int:
        """App id of the shared opcode-budget helper app. Deployed on first use."""
        if self._budget_helper_id is None:
            from algosdk import encoding
            from algosdk.transaction import (
                ApplicationCreateTxn,
                OnComplete,
                StateSchema,
                wait_for_confirmation,
            )

            algod = self.algod
            approval = encoding.base64.b64decode(
                algod.compile("#pragma version 10\nint 1")["result"]
            )
            clear = approval
            sp = algod.suggested_params()
            txn = ApplicationCreateTxn(
                sender=self.account.address,
                sp=sp,
                on_complete=OnComplete.NoOpOC,
                approval_program=approval,
                clear_program=clear,
                global_schema=StateSchema(num_uints=0, num_byte_slices=0),
                local_schema=StateSchema(num_uints=0, num_byte_slices=0),
            )
            txid = algod.send_transaction(txn.sign(self.account.private_key))
            result = wait_for_confirmation(algod, txid, 4)
            self._budget_helper_id = result["application-index"]
        return self._budget_helper_id
