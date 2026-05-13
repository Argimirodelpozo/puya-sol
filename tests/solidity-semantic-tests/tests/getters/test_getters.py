"""Auto-generated tests for the getters category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_array_mapping_struct(harness):
    """getters/contracts/array_mapping_struct.sol"""
    app = harness.compile_and_deploy("getters/contracts/array_mapping_struct.sol")
    # m(uint256,uint256): 0, 0 -> 0x00, 0x00
    r = harness.call(app, "m(uint256,uint256)", 0, 0)
    assert tuple(r.abi_return) == (0, 0)
    # m(uint256,uint256): 1, 0 -> 1, 2
    r = harness.call(app, "m(uint256,uint256)", 1, 0)
    assert tuple(r.abi_return) == (1, 2)
    # m(uint256,uint256): 1, 1 -> 3, 4
    r = harness.call(app, "m(uint256,uint256)", 1, 1)
    assert tuple(r.abi_return) == (3, 4)
    # m(uint256,uint256): 1, 2 -> 0x00, 0x00
    r = harness.call(app, "m(uint256,uint256)", 1, 2)
    assert tuple(r.abi_return) == (0, 0)
    # n(uint256,uint256): 0, 0 -> 0x00, 0x00
    r = harness.call(app, "n(uint256,uint256)", 0, 0)
    assert tuple(r.abi_return) == (0, 0)
    # n(uint256,uint256): 1, 0 -> 7, 8
    r = harness.call(app, "n(uint256,uint256)", 1, 0)
    assert tuple(r.abi_return) == (7, 8)
    # n(uint256,uint256): 1, 1 -> 9, 0x0a
    r = harness.call(app, "n(uint256,uint256)", 1, 1)
    assert tuple(r.abi_return) == (9, 10)
    # n(uint256,uint256): 1, 2 -> 0x00, 0x00
    r = harness.call(app, "n(uint256,uint256)", 1, 2)
    assert tuple(r.abi_return) == (0, 0)

def test_arrays(harness):
    """getters/contracts/arrays.sol"""
    app = harness.compile_and_deploy("getters/contracts/arrays.sol")
    # a(uint256,uint256): 0, 0 -> FAILURE
    r = harness.call(app, "a(uint256,uint256)", 0, 0, expect_revert=True)
    assert r.reverted
    # a(uint256,uint256): 1, 0 -> 3
    r = harness.call(app, "a(uint256,uint256)", 1, 0)
    assert r.abi_return == 3
    # a(uint256,uint256): 1, 1 -> 4
    r = harness.call(app, "a(uint256,uint256)", 1, 1)
    assert r.abi_return == 4
    # a(uint256,uint256): 2, 0 -> FAILURE
    r = harness.call(app, "a(uint256,uint256)", 2, 0, expect_revert=True)
    assert r.reverted

def test_bytes(harness):
    """getters/contracts/bytes.sol"""
    app = harness.compile_and_deploy("getters/contracts/bytes.sol")
    # b() -> 0x20, 0x03, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "b()")
    assert tuple(r.abi_return) == (32, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)

def test_mapping(harness):
    """getters/contracts/mapping.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping.sol")
    # x(uint256,uint256): 1, 2 -> 3
    r = harness.call(app, "x(uint256,uint256)", 1, 2)
    assert r.abi_return == 3
    # x(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "x(uint256,uint256)", 0, 0)
    assert r.abi_return == 0

def test_mapping_array_struct(harness):
    """getters/contracts/mapping_array_struct.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_array_struct.sol")
    # m(uint256,uint256): 0, 0 -> FAILURE
    r = harness.call(app, "m(uint256,uint256)", 0, 0, expect_revert=True)
    assert r.reverted
    # m(uint256,uint256): 1, 0 -> 1, 2
    r = harness.call(app, "m(uint256,uint256)", 1, 0)
    assert tuple(r.abi_return) == (1, 2)
    # m(uint256,uint256): 1, 1 -> 3, 4
    r = harness.call(app, "m(uint256,uint256)", 1, 1)
    assert tuple(r.abi_return) == (3, 4)
    # m(uint256,uint256): 1, 2 -> FAILURE
    r = harness.call(app, "m(uint256,uint256)", 1, 2, expect_revert=True)
    assert r.reverted
    # n(uint256,uint256): 0, 0 -> 0x00, 0x00
    r = harness.call(app, "n(uint256,uint256)", 0, 0)
    assert tuple(r.abi_return) == (0, 0)
    # n(uint256,uint256): 1, 0 -> 7, 8
    r = harness.call(app, "n(uint256,uint256)", 1, 0)
    assert tuple(r.abi_return) == (7, 8)
    # n(uint256,uint256): 1, 1 -> 9, 0x0a
    r = harness.call(app, "n(uint256,uint256)", 1, 1)
    assert tuple(r.abi_return) == (9, 10)
    # n(uint256,uint256): 1, 2 -> 0x00, 0x00
    r = harness.call(app, "n(uint256,uint256)", 1, 2)
    assert tuple(r.abi_return) == (0, 0)

def test_mapping_of_string(harness):
    """getters/contracts/mapping_of_string.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_of_string.sol")
    # x(string,uint256): 0x40, 0, 3, "abc" -> 1
    r = harness.call(app, "x(string,uint256)", 64, 0, 3, bytes.fromhex('616263'))
    assert r.abi_return == 1
    # x(string,uint256): 0x40, 1, 3, "abc" -> 2
    r = harness.call(app, "x(string,uint256)", 64, 1, 3, bytes.fromhex('616263'))
    assert r.abi_return == 2
    # x(string,uint256): 0x40, 2, 3, "abc" -> 3
    r = harness.call(app, "x(string,uint256)", 64, 2, 3, bytes.fromhex('616263'))
    assert r.abi_return == 3
    # x(string,uint256): 0x40, 0, 3, "def" -> 0x00
    r = harness.call(app, "x(string,uint256)", 64, 0, 3, bytes.fromhex('646566'))
    assert r.abi_return == 0
    # x(string,uint256): 0x40, 1, 3, "def" -> 9
    r = harness.call(app, "x(string,uint256)", 64, 1, 3, bytes.fromhex('646566'))
    assert r.abi_return == 9
    # x(string,uint256): 0x40, 2, 3, "def" -> 0x00
    r = harness.call(app, "x(string,uint256)", 64, 2, 3, bytes.fromhex('646566'))
    assert r.abi_return == 0

