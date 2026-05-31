contract C {
    function se(uint256 b, uint256 x) public pure returns (uint256 ret) {
        assembly { ret := signextend(b, x) }
    }
}
