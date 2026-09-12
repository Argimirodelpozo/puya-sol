// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

library MemberConstants {
    bytes1 constant ONE = 0x12;
    bytes4 constant PAD = ONE;
    int8 constant NEG = -7;
}

contract ExpressionMembers {
    uint256 count;
    bytes4 constant LOCAL = MemberConstants.PAD;
    function target() external {}
    function other() external {}
    function next() internal returns (function () external) { ++count; return this.target; }
    function receiver(uint256 amount) internal returns (ExpressionMembers) { count += amount; return this; }
    function value() internal returns (bytes4) { ++count; return 0x12345678; }
    function gasOption() internal returns (uint256) { count *= 10; ++count; return 100000; }
    function condition(bool yes) internal returns (bool) { ++count; return yes; }

    function constants() external pure returns (bytes4, uint256, bytes1, int256, bool) {
        return (MemberConstants.PAD, MemberConstants.PAD.length, MemberConstants.PAD[3],
                MemberConstants.NEG, LOCAL == MemberConstants.PAD);
    }
    function dynamicSelector() external returns (bool, uint256) {
        count = 0;
        bytes4 selected = next().selector;
        return (selected == this.target.selector, count);
    }
    function optionsSelector() external returns (bool, uint256) {
        count = 0;
        bytes4 selected = (receiver(1).target{gas: gasOption()}).selector;
        return (selected == this.target.selector, count);
    }
    function conditionalSelector(bool yes, bool different) external returns (bool, uint256) {
        count = 0;
        bytes4 selected = different
            ? (condition(yes) ? receiver(10).target : receiver(100).other).selector
            : (condition(yes) ? receiver(10).target : receiver(100).target).selector;
        return (selected == (different && !yes ? this.other.selector : this.target.selector), count);
    }
    function mixedSelector(bool yes) external returns (bool, uint256) {
        count = 0;
        function () external pointer = this.other;
        bytes4 selected = (condition(yes) ? receiver(10).target : pointer).selector;
        return (selected == (yes ? this.target.selector : this.other.selector), count);
    }
    function fixedLength() external returns (uint256, uint256) {
        count = 0;
        uint256 length = value().length;
        return (length, count);
    }
    function sliceLength(uint256[] calldata data, uint256 start, uint256 end) external pure returns (uint256) {
        return uint256[](data[start:end]).length;
    }
    function nestedLength(uint256[] calldata data, uint256 start, uint256 end) external pure returns (uint256) {
        return uint256[](data[1:][start:end]).length;
    }
}
