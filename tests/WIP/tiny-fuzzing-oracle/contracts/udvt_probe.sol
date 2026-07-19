// Cold probe: user-defined value types (UDVT) wrap/unwrap + arithmetic via using-for.
type Amount is uint128;
type Signed is int64;
using {addA, ltA} for Amount global;
using {addS} for Signed global;
function addA(Amount a, Amount b) pure returns (Amount) { return Amount.wrap(Amount.unwrap(a) + Amount.unwrap(b)); }
function ltA(Amount a, Amount b) pure returns (bool) { return Amount.unwrap(a) < Amount.unwrap(b); }
function addS(Signed a, Signed b) pure returns (Signed) { return Signed.wrap(Signed.unwrap(a) + Signed.unwrap(b)); }
contract C {
    Amount stored;
    function wrapUnwrap(uint128 x) public pure returns (uint128) { return Amount.unwrap(Amount.wrap(x)); }
    function addAmounts(uint128 x, uint128 y) public pure returns (uint128) { return Amount.unwrap(Amount.wrap(x).addA(Amount.wrap(y))); }
    function cmp(uint128 x, uint128 y) public pure returns (bool) { return Amount.wrap(x).ltA(Amount.wrap(y)); }
    function signedAdd(int64 x, int64 y) public pure returns (int64) { return Signed.unwrap(addS(Signed.wrap(x), Signed.wrap(y))); }
    function store(uint128 x) public { stored = Amount.wrap(x); }
    function load() public view returns (uint128) { return Amount.unwrap(stored); }
}
