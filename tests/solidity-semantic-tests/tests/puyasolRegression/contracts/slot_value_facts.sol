pragma solidity ^0.8.20;

contract SlotValueFacts {
    struct Packed { address account; uint96 padding; }
    struct Alone { address account; uint256 padding; }
    Packed private packed;
    Alone private alone;
    address[2] private accounts;
    string[2] private messages;
    bytes[2] private buffers;
    bool[] private flags;
    uint256[][3] private branches;

    function fixedAccounts(address first, address second) external returns (bool, bool) {
        accounts = [first, second];
        address[2] memory copy = accounts;
        bool roundtrip = copy[0] == first && copy[1] == second;
        delete accounts;
        return (roundtrip, accounts[0] == address(0) && accounts[1] == address(0));
    }

    function packedAccount(address value) external returns (bool, bool, bool) {
        packed.account = value;
        packed.padding = 17;
        Packed memory copy = packed;
        bool read = copy.account == value && copy.padding == 17;
        packed = copy;
        bool write = packed.account == value && packed.padding == 17;
        delete packed;
        packed.padding = 19;
        return (read, write, packed.account == address(0) && packed.padding == 19);
    }

    function standaloneAccount(address value) external returns (bool, bool) {
        alone.account = value;
        alone.padding = 17;
        Alone memory copy = alone;
        bool read = copy.account == value && copy.padding == 17;
        alone = copy;
        return (read, alone.account == value && alone.padding == 17);
    }

    function fixedStrings() external returns (uint64, uint64, uint64, uint64) {
        string[2] memory input = [string("a"), "0123456789012345678901234567890123456789"];
        messages = input;
        string[2] memory copy = messages;
        delete messages;
        return (uint64(bytes(copy[0]).length), uint64(bytes(copy[1]).length),
                uint64(bytes(messages[0]).length), uint64(bytes(messages[1]).length));
    }

    function fixedBytes() external returns (bytes32, bytes32, uint64, uint64) {
        bytes[2] memory input;
        input[0] = hex"001122";
        input[1] = hex"0123456789012345678901234567890123456789012345678901234567890123456789";
        buffers = input;
        bytes[2] memory copy = buffers;
        delete buffers;
        return (keccak256(copy[0]), keccak256(copy[1]), uint64(buffers[0].length), uint64(buffers[1].length));
    }

    function booleans(uint256 n) external returns (uint256, uint256, uint256) {
        bool[] memory input = new bool[](n);
        for (uint256 i; i < n; ++i) input[i] = i % 3 == 0;
        flags = input;
        bool[] memory copy = flags;
        uint256 count;
        for (uint256 i; i < n; ++i) if (copy[i]) ++count;
        flags = new bool[](0);
        for (uint256 i; i < n; ++i) flags.push();
        uint256 stale;
        for (uint256 i; i < n; ++i) if (flags[i]) ++stale;
        delete flags;
        flags.push();
        return (count, stale, flags[0] ? 1 : 0);
    }

    function shorterNested() external returns (uint256, uint256, uint256) {
        branches[2].push(7);
        uint256[][2] memory input;
        input[0] = new uint256[](1);
        input[0][0] = 11;
        input[1] = new uint256[](2);
        input[1][1] = 22;
        branches = input;
        uint256[][3] memory copy = branches;
        delete branches;
        return (copy[0][0], copy[1][1], copy[2].length);
    }
}
