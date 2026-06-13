// CUSTOM (puya-sol regression): a public getter must apply the same
// sub-64-bit ABI param validation as a regular method. The getter for
// `mapping(uint8 => V)` decodes its key as a full uint64 (the sub-64->uint64
// selector rule), so a raw caller can pass 256; without the entry mask/
// assert the getter would hash the wrong storage slot instead of reverting.
contract C {
    mapping(uint8 => uint256) public m;

    constructor() {
        m[0] = 99;
        m[5] = 42;
    }
}
