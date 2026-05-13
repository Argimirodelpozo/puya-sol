"""Auto-generated tests for the userDefinedValueType category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_abicodec(harness):
    """userDefinedValueType/contracts/abicodec.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/abicodec.sol")
    # g() -> true
    r = harness.call(app, "g()")
    assert r.abi_return is True

def test_assembly_access_bytes2_abicoder_v1(harness):
    """userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v1.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v1.sol")
    # f(bytes2): "ab" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes2)", bytes.fromhex('6162'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # g(bytes2): "ab" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g(bytes2)", bytes.fromhex('6162'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # f(bytes2): "abcdef" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes2)", bytes.fromhex('616263646566'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # g(bytes2): "abcdef" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g(bytes2)", bytes.fromhex('616263646566'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # h(uint256): "ab" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h(uint256)", bytes.fromhex('6162'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # h(uint256): "abcdef" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h(uint256)", bytes.fromhex('616263646566'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680

def test_assembly_access_bytes2_abicoder_v2(harness):
    """userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v2.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v2.sol")
    # f(bytes2): "ab" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes2)", bytes.fromhex('6162'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # g(bytes2): "ab" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g(bytes2)", bytes.fromhex('6162'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # f(bytes2): "abcdef" -> FAILURE
    r = harness.call(app, "f(bytes2)", bytes.fromhex('616263646566'), expect_revert=True)
    assert r.reverted
    # g(bytes2): "abcdef" -> FAILURE
    r = harness.call(app, "g(bytes2)", bytes.fromhex('616263646566'), expect_revert=True)
    assert r.reverted
    # h(uint256): "ab" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h(uint256)", bytes.fromhex('6162'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # h(uint256): "abcdef" -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h(uint256)", bytes.fromhex('616263646566'))
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680

def test_calldata(harness):
    """userDefinedValueType/contracts/calldata.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/calldata.sol")
    # test_f() -> true
    r = harness.call(app, "test_f()")
    assert r.abi_return is True
    # test_g() -> true
    r = harness.call(app, "test_g()")
    assert r.abi_return is True
    # addresses(uint256): 0 -> 0x18
    r = harness.call(app, "addresses(uint256)", 0)
    assert r.abi_return == 24
    # addresses(uint256): 1 -> 0x19
    r = harness.call(app, "addresses(uint256)", 1)
    assert r.abi_return == 25
    # addresses(uint256): 3 -> 0x1b
    r = harness.call(app, "addresses(uint256)", 3)
    assert r.abi_return == 27
    # addresses(uint256): 4 -> 0x1c
    r = harness.call(app, "addresses(uint256)", 4)
    assert r.abi_return == 28
    # addresses(uint256): 5 -> FAILURE
    r = harness.call(app, "addresses(uint256)", 5, expect_revert=True)
    assert r.reverted

def test_calldata_to_storage(harness):
    """userDefinedValueType/contracts/calldata_to_storage.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/calldata_to_storage.sol")
    # s() -> 0, 0, 0x00, 0
    r = harness.call(app, "s()")
    assert tuple(r.abi_return) == (0, 0, 0, 0)
    # f((uint8,uint16,bytes2,uint8)): 1, 0xff, "ab", 15 ->
    r = harness.call(app, "f((uint8,uint16,bytes2,uint8))", 1, 255, bytes.fromhex('6162'), 15)
    # (void return — call succeeding is the assertion)
    # s() -> 1, 0xff, 0x6162000000000000000000000000000000000000000000000000000000000000, 15
    r = harness.call(app, "s()")
    assert tuple(r.abi_return) == (1, 255, 44047497324925121336511606693520958599579173549109180625971642598225011015680, 15)
    # g(uint16[]): 0x20, 3, 1, 2, 3 -> 0x20, 3, 1, 2, 3
    r = harness.call(app, "g(uint16[])", 32, 3, 1, 2, 3)
    # TODO: verify structural decoding matches expected: 32, 3, 1, 2, 3
    assert not r.reverted
    # small(uint256): 0 -> 1
    r = harness.call(app, "small(uint256)", 0)
    assert r.abi_return == 1
    # small(uint256): 1 -> 2
    r = harness.call(app, "small(uint256)", 1)
    assert r.abi_return == 2
    # h(bytes2[]): 0x20, 3, "ab", "cd", "ef" -> 0x20, 3, "ab", "cd", "ef"
    r = harness.call(app, "h(bytes2[])", 32, 3, bytes.fromhex('6162'), bytes.fromhex('6364'), bytes.fromhex('6566'))
    # TODO: verify expected: 0x20 | 3 | "ab" | "cd" | "ef"
    assert not r.reverted
    # l(uint256): 0 -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "l(uint256)", 0)
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # l(uint256): 1 -> 0x6364000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "l(uint256)", 1)
    assert r.abi_return == 44955656716221210881917421608902818716714500272103248770446148185689417580544

