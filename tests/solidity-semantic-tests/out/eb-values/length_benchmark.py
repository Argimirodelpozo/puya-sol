"""Measure existing compiled array-length programs; no compiler invocation."""

import json
from pathlib import Path
import sys

from algosdk.atomic_transaction_composer import AtomicTransactionComposer
from algosdk.v2client.models import SimulateRequest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from framework.call import call
from framework.deploy import deploy
from framework.localnet import LocalNet

directory = Path(sys.argv[1]).resolve()
name = "NamedArrayLengths"
localnet = LocalNet()
app = deploy(localnet, {"arc56": directory / (name + ".arc56.json"),
                       "approval_teal": directory / (name + ".approval.teal"),
                       "clear_teal": directory / (name + ".clear.teal")}, fund_wei=30_000_000)
method = next(m.to_abi_method() for m in app.app_spec.methods if m.name == "lengths")
rows = []
for operation, expected in [(None, 0)] + [("push", n) for n in range(1, 10)] + [("pop", 8), ("clear", 0)]:
    if operation:
        call(localnet, app, operation + "()", extra_fee=40_000)
    atc = AtomicTransactionComposer()
    atc.add_method_call(app.app_id, method, localnet.account.address,
                        localnet.algod.suggested_params(), localnet.account.signer)
    simulated = atc.simulate(localnet.algod, SimulateRequest(txn_groups=[], allow_unnamed_resources=True))
    group = simulated.simulate_response["txn-groups"][0]
    assert not group.get("failure-message"), group
    values = simulated.abi_results[0].return_value
    assert values == [expected] * 4, values
    rows.append({"operation": operation, "length": expected,
                 "budget": group["app-budget-consumed"]})
print(json.dumps({"approval_bytes": (directory / (name + ".approval.bin")).stat().st_size,
                  "measurements": rows}, indent=2))
