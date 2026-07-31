// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Checkpoints {
    struct Checkpoint { uint32 fromBlock; uint224 votes; }
    mapping(address => Checkpoint[]) public ckpts;
    mapping(address => address) public delegates;
    mapping(address => uint256) public balanceOf;
    function _writeCheckpoint(address a, uint224 newVotes, uint32 blk) internal {
        Checkpoint[] storage cp = ckpts[a];
        if (cp.length > 0 && cp[cp.length - 1].fromBlock == blk) cp[cp.length - 1].votes = newVotes;
        else cp.push(Checkpoint(blk, newVotes));
    }
    function delegate(address from, address to, uint32 blk) external {
        address old = delegates[from];
        delegates[from] = to;
        uint256 amt = balanceOf[from];
        if (old != address(0) && amt > 0) {
            uint256 n = ckpts[old].length;
            uint224 prev = n == 0 ? 0 : ckpts[old][n-1].votes;
            _writeCheckpoint(old, uint224(prev - amt), blk);
        }
        if (to != address(0) && amt > 0) {
            uint256 n = ckpts[to].length;
            uint224 prev = n == 0 ? 0 : ckpts[to][n-1].votes;
            _writeCheckpoint(to, uint224(prev + amt), blk);
        }
    }
    function mint(address to, uint256 amt) external { balanceOf[to] += amt; }
    function getPastVotes(address a, uint32 blk) external view returns (uint224) {
        Checkpoint[] storage cp = ckpts[a];
        if (cp.length == 0) return 0;
        uint256 lo = 0; uint256 hi = cp.length;
        while (lo < hi) {                                  // binary search
            uint256 mid = (lo + hi) / 2;
            if (cp[mid].fromBlock > blk) hi = mid; else lo = mid + 1;
        }
        return lo == 0 ? 0 : cp[lo-1].votes;
    }
    function numCkpts(address a) external view returns (uint256) { return ckpts[a].length; }
}
