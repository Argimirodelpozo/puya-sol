"""ABI method invocation helper.

`call(client, method, args=..., sender=...)` invokes an ABI method and
returns its ABI-decoded return value, with sane defaults for fee + box
auto-population. Wraps `algokit_utils`' AppClient.send.call.
"""
import algokit_utils as au

from .deploy import AUTO_POPULATE


def call(
    client: au.AppClient,
    method: str,
    args=None,
    *,
    sender=None,
    extra_fee: int = 20_000,
    populate=AUTO_POPULATE,
    box_references=None,
    app_references=None,
):
    """Invoke an ABI method on `client` and return the decoded ABI return.

    Caller-provided fee is in microAlgos. `extra_fee` covers inner-call costs
    (proxy storage writes, box accesses); 20_000 is a safe default for most
    standalone-contract methods.
    """
    return client.send.call(
        au.AppClientMethodCallParams(
            method=method,
            args=args or [],
            sender=sender.address if sender else None,
            extra_fee=au.AlgoAmount(micro_algo=extra_fee),
            box_references=box_references or [],
            app_references=app_references or [],
        ),
        send_params=populate,
    ).abi_return
