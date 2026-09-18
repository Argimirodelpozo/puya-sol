pragma solidity ^0.8.28;

// The test replaces these marker comments with zero, one or three parentheses.
contract ParenthesizedExpressions {
    error Failure(uint256 count);
    uint256 transient temporary;
    uint256[2] small;
    uint256[130] paged;
    uint256 calls;
    bytes4 constant FIRST = hex"01020304";
    bytes4 constant SECOND = FIRST;

    function transientValue() external returns (uint256, uint256) {
        /*(*/temporary/*)*/ = 5;
        /*(*/temporary/*)*/ += 2;
        ++/*(*/temporary/*)*/;
        uint256 before = /*(*/temporary/*)*/;
        delete /*(*/temporary/*)*/;
        return (before, /*(*/temporary/*)*/);
    }

    function namedArrays() external returns (uint256, uint256) {
        /*(*/small/*)*/[1] = 7;
        /*(*/paged/*)*/[129] = 9;
        return (/*(*/small/*)*/[1], /*(*/paged/*)*/[129]);
    }

    function constants() external pure returns (bytes4, bytes4, bytes20) {
        bytes4 first;
        bytes4 second;
        assembly { first := FIRST second := SECOND }
        return (/*(*/first/*)*/, /*(*/second/*)*/, ripemd160(/*(*/""/*)*/));
    }

    function increment(uint256 n) internal pure returns (uint256) { return n + 1; }
    function identity(uint256 n) external pure returns (uint256) { return n; }
    function functionValues() external view returns (uint256, bool) {
        function(uint256) internal pure returns (uint256) pointer = /*(*/increment/*)*/;
        function(uint256) external pure returns (uint256) other = /*(*/ParenthesizedExpressions(address(this))/*)*/.identity;
        return (/*(*/pointer/*)*/(7), /*(*/other/*)*/.address == address(this));
    }

    function next() internal returns (uint256) { return calls++; }
    function effects() external returns (uint256, uint256) {
        calls = 0;
        /*(*/small/*)*/[/*(*/next()/*)*/] = 41;
        return (/*(*/small/*)*/[0], calls);
    }

    function customRequire(bool ok) external returns (uint256) {
        calls = 0;
        require(ok, Failure(/*(*/next()/*)*/));
        return calls;
    }

    function tuplesAndArrays() external pure returns (uint256, uint256, uint256) {
        (uint256 a, uint256 b) = /*(*/(uint256(3), uint256(5))/*)*/;
        /*(*/(/*(*/a/*)*/, /*(*/b/*)*/)/*)*/ = /*(*/(b, a)/*)*/;
        uint256[1] memory singleton = /*(*/[uint256(7)]/*)*/;
        (, a) = /*(*/(a, b)/*)*/;
        return (a, b, /*(*/singleton/*)*/[0]);
    }
}
