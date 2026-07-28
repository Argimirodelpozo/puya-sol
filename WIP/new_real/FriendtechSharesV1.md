# FriendtechSharesV1

**Status:** ✅ compiles on puya-sol + differential-clean vs live solc+EVM.

## Source (real deployed contract, fetched off-chain)
- Protocol: friend.tech
- Chain: **Base** mainnet
- Address: `0xCF205808Ed36593aa40a44F10c7f7C2F67d4A4d4`
- Fetched via Blockscout keyless verified-source API:
  `https://base.blockscout.com/api/v2/smart-contracts/0xCF205808Ed36593aa40a44F10c7f7C2F67d4A4d4`
- Compiler: `0.8.18+commit.87f61d96`
- Flattened single file (inlines OpenZeppelin `Context` + `Ownable`, 0 external imports), 205 lines.
- Constructor takes no args (deploys cleanly in the fuzz harness).

## What it is
Bonding-curve "shares" market. `getPrice(supply, amount)` is the sum-of-squares
pricing (`Σ i²` over the range), with `getBuyPrice`/`getSellPrice` and
`...AfterFee` variants, protocol/subject fee-percent setters, and payable
`buyShares`/`sellShares`.

## What I tried
- puya-sol frontend + full backend compile: **clean** (`puya completed successfully`).
- Differential fuzz vs live solc + EVM (`fuzz_state.py`, stateful sequence):
  **465 sequenced calls, 0 divergences** — state (`sharesSupply`/`sharesBalance`),
  return values, events, and revert payloads all match EVM. Exercises the
  sum-of-squares bonding-curve math and the view price functions heavily.

## Notes
- Real, famous, non-trivial-math contract → certified clean on the AVM path.
- Etherscan API V1 is deprecated and Sourcify was mid-brownout; Blockscout's
  `api/v2/smart-contracts/<addr>` (per chain) is the working keyless fetch route.
