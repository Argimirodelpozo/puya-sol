// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {Global as G} from "libs/AVM.sol";

contract CreationRuntimeOnly {
    uint64 public value;
    constructor() { value = 3; }
    function runtimeOnly() external { assembly { sstore(0, 4) } }
}
abstract contract CreationOverrideBase {
    uint64 public value;
    constructor() { value = choose(); }
    function choose() internal virtual returns (uint64) { assembly { mstore(128, 0) } return 1; }
}
contract CreationOverride is CreationOverrideBase {
    function choose() internal pure override returns (uint64) { return 7; }
}
contract CreationBaseArg {
    uint64 public value;
    constructor(uint64 n) { value = n; }
}
function creationDeep(uint64 depth) pure returns (uint64 n) {
    if (depth != 0) return creationDeep(depth - 1);
    assembly { n := 9 }
}
contract CreationInheritedArg is CreationBaseArg(creationDeep(3)) {}
contract CreationModifier {
    uint64 public value;
    modifier init(uint64 n) { value = n; _; }
    constructor() init(creationDeep(2)) { ++value; }
}
contract CreationSelf {
    uint64 public value;
    constructor() { value = this.target(); }
    function target() external pure returns (uint64 n) { assembly { n := 11 } }
}
contract CreationMemoryOnly {
    uint64 public value;
    constructor() { uint64[] memory values = new uint64[](3); value = uint64(values.length); }
}
contract CreationNative {
    uint64 public value = G.currentApplicationId();
}
