// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Found by the differential fuzzer: a function PARAM used as a memory offset in inline assembly
// resolved to its CALLDATA head-offset constant (m_localConstants double-duty) instead of its runtime
// value, so mstore(off,v)/mload(off) hit a fixed wrong slot.
contract C {
    function paramOff(uint256 off, uint256 v) external pure returns (uint256 r) {
        off = off % 256; assembly { mstore(off, v) r := mload(off) }   // -> v
    }
    function paramOffAdd(uint256 off, uint256 v) external pure returns (uint256 r) {
        off = off % 256; assembly { mstore(add(off, 0), v) r := mload(off) }
    }
    function twoParams(uint256 a, uint256 b, uint256 v) external pure returns (uint256) {
        a = a % 128; b = (b % 128) + 128;
        assembly { mstore(a, v) mstore(b, add(v, 1)) }
        uint256 ra; uint256 rb;
        assembly { ra := mload(a) rb := mload(b) }
        return ra + rb;   // v + (v+1)
    }
    function constOff(uint256 v) external pure returns (uint256 r) { assembly { mstore(64, v) r := mload(64) } }
    function letOff(uint256 o, uint256 v) external pure returns (uint256 r) {
        assembly { let off := mod(o, 256) mstore(off, v) r := mload(off) }
    }
}
