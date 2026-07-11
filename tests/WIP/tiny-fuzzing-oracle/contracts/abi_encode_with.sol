// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// abi.encodeWith{Selector,Signature,Call} + encodePacked invariants. Each returns a bool that should
// be TRUE on both EVM and AVM (the identity holds within each side's own encoding), so an AVM false
// where EVM is true is a real bug. No raw-byte comparison across stacks.
contract C {
    function g(uint256 a, int16 b) external pure returns (uint256, int16) { return (a, b); }

    // encodeWithSelector(sel, args) must equal sel ++ encode(args)
    function idEncSel(bytes4 sel, uint256 a, int16 b) external pure returns (bool) {
        return keccak256(abi.encodeWithSelector(sel, a, b))
            == keccak256(bytes.concat(sel, abi.encode(a, b)));
    }
    // encodeWithSignature(sig, args) == encodeWithSelector(bytes4(keccak256(sig)), args)
    function idEncSig(uint256 a, bytes calldata b) external pure returns (bool) {
        return keccak256(abi.encodeWithSignature("h(uint256,bytes)", a, b))
            == keccak256(abi.encodeWithSelector(bytes4(keccak256("h(uint256,bytes)")), a, b));
    }
    // encodeCall(g, args) == encodeWithSelector(g.selector, args)
    function idEncCall(uint256 a, int16 b) external view returns (bool) {
        return keccak256(abi.encodeCall(this.g, (a, b)))
            == keccak256(abi.encodeWithSelector(this.g.selector, a, b));
    }
    // encodePacked tight-packing lengths (uint8=1, uint16=2, uint32=4, address=20, bytesN=N)
    function idPackedLen(uint8 a, uint16 b, uint32 c, address d) external pure returns (bool) {
        return abi.encodePacked(a, b, c, d).length == 1 + 2 + 4 + 20;
    }
    // encodePacked of bytes/string is the raw content (no length prefix, no padding)
    function idPackedBytes(bytes calldata x, string calldata y) external pure returns (bool) {
        return abi.encodePacked(x, y).length == x.length + bytes(y).length;
    }
    // encode(a) for a single dynamic arg has a 0x20 offset head then the body
    function idEncodeBytesHead(bytes calldata x) external pure returns (bool) {
        bytes memory e = abi.encode(x);
        // first 32 bytes = offset 0x20
        return e.length >= 64 && uint256(bytes32(e)) == 0x20;
    }
}
