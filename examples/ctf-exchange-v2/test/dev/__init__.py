"""Shared test utilities for ctf-exchange v2 Python tests.

Modules:
  addrs    — addr(), app_id_to_address(), algod_addr_for_app()
  arc56    — Arc56Contract loader, TEAL compile, memory-init injection
  deploy   — deploy_app(), create_app() — generic puya-sol contract deployers
  localnet — algod/kmd/localnet/admin/funded_account fixtures (re-exported via conftest)
  signing  — EOA ECDSA helpers (eth-style r‖s‖v) for order signing
  orders   — Order ABI tuple + canonical-hash helper

Per-category fixtures (collateral, adapters, exchange) live in the test
sub-packages' own conftest.py and import from here.
"""