def test_cleanup(harness):
    """userDefinedValueType/contracts/cleanup.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/cleanup.sol")
    # ret() -> 0xff
    r = harness.call(app, "ret()")
    assert r.abi_return == 255
    # f(uint8): 0x1ff -> FAILURE
    r = harness.call(app, "f(uint8)", 511, expect_revert=True)
    assert r.reverted
    # f(uint8): 0xff -> 0xff
    r = harness.call(app, "f(uint8)", 255)
    assert r.abi_return == 255
    # mem() -> 0x20, 2, 0xff, 0xff
    r = harness.call(app, "mem()")
    assert tuple(r.abi_return) == (32, 2, 255, 255)
    # stor() -> 1, 0xff, 2
    r = harness.call(app, "stor()")
    assert tuple(r.abi_return) == (1, 255, 2)

def test_cleanup_abicoderv1(harness):
    """userDefinedValueType/contracts/cleanup_abicoderv1.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/cleanup_abicoderv1.sol")
    # ret() -> 0xff
    r = harness.call(app, "ret()")
    assert r.abi_return == 255
    # f(uint8): 0x1ff -> 0xff
    r = harness.call(app, "f(uint8)", 511)
    assert r.abi_return == 255
    # f(uint8): 0xff -> 0xff
    r = harness.call(app, "f(uint8)", 255)
    assert r.abi_return == 255
    # mem() -> 0x20, 2, 0x01ff, 0xff
    r = harness.call(app, "mem()")
    assert tuple(r.abi_return) == (32, 2, 511, 255)
    # stor() -> 1, 0xff, 2
    r = harness.call(app, "stor()")
    assert tuple(r.abi_return) == (1, 255, 2)

def test_constant(harness):
    """userDefinedValueType/contracts/constant.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/constant.sol")
    # s() -> 165521356710917456517261742455526507355687727119203895813322792776
    r = harness.call(app, "s()")
    assert r.abi_return == 165521356710917456517261742455526507355687727119203895813322792776
    # t() -> 165521356710917456517261742455526507355687727119203895813322792776
    r = harness.call(app, "t()")
    assert r.abi_return == 165521356710917456517261742455526507355687727119203895813322792776
    # u() -> 165521356710917456517261742455526507355687727119203895813322792776
    r = harness.call(app, "u()")
    assert r.abi_return == 165521356710917456517261742455526507355687727119203895813322792776

def test_conversion(harness):
    """userDefinedValueType/contracts/conversion.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/conversion.sol")
    # f(uint256): 1 -> 1
    r = harness.call(app, "f(uint256)", 1)
    assert r.abi_return == 1
    # f(uint256): 2 -> 2
    r = harness.call(app, "f(uint256)", 2)
    assert r.abi_return == 2
    # f(uint256): 257 -> 1
    r = harness.call(app, "f(uint256)", 257)
    assert r.abi_return == 1
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert r.abi_return == 1
    # g(uint256): 2 -> 2
    r = harness.call(app, "g(uint256)", 2)
    assert r.abi_return == 2
    # g(uint256): 255 -> -1
    r = harness.call(app, "g(uint256)", 255)
    assert r.abi_return == -1
    # g(uint256): 257 -> 1
    r = harness.call(app, "g(uint256)", 257)
    assert r.abi_return == 1
    # h(uint8): 1 -> 1
    r = harness.call(app, "h(uint8)", 1)
    assert r.abi_return == 1
    # h(uint8): 2 -> 2
    r = harness.call(app, "h(uint8)", 2)
    assert r.abi_return == 2
    # h(uint8): 255 -> -1
    r = harness.call(app, "h(uint8)", 255)
    assert r.abi_return == -1
    # h(uint8): 257 -> FAILURE
    r = harness.call(app, "h(uint8)", 257, expect_revert=True)
    assert r.reverted
    # i(uint8): 250 -> 250
    r = harness.call(app, "i(uint8)", 250)
    assert r.abi_return == 250
    # j(uint8): 1 -> 1
    r = harness.call(app, "j(uint8)", 1)
    assert r.abi_return == 1
    # j(uint8): 2 -> 2
    r = harness.call(app, "j(uint8)", 2)
    assert r.abi_return == 2
    # j(uint8): 255 -> 0xff
    r = harness.call(app, "j(uint8)", 255)
    assert r.abi_return == 255
    # j(uint8): 257 -> FAILURE
    r = harness.call(app, "j(uint8)", 257, expect_revert=True)
    assert r.reverted
    # k(uint8): 1 -> 1
    r = harness.call(app, "k(uint8)", 1)
    assert r.abi_return == 1
    # k(uint8): 2 -> 2
    r = harness.call(app, "k(uint8)", 2)
    assert r.abi_return == 2
    # k(uint8): 255 -> 0xff
    r = harness.call(app, "k(uint8)", 255)
    assert r.abi_return == 255
    # k(uint8): 257 -> FAILURE
    r = harness.call(app, "k(uint8)", 257, expect_revert=True)
    assert r.reverted
    # m(uint16): 1 -> 1
    r = harness.call(app, "m(uint16)", 1)
    assert r.abi_return == 1
    # m(uint16): 2 -> 2
    r = harness.call(app, "m(uint16)", 2)
    assert r.abi_return == 2
    # m(uint16): 255 -> 0xff
    r = harness.call(app, "m(uint16)", 255)
    assert r.abi_return == 255
    # m(uint16): 257 -> 1
    r = harness.call(app, "m(uint16)", 257)
    assert r.abi_return == 1

