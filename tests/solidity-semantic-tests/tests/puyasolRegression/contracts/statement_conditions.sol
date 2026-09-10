// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StatementLoops {
    function condition(uint64[] memory state, uint64 limit) internal pure returns (bool) {
        return ++state[0] < limit;
    }
    function run(uint64 kind, uint64 limit) external pure returns (uint64, uint64, uint64, uint64) {
        uint64[] memory state = new uint64[](1);
        uint64 body;
        uint64 sum;
        uint64 posts;
        if (kind == 0) {
            while (condition(state, limit)) {
                ++body;
                if (body % 2 == 1) continue;
                sum += body;
            }
        } else if (kind == 1) {
            for (; condition(state, limit); ++posts) {
                ++body;
                if (body % 2 == 1) continue;
                sum += body;
            }
        } else {
            do {
                ++body;
                if (body % 2 == 1) continue;
                sum += body;
            } while (condition(state, limit));
        }
        return (state[0], body, sum, posts);
    }
    function branch(uint64 limit) external pure returns (uint64, uint64) {
        uint64[] memory state = new uint64[](1);
        uint64 taken;
        if (condition(state, limit)) taken = 7;
        return (state[0], taken);
    }
}
