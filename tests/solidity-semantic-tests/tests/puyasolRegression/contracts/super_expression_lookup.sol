// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ExpressionBase {
    uint256 public trace;
    function digit() internal pure virtual returns (uint256) { return 1; }
    function virtualDigit() internal pure virtual returns (uint256) { return 1; }
    function publicDigit() public pure virtual returns (uint256) { return 1; }
    function selfPointer() public view returns (uint256) {
        return invokeExternal(this.publicDigit);
    }
    function invokeExternal(function() external pure returns (uint256) target)
        internal view returns (uint256) { return target(); }
    function touch(uint256[] memory) internal pure virtual {}
    modifier stamp() virtual { _; }
    function inheritedVirtual() public stamp returns (uint256) { return virtualDigit(); }
}

contract ExpressionLeft is ExpressionBase {
    function digit() internal pure virtual override returns (uint256) {
        return super.digit() + 2;
    }
    function touch(uint256[] memory left) internal pure virtual override { left[0] += 10; }
    modifier leftStamp() { trace = trace * 10 + super.digit(); _; }
    modifier leftTouch(uint256[] memory value) { super.touch(value); _; }
    function leftPointer() internal pure returns (function() internal pure returns (uint256)) {
        return super.digit;
    }
}

contract ExpressionRight is ExpressionBase {
    function digit() internal pure virtual override returns (uint256) {
        return super.digit() + 2;
    }
    function touch(uint256[] memory right) internal pure virtual override { right[0] += 100; }
    modifier rightStamp() { trace = trace * 10 + super.digit(); _; }
    modifier rightTouch(uint256[] memory value) { super.touch(value); _; }
    function rightPointer() internal pure returns (function() internal pure returns (uint256)) {
        return super.digit;
    }
}

contract SuperExpressionLookup is ExpressionLeft, ExpressionRight {
    function publicDigit() public pure override returns (uint256) { return 7; }
    function virtualDigit() internal pure override returns (uint256) { return 7; }
    modifier stamp() override { trace = super.virtualDigit(); _; }
    function digit() internal pure override(ExpressionLeft, ExpressionRight)
        returns (uint256) { return 7; }
    function touch(uint256[] memory leaf) internal pure override(ExpressionLeft, ExpressionRight) {
        leaf[0] += 1000;
    }
    modifier resetTrace() { trace = 0; _; }
    modifier argumentStamp(uint256 value) { trace = value; _; }

    function combined() public resetTrace ExpressionLeft.leftStamp ExpressionRight.rightStamp
        returns (uint256)
    {
        trace = trace * 100 + super.digit() * 10 + digit();
        return trace;
    }
    function modifierArgument() public argumentStamp(super.digit()) returns (uint256) {
        return trace;
    }
    function mixedCalls() public pure returns (uint256) {
        return ExpressionLeft.digit() * 10000 + ExpressionRight.digit() * 1000
            + super.digit() * 100 + digit() * 10 + ExpressionBase.digit();
    }
    function localPointers() public pure returns (uint256) {
        function() internal pure returns (uint256) a = super.digit;
        function() internal pure returns (uint256) b = ExpressionBase.digit;
        function() internal pure returns (uint256) c = digit;
        return a() * 100 + b() * 10 + c();
    }
    function invoke(function() internal pure returns (uint256) target)
        internal pure returns (uint256) { return target(); }
    function passedPointers() public pure returns (uint256) {
        return invoke(super.digit) * 100 + invoke(ExpressionBase.digit) * 10 + invoke(digit);
    }
    function publicBasePointer() public pure returns (uint256) {
        return invoke(super.publicDigit);
    }
    function returnedPointers() public pure returns (uint256) {
        return invoke(leftPointer()) * 10 + invoke(rightPointer());
    }
    function dynamicPointer(bool useSuper) public pure returns (uint256) {
        function() internal pure returns (uint256) target = useSuper ? super.digit : ExpressionBase.digit;
        return invoke(target);
    }
    function sameImplementation() public pure returns (bool) {
        return super.digit == ExpressionRight.digit && super.digit != digit;
    }
    function applyTouches(uint256[] memory value) internal
        ExpressionLeft.leftTouch(value) ExpressionRight.rightTouch(value) returns (uint256)
    {
        super.touch(value);
        touch(value);
        return value[0];
    }
    function memoryModifiers(uint256 start) public returns (uint256) {
        uint256[] memory value = new uint256[](1);
        value[0] = start;
        return applyTouches(value);
    }
}

abstract contract ConstructorExpressionBase {
    uint256 public argument;
    constructor(uint256 value) { argument = value; }
    function digit() internal pure virtual returns (uint256) { return 1; }
}
abstract contract ConstructorExpressionLeft is ConstructorExpressionBase {
    uint256 public leftInit = super.digit();
    uint256 public leftBody;
    constructor() { leftBody = super.digit(); }
    function digit() internal pure virtual override returns (uint256) { return 3; }
}
abstract contract ConstructorExpressionRight is ConstructorExpressionBase {
    uint256 public rightInit = super.digit();
    uint256 public rightBody;
    constructor() {
        function() internal pure returns (uint256) target = super.digit;
        rightBody = target();
    }
    function digit() internal pure virtual override returns (uint256) { return 5; }
}
contract SuperConstructorExpressions is ConstructorExpressionLeft, ConstructorExpressionRight {
    uint256 public leafInit = super.digit();
    uint256 public modifierValue;
    modifier capture(uint256 value) { modifierValue = value; _; }
    constructor() ConstructorExpressionBase(super.digit()) capture(super.digit()) {}
    function digit() internal pure override(ConstructorExpressionLeft, ConstructorExpressionRight)
        returns (uint256) { return 7; }
}
