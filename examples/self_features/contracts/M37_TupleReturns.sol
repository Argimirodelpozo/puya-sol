// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M37: ARC56 tuple return struct name collision (Gap 11 verification).
 * Tests that methods returning different named tuples get unique struct names.
 */

contract TupleReturnsTest {
    struct Point {
        uint256 x;
        uint256 y;
    }

    // Returns a 2-field named tuple
    function getPoint(uint256 x, uint256 y)
        external pure returns (uint256 px, uint256 py)
    {
        return (x, y);
    }

    // Returns a 3-field named tuple (different from getPoint)
    function getInfo(uint256 id)
        external pure returns (uint256 value, uint256 count, bool active)
    {
        return (id * 2, id + 1, id > 0);
    }

    // Returns an unnamed tuple (should not collide)
    function getPair(uint256 a, uint256 b)
        external pure returns (uint256, uint256)
    {
        return (a + b, a * b);
    }

    // Returns a single value (no tuple)
    function getSum(uint256 a, uint256 b)
        external pure returns (uint256)
    {
        return a + b;
    }
}
