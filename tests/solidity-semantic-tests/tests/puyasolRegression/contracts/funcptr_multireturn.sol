// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Internal function pointers to PUBLIC multi-return targets: the callee
// returns the WIRE tuple (build-time return encoding), the dispatch adapts
// each element back to native. Elements cover: encoded biguint (uint256),
// untouched uint64, signed narrow (int32 -> uint256 wire, uint64 carrier),
// and an encoded ARC4 aggregate (uint256[]).
contract FuncPtrMultiReturn {
    function multi() public pure returns (uint256 a, uint64 b, int32 c) {
        return (10 ** 30, 42, -7);
    }

    function multiArr() public pure returns (uint256[] memory xs, uint256 s) {
        xs = new uint256[](2);
        xs[0] = 5;
        xs[1] = 6;
        s = 11;
    }

    function callMulti(uint256 which)
        public
        pure
        returns (uint256, uint64, int32)
    {
        function() pure returns (uint256, uint64, int32) f;
        if (which == 1) f = multi;
        // which == 0: f stays uninitialized -> dispatch default -> Panic(0x51)
        return f();
    }

    function callMultiArr() public pure returns (uint256, uint256, uint256) {
        function() pure returns (uint256[] memory, uint256) f = multiArr;
        (uint256[] memory xs, uint256 s) = f();
        return (xs[0], xs[1], s);
    }
}
