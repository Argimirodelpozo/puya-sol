// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract E721Min {
    struct O { address addr; uint64 ts; bool burned; }
    uint256 public ci;
    mapping(uint256 => O) own;
    function setupBurned0() external { own[0] = O(address(0x1234), 1, true); ci = 400; } // token 0 burned
    function setupClean() external { own[0] = O(address(0x1234), 1, false); ci = 400; }
    function ownerOf(uint256 t) external view returns (address) {
        require(t < ci, "nonexistent");
        uint256 cur = t;
        while (true) {
            O memory o = own[cur];
            if (o.addr != address(0) && !o.burned) return o.addr;
            if (cur == 0) break;
            cur--;
        }
        revert("not found");
    }
}
