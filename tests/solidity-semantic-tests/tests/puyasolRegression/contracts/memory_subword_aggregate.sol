// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. solc-todo.md opportunity C (element/field sizes):
// reusing solc's calldataEncodedSize for computeEncodedElementSize turned out non-viable (it is WType-
// based; bool/address use puya's own widths 8/32 not solc's 1/20; and sizes are context-dependent —
// box storage is packed-ARC4 while the memory blob is 32-byte words). No latent size bug exists. This
// guards the one area not previously covered: MEMORY aggregates with sub-word (uint128/int16/uint8)
// fields/elements — struct field read + mutate, array index read — must match EVM.
contract MemorySubwordAggregate {
    struct S { uint128 a; int16 b; uint8 c; uint128 d; }
    function field_a(uint128 a, int16 b, uint8 c, uint128 d) external pure returns (uint128) { S memory s = S(a,b,c,d); return s.a; }
    function field_b(uint128 a, int16 b, uint8 c, uint128 d) external pure returns (int16) { S memory s = S(a,b,c,d); return s.b; }
    function field_d(uint128 a, int16 b, uint8 c, uint128 d) external pure returns (uint128) { S memory s = S(a,b,c,d); return s.d; }
    function mutate_b(uint128 a, int16 b, uint8 c, uint128 d, int16 nb) external pure returns (int16) { S memory s = S(a,b,c,d); s.b = nb; return s.b; }
    function arr_idx(uint128 x, uint128 y, uint128 z, uint256 i) external pure returns (uint128) { uint128[] memory m = new uint128[](3); m[0]=x; m[1]=y; m[2]=z; return m[i % 3]; }
    function sarr_idx(int16 a, int16 b, uint256 i) external pure returns (int16) { int16[] memory m = new int16[](3); m[0]=a; m[1]=b; m[2]=-1; return m[i % 3]; }
}
