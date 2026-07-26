// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored). Guards asm logN event emission:
// log0..log4 map to a single AVM `log` = topic1 ++ … ++ topicN (each 32 bytes)
// ++ memory[offset:offset+length]. Previously log2/log3/log4 hard-errored
// ("unsupported Yul builtin"), blocking real event-emitting contracts (solady
// Ownable etc.). Topics pass through as-is (topic0 is typically the keccak
// event-signature hash); length must be constant.
contract AsmLogEmit {
    // 2 topics + 32 bytes of data → 96-byte flat log.
    function emit2(uint256 t0, uint256 t1, uint256 b) external {
        assembly { mstore(0x00, b) log2(0x00, 0x20, t0, t1) }
    }
    // data only, no topics → 32-byte log.
    function emit0(uint256 b) external {
        assembly { mstore(0x00, b) log0(0x00, 0x20) }
    }
}