def test_conversion_abicoderv1(harness):
    """userDefinedValueType/contracts/conversion_abicoderv1.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/conversion_abicoderv1.sol")
    # f(uint256): 1 -> 1
    r = harness.call(app, "f(uint256)", 1)
    assert r.abi_return == 1
    # f(uint256): 2 -> 2
    r = harness.call(app, "f(uint256)", 2)
    assert r.abi_return == 2
    # f(uint256): 257 -> 1
    r = harness.call(app, "f(uint256)", 257)
    assert r.abi_return == 1
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert r.abi_return == 1
    # g(uint256): 2 -> 2
    r = harness.call(app, "g(uint256)", 2)
    assert r.abi_return == 2
    # g(uint256): 255 -> -1
    r = harness.call(app, "g(uint256)", 255)
    assert r.abi_return == -1
    # g(uint256): 257 -> 1
    r = harness.call(app, "g(uint256)", 257)
    assert r.abi_return == 1
    # h(uint8): 1 -> 1
    r = harness.call(app, "h(uint8)", 1)
    assert r.abi_return == 1
    # h(uint8): 2 -> 2
    r = harness.call(app, "h(uint8)", 2)
    assert r.abi_return == 2
    # h(uint8): 255 -> -1
    r = harness.call(app, "h(uint8)", 255)
    assert r.abi_return == -1
    # h(uint8): 257 -> 1
    r = harness.call(app, "h(uint8)", 257)
    assert r.abi_return == 1
    # i(uint8): 250 -> 250
    r = harness.call(app, "i(uint8)", 250)
    assert r.abi_return == 250
    # j(uint8): 1 -> 1
    r = harness.call(app, "j(uint8)", 1)
    assert r.abi_return == 1
    # j(uint8): 2 -> 2
    r = harness.call(app, "j(uint8)", 2)
    assert r.abi_return == 2
    # j(uint8): 255 -> 0xff
    r = harness.call(app, "j(uint8)", 255)
    assert r.abi_return == 255
    # j(uint8): 257 -> 1
    r = harness.call(app, "j(uint8)", 257)
    assert r.abi_return == 1
    # k(uint8): 1 -> 1
    r = harness.call(app, "k(uint8)", 1)
    assert r.abi_return == 1
    # k(uint8): 2 -> 2
    r = harness.call(app, "k(uint8)", 2)
    assert r.abi_return == 2
    # k(uint8): 255 -> 0xff
    r = harness.call(app, "k(uint8)", 255)
    assert r.abi_return == 255
    # k(uint8): 257 -> 1
    r = harness.call(app, "k(uint8)", 257)
    assert r.abi_return == 1
    # m(uint16): 1 -> 1
    r = harness.call(app, "m(uint16)", 1)
    assert r.abi_return == 1
    # m(uint16): 2 -> 2
    r = harness.call(app, "m(uint16)", 2)
    assert r.abi_return == 2
    # m(uint16): 255 -> 0xff
    r = harness.call(app, "m(uint16)", 255)
    assert r.abi_return == 255
    # m(uint16): 257 -> 1
    r = harness.call(app, "m(uint16)", 257)
    assert r.abi_return == 1

def test_dirty_slot(harness):
    """userDefinedValueType/contracts/dirty_slot.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/dirty_slot.sol")
    # a() -> 13
    r = harness.call(app, "a()")
    assert r.abi_return == 13
    # b() -> 0x0401000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "b()")
    assert r.abi_return == 1811018241397843937822879938261491478723170994297509432074646356324935270400
    # get_b(uint256): 0 -> 0x0400000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "get_b(uint256)", 0)
    assert r.abi_return == 1809251394333065553493296640760748560207343510400633813116524750123642650624
    # get_b(uint256): 1 -> 0x0100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "get_b(uint256)", 1)
    assert r.abi_return == 452312848583266388373324160190187140051835877600158453279131187530910662656
    # get_b(uint256): 2 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "get_b(uint256)", 2, expect_revert=True)
    assert r.reverted
    # write_a() ->
    r = harness.call(app, "write_a()")
    # (void return — call succeeding is the assertion)
    # a() -> 0x2001
    r = harness.call(app, "a()")
    assert r.abi_return == 8193
    # write_b() ->
    r = harness.call(app, "write_b()")
    # (void return — call succeeding is the assertion)
    # b() -> 0x5403000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "b()")
    assert r.abi_return == 37999579822188711776347979348477948519901696170103936932321384571200373522432
    # get_b(uint256): 0 -> 0x5400000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "get_b(uint256)", 0)
    assert r.abi_return == 37994279280994376623359229455975719764354213718413310075447019752596495663104
    # get_b(uint256): 1 -> 0x0300000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "get_b(uint256)", 1)
    assert r.abi_return == 1356938545749799165119972480570561420155507632800475359837393562592731987968
    # get_b(uint256): 2 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "get_b(uint256)", 2, expect_revert=True)
    assert r.reverted

