// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract Structs {
    struct Point {
        uint256 x;
        uint256 y;
    }

    struct Named {
        string name;
        uint256 value;
    }

    Point public origin;
    mapping(uint256 => Point) public points;

    function setOrigin(uint256 x, uint256 y) external {
        origin = Point(x, y);
    }

    function getOrigin() external view returns (uint256, uint256) {
        return (origin.x, origin.y);
    }

    function addPoints(uint256 x1, uint256 y1, uint256 x2, uint256 y2)
        external pure returns (uint256, uint256)
    {
        Point memory a = Point(x1, y1);
        Point memory b = Point(x2, y2);
        return (a.x + b.x, a.y + b.y);
    }

    function setPoint(uint256 id, uint256 x, uint256 y) external {
        points[id] = Point(x, y);
    }

    function getPointX(uint256 id) external view returns (uint256) {
        return points[id].x;
    }

    function getPointY(uint256 id) external view returns (uint256) {
        return points[id].y;
    }
}
