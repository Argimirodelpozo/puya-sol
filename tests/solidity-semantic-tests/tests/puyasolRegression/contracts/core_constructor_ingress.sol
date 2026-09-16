// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

type Small is uint8;

contract ScalarIngress {
    enum E { A, B }
    uint public seen;
    constructor(Small number, bool flag, E choice) {
        seen = uint(Small.unwrap(number)) + (flag ? 1000 : 0) + uint(choice) * 100;
    }
}

contract SignedIngress {
    int public small;
    int public wide;
    constructor(int8 a, int96 b) { small = a; wide = b; }
}

contract UnsignedIngress {
    uint public seen;
    constructor(uint96 value) { seen = value; }
}
