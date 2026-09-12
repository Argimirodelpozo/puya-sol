pragma solidity ^0.8.20;
contract LargeChild {
    uint64 public a0 = 1;
    uint64 public a1 = 2;
    uint64 public a2 = 3;
    uint64 public a3 = 4;
    uint64 public a4 = 5;
    uint64 public a5 = 6;
    uint64 public a6 = 7;
    uint64 public a7 = 8;
    uint64 public a8 = 9;
    uint64 public a9 = 10;
    uint64 public a10 = 11;
    uint64 public a11 = 12;
    uint64 public a12 = 13;
    uint64 public a13 = 14;
    uint64 public a14 = 15;
    uint64 public a15 = 16;
    uint64 public a16 = 17;
}
contract ChildFactory {
    function make() external returns (uint64) {
        LargeChild child = new LargeChild();
        return child.a16();
    }
}
