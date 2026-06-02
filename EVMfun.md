# EVM Fun — things EVM does the hard way that the AVM does in one opcode

A running log of EVM idioms that exist purely to work around a *missing EVM
instruction*, and which the AVM provides natively. When we port EVM Solidity to
the AVM, faithfully translating these emulations produces enormous, slow TEAL —
so the right move is to recognise the idiom and emit the native AVM opcode
instead. Each entry: what the EVM code is doing, why, the AVM equivalent, and
where we handle it.

---

## 1. Most/least-significant bit → `bitlen`

**Where:** Uniswap V4 `BitMath.mostSignificantBit` / `leastSignificantBit`
(`examples/uniswap-v4/contracts/libraries/BitMath.sol`), used throughout the
swap path (`TickMath`, `TickBitmap`).

**What the EVM does.** The EVM has no "find the highest set bit" /
count-leading-zeros / bit-length instruction (CLZ only arrived with EIP-7939 /
Osaka and isn't deployable yet). So `mostSignificantBit` finds the top bit of a
256-bit number with a **branchless binary search** — five `shl/lt/shr` steps
that halve the search window (128 → 64 → 32 → 16 → 8) — and resolves the last 3
bits with a **de Bruijn-sequence perfect hash** (`byte(... mul by a magic
constant ..., lookup_table)`). `leastSignificantBit` isolates the low bit with
`x & -x` and does the same de Bruijn trick. It's genuinely elegant, gas-optimal
EVM code:

```solidity
r := shl(7, lt(0xff…ff(128b), x))            // top half? → +128
r := or(r, shl(6, lt(0xff…ff(64b), shr(r,x)))) // … +64
r := or(r, shl(5, lt(0xffffffff, shr(r,x))))   // … +32
r := or(r, shl(4, lt(0xffff, shr(r,x))))       // … +16
r := or(r, shl(3, lt(0xff,   shr(r,x))))       // … +8
r := or(r, byte(and(0x1f, shr(shr(r,x), 0x8421…54be)), 0x0706…0000)) // de Bruijn, low 3 bits
```

**Why it's a problem on the AVM.** Every one of those ops is on a *256-bit*
value, and on the AVM a 256-bit number is a byte array — so each `shr`/`lt`/
`byte` becomes a fistful of `concat`/`len`/`extract_uint64`/compare ops. A
6-line Yul function lowers to **~1010 lines of TEAL** (and `leastSignificantBit`
~476). Multiply across `TickMath`/`TickBitmap` and the Uniswap V4 `swap` chunk
balloons past the 8 KB program cap.

**The AVM equivalent.** The AVM has the **`bitlen`** opcode: the bit length of a
value (highest-set-bit index + 1; 0 for 0). So the whole thing collapses to:

- `mostSignificantBit(x)  = bitlen(x) - 1`
- `leastSignificantBit(x) = bitlen(x & -x) - 1`

i.e. *one opcode* instead of ~1000 lines.

**How we handle it.** `Bits.bitlen` in the AVM standard library
(`WIP/tokens/AVM.sol`) is real Solidity — `assembly { r := sub(256, clz(x)) }`
— and puya-sol lowers the Yul `clz` builtin to the AVM `bitlen` opcode
(`src/builder/assembly/CoreTranslation.cpp`, `clz(x) == 256 - bitlen(x)`).
`BitMath` imports `Bits` and calls `bitlen` instead of the binary search. Note:
`clz` is an Osaka EVM builtin, so callers compile with `--evm-version osaka`.

**Impact (Uniswap V4 PoolManager `swap` chunk):** 13,214 B → ~11,157 B from MSB/
LSB alone (it also shrinks every `TickMath`/`TickBitmap` function that calls
them). The remaining bulk is `TickMath`'s log/exp magic-constant math, which has
no single-opcode equivalent.

---

<!-- Add new entries above this line as we find them. Template:
## N. <EVM idiom> → <AVM opcode>
**Where:** … **What the EVM does:** … **Why it's a problem:** … **AVM
equivalent:** … **How we handle it:** … **Impact:** …
-->