def test_dirty_uint8_read(harness):
    """userDefinedValueType/contracts/dirty_uint8_read.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/dirty_uint8_read.sol")
    # x() -> -5
    r = harness.call(app, "x()")
    assert r.abi_return == -5
    # create_dirty_slot() ->
    r = harness.call(app, "create_dirty_slot()")
    # (void return — call succeeding is the assertion)
    # read_unclean_value() -> 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb
    r = harness.call(app, "read_unclean_value()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639931

def test_erc20(harness):
    """userDefinedValueType/contracts/erc20.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/erc20.sol")
    # totalSupply() -> 20
    r = harness.call(app, "totalSupply()")
    assert r.abi_return == 20
    # transfer(address,uint256): 2, 5 -> true
    r = harness.call(app, "transfer(address,uint256)", 2, 5)
    assert r.abi_return is True
    # decreaseAllowance(address,uint256): 2, 0 -> true
    r = harness.call(app, "decreaseAllowance(address,uint256)", 2, 0)
    assert r.abi_return is True
    # decreaseAllowance(address,uint256): 2, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "decreaseAllowance(address,uint256)", 2, 1, expect_revert=True)
    assert r.reverted
    # transfer(address,uint256): 2, 14 -> true
    r = harness.call(app, "transfer(address,uint256)", 2, 14)
    assert r.abi_return is True
    # transfer(address,uint256): 2, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "transfer(address,uint256)", 2, 2, expect_revert=True)
    assert r.reverted

