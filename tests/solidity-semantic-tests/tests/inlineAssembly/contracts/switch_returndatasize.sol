// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Callee { function ping() external pure returns (uint256) { return 7; } }
contract SwitchRds {
    Callee public c;
    constructor() { c = new Callee(); }
    /// GPv2SafeERC20's non-standard-ERC20 probe: switch on a uint64-natured builtin
    function probe() external returns (uint256 r) {
        address t = address(c);
        bytes4 sel = Callee.ping.selector;
        assembly {
            mstore(0x00, sel)
            let ok := call(gas(), t, 0, 0x00, 4, 0x20, 0x20)
            switch returndatasize()
            case 0 { r := 1 }
            case 32 { r := 2 }
            default { r := 3 }
        }
    }
}
