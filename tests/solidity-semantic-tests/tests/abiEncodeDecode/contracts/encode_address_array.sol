// CUSTOM (puya-sol): abi.encode(address[]) must lay out each element as a
// 32-byte word (the ARC4 account), not stride at 20 bytes. Regression for
// the elemByteSize=20-vs-32 bug.
contract C {
    function enc(address[] memory a) public pure returns (bytes memory) {
        return abi.encode(a);
    }
}
