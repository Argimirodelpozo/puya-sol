// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract BlockTryCallee {
    function one() external pure returns (int16) { return -123; }
    function tupleValue() external pure returns (int16, uint8, bytes memory) {
        return (-123, 251, hex"00ff0102");
    }
    function ping() external pure {}
    function checked(bool fail) external pure { require(!fail); }
}

contract BlockTryCaller {
    uint64 public count;

    function one(BlockTryCallee other) external view returns (int16) {
        try other.one() returns (int16 value) { return value; }
        catch { return 999; }
        return 998;
    }
    function tupleValue(BlockTryCallee other) external view returns (int16, uint8, bytes memory) {
        try other.tupleValue() returns (int16 a, uint8 b, bytes memory c) { return (a, b, c); }
        catch { return (999, 0, hex""); }
    }
    function unnamed(BlockTryCallee other) external view returns (uint8, bytes memory) {
        try other.tupleValue() returns (int16, uint8 value, bytes memory data) {
            return (value, data);
        } catch { return (0, hex""); }
    }
    function omitted(BlockTryCallee other) external view returns (uint64) {
        try other.tupleValue() { return 7; }
        catch { return 999; }
    }
    function voidReturn(BlockTryCallee other) external view returns (uint64) {
        try other.ping() { return 11; }
        catch { return 999; }
    }
    function nested(BlockTryCallee other) external view returns (int16) {
        int16 value = 5;
        int16 result;
        try other.one() returns (int16 value) {
            result = value;
            try other.one() returns (int16 value) { result += value; }
            catch { return 999; }
        } catch { return 998; }
        return result + value;
    }
    function selfValue() external pure returns (int16) { return -22; }
    function selfTry() external view returns (int16) {
        try this.selfValue() returns (int16 value) { return value; }
        catch { return 999; }
    }
    function acceptedFailure(BlockTryCallee other, bool fail) external returns (uint64) {
        ++count;
        try other.checked(fail) { return count; }
        catch { return 999; }
    }
}
