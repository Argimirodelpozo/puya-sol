"""Generic deploy_app/create_app helpers for puya-sol output artifacts."""
from pathlib import Path

import algokit_utils as au
from algosdk.transaction import (
    ApplicationCreateTxn,
    OnComplete,
    PaymentTxn,
    StateSchema,
    wait_for_confirmation,
)

from .addrs import algod_addr_for_app
from .arc56 import compile_teal, inject_memory_init, load_arc56


# Re-exported for convenience so tests can write `from dev.deploy import AUTO_POPULATE`.
NO_POPULATE = au.SendParams(populate_app_call_resources=False)
AUTO_POPULATE = au.SendParams(populate_app_call_resources=True)


def _find_helper_dirs(base_dir: Path) -> list[Path]:
    """Return puya-sol-emitted sibling helper subdirs (`*__Helper<N>/`).

    The splitter peels named functions out of the orch into helper contracts;
    each helper lands in its own subdir alongside the orch. Helpers carry a
    `<name>__Helper<N>.arc56.json` and the orch's TEAL references them via
    `TMPL_<name>__Helper<N>_APP_ID` template vars.
    """
    if not base_dir.is_dir():
        return []
    return sorted(
        d for d in base_dir.iterdir()
        if d.is_dir()
        and "__Helper" in d.name
        and (d / f"{d.name}.arc56.json").exists()
    )


def _deploy_helper(localnet, sender, helper_dir: Path) -> int:
    """Deploy a single helper contract and return its app_id."""
    name = helper_dir.name
    algod = localnet.client.algod
    spec = load_arc56(helper_dir / f"{name}.arc56.json")
    teal = inject_memory_init((helper_dir / f"{name}.approval.teal").read_text())
    return create_app(
        localnet, sender,
        compile_teal(algod, teal),
        compile_teal(algod, (helper_dir / f"{name}.clear.teal").read_text()),
        spec.state.schema.global_state,
    )


def deploy_app(
    localnet,
    sender,
    base_dir: Path,
    name: str,
    *,
    post_init_args=None,
    post_init_app_refs=None,
    create_args=None,
    create_boxes=None,
    create_apps=None,
    fund_amount: int = 10_000_000,
    extra_fee: int = 0,
) -> au.AppClient:
    """Deploy a puya-sol-emitted app, fund it, optionally invoke __postInit.

    Resolves artifacts from either layout: the historical flat layout
    (`base_dir/<name>.arc56.json`) or the post-split nested layout
    (`base_dir/<name>/<name>.arc56.json`, one subdir per emitted contract).

    Auto-deploys any sibling `*__Helper<N>` contracts found under `base_dir`,
    substitutes their app ids into the orch TEAL's `TMPL_<helper>_APP_ID`
    template vars, and adds them to `post_init_app_refs`. The orch's
    inner-call to a helper resolves once the template is filled in.
    """
    direct = base_dir / f"{name}.arc56.json"
    nested = base_dir / name / f"{name}.arc56.json"
    if direct.exists():
        artifact_dir = base_dir
    elif nested.exists():
        artifact_dir = base_dir / name
    else:
        raise FileNotFoundError(
            f"deploy_app: no arc56 spec for '{name}' under {base_dir} "
            f"(checked {direct} and {nested})")

    spec = load_arc56(artifact_dir / f"{name}.arc56.json")
    algod = localnet.client.algod
    sch = spec.state.schema.global_state

    helper_app_ids: list[int] = []
    helper_substitutions: list[tuple[str, int]] = []
    for helper_dir in _find_helper_dirs(base_dir):
        h_app_id = _deploy_helper(localnet, sender, helper_dir)
        helper_app_ids.append(h_app_id)
        helper_substitutions.append((helper_dir.name, h_app_id))

    approval_text = (artifact_dir / f"{name}.approval.teal").read_text()
    clear_text = (artifact_dir / f"{name}.clear.teal").read_text()
    for h_name, h_id in helper_substitutions:
        tmpl = f"TMPL_{h_name}_APP_ID"
        approval_text = approval_text.replace(tmpl, str(h_id))
        clear_text = clear_text.replace(tmpl, str(h_id))

    approval_bin = compile_teal(algod, approval_text)
    clear_bin = compile_teal(algod, clear_text)

    max_size = max(len(approval_bin), len(clear_bin))
    extra_pages = max(0, (max_size - 1) // 2048)
    if extra_pages > 3:
        raise RuntimeError(
            f"{name}: compiled size {max_size} exceeds AVM 8192-byte cap "
            f"(needs {extra_pages} extra pages, max 3)"
        )

    sp = algod.suggested_params()
    sp.fee = max(sp.min_fee, sp.fee) + extra_fee
    sp.flat_fee = True
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_bin, clear_program=clear_bin,
        global_schema=StateSchema(num_uints=sch.ints, num_byte_slices=sch.bytes),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=create_args or [],
        boxes=create_boxes or [],
        foreign_apps=create_apps or [],
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]

    if fund_amount > 0:
        sp2 = algod.suggested_params()
        pay = PaymentTxn(sender.address, sp2, algod_addr_for_app(app_id), fund_amount)
        wait_for_confirmation(
            algod, algod.send_transaction(pay.sign(sender.private_key)), 4)

    client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec,
        app_id=app_id, default_sender=sender.address,
    ))

    if post_init_args is not None:
        merged_app_refs = list(post_init_app_refs or []) + helper_app_ids
        client.send.call(au.AppClientMethodCallParams(
            method="__postInit",
            args=post_init_args,
            extra_fee=au.AlgoAmount(micro_algo=20_000),
            app_references=merged_app_refs,
        ), send_params=AUTO_POPULATE)

    return client


def create_app(
    localnet,
    sender,
    approval: bytes,
    clear: bytes,
    schema,
    *,
    app_args=None,
    fund_amount: int = 10_000_000,
    extra_pages: int | None = None,
) -> int:
    """Lower-level: create an app from already-compiled TEAL bytes.

    Used by split-exchange fixtures that need to substitute helper app ids
    into the orch's TEAL before compilation.
    """
    algod = localnet.client.algod
    if extra_pages is None:
        extra_pages = max(0, (max(len(approval), len(clear)) - 1) // 2048)
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=schema.ints, num_byte_slices=schema.bytes),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=app_args or [],
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]
    if fund_amount > 0:
        sp2 = algod.suggested_params()
        pay = PaymentTxn(sender.address, sp2, algod_addr_for_app(app_id), fund_amount)
        wait_for_confirmation(
            algod, algod.send_transaction(pay.sign(sender.private_key)), 4)
    return app_id