def test_fixedpoint(harness):
    """userDefinedValueType/contracts/fixedpoint.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/fixedpoint.sol")
    # add(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "add(uint256,uint256)", 0, 0)
    assert r.abi_return == 0
    # add(uint256,uint256): 25, 45 -> 0x46
    r = harness.call(app, "add(uint256,uint256)", 25, 45)
    assert r.abi_return == 70
    # add(uint256,uint256): 115792089237316195423570985008687907853269984665640564039457584007913129639935, 10 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "add(uint256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 10, expect_revert=True)
    assert r.reverted
    # mul(uint256,uint256): 340282366920938463463374607431768211456, 45671926166590716193865151022383844364247891968 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "mul(uint256,uint256)", 0x100000000000000000000000000000000, 0x800000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # mul(uint256,uint256): 340282366920938463463374607431768211456, 20 -> 6805647338418769269267492148635364229120
    r = harness.call(app, "mul(uint256,uint256)", 0x100000000000000000000000000000000, 20)
    assert r.abi_return == 6805647338418769269267492148635364229120
    # floor(uint256): 11579208923731619542357098500868790785326998665640564039457584007913129639930 -> 11579208923731619542357098500868790785326998665640564039457
    r = harness.call(app, "floor(uint256)", 0x199999999999999999999999999999999999a36a4c7d21800d38571bfffffffa)
    assert r.abi_return == 11579208923731619542357098500868790785326998665640564039457
    # floor(uint256): 115792089237316195423570985008687907853269984665640564039457584007913129639935 -> 115792089237316195423570985008687907853269984665640564039457
    r = harness.call(app, "floor(uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457
    # toUFixed256x18(uint256): 0 -> 0
    r = harness.call(app, "toUFixed256x18(uint256)", 0)
    assert r.abi_return == 0
    # toUFixed256x18(uint256): 5 -> 5000000000000000000
    r = harness.call(app, "toUFixed256x18(uint256)", 5)
    assert r.abi_return == 5000000000000000000
    # toUFixed256x18(uint256): 115792089237316195423570985008687907853269984665640564039457 -> 115792089237316195423570985008687907853269984665640564039457000000000000000000
    r = harness.call(app, "toUFixed256x18(uint256)", 0x12725dd1d243aba0e75fe645cc4873f9e65afe688c928e1f21)
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457000000000000000000
    # toUFixed256x18(uint256): 115792089237316195423570985008687907853269984665640564039458 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "toUFixed256x18(uint256)", 0x12725dd1d243aba0e75fe645cc4873f9e65afe688c928e1f22, expect_revert=True)
    assert r.reverted

def test_immutable_signed(harness):
    """userDefinedValueType/contracts/immutable_signed.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/immutable_signed.sol")
    # direct() -> -2, 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "direct()")
    assert tuple(r.abi_return) == (-2, 44047497324925121336511606693520958599579173549109180625971642598225011015680)
    # viaasm() -> 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "viaasm()")
    assert tuple(r.abi_return) == (115792089237316195423570985008687907853269984665640564039457584007913129639934, 44047497324925121336511606693520958599579173549109180625971642598225011015680)

def test_in_parenthesis(harness):
    """userDefinedValueType/contracts/in_parenthesis.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/in_parenthesis.sol")
    # f() -> 5, 10
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (5, 10)

def test_mapping_key(harness):
    """userDefinedValueType/contracts/mapping_key.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/mapping_key.sol")
    # set(int256,int256): 1, 1 ->
    r = harness.call(app, "set(int256,int256)", 1, 1)
    # (void return — call succeeding is the assertion)
    # m(int256): 1 -> 1
    r = harness.call(app, "m(int256)", 1)
    assert r.abi_return == 1
    # set_unwrapped(int256,int256): 1, 2 ->
    r = harness.call(app, "set_unwrapped(int256,int256)", 1, 2)
    # (void return — call succeeding is the assertion)
    # m(int256): 1 -> 2
    r = harness.call(app, "m(int256)", 1)
    assert r.abi_return == 2
    # m(int256): 2 -> 0
    r = harness.call(app, "m(int256)", 2)
    assert r.abi_return == 0

