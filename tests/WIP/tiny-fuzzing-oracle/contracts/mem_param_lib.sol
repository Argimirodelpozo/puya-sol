// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
struct S { uint256 x; uint256 y; }
library L {
    function mutStruct(S memory s) internal pure { s.x = 11; }
}
contract MemParamLib {
    using L for S;
    function libStructParam() external pure returns (uint256) {
        S memory s = S(5, 0);
        L.mutStruct(s); return s.x;   // EVM: 11 (memory by ref)
    }
    function freeStructParam() external pure returns (uint256) {
        S memory s = S(5, 0);
        _free(s); return s.x;         // EVM: 11
    }
}
function _free(S memory s) pure { s.x = 11; }
