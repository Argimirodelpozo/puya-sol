// Storage-REFERENCE argument to a modifier: the modifier param aliases the
// arg's storage location, so writes in the modifier body mutate the real state
// var, not a local copy. Found by coverage-guided fuzzing (the storage-ref
// modifier-chain alias path was 0%-covered; writes were silently dropped).
contract C {
    uint256[] arr;
    mapping(uint256 => uint256) m;
    constructor() { arr.push(10); arr.push(20); arr.push(30); }
    modifier bumpArr(uint256[] storage a, uint256 i) { a[i] += 1; _; }
    modifier bumpMap(mapping(uint256 => uint256) storage mm, uint256 k) { mm[k] += 5; _; }
    // storage-ref arg + does the modifier write, returns the post-value
    function fArr(uint256 i) public bumpArr(arr, i) returns (uint256) { return arr[i]; }
    function fMap(uint256 k) public bumpMap(m, k) returns (uint256) { return m[k]; }
    function getArr(uint256 i) public view returns (uint256) { return arr[i]; }
    function getMap(uint256 k) public view returns (uint256) { return m[k]; }
}
