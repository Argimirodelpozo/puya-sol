// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

contract StructTest {
    struct Point {
        uint256 x;
        uint256 y;
    }

    function makePoint(uint256 x, uint256 y) external pure returns (uint256, uint256) {
        Point memory p = Point(x, y);
        return (p.x, p.y);
    }

    function addPoints(uint256 x1, uint256 y1, uint256 x2, uint256 y2) external pure returns (uint256, uint256) {
        Point memory a = Point(x1, y1);
        Point memory b = Point(x2, y2);
        return (a.x + b.x, a.y + b.y);
    }
}
