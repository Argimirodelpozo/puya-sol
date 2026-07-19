// Cold probe #3: advanced modifier shapes — repeated/looped placeholder,
// return-modifying body, conditional placeholder + returns, inheritance/override.
contract Base {
    uint256 log;
    modifier count() { log += 1; _; log += 10; }
    modifier repeat(uint256 n) { for (uint256 i = 0; i < n && i < 3; i++) { _; } }
    modifier clampAfter(uint256 cap) { _; if (log > cap) log = cap; }
    function getLog() public view returns (uint256) { return log; }
}
contract C is Base {
    uint256 acc;
    // repeated placeholder: body runs n times, accumulating
    function accumulate(uint256 n, uint256 v) public repeat(n) returns (uint256) {
        acc += v; return acc;
    }
    // modifier wraps + modifies state after named return
    function counted(int64 a, int64 b) public count() returns (int64 r) { r = a * b; }
    // stacked: repeat + count on a returning function
    function both(uint256 n) public repeat(n) count() returns (uint256) { acc += 1; return acc; }
    // conditional-after modifier interacting with a return
    function clamped(uint256 add) public clampAfter(100) returns (uint256) {
        log += add; return log;
    }
    function getAcc() public view returns (uint256) { return acc; }
}
