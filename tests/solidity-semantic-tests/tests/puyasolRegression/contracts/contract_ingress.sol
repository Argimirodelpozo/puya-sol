pragma solidity ^0.8.20;

contract ContractIngress {
    enum Choice { A, B }
    mapping(Choice => uint256) public byChoice;
    mapping(uint8 => uint256) public byNumber;
    constructor() { byChoice[Choice.B] = 9; byNumber[7] = 11; }
    modifier skip() { if (false) _; }
    function skipped(uint8) external pure skip returns (uint256) { return 99; }
    function shared(uint8 value) public pure returns (uint256) { return value; }
    function ignored(uint8) internal pure returns (uint256) { return 7; }
    function fromDirty(uint256 word) external pure returns (uint256) {
        uint8 value;
        assembly { value := word }
        return ignored(value) + shared(value);
    }
    function recurse(uint8 value) public pure returns (uint256) {
        return value == 0 ? 1 : 1 + recurse(value - 1);
    }
    function enumArg(Choice value) external pure returns (uint256) { return uint256(value); }
    function enumIgnored(Choice) external pure returns (uint256) { return 77; }
    function selfEnum(uint256 word, bool pointer) external view returns (uint256) {
        Choice value;
        assembly { value := word }
        if (pointer) {
            function(Choice) external view returns (uint256) target = this.enumIgnored;
            return target(value);
        }
        return this.enumIgnored(value);
    }
    function observe(uint8 value) external pure returns (uint256 result) {
        assembly { result := value }
    }
    function selfWord(uint256 word) external view returns (uint256) {
        uint8 value;
        assembly { value := word }
        return this.observe(value);
    }
    function carriers(uint8 small, uint128 wide) public pure returns (uint128) { return small + wide; }
    function internalPointer() external pure returns (uint128) {
        function(uint8, uint128) internal pure returns (uint128) target = carriers;
        return target(7, 1 << 90);
    }
    function externalPointer() external view returns (uint128) {
        function(uint8, uint128) external view returns (uint128) target = this.carriers;
        return target(7, 1 << 90);
    }
}
