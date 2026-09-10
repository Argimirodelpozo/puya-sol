// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StorageReturnProtocol {
    struct Pair { uint16 x; uint16 y; }
    Pair private first;
    Pair private second;
    uint64 private calls;

    modifier tick() { ++calls; _; }

    function pair(bool swap) internal tick returns (Pair storage a, int8 n, Pair storage b) {
        a = swap ? second : first;
        b = swap ? first : second;
        n = -7;
    }

    function forward(bool swap) internal returns (Pair storage, int8, Pair storage) {
        return ((pair(swap)));
    }

    function reset() private {
        delete first;
        delete second;
        calls = 0;
    }

    function single(bool swap) internal returns (Pair storage) {
        (Pair storage result, , ) = forward(swap);
        return result;
    }

    function singleReference(bool swap) external returns (uint256, uint256, uint64) {
        reset();
        single(swap).x = 53;
        return (first.x, second.x, calls);
    }

    function references(bool swap) external returns (uint256, uint256, uint256, uint256, int128, uint64) {
        reset();
        (Pair storage a, int8 n, Pair storage b) = forward(swap);
        a.x = 23;
        b.y = 29;
        return (first.x, first.y, second.x, second.y, n, calls);
    }

    function copies(bool swap) external returns (uint256, uint256, uint256, uint256, uint256, uint256, int128, uint64) {
        reset();
        first = Pair(3, 5);
        second = Pair(7, 11);
        (Pair memory a, int8 n, Pair memory b) = forward(swap);
        a.x = 99;
        b.y = 88;
        return (first.x, first.y, second.x, second.y, a.x, b.y, n, calls);
    }

    function rebind() external returns (uint256, uint256, uint256, uint256, uint64) {
        reset();
        (Pair storage a, , Pair storage b) = forward(false);
        (a, , b) = forward(true);
        a.x = 31;
        b.y = 37;
        return (first.x, first.y, second.x, second.y, calls);
    }

    function parameters(Pair storage a, Pair storage b) internal pure returns (Pair storage, uint256, Pair storage) {
        return (a, 7, b);
    }

    function parameterReferences() external returns (uint256, uint256, uint256) {
        reset();
        (Pair storage a, uint256 n, Pair storage b) = parameters(first, second);
        a.x = 41;
        b.y = 43;
        return (first.x, second.y, n);
    }

    function local(bool chooseFirst) internal view returns (Pair storage, uint256) {
        Pair storage result = chooseFirst ? first : second;
        return (result, 7);
    }

    function localReference(bool chooseFirst) external returns (uint256, uint256, uint256) {
        reset();
        (Pair storage a, uint256 n) = local(chooseFirst);
        a.x = 47;
        return (first.x, second.x, n);
    }
}
