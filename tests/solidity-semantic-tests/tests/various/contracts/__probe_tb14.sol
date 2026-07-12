contract C {
    bytes14 transient x;
    bytes14 y;
    function f() public returns (bytes14, bytes14) {
        x = 0xffffffffffffffffffffffffffff;
        y = 0xffffffffffffffffffffffffffff;
        return (x, y);
    }
}
