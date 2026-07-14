// A side-effecting SIGNED sub-word mapping key must evaluate ONCE in compound
// assign / delete (read-modify-write references the derived box key twice). The
// key materialization guard only caught AssignmentExpression keys; a call-valued
// key (k() with cnt++) was materialized only for UNSIGNED keys via puya CSE
// (identical derivations merged) — a signed key's sign-extension defeats CSE, so
// k() ran twice. Found by the corpus-mutation fuzzer (mapping_key_side_effect_once
// uint256->int48).
contract C {
    uint256 public cnt;
    mapping(int48 => uint256) ms;
    mapping(int16 => uint256) ms16;
    function ks(int48 v) internal returns (int48) { cnt++; return v; }
    function ks16(int16 v) internal returns (int16) { cnt++; return v; }
    function compound() external returns (uint256, uint256) { ms[2]=10; cnt=0; ms[ks(2)] += 5; return (uint256(ms[2]), cnt); }   // (15,1)
    function del() external returns (uint256, uint256) { ms[6]=1; cnt=0; delete ms[ks(6)]; return (ms[6], cnt); }                 // (0,1)
    function write() external returns (uint256, uint256) { cnt=0; ms[ks(1)] = 55; return (ms[1], cnt); }                          // (55,1)
    function read() external returns (uint256, uint256) { ms[3]=7; cnt=0; uint256 v = ms[ks(3)]; return (v, cnt); }               // (7,1)
    function compound16() external returns (uint256, uint256) { ms16[-4]=20; cnt=0; ms16[ks16(-4)] += 3; return (ms16[-4], cnt); } // (23,1)
}
