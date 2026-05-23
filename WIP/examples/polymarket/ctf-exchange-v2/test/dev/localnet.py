"""Localnet fixture factories. Top-level conftest.py wires these as
session/function-scoped pytest fixtures."""
import algokit_utils as au
from algosdk.transaction import PaymentTxn, wait_for_confirmation


def make_algod_client():
    return au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod"))


def make_kmd_client():
    return au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd"))


def make_localnet_clients(algod_client, kmd_client):
    return au.AlgoSdkClients(algod=algod_client, kmd=kmd_client)


def make_admin_account(localnet_clients):
    return au.AlgorandClient(localnet_clients).account.localnet_dispenser()


def make_localnet(localnet_clients, admin):
    client = au.AlgorandClient(localnet_clients)
    client.set_suggested_params_cache_timeout(0)
    client.account.set_signer_from_account(admin)
    return client


def fund_random_account(localnet, admin, amount: int = 1_000_000):
    """Create a fresh signing account on localnet, fund it from admin."""
    acct = localnet.account.random()
    algod = localnet.client.algod
    sp = algod.suggested_params()
    pay = PaymentTxn(admin.address, sp, acct.address, amount)
    txid = algod.send_transaction(pay.sign(admin.private_key))
    wait_for_confirmation(algod, txid, 4)
    return acct
