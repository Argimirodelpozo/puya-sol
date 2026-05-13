"""LocalNet client wrapper. One client per pytest session."""
from __future__ import annotations

import algokit_utils as au


class LocalNet:
    """Lightweight wrapper exposing (client, account) plus the raw algod."""

    def __init__(self) -> None:
        algod = au.ClientManager.get_algod_client(
            au.ClientManager.get_default_localnet_config("algod")
        )
        kmd = au.ClientManager.get_kmd_client(
            au.ClientManager.get_default_localnet_config("kmd")
        )
        self.client = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
        self.client.set_suggested_params_cache_timeout(0)
        self.account = self.client.account.localnet_dispenser()
        self.client.account.set_signer_from_account(self.account)

    @property
    def algod(self):
        return self.client.client.algod
