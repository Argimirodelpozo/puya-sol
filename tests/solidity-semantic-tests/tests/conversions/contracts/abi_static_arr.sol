contract C {
    function encU128() public pure returns (bytes memory) {
        uint128[3] memory a; a[0]=1; a[1]=2; a[2]=3;
        return abi.encode(a);
    }
    function encI128() public pure returns (bytes memory) {
        int128[3] memory a; a[0]=-7; a[1]=5; a[2]=-1;
        return abi.encode(a);
    }
    function rtU128() public pure returns (uint128, uint128, uint128) {
        uint128[3] memory a; a[0]=11; a[1]=22; a[2]=33;
        uint128[3] memory b = abi.decode(abi.encode(a), (uint128[3]));
        return (b[0], b[1], b[2]);
    }
    // signed <=64-bit scalar: must SIGN-extend (0xff..fffd), not zero-pad
    function encI64() public pure returns (bytes memory) {
        int64 x = -3;
        return abi.encode(x);
    }
    // signed <=64-bit static-array elements: each element sign-extends
    function encI64arr() public pure returns (bytes memory) {
        int64[2] memory a; a[0]=-3; a[1]=5;
        return abi.encode(a);
    }
    // round-trip a signed static array through encode->decode
    function rtI128() public pure returns (int128, int128, int128) {
        int128[3] memory a; a[0]=-7; a[1]=5; a[2]=-1;
        int128[3] memory b = abi.decode(abi.encode(a), (int128[3]));
        return (b[0], b[1], b[2]);
    }
    // abi.encodePacked of a signed static array: EVM still pads each element to
    // 32 bytes, sign-extending negatives (separate handleEncode packed path).
    function packI64arr() public pure returns (bytes memory) {
        int64[2] memory a; a[0]=-3; a[1]=5;
        return abi.encodePacked(a);
    }
}
