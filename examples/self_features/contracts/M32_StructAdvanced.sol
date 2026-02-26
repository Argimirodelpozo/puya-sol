// SPDX-License-Identifier: MIT
pragma solidity >=0.8.8;

// ─── Struct Definitions ────────────────────────────────────────────
struct Point {
    uint256 x;
    uint256 y;
}

struct Line {
    Point start;
    Point end_;
}

struct Container {
    uint256 id;
    uint256[3] values;
}

// ─── Main Test Contract ────────────────────────────────────────────
contract StructAdvancedTest {

    // ── Nested struct construction + access ──
    function testNestedStruct(
        uint256 x1, uint256 y1, uint256 x2, uint256 y2
    ) external pure returns (uint256, uint256, uint256, uint256) {
        Line memory line = Line(
            Point(x1, y1),
            Point(x2, y2)
        );
        return (line.start.x, line.start.y, line.end_.x, line.end_.y);
    }

    // ── Struct with array member ──
    function testStructWithArray(
        uint256 id, uint256 a, uint256 b, uint256 c
    ) external pure returns (uint256, uint256) {
        uint256[3] memory vals;
        vals[0] = a;
        vals[1] = b;
        vals[2] = c;
        Container memory cont = Container(id, vals);
        return (cont.id, cont.values[1]);
    }

    // ── Struct field mutation (copy-on-write) ──
    function testFieldMutation(
        uint256 x, uint256 y, uint256 newX
    ) external pure returns (uint256, uint256) {
        Point memory p = Point(x, y);
        p.x = newX;
        return (p.x, p.y);
    }

    // ── Nested struct field mutation ──
    function testNestedFieldMutation(
        uint256 x1, uint256 y1, uint256 x2, uint256 y2, uint256 newX
    ) external pure returns (uint256, uint256) {
        Line memory line = Line(Point(x1, y1), Point(x2, y2));
        line.start.x = newX;
        return (line.start.x, line.start.y);
    }

    // ── Struct as internal helper (pass & return) ──
    function _addPoints(Point memory a, Point memory b) internal pure returns (Point memory) {
        return Point(a.x + b.x, a.y + b.y);
    }

    function testStructPassReturn(
        uint256 x1, uint256 y1, uint256 x2, uint256 y2
    ) external pure returns (uint256, uint256) {
        Point memory a = Point(x1, y1);
        Point memory b = Point(x2, y2);
        Point memory result = _addPoints(a, b);
        return (result.x, result.y);
    }

    // ── Multiple struct returns ──
    function _splitLine(Line memory l) internal pure returns (Point memory, Point memory) {
        return (l.start, l.end_);
    }

    function testMultiStructReturn(
        uint256 x1, uint256 y1, uint256 x2, uint256 y2
    ) external pure returns (uint256, uint256, uint256, uint256) {
        Line memory line = Line(Point(x1, y1), Point(x2, y2));
        (Point memory s, Point memory e) = _splitLine(line);
        return (s.x, s.y, e.x, e.y);
    }
}
