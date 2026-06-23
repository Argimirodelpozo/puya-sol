// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the abi-round-trip fuzz probe.
// abi.decode(abi.encode(b), (bytes)) did NOT round-trip a `bytes` value: handleDecode short-circuited
// (`decoded->wtype == targetType` — both `bytes`) and returned the ARC4 byte[] encoding (uint16 length
// prefix + data) instead of ARC4-decoding it back to raw bytes. So the result was 2 bytes too long and
// its first byte was the length high-byte, not b[0]. `string` was already correct (its wtype `bytes` !=
// target `string`, so it fell through to ARC4Decode). FIX: exclude dynamic bytes/string targets from the
// short-circuit so `bytes` also routes through ARC4Decode (strips the length prefix).
contract C {
    function rtLen(bytes calldata b) external pure returns (uint256) {
        return abi.decode(abi.encode(b), (bytes)).length;
    }
    function rtFirst(bytes calldata b) external pure returns (uint8) {
        bytes memory r = abi.decode(abi.encode(b), (bytes));
        return r.length == 0 ? 255 : uint8(r[0]);
    }
    function rtEq(bytes calldata b) external pure returns (bool) {
        return keccak256(abi.decode(abi.encode(b), (bytes))) == keccak256(b);
    }
    // string control — must stay correct (it already round-tripped)
    function stEq(string calldata s) external pure returns (bool) {
        return keccak256(bytes(abi.decode(abi.encode(s), (string)))) == keccak256(bytes(s));
    }
}
