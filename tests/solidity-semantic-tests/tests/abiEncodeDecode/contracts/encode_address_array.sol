// CUSTOM (puya-sol): abi.encode(address[]) must adapt each internal account to
// a canonical 20-byte EVM address and place it in a left-zero-padded word.
contract C {
    function enc(address[] memory a) public pure returns (bytes memory) {
        return abi.encode(a);
    }
}
