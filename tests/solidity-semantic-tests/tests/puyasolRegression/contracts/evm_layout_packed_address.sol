// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Compound's RewardConfig / CoW's EthFlowOrder shape: an `address` PACKED into
// one storage slot with small ints. Under --evm-storage-layout the packed-slot
// codec had no arm for the arc4 address alias (byte[32] at packed size 20) and
// hard-errored "unsupported type 'address' in packed storage slot". Convention:
// the slot stores the TRAILING 20 bytes of the 32-byte AVM form — the same
// truncation the slot readers fold.

contract EvmLayoutPackedAddress {
    struct Config {
        address token;      // 20 bytes ─┐ one slot
        uint64 rescale;     //  8 bytes  │
        bool upscale;       //  1 byte  ─┘
    }
    mapping(address => Config) public configs;
    Config public single;

    function set(address who, address token, uint64 r, bool u) external {
        configs[who] = Config(token, r, u);
    }
    function setSingle(address token, uint64 r, bool u) external {
        single = Config(token, r, u);
    }
    function roundTrip(address who) external view returns (address, uint64, bool) {
        Config storage c = configs[who];
        return (c.token, c.rescale, c.upscale);
    }
}
