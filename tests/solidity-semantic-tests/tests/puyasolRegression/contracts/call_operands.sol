pragma solidity ^0.8.20;

contract CallsTarget {
    struct S { uint256 n; uint256[] hidden; }
    S public item;
    constructor() { item.n = 7; }
    function pair(uint256 a, uint256 b) external pure returns (uint256) { return 10 * a + b; }
}

contract CallsProbes {
    uint256 private counter;
    uint256[] private data;
    bytes private raw;
    struct Holder { uint256[] values; }
    Holder private holder;
    struct Pair { uint256 a; uint256 b; }
    Pair[] private pairs;

    function reason() internal returns (string memory) { ++counter; return "reason"; }
    function requireMessage() external returns (uint256) {
        counter = 0;
        require(true, reason());
        return counter;
    }
    function builtin() external returns (uint256) {
        counter = 0;
        return addmod(counter++, counter, 100);
    }
    function externalArgs(CallsTarget target) external returns (uint256) {
        counter = 0;
        return target.pair(counter++, counter);
    }
    function getter(CallsTarget target) external view returns (uint256) { return target.item(); }
    function namedTarget(uint256 a, uint256 b) internal pure returns (uint256) { return a * 10 + b; }
    function named() external returns (uint256) {
        counter = 0;
        return namedTarget({b: counter++, a: counter++});
    }
    function pick(CallsTarget target) internal returns (CallsTarget) {
        counter = counter * 10 + 1;
        return target;
    }
    function gasValue() internal returns (uint256) { counter = counter * 10 + 2; return 500000; }
    function argValue() internal returns (uint256) { counter = counter * 10 + 3; return 7; }
    function externalOptions(CallsTarget target) external returns (uint256) {
        counter = 0;
        pick(target).pair{gas: gasValue()}(argValue(), 0);
        return counter;
    }
    function nestedPushValue() internal returns (uint256) { data.push(7); return 8; }
    function nestedPush() external returns (uint256, uint256) {
        delete data;
        data.push(nestedPushValue());
        return (data.length, data[0]);
    }
    function nestedByteValue() internal returns (bytes1) { raw.push(0x07); return 0x08; }
    function nestedBytes() external returns (bytes memory) {
        delete raw;
        raw.push(nestedByteValue());
        return raw;
    }
    function allocate(uint256 n) external pure returns (uint256) {
        return new bytes(n).length;
    }
    function allocateArray(uint256 n) external pure returns (uint256) {
        return new uint256[](n).length;
    }
    function allocateString(uint256 n) external pure returns (uint256) {
        return bytes(new string(n)).length;
    }
    function allocateBool(uint256 n) external pure returns (uint256) {
        return new bool[](n).length;
    }
    function requireCondition() external returns (uint256) {
        counter = 0;
        require(counter++ == 0, reason());
        return counter;
    }
    function nestedAlias() external returns (bytes memory) {
        delete raw;
        bytes storage alias_ = raw;
        alias_.push(nestedByteValue());
        return raw;
    }
    function allocateOnce() external returns (uint256, uint256) {
        counter = 0;
        uint256[] memory array = new uint256[](counter++ + 3);
        return (array.length, counter);
    }
    function makeArray() internal returns (uint256[] memory values) {
        ++counter;
        values = new uint256[](1);
        values[0] = 7;
    }
    function takeArray(uint256[] memory values, uint256 n) internal pure returns (uint256) {
        return values[0] * 10 + n;
    }
    function mutableOperand() external returns (uint256) {
        counter = 0;
        return takeArray(makeArray(), counter);
    }
    function abiOperands() external returns (uint256, uint256) {
        counter = 0;
        return abi.decode(abi.encode(counter++, counter), (uint256, uint256));
    }
    function packedOperands() external returns (bytes memory) {
        counter = 0;
        return abi.encodePacked(counter++, counter);
    }
    function namedExternal(CallsTarget target) external returns (uint256) {
        counter = 0;
        return target.pair({b: counter++, a: counter++});
    }
    function namedStruct() external returns (uint256) {
        counter = 0;
        Pair memory pair = Pair({b: counter++, a: counter++});
        return pair.a * 10 + pair.b;
    }
    function nestedFieldValue() internal returns (uint256) { holder.values.push(7); return 8; }
    function nestedFieldPush() external returns (uint256, uint256, uint256) {
        delete holder;
        holder.values.push(nestedFieldValue());
        return (holder.values.length, holder.values[0], holder.values[1]);
    }
    function pushedStruct() external returns (uint256, uint256) {
        delete pairs;
        pairs.push().a = 7;
        pairs.push().b = 8;
        return (pairs[0].a, pairs[1].b);
    }
}
