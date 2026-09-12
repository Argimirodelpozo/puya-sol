pragma solidity ^0.8.20;

contract EventEncodingFacts {
    struct Pair { uint8 a; uint16 b; }
    event Value(Pair value);
    event Array(Pair[] value);
    event Mixed(uint8 tiny, uint128 wide, bool yes, bytes data, string text);
    event Widen(uint256 value);
    event Empty();
    event Seen(Pair value, uint64 marker);
    function value() external returns (bytes32) {
        emit Value(Pair(7, 300)); return Value.selector;
    }
    function array() external returns (bytes32) {
        Pair[] memory pairs = new Pair[](2);
        pairs[0] = Pair(7, 300); pairs[1] = Pair(8, 400);
        emit Array(pairs); return Array.selector;
    }
    function mixed() external returns (bytes32) {
        emit Mixed(7, 9, true, hex"0102", "text"); return Mixed.selector;
    }
    function widen() external returns (bytes32) {
        emit Widen(uint8(7)); return Widen.selector;
    }
    function empty() external returns (bytes32) { emit Empty(); return Empty.selector; }
    function update(Pair memory pair) private pure returns (uint64) { pair.a = 9; return 11; }
    function aliasMutated() external returns (bytes32) {
        Pair memory pair = Pair(7, 300);
        emit Seen(pair, update(pair));
        return Seen.selector;
    }
}
