// Cold-modifier sweep #2: storage-REFERENCE args to modifiers (mappingHolder /
// indexedPath / fieldPath / stateRead alias paths — cold) + modifiers on
// MULTIPLE named returns (__mod_retval_ multi path — cold). Differentially
// fuzzable: mutations observed via getters + returns.
contract C {
    uint256[] arr;
    mapping(uint256 => uint256) bal;
    struct P { uint256 x; uint256 y; }
    P p;
    uint256 counter;

    constructor() { arr.push(0); arr.push(0); arr.push(0); }

    // modifier takes a STORAGE-REF (dynamic array) arg and MUTATES it — writes
    // must propagate to the real storage (stateRead alias path)
    modifier bump(uint256[] storage a, uint256 i) { if (i < a.length) a[i] += 1; _; }
    // modifier takes a MAPPING-derived storage ref (indexedPath alias)
    modifier credit(uint256 k, uint256 amt) { bal[k] += amt; _; }
    // modifier reads a STRUCT FIELD ref (fieldPath) — here via the struct itself
    modifier touchP() { p.x += 1; _; p.y += 2; }
    // plain counting modifier
    modifier tick() { counter += 1; _; }

    // function with MULTIPLE named returns wrapped by a modifier
    function multiNamed(uint64 a, uint64 b) public tick() returns (uint64 s, uint64 d, uint64 m) {
        s = a + b;
        d = a >= b ? a - b : b - a;
        m = a * b;
    }

    // storage-ref arg modifier on a function that ALSO returns a named value
    function bumpAndGet(uint256 i) public bump(arr, i) returns (uint256 r) {
        r = i < arr.length ? arr[i] : 0;
    }

    function creditGet(uint256 k, uint256 amt) public credit(k, amt) returns (uint256) {
        return bal[k];
    }

    function touched() public touchP() returns (uint256, uint256) { return (p.x, p.y); }

    // getters
    function getArr(uint256 i) public view returns (uint256) { return i < arr.length ? arr[i] : 999; }
    function getBal(uint256 k) public view returns (uint256) { return bal[k]; }
    function getP() public view returns (uint256, uint256) { return (p.x, p.y); }
    function getCounter() public view returns (uint256) { return counter; }
}
