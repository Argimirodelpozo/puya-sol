// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored). Guards a BARE INTEGER LITERAL used
// as a mapping key whose declared key type is bytes32 (the ENS ENSRegistry
// `records[0x0]` idiom). Such a literal is an IntegerConstant of wtype uint64;
// the mapping-key coercion had no uint64->bytesN case, so makeKeyBytes fell to
// its fallback reinterpretCast(uint64 -> bytes) — an invalid scalar->bytes cast
// puya rejects ("unsupported type cast from uint64 to bytes"). Fixed by adding
// uint64 -> bytes[N] (itob + leftPad) to TypeCoercion::implicitNumericCast, so
// the literal key encodes to the SAME 32-byte value as bytes32(0) / a bytes32
// key param. This guard proves the ctor literal-key write and the param-key
// read hit the SAME box (else the read returns 0). See ens-compile memory.
contract MappingLiteralKey {
    struct R { address owner; uint64 ttl; }
    mapping(bytes32 => R) records;

    constructor() {
        records[0x0].ttl = 42;   // bare-literal key write in the ctor (__postInit)
    }

    function ttlLit() external view returns (uint64) { return records[0x0].ttl; }
    function ttlVia(bytes32 node) external view returns (uint64) { return records[node].ttl; }
    function setViaParam(bytes32 node, uint64 t) external { records[node].ttl = t; }
}