def test_memory_to_storage(harness):
    """userDefinedValueType/contracts/memory_to_storage.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/memory_to_storage.sol")
    # s() -> 0, 0, 0x00, 0
    r = harness.call(app, "s()")
    assert tuple(r.abi_return) == (0, 0, 0, 0)
    # f((uint8,uint16,bytes2,uint8)): 1, 0xff, "ab", 15 ->
    r = harness.call(app, "f((uint8,uint16,bytes2,uint8))", 1, 255, bytes.fromhex('6162'), 15)
    # (void return — call succeeding is the assertion)
    # s() -> 1, 0xff, 0x6162000000000000000000000000000000000000000000000000000000000000, 15
    r = harness.call(app, "s()")
    assert tuple(r.abi_return) == (1, 255, 44047497324925121336511606693520958599579173549109180625971642598225011015680, 15)
    # g(uint16[]): 0x20, 3, 1, 2, 3 -> 0x20, 3, 1, 2, 3
    r = harness.call(app, "g(uint16[])", 32, 3, 1, 2, 3)
    # TODO: verify structural decoding matches expected: 32, 3, 1, 2, 3
    assert not r.reverted
    # small(uint256): 0 -> 1
    r = harness.call(app, "small(uint256)", 0)
    assert r.abi_return == 1
    # small(uint256): 1 -> 2
    r = harness.call(app, "small(uint256)", 1)
    assert r.abi_return == 2
    # h(bytes2[]): 0x20, 3, "ab", "cd", "ef" -> 0x20, 3, "ab", "cd", "ef"
    r = harness.call(app, "h(bytes2[])", 32, 3, bytes.fromhex('6162'), bytes.fromhex('6364'), bytes.fromhex('6566'))
    # TODO: verify expected: 0x20 | 3 | "ab" | "cd" | "ef"
    assert not r.reverted
    # l(uint256): 0 -> 0x6162000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "l(uint256)", 0)
    assert r.abi_return == 44047497324925121336511606693520958599579173549109180625971642598225011015680
    # l(uint256): 1 -> 0x6364000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "l(uint256)", 1)
    assert r.abi_return == 44955656716221210881917421608902818716714500272103248770446148185689417580544

def test_multisource(harness):
    """userDefinedValueType/contracts/multisource.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/multisource.sol")
    # f(int256): 5 -> 5
    r = harness.call(app, "f(int256)", 5)
    assert r.abi_return == 5
    # f(address): 1 -> 1
    r = harness.call(app, "f(address)", 1)
    assert r.abi_return == 1

def test_multisource_module(harness):
    """userDefinedValueType/contracts/multisource_module.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/multisource_module.sol")
    # f(int256): 5 -> 5
    r = harness.call(app, "f(int256)", 5)
    assert r.abi_return == 5
    # g(int256): 1 -> 1
    r = harness.call(app, "g(int256)", 1)
    assert r.abi_return == 1

def test_ownable(harness):
    """userDefinedValueType/contracts/ownable.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/ownable.sol")
    # renounceOwnership() ->
    r = harness.call(app, "renounceOwnership()")
    # (void return — call succeeding is the assertion)
    # owner() -> 0
    r = harness.call(app, "owner()")
    assert r.abi_return == 0
    # setOwner(address): 0x1212121212121212121212121212120000000012 -> FAILURE, hex"5fc483c5"
    r = harness.call(app, "setOwner(address)", 0x1212121212121212121212121212120000000012, expect_revert=True)
    assert r.reverted

def test_parameter(harness):
    """userDefinedValueType/contracts/parameter.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/parameter.sol")
    # id(address): 5 -> 5
    r = harness.call(app, "id(address)", 5)
    assert r.abi_return == 5
    # id(address): 0xffffffffffffffffffffffffffffffffffffffff -> 0xffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "id(address)", 0xffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 1461501637330902918203684832716283019655932542975
    # id(address): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> FAILURE
    r = harness.call(app, "id(address)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # unwrap(address): 5 -> 5
    r = harness.call(app, "unwrap(address)", 5)
    assert r.abi_return == 5
    # unwrap(address): 0xffffffffffffffffffffffffffffffffffffffff -> 0xffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "unwrap(address)", 0xffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 1461501637330902918203684832716283019655932542975
    # unwrap(address): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> FAILURE
    r = harness.call(app, "unwrap(address)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # wrap(address): 5 -> 5
    r = harness.call(app, "wrap(address)", 5)
    assert r.abi_return == 5
    # wrap(address): 0xffffffffffffffffffffffffffffffffffffffff -> 0xffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "wrap(address)", 0xffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 1461501637330902918203684832716283019655932542975
    # wrap(address): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> FAILURE
    r = harness.call(app, "wrap(address)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # unwrap_assembly(address): 5 -> 5
    r = harness.call(app, "unwrap_assembly(address)", 5)
    assert r.abi_return == 5
    # unwrap_assembly(address): 0xffffffffffffffffffffffffffffffffffffffff -> 0xffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "unwrap_assembly(address)", 0xffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 1461501637330902918203684832716283019655932542975
    # unwrap_assembly(address): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> FAILURE
    r = harness.call(app, "unwrap_assembly(address)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted
    # wrap_assembly(address): 5 -> 5
    r = harness.call(app, "wrap_assembly(address)", 5)
    assert r.abi_return == 5
    # wrap_assembly(address): 0xffffffffffffffffffffffffffffffffffffffff -> 0xffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "wrap_assembly(address)", 0xffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 1461501637330902918203684832716283019655932542975
    # wrap_assembly(address): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> FAILURE
    r = harness.call(app, "wrap_assembly(address)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted

def test_simple(harness):
    """userDefinedValueType/contracts/simple.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/simple.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert r.abi_return == 0
    # g() -> 1, 1
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (1, 1)

