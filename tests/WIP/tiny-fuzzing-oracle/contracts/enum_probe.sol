// Cold probe: enum ORDERING comparisons (Lt/Lte/Gt/Gte) + enum bool-context +
// enum<->int casts (SolEnumBuilder cold lines).
contract C {
    enum E { A, B, C, D, F }
    function lt(uint8 x, uint8 y) public pure returns (bool) { return E(x % 5) < E(y % 5); }
    function lte(uint8 x, uint8 y) public pure returns (bool) { return E(x % 5) <= E(y % 5); }
    function gt(uint8 x, uint8 y) public pure returns (bool) { return E(x % 5) > E(y % 5); }
    function gte(uint8 x, uint8 y) public pure returns (bool) { return E(x % 5) >= E(y % 5); }
    function eq(uint8 x, uint8 y) public pure returns (bool) { return E(x % 5) == E(y % 5); }
    function toInt(uint8 x) public pure returns (uint8) { return uint8(E(x % 5)); }
    function fromInt(uint8 x) public pure returns (uint8) { return uint8(E(x % 5)); }
    function roundtrip(uint8 x) public pure returns (bool) { E e = E(x % 5); return uint8(e) == (x % 5); }
    function maxEnum(uint8 x, uint8 y) public pure returns (uint8) { E a=E(x%5); E b=E(y%5); return uint8(a > b ? a : b); }
}
