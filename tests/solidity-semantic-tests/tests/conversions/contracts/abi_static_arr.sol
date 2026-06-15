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
}
