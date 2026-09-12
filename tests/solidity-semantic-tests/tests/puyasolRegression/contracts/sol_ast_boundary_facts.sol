pragma solidity ^0.8.20;

contract FrameBase {
    struct Value { uint256 x; }

    function mutate(Value memory value) public pure {
        value.x = 7;
        value = Value(9);
        value.x = 11;
    }

    function inheritedFrame() external pure returns (uint256) {
        Value memory value = Value(5);
        mutate(value);
        return value.x;
    }
}

contract SolAstBoundaryFacts is FrameBase {
    struct Fields { uint64 selector; uint64 length; }
    uint64 private counter;

    function frame() external pure returns (uint256) {
        Value memory value = Value(5);
        mutate(value);
        return value.x;
    }

    function fields() external pure returns (uint64, uint64, uint64, uint64) {
        Fields memory value = Fields(17, 23);
        uint64[3] memory fixedValues;
        bytes memory dynamicValues = new bytes(4);
        return (value.selector, value.length, uint64(fixedValues.length), uint64(dynamicValues.length));
    }

    function f() external pure {}

    function step(uint64 digit) internal returns (uint64) {
        counter = counter * 10 + digit;
        return counter;
    }

    function addressOptions() external returns (bool, uint64) {
        counter = 0;
        address receiver = this.f{gas: step(1)}.address;
        return (receiver == address(this), counter);
    }

    function conditionalAddress(bool choose) external returns (bool, uint64) {
        counter = 0;
        address receiver = (choose ? this.f : this.f){gas: step(choose ? 1 : 2)}.address;
        return (receiver == address(this), counter);
    }

    function selectorOptions() external returns (bool, uint64) {
        counter = 0;
        bytes4 selector = this.f{gas: step(1)}.selector;
        return (selector == this.f.selector, counter);
    }

    function unusedOptions() external returns (uint64) {
        counter = 0;
        this.f{gas: step(1)};
        return counter;
    }
}
