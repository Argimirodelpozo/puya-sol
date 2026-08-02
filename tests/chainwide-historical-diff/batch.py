#!/usr/bin/env python3
"""Run the historical replay across a list of candidate contracts, sequentially.

  python3 batch.py [--max-txns N] [--only tag1,tag2]

Sequential ON PURPOSE: one LocalNet, and concurrent puya-sol compiles poison the
shared compile cache. Failures (unverified / multi-file / old solc / external-
dependency constructor) are logged and skipped; the batch keeps going.
Aggregate results land in cases/_batch_summary.json.
"""
from __future__ import annotations

import sys
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import CASES, dump_json
from differ import print_report
from fetch import fetch_case
from replay import replay

# (host, address, tag) — single-file verified ^0.8.x contracts with history.
CANDIDATES = [
    ("optimism.blockscout.com", "0x4200000000000000000000000000000000000042", "op_gov"),
    ("eth.blockscout.com",      "0x6982508145454Ce325dDbE47a25d4ec3d2311933", "pepe"),
    ("base.blockscout.com",     "0xCF205808Ed36593aa40a44F10c7f7C2F67d4A4d4", "friendtech"),
    ("base.blockscout.com",     "0xAC1Bd2486aAf3B5C0fc3Fd868558b082a531B2B4", "toshi"),
    ("eth.blockscout.com",      "0xaaeE1A9723aaDB7afA2810263653A34bA2C21C7a", "mog"),
    ("base.blockscout.com",     "0x4ed4E862860beD51a9570b96d89aF5E1B0Efefed", "degen"),
    ("eth.blockscout.com",      "0x163f8C2467924be0ae7B5347228CABF260318753", "wld"),
    ("gnosis.blockscout.com",   "0x177127622c4A00F3d409B75571e12cB3c8973d3c", "gno_cow"),
    # probed eligible: single-file, ^0.8.x, verified
    ("eth.blockscout.com",      "0xA35923162C49cF95e6BF26623385eb431ad920D3", "turbo"),
    ("eth.blockscout.com",      "0x72e4f9F808C49A2a61dE9C5896298920Dc4EEEa9", "bitcoin_hpos"),
    ("eth.blockscout.com",      "0x4d224452801ACEd8B2F0aebE155379bb5D594381", "ape"),
    ("eth.blockscout.com",      "0x5026F006B85729a8b14553FAE6af249aD16c9aaB", "kizuna"),
    ("base.blockscout.com",     "0x6921B130D297cc43754afba22e5EAc0FBf8Db75b", "doginme"),
    ("eth.blockscout.com",      "0x7D8146cf21e8D7cbe46054e01588207b51198729", "boba"),
    ("eth.blockscout.com",      "0x12970E6868f88f6557B76120662c1B3E50A646bf", "ladys"),
    ("base.blockscout.com",     "0x0578d8A44db98B23BF096A382e016e29a5Ce0ffe", "higher"),
    ("eth.blockscout.com",      "0xB90B2A35C65dBC466b04240097Ca756ad2005295", "bobo"),
    ("eth.blockscout.com",      "0x6c22910c6F75F828B305e57c6a54855D8adeAbf8", "sats"),
    # ERC-721s (harvested): richer state than ERC-20 — ownership + per-token and
    # operator approvals, tokenURI strings, and safeTransferFrom's receiver hook
    # is a genuine external call.
    ("base.blockscout.com",     "0x3b916B8f6A710e9240FF08c1dD646dD8E8ED9e1e", "e741"),
    ("base.blockscout.com",     "0xa25e0AF7Dd580fcE7121FD78E95c3f3beE258e8f", "berries"),
    ("base.blockscout.com",     "0x0982B3A5B24B2BD8eF74126E15Fca2DeCfD75A28", "heronft"),
    ("base.blockscout.com",     "0x8DC80A209A3362f0586e6C116973Bb6908170c84", "builder"),
    # MULTI-FILE (harvested, 3→41 files): the class that was unsupported until
    # the tree+remappings path landed. ~86% of popular Base ERC-20s look like this.
    ("base.blockscout.com",     "0xacfE6019Ed1A7Dc6f7B508C02d1b04ec88cC21bf", "venice"),   # 3 files
    ("base.blockscout.com",     "0x50dA645f148798F68EF2d7dB7C1CB22A6819bb2C", "bridgetok"), # 7
    ("base.blockscout.com",     "0xB6fe221Fe9EeF5aBa221c348bA20A1Bf5e73624c", "opmint9"),   # 9
    ("base.blockscout.com",     "0x98d0baa52b2D063E780DE12F615f963Fe8537553", "kaito"),     # 16
    ("base.blockscout.com",     "0xC96dE26018A54D51c097160568752c4E3BD6C364", "fbtc"),      # 21
    ("base.blockscout.com",     "0x3055913c90Fcc1A6CE9a358911721eEb942013A1", "cakeoft"),   # 23
    ("base.blockscout.com",     "0x6985884C4392D348587B19cb9eAAf157F13271cd", "zro"),       # 39
    ("base.blockscout.com",     "0x58538e6A46E07434d7E7375Bc268D3cb839C0133", "enaoft"),    # 41
    # ── batch 2 (harvested 2026-07-30 from the ERC-20 token listings, which are
    # ranked by holders — established contracts with real history, unlike the
    # verified-contracts listing which is newest-first and mostly empty).
    # Chosen for SHAPE DIVERSITY: optimism returns a dozen near-identical
    # L2StandardERC20/OptimismMintableERC20 clones, and replaying eight copies
    # of the same contract buys nothing over the one already covered.
    ("eth.blockscout.com",      "0x4c9EDD5852cd905f086C759E8383e09bff1E68B3", "usde"),
    ("eth.blockscout.com",      "0xfAbA6f8e4a5E8Ab82F62fe7C39859FA577269BE3", "ondo"),
    ("eth.blockscout.com",      "0x56072C95FAA701256059aa122697B133aDEd9279", "sky"),
    ("eth.blockscout.com",      "0x54D2252757e1672EEaD234D27B1270728fF90581", "bgb"),
    ("eth.blockscout.com",      "0x925206b8a707096Ed26ae47C84747fE0bb734F59", "wbt"),
    ("eth.blockscout.com",      "0x80ac24aA929eaF5013f6436cdA2a7ba190f5Cc0b", "maplepool"),
    ("eth.blockscout.com",      "0x9D39A5DE30e57443BfF2A8307A4256c8797A3497", "susde"),   # ERC-4626 vault
    ("eth.blockscout.com",      "0x8d010bf9C26881788b4e6bf5Fd1bdC358c8F90b8", "erc6160"),
    ("optimism.blockscout.com", "0x4a971e87ad1F61f7f3081645f52a99277AE917cF", "xvs"),
    ("optimism.blockscout.com", "0x2E3D870790dC77A83DD1d18184Acc7439A53f475", "ccfrax"),
    ("optimism.blockscout.com", "0x76FB31fb4af56892A25e32cFC43De717950c9278", "l2custom"),
    ("optimism.blockscout.com", "0x23ee2343B892b1BB63503a4FAbc840E0e2C6810f", "burnmint"),
    # ── batch 3: gnosis / polygon / arbitrum. Same diversity rule — the
    # LayerZero OFT and CrossChainCanonical clones repeat on every chain, so
    # only one of each shape is worth a slot here.
    ("gnosis.blockscout.com",   "0xaf204776c7245bF4147c2612BF6e5972Ee483701", "sdai"),      # ERC-4626
    ("gnosis.blockscout.com",   "0xcB444e90D8198415266c6a2724b7900fb12FC56E", "eure"),
    ("gnosis.blockscout.com",   "0x1509706a6c66CA549ff0cB464de88231DDBe213B", "auraoft"),
    ("polygon.blockscout.com",  "0xBbba073C31bF03b8ACf7c28EF0738DeCF3695683", "sand"),      # meta-tx
    ("polygon.blockscout.com",  "0xA3f751662e282E83EC3cBc387d225Ca56dD63D3A", "apepe"),
    ("polygon.blockscout.com",  "0xAC0F66379A6d7801D7726d5a943356A172549Adb", "xtoken"),
    ("arbitrum.blockscout.com", "0x41CA7586cC1311807B4605fBB748a3B8862b42b5", "burnminterc20"),
    ("arbitrum.blockscout.com", "0x25d887Ce7a35172C62FeBFD67a1856F20FaEbB00", "pepeoft"),
    # ── batch 4: deep eth token-listing harvest (pagination fixed — booleans in
    # next_page_params must be JSON true/false, Python's str(False) 422'd and
    # silently capped every chain at one 50-row page). Picked for STRUCTURAL
    # variety: ERC-4626 vaults, tax-on-transfer, governance, bridged/wrapped.
    ("eth.blockscout.com",      "0x57e114B691Db790C35207b2e685D4A43181e6061", "ena"),
    ("eth.blockscout.com",      "0x40D16FC0246aD3160Ccc09B8D0D3A2cD28aE6C2f", "gho"),
    ("eth.blockscout.com",      "0x18084fbA666a33d37592fA2633fD49a74DD93a88", "tbtc"),
    ("eth.blockscout.com",      "0xE0f63A424a4439cBE457D80E4f4b51aD25b2c56C", "spx"),
    ("eth.blockscout.com",      "0xC18360217D8F7Ab5e7c516566761Ea12Ce7F9D72", "ens_tok"),
    ("eth.blockscout.com",      "0xCa14007Eff0dB1f8135f4C25B34De49AB0d42766", "strk"),
    ("eth.blockscout.com",      "0x83F20F44975D03b1b09e64809B757c47f942BEeA", "sdai_eth"),   # ERC-4626
    ("eth.blockscout.com",      "0xDEf1CA1fb7FBcDC777520aa7f396b4E015F497aB", "cow"),
    ("eth.blockscout.com",      "0x320623b8E4fF03373931769A31Fc52A4E78B5d70", "rsr"),
    ("eth.blockscout.com",      "0xBEEF01735c132Ada46AA9aA4c54623cAA92A64CB", "metamorpho"),  # ERC-4626
    ("eth.blockscout.com",      "0xac3E018457B222d93114458476f3E3416Abbe38F", "sfrxeth"),     # ERC-4626
    ("eth.blockscout.com",      "0x5E8422345238F34275888049021821E8E08CAa1f", "frxeth"),
    ("eth.blockscout.com",      "0xcf0C122c6b73ff809C693DB761e7BaeBe62b6a2E", "floki"),       # tax-on-transfer
    ("eth.blockscout.com",      "0xF57e7e7C23978C3cAEC3C3548E3D615c346e79fF", "imx"),
    ("eth.blockscout.com",      "0xf0bb20865277aBd641a307eCe5Ee04E79073416C", "boringvault"),
    ("eth.blockscout.com",      "0x470EBf5f030Ed85Fc1ed4C2d36B9DD02e77CF1b7", "temple"),
    ("eth.blockscout.com",      "0xcccCCCcCCC33D538DBC2EE4fEab0a7A1FF4e8A94", "cfg"),
    ("eth.blockscout.com",      "0xe76C6c83af64e4C60245D8C7dE953DF673a7A33D", "rail"),
    ("eth.blockscout.com",      "0x8881562783028F5c1BCB985d2283D5E170D88888", "shfl"),
    ("eth.blockscout.com",      "0x455e53CBB86018Ac2B8092FdCd39d8444aFFC3F6", "pol"),
    # ── batch 5: deep L2 harvest (base / optimism / gnosis / polygon /
    # arbitrum), 217 candidates. Same diversity rule; one of each shape.
    ("base.blockscout.com",     "0x940181a94A35A4569E4529A3CDfB74e38FD98631", "aero"),
    ("base.blockscout.com",     "0x532f27101965dd16442E59d40670FaF5eBB142E4", "brett"),
    ("base.blockscout.com",     "0x0C03Ce270B4826Ec62e7DD007f0B716068639F7B", "tig"),
    ("base.blockscout.com",     "0x58ecEF26335Af7b04A998105a6603B0Dc475aF33", "babydoge"),
    ("base.blockscout.com",     "0x182FA643E5f29d5EcA75e7b9CF9336A3fe4620b2", "b_token6"),
    ("base.blockscout.com",     "0xf43eB8De897Fbc7F2502483B2Bef7Bb9EA179229", "zen"),
    ("optimism.blockscout.com", "0x9560e827aF36c94D2Ac33a39bCE1Fe78631088Db", "velo"),
    ("optimism.blockscout.com", "0x9A601C5bb360811d96A23689066af316a30c3027", "pika"),
    ("optimism.blockscout.com", "0x10398AbC267496E49106B07dd6BE13364D10dC71", "systemcoin"),
    ("optimism.blockscout.com", "0x2dAD3a13ef0C6366220f989157009e501e7938F8", "extra"),
    ("optimism.blockscout.com", "0x14778860E937f509e651192a90589dE711Fb88a9", "cyber"),
    ("gnosis.blockscout.com",   "0xeEeEEb57642040bE42185f49C52F7E9B38f8eeeE", "elk"),
    ("gnosis.blockscout.com",   "0x5Cb9073902F2035222B9749F8fB0c9BFe5527108", "gbp"),
    ("gnosis.blockscout.com",   "0x536dd2E72686d3d7d6A1d4A6FEbD69Ec1B7C05Bd", "pbnj"),
    ("gnosis.blockscout.com",   "0x1D1498166DDCEeE616a6d99868e1E0677300056f", "xerc20"),
    ("polygon.blockscout.com",  "0x8DE5B80a0C1B02Fe4976851D030B36122dbb8624", "vanry"),
    ("polygon.blockscout.com",  "0x564906ec1DF8399F00e4ad32c0eCAC0404a27A1c", "wallettok"),
    ("polygon.blockscout.com",  "0x37C5eBfaE4E4dA225e9D8042B05063c4A2C94bb6", "staup"),
    ("polygon.blockscout.com",  "0xbFc70507384047Aa74c29Cdc8c5Cb88D0f7213AC", "polyalierc20"),
    ("arbitrum.blockscout.com", "0x1337420dED5ADb9980CFc35f8f2B054ea86f8aB1", "sqd"),
]