def test_storage_layout(harness):
    """userDefinedValueType/contracts/storage_layout.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/storage_layout.sol")
    # storage_a() -> 0, 0
    r = harness.call(app, "storage_a()")
    assert tuple(r.abi_return) == (0, 0)
    # storage_b() -> 0, 1
    r = harness.call(app, "storage_b()")
    assert tuple(r.abi_return) == (0, 1)
    # storage_c() -> 0, 2
    r = harness.call(app, "storage_c()")
    assert tuple(r.abi_return) == (0, 2)
    # storage_d() -> 0, 3
    r = harness.call(app, "storage_d()")
    assert tuple(r.abi_return) == (0, 3)
    # storage_e() -> 1, 0
    r = harness.call(app, "storage_e()")
    assert tuple(r.abi_return) == (1, 0)
    # storage_f() -> 2, 0
    r = harness.call(app, "storage_f()")
    assert tuple(r.abi_return) == (2, 0)
    # storage_g() -> 2, 0x14
    r = harness.call(app, "storage_g()")
    assert tuple(r.abi_return) == (2, 20)

def test_storage_layout_struct(harness):
    """userDefinedValueType/contracts/storage_layout_struct.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/storage_layout_struct.sol")
    # storage_a() -> 0, 0
    r = harness.call(app, "storage_a()")
    assert tuple(r.abi_return) == (0, 0)
    # set_a(int64,int64): 100, 200 ->
    r = harness.call(app, "set_a(int64,int64)", 100, 200)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 0 -> 0xc80000000000000064
    r = harness.call(app, "read_slot(uint256)", 0)
    assert r.abi_return == 3689348814741910323300
    # storage_ra() -> 1, 0
    r = harness.call(app, "storage_ra()")
    assert tuple(r.abi_return) == (1, 0)
    # set_ra(int64,int64): 100, 200 ->
    r = harness.call(app, "set_ra(int64,int64)", 100, 200)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 1 -> 0xc80000000000000064
    r = harness.call(app, "read_slot(uint256)", 1)
    assert r.abi_return == 3689348814741910323300
    # storage_b() -> 2, 0
    r = harness.call(app, "storage_b()")
    assert tuple(r.abi_return) == (2, 0)
    # set_b(int64,int64): 0, 200 ->
    r = harness.call(app, "set_b(int64,int64)", 0, 200)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 2 -> 3689348814741910323200
    r = harness.call(app, "read_slot(uint256)", 2)
    assert r.abi_return == 3689348814741910323200
    # storage_rb() -> 3, 0
    r = harness.call(app, "storage_rb()")
    assert tuple(r.abi_return) == (3, 0)
    # set_rb(int64,int64): 0, 200 ->
    r = harness.call(app, "set_rb(int64,int64)", 0, 200)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 3 -> 3689348814741910323200
    r = harness.call(app, "read_slot(uint256)", 3)
    assert r.abi_return == 3689348814741910323200
    # storage_c() -> 4, 0
    r = harness.call(app, "storage_c()")
    assert tuple(r.abi_return) == (4, 0)
    # set_c(int64,int64): 100, 0 ->
    r = harness.call(app, "set_c(int64,int64)", 100, 0)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 4 -> 0x64
    r = harness.call(app, "read_slot(uint256)", 4)
    assert r.abi_return == 100
    # storage_rc() -> 5, 0
    r = harness.call(app, "storage_rc()")
    assert tuple(r.abi_return) == (5, 0)
    # set_rc(int64,int64): 100, 0 ->
    r = harness.call(app, "set_rc(int64,int64)", 100, 0)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 5 -> 0x64
    r = harness.call(app, "read_slot(uint256)", 5)
    assert r.abi_return == 100
    # storage_d() -> 6, 0
    r = harness.call(app, "storage_d()")
    assert tuple(r.abi_return) == (6, 0)
    # set_d(int96,address): 39614081257132168796771975167, 1461501637330902918203684832716283019655932542975 ->
    r = harness.call(app, "set_d(int96,address)", 0x7fffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffff)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 6 -> -39614081257132168796771975169
    r = harness.call(app, "read_slot(uint256)", 6)
    assert r.abi_return == -39614081257132168796771975169
    # storage_rd() -> 7, 0
    r = harness.call(app, "storage_rd()")
    assert tuple(r.abi_return) == (7, 0)
    # set_rd(int96,address): 39614081257132168796771975167, 1461501637330902918203684832716283019655932542975 ->
    r = harness.call(app, "set_rd(int96,address)", 0x7fffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffff)
    # (void return — call succeeding is the assertion)
    # read_slot(uint256): 7 -> -39614081257132168796771975169
    r = harness.call(app, "read_slot(uint256)", 7)
    assert r.abi_return == -39614081257132168796771975169
    # read_contents_asm() -> 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd, 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd
    r = harness.call(app, "read_contents_asm()")
    assert tuple(r.abi_return) == (115792089237316195423570985008687907853269984665640564039457584007913129639934, 115792089237316195423570985008687907853269984665640564039457584007913129639934, 115792089237316195423570985008687907853269984665640564039457584007913129639933, 115792089237316195423570985008687907853269984665640564039457584007913129639933)

