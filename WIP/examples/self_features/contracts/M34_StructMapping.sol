// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M34: Struct mapping field mutation (Gap 8 verification).
 * Tests that writing to fields of structs stored in mappings persists correctly.
 * Patterns tested:
 *   1. mapping[key].field = value      (direct inline)
 *   2. mapping[key].field += value     (compound inline)
 *   3. Struct storage p = mapping[key]; p.field = value  (storage pointer)
 *   4. Multiple field updates on same struct
 *   5. Nested struct in mapping
 */

struct Position {
    uint256 supply;
    uint256 borrow;
    uint256 collateral;
}

struct Order {
    uint256 price;
    uint256 quantity;
    bool filled;
}

contract StructMappingTest {
    mapping(uint256 => Position) private _positions;
    mapping(uint256 => Order) private _orders;

    // ── Pattern 1: Direct inline field assignment ──
    function setSupply(uint256 id, uint256 amount) external {
        _positions[id].supply = amount;
    }

    function getSupply(uint256 id) external view returns (uint256) {
        return _positions[id].supply;
    }

    // ── Pattern 2: Compound inline field assignment ──
    function addSupply(uint256 id, uint256 amount) external {
        _positions[id].supply += amount;
    }

    function subSupply(uint256 id, uint256 amount) external {
        _positions[id].supply -= amount;
    }

    // ── Pattern 3: Multiple field updates ──
    function updatePosition(uint256 id, uint256 supply, uint256 borrow, uint256 collateral) external {
        _positions[id].supply = supply;
        _positions[id].borrow = borrow;
        _positions[id].collateral = collateral;
    }

    function getPosition(uint256 id) external view returns (uint256, uint256, uint256) {
        Position storage p = _positions[id];
        return (p.supply, p.borrow, p.collateral);
    }

    // ── Pattern 4: Storage pointer field write ──
    function updateViaPointer(uint256 id, uint256 newBorrow) external {
        Position storage p = _positions[id];
        p.borrow = newBorrow;
    }

    // ── Pattern 5: Storage pointer compound assignment ──
    function addCollateralViaPointer(uint256 id, uint256 amount) external {
        Position storage p = _positions[id];
        p.collateral += amount;
    }

    // ── Pattern 6: Write full struct, then mutate single field ──
    function createAndModify(uint256 id, uint256 price, uint256 qty) external {
        _orders[id] = Order(price, qty, false);
        _orders[id].filled = true;
    }

    function getOrder(uint256 id) external view returns (uint256, uint256, bool) {
        return (_orders[id].price, _orders[id].quantity, _orders[id].filled);
    }

    // ── Pattern 7: Multiple field updates via storage pointer ──
    function fillOrder(uint256 id, uint256 newPrice) external {
        Order storage o = _orders[id];
        o.price = newPrice;
        o.filled = true;
    }

    // ── Pattern 8: Independent mappings, same key ──
    function setBoth(uint256 id, uint256 supplyAmt, uint256 price) external {
        _positions[id].supply = supplyAmt;
        _orders[id].price = price;
    }
}
