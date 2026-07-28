# PepeToken (PEPE)

**Status:** ✅ compiles on puya-sol + differential-clean vs live solc+EVM (with a
one-line constructor adaptation, see below).

## Source (real deployed contract, fetched off-chain)
- Token: PEPE (Pepe)
- Chain: **Ethereum** mainnet
- Address: `0x6982508145454Ce325dDbE47a25d4ec3d2311933`
- Fetched via Blockscout keyless verified-source API:
  `https://eth.blockscout.com/api/v2/smart-contracts/0x6982508145454Ce325dDbE47a25d4ec3d2311933`
- Compiler: `v0.8.0+commit.c7dfd78e`
- Flattened single file (inlines OpenZeppelin ERC20 + Context + Ownable), 631 lines, 0 external imports.

## What it is
Real memecoin. Beyond standard ERC20 it has anti-whale / anti-bot mechanics:
- `blacklists` mapping + `blacklist(addr,bool)` (owner-gated) — blocked addresses revert on transfer.
- `setRule(limited, uniswapV2Pair, maxHoldingAmount, minHoldingAmount)` — trading limits.
- `_beforeTokenTransfer` override enforces blacklist + per-holder max/min holding when `limited`.
- `burn(value)`.

## Adaptation
**None** — this is the verbatim on-chain source. The constructor
`constructor(uint256 _totalSupply)` is supplied a deploy value automatically by
the fuzz harness (constructor-arg support: the same generated value is used on
both the EVM oracle and the AVM side, so the two instances construct identically).

## What I tried
- puya-sol frontend + backend compile: **clean**.
- Differential fuzz vs live solc + EVM (`fuzz_state.py`, stateful): **830 sequenced
  calls, 0 divergences** — transfers/approvals/transferFrom, blacklist reverts,
  holding-limit `require` reverts (the `_beforeTokenTransfer` hook), burn, and
  ownership all match EVM (state + events + revert payloads).
