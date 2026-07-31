// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract RevLoop {
    // direct revert string (baseline — should carry payload)
    function direct(uint256 x) external pure returns (uint256) {
        if (x > 5) revert("too big");
        return x;
    }
    // revert after while(true){...break} loop (the ownerOf shape)
    function afterWhile(uint256 x) external pure returns (uint256) {
        uint256 cur = x;
        while (true) {
            if (cur == 100) return cur;
            if (cur == 0) break;
            cur--;
        }
        revert("not found");
    }
    // revert after for loop
    function afterFor(uint256 x) external pure returns (uint256) {
        for (uint256 i = 0; i < x % 5; i++) { if (i == 3) return i; }
        revert("for exhausted");
    }
    // require then revert (two-payload paths)
    function twoPath(uint256 x) external pure returns (uint256) {
        require(x < 1000, "nonexistent");
        uint256 cur = x;
        while (true) { if (cur == 50) return cur; if (cur == 0) break; cur--; }
        revert("not found");
    }
}