def main():
    argv = list(sys.argv[1:])
    max_txns = 200
    only = None
    if "--max-txns" in argv:
        i = argv.index("--max-txns"); max_txns = int(argv[i + 1]); del argv[i:i + 2]
    if "--only" in argv:
        i = argv.index("--only"); only = set(argv[i + 1].split(",")); del argv[i:i + 2]
    refetch = "--refetch" in argv        # re-pull history (e.g. for a deeper window)
    if refetch:
        argv.remove("--refetch")
    evm_layout = "--evm-layout" in argv  # slot-mode AVM leg (--evm-storage-layout)
    if evm_layout:
        argv.remove("--evm-layout")
    evm_memory = "--evm-memory" in argv   # + stage-3 universal blob memory
    if evm_memory:
        argv.remove("--evm-memory")

    summary = []
    for host, addr, tag in CANDIDATES:
        if only and tag not in only:
            continue
        print(f"\n{'='*70}\n[batch] {tag}  ({host} {addr})\n{'='*70}", flush=True)
        entry = {"tag": tag, "host": host, "address": addr}
        try:
            if refetch or not (CASES / tag / "case.json").exists():
                fetch_case(host, addr, tag, max_txns)
            rep = replay(tag, max_txns, evm_layout=evm_layout,
                         evm_memory=evm_memory)
            print_report(rep)
            c = rep["counts"]
            entry.update({
                "status": "done", "name": rep["name"],
                "replayed": rep["replayed"], "window": rep["txns_in_window"],
                "skips": rep["skips"],
                "real_divergences": c["status_div"] + c["value_div"]
                                    + c["event_div"] + c["snapshot_div"],
                "counts": c,
            })
        except SystemExit as e:
            entry.update({"status": "skipped", "why": str(e)[:200]})
            print(f"[batch] {tag}: SKIPPED — {str(e)[:200]}", flush=True)
        except Exception as e:
            entry.update({"status": "error", "why": f"{type(e).__name__}: {e}"[:200]})
            print(f"[batch] {tag}: ERROR — {type(e).__name__}: {e}", flush=True)
            traceback.print_exc()
        summary.append(entry)
        dump_json(CASES / ("_batch_summary_evmlayout.json" if evm_layout
                           else "_batch_summary.json"), summary)

    print(f"\n{'='*70}\n[batch] SUMMARY\n{'='*70}")
    for e in summary:
        if e["status"] == "done":
            flag = "❌" if e["real_divergences"] else "✅"
            print(f"  {flag} {e['tag']:<12} {e.get('name','?'):<24} "
                  f"{e['replayed']}/{e['window']} replayed  "
                  f"divergences={e['real_divergences']}  skips={e['skips']}")
        else:
            print(f"  ·  {e['tag']:<12} {e['status'].upper()}: {e.get('why','')[:90]}")


if __name__ == "__main__":
    main()
