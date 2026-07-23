// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the storage-ref-pointer return rewrite INSIDE loops: the old
// hand-rolled walker missed WhileLoop/Switch/ForInLoop containers, so a
// `return stateVar[i];` inside a while loop skipped the index rewrite
// (awst::forEachChildBlock consolidation, possible_solc item 8 reassessment).
contract StorageRefReturnLoop {
    uint256[][] grid;

    function seed() external {
        grid.push();
        grid.push();
        grid.push();
        grid[0].push(1);
        grid[1].push(2);
        grid[1].push(3);
        grid[2].push(4);
    }

    function rowWithLen(uint256 n) internal view returns (uint256[] storage) {
        uint256 i = 0;
        while (i < grid.length) {
            if (grid[i].length == n) return grid[i]; // return inside the loop
            i++;
        }
        return grid[0];
    }

    function firstOfLen(uint256 n) external view returns (uint256) {
        return rowWithLen(n)[0];
    }
}
