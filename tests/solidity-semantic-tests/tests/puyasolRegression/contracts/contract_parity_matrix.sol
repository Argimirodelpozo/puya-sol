// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// contract/ semantics matrix vs solc: constructor ordering across
// inheritance (base ctors run most-base-first per C3 linearization; state
// initializers run with each contract's ctor), base-ctor ARGUMENT evaluation
// order, modifier-vs-ctor interleavings, and getter return shapes.

contract OrderLog {
    uint256[] internal log_;

    function _log(uint256 v) internal {
        log_.push(v);
    }

    function logAt(uint256 i) public view returns (uint256) {
        return log_[i];
    }

    function logLen() public view returns (uint256) {
        return log_.length;
    }
}

contract BaseA is OrderLog {
    uint256 public a = 100; // initializer

    constructor(uint256 x) {
        _log(1);
        _log(x);
    }
}

contract BaseB is BaseA {
    uint256 public b = 200;

    constructor(uint256 y) BaseA(y + 1) {
        _log(2);
        _log(y);
    }
}

contract DerivedOrder is BaseB {
    uint256 public d = 300;

    // solc: base ctors run most-base-first: BaseA(11), then BaseB(10),
    // then Derived body. Initializers run just before their contract's body.
    constructor() BaseB(10) {
        _log(3);
        _log(a + b + d); // 600: ALL initializers already ran
    }
}

contract SideEffectArgs is OrderLog {
    uint256 public seen;

    function bump(uint256 v) internal returns (uint256) {
        _log(v);
        return v;
    }

    // Argument evaluation order inside ONE modifier-free ctor call chain.
    constructor() {
        seen = bump(7) + bump(8) * 2;
    }
}

contract GetterShapes {
    struct S {
        uint256 x;
        uint256[] arr;   // dropped from the public getter return
        mapping(uint256 => uint256) m; // dropped too
        uint64 y;
    }
    mapping(uint256 => S) public items;   // getter returns (x, y) only
    uint256[3] public fixedArr;           // getter takes an index
    mapping(uint256 => mapping(bool => uint256)) public nested;

    constructor() {
        items[5].x = 41;
        items[5].y = 42;
        fixedArr[1] = 7;
        nested[9][true] = 77;
    }
}

contract VBase is OrderLog {
    // Virtual call from a base ctor dispatches to the MOST DERIVED override.
    // Oracle-corrected expectation: solc runs ALL state initializers first
    // (base->derived), THEN ctor bodies base->derived — so the override
    // already sees v == 42 here (log 47, not 5).
    constructor() {
        _log(f());
    }

    function f() public view virtual returns (uint256) {
        return 1;
    }
}

contract VDerived is VBase {
    uint256 public v = 42;

    function f() public view override returns (uint256) {
        return v + 5;
    }
}