def test_mapping_to_struct(harness):
    """getters/contracts/mapping_to_struct.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_to_struct.sol")
    # x(uint256,uint256): 1, 2 -> 3, 4, 5, 6
    r = harness.call(app, "x(uint256,uint256)", 1, 2)
    assert tuple(r.abi_return) == (3, 4, 5, 6)
    # x(uint256,uint256): 0, 0 -> 0x00, 0x00, 0x00, 0x00
    r = harness.call(app, "x(uint256,uint256)", 0, 0)
    assert tuple(r.abi_return) == (0, 0, 0, 0)

def test_mapping_with_names(harness):
    """getters/contracts/mapping_with_names.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_with_names.sol")
    # x(uint256,uint256): 1, 2 -> 3
    r = harness.call(app, "x(uint256,uint256)", 1, 2)
    assert r.abi_return == 3
    # x(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "x(uint256,uint256)", 0, 0)
    assert r.abi_return == 0

def test_string_and_bytes(harness):
    """getters/contracts/string_and_bytes.sol"""
    app = harness.compile_and_deploy("getters/contracts/string_and_bytes.sol")
    # a() -> 0x20, 11, "hello world"
    r = harness.call(app, "a()")
    assert r.abi_return == 'hello world'
    # b() -> 0x20, 4, "ABCD"
    r = harness.call(app, "b()")
    assert r.abi_return == 'ABCD'
    # c() -> 0x20, 4, -439061522557375173052089223601630338202760422010735733633791622124826263552
    r = harness.call(app, "c()")
    assert tuple(r.abi_return) == (32, 4, -439061522557375173052089223601630338202760422010735733633791622124826263552)
    # d() -> 0x20, 4, "abcd"
    r = harness.call(app, "d()")
    assert r.abi_return == 'abcd'

def test_struct_with_bytes(harness):
    """getters/contracts/struct_with_bytes.sol"""
    app = harness.compile_and_deploy("getters/contracts/struct_with_bytes.sol")
    # s() -> 7, 0x40, 3, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "s()")
    assert tuple(r.abi_return) == (7, 64, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)

def test_struct_with_bytes_simple(harness):
    """getters/contracts/struct_with_bytes_simple.sol"""
    app = harness.compile_and_deploy("getters/contracts/struct_with_bytes_simple.sol")
    # s() -> 0x07, 0x40, 0x03, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "s()")
    assert tuple(r.abi_return) == (7, 64, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)

def test_transient_value_types(harness):
    """getters/contracts/transient_value_types.sol"""
    app = harness.compile_and_deploy("getters/contracts/transient_value_types.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0
    # f() -> -1
    r = harness.call(app, "f()")
    assert r.abi_return == -1
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0

def test_transient_value_types_multi_frame_call(harness):
    """getters/contracts/transient_value_types_multi_frame_call.sol"""
    app = harness.compile_and_deploy("getters/contracts/transient_value_types_multi_frame_call.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0
    # f() -> -2
    r = harness.call(app, "f()")
    assert r.abi_return == -2
    # h() -> -1
    r = harness.call(app, "h()")
    assert r.abi_return == -1
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0

def test_value_types(harness):
    """getters/contracts/value_types.sol"""
    app = harness.compile_and_deploy("getters/contracts/value_types.sol")
    # a() -> 3
    r = harness.call(app, "a()")
    assert r.abi_return == 3
    # b() -> 4
    r = harness.call(app, "b()")
    assert r.abi_return == 4
    # c() -> 5
    r = harness.call(app, "c()")
    assert r.abi_return == 5
    # d() -> 6
    r = harness.call(app, "d()")
    assert r.abi_return == 6
    # e() -> 0x7f00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "e()")
    assert r.abi_return == 57443731770074831323412168344153766786583156455220123566449660816425654157312
    # f() -> 0x6465616462656566313564656164000000000010000000000000000000000000
    r = harness.call(app, "f()")
    assert r.abi_return == 45410408587621877570176092079161104025617164191141970807559803119332539498496
    # g() -> 0x6465616462656566313564656164000000000000000000000000000000000010
    r = harness.call(app, "g()")
    assert r.abi_return == 45410408587621877570176092079161104025617164189874320207331573717835836293136
    # h() -> true
    r = harness.call(app, "h()")
    assert r.abi_return is True
    # i() -> 0x5555555555555555555555555555555555555555
    r = harness.call(app, "i()")
    assert r.abi_return == 487167212443634306067894944238761006551977514325