def test_storage_signed(harness):
    """userDefinedValueType/contracts/storage_signed.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/storage_signed.sol")
    # a() -> -2
    r = harness.call(app, "a()")
    assert r.abi_return == -2
    # direct() -> -2
    r = harness.call(app, "direct()")
    assert r.abi_return == -2
    # indirect() -> -2
    r = harness.call(app, "indirect()")
    assert r.abi_return == -2
    # toMemDirect() -> -2
    r = harness.call(app, "toMemDirect()")
    assert r.abi_return == -2
    # toMemIndirect() -> -2
    r = harness.call(app, "toMemIndirect()")
    assert r.abi_return == -2
    # div() -> -1
    r = harness.call(app, "div()")
    assert r.abi_return == -1
    # viaasm() -> 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe
    r = harness.call(app, "viaasm()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639934

def test_wrap_unwrap(harness):
    """userDefinedValueType/contracts/wrap_unwrap.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/wrap_unwrap.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_wrap_unwrap_via_contract_name(harness):
    """userDefinedValueType/contracts/wrap_unwrap_via_contract_name.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/wrap_unwrap_via_contract_name.sol")
    # f(uint256): 0x42 -> 0x42
    r = harness.call(app, "f(uint256)", 66)
    assert r.abi_return == 66
    # g(uint256): 0x42 -> 0x42
    r = harness.call(app, "g(uint256)", 66)
    assert r.abi_return == 66
    # h(uint256): 0x42 -> 0x42
    r = harness.call(app, "h(uint256)", 66)
    assert r.abi_return == 66
    # i(uint256): 0x42 -> 0x42
    r = harness.call(app, "i(uint256)", 66)
    assert r.abi_return == 66

def test_zero_cost_abstraction_comparison_elementary(harness):
    """userDefinedValueType/contracts/zero_cost_abstraction_comparison_elementary.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/zero_cost_abstraction_comparison_elementary.sol")
    # getX() -> 0
    r = harness.call(app, "getX()")
    assert r.abi_return == 0
    # setX(int256): 5 ->
    r = harness.call(app, "setX(int256)", 5)
    # (void return — call succeeding is the assertion)
    # getX() -> 5
    r = harness.call(app, "getX()")
    assert r.abi_return == 5
    # add(int256,int256): 200, 99 -> 299
    r = harness.call(app, "add(int256,int256)", 200, 99)
    assert r.abi_return == 299

def test_zero_cost_abstraction_comparison_userdefined(harness):
    """userDefinedValueType/contracts/zero_cost_abstraction_comparison_userdefined.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/zero_cost_abstraction_comparison_userdefined.sol")
    # getX() -> 0
    r = harness.call(app, "getX()")
    assert r.abi_return == 0
    # setX(int256): 5 ->
    r = harness.call(app, "setX(int256)", 5)
    # (void return — call succeeding is the assertion)
    # getX() -> 5
    r = harness.call(app, "getX()")
    assert r.abi_return == 5
    # add(int256,int256): 200, 99 -> 299
    r = harness.call(app, "add(int256,int256)", 200, 99)
    assert r.abi_return == 299
