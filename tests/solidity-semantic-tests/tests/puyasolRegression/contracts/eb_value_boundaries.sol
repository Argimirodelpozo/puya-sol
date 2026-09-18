pragma solidity ^0.8.20;

contract EnumArrayPlaces {
    enum E { A, B, C }
    E[2] fixedValues;
    E[] dynamicValues;
    uint calls;
    event Value(E value);

    function memoryWrite() external pure returns (uint) {
        E[2] memory a;
        a[0] = E.B;
        return uint(a[0]);
    }
    function parameterWrite(E[2] memory a) external pure returns (uint) {
        a[0] = E.B;
        return uint(a[0]) * 10 + uint(a[1]);
    }
    function dynamicWrite(E[] memory a) external pure returns (uint) {
        a[0] = E.C;
        return uint(a[0]);
    }
    function tupleWrite() external pure returns (uint) {
        E[2] memory a;
        (a[0], a[1]) = (E.B, E.C);
        return uint(a[0]) * 10 + uint(a[1]);
    }
    function nestedWrite() external pure returns (uint) {
        E[2][2] memory a;
        a[1][0] = E.C;
        return uint(a[1][0]);
    }
    function storageReference() external returns (uint) {
        E[2] storage a = fixedValues;
        a[1] = E.B;
        return uint(a[1]);
    }
    function deleteElement() external pure returns (uint) {
        E[2] memory a = [E.C, E.B];
        delete a[0];
        return uint(a[0]) * 10 + uint(a[1]);
    }
    function storageWrite() external returns (uint, uint) {
        fixedValues[0] = E.B;
        if (dynamicValues.length == 0) dynamicValues.push();
        dynamicValues[0] = E.C;
        return (uint(fixedValues[0]), uint(dynamicValues[0]));
    }
    function index() internal returns (uint) { ++calls; return 0; }
    function value() internal returns (E) { ++calls; return E.B; }
    function evaluateOnce() external returns (uint, uint) {
        calls = 0;
        E[2] memory a;
        a[index()] = value();
        return (uint(a[0]), calls);
    }
    function castDiscard(uint word) external pure returns (uint) {
        E(word);
        return 7;
    }
    function dirty(uint word, uint action) external returns (uint) {
        E e;
        assembly { e := word }
        if (action == 0) return uint(e);
        if (action == 1) return e == E.A ? 0 : 1;
        if (action == 2) { E[2] memory a; a[0] = e; return uint(a[0]); }
        emit Value(e);
        return 7;
    }
    function implicitReturn(uint word) external pure returns (E e) {
        assembly { e := word }
    }
    function explicitReturn(uint word) external pure returns (E) {
        E e;
        assembly { e := word }
        return e;
    }
}

contract NamedArrayLengths {
    uint128[] numbers;
    uint8[] small;
    bytes[] strings;
    mapping(uint => uint)[] maps;

    function lengths() external view returns (uint, uint, uint, uint) {
        return (numbers.length, small.length, strings.length, maps.length);
    }
    function push() external {
        numbers.push(17);
        small.push(1);
        strings.push(hex"010203");
        maps.push();
    }
    function pop() external {
        numbers.pop();
        small.pop();
        strings.pop();
        maps.pop();
    }
    function clear() external {
        delete numbers;
        delete small;
        delete strings;
        delete maps;
    }
}

contract ValueComparisons {
    function fixedBytes(bytes2 a, bytes4 b) external pure returns (uint) {
        return (a == b ? 1 : 0) | (a != b ? 2 : 0) | (a < b ? 4 : 0)
            | (a <= b ? 8 : 0) | (a > b ? 16 : 0) | (a >= b ? 32 : 0);
    }
    function narrow(int64 a, int64 b) external pure returns (uint) {
        return (a == b ? 1 : 0) | (a != b ? 2 : 0) | (a < b ? 4 : 0)
            | (a <= b ? 8 : 0) | (a > b ? 16 : 0) | (a >= b ? 32 : 0);
    }
    function wide(int128 a, int128 b) external pure returns (uint) {
        return (a == b ? 1 : 0) | (a != b ? 2 : 0) | (a < b ? 4 : 0)
            | (a <= b ? 8 : 0) | (a > b ? 16 : 0) | (a >= b ? 32 : 0);
    }
    function converted(bytes20 raw, uint160 n, bool b) external pure returns (bytes20, bytes20, bool) {
        return (bytes20(address(raw)), bytes20(address(n)), bool(b));
    }
}
