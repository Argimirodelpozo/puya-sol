"""Auto-generated tests for the externalContracts category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_FixedFeeRegistrar(harness):
    """externalContracts/FixedFeeRegistrar.sol"""
    app = harness.compile_and_deploy("externalContracts/FixedFeeRegistrar.sol")
    # reserve(string), 69 ether: 0x20, 3, "abc" ->
    r = harness.call(app, "reserve(string)", 32, 3, bytes.fromhex('616263'), payment_wei=69000000000000000000)
    # (void return — call succeeding is the assertion)
    # owner(string): 0x20, 3, "abc" -> 0x1212121212121212121212121212120000000012
    r = harness.call(app, "owner(string)", 32, 3, bytes.fromhex('616263'))
    assert r.abi_return == 103164821458651970696730694074090566015747358738
    # reserve(string), 70 ether: 0x20, 3, "def" ->
    r = harness.call(app, "reserve(string)", 32, 3, bytes.fromhex('646566'), payment_wei=70000000000000000000)
    # (void return — call succeeding is the assertion)
    # owner(string): 0x20, 3, "def" -> 0x1212121212121212121212121212120000000012
    r = harness.call(app, "owner(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 103164821458651970696730694074090566015747358738
    # reserve(string), 68 ether: 0x20, 3, "ghi" ->
    r = harness.call(app, "reserve(string)", 32, 3, bytes.fromhex('676869'), payment_wei=68000000000000000000)
    # (void return — call succeeding is the assertion)
    # owner(string): 0x20, 3, "ghi" -> 0
    r = harness.call(app, "owner(string)", 32, 3, bytes.fromhex('676869'))
    assert r.abi_return == 0
    # reserve(string), 69 ether: 0x20, 3, "abc" ->
    r = harness.call(app, "reserve(string)", 32, 3, bytes.fromhex('616263'), payment_wei=69000000000000000000)
    # (void return — call succeeding is the assertion)
    # owner(string): 0x20, 3, "abc" -> 0x1212121212121212121212121212120000000012
    r = harness.call(app, "owner(string)", 32, 3, bytes.fromhex('616263'))
    assert r.abi_return == 103164821458651970696730694074090566015747358738
    # setContent(string,bytes32): 0x40, 0, 3, "abc" ->
    r = harness.call(app, "setContent(string,bytes32)", 64, 0, 3, bytes.fromhex('616263'))
    # (void return — call succeeding is the assertion)
    # transfer(string,address): 0x40, 555, 3, "abc" ->
    r = harness.call(app, "transfer(string,address)", 64, 555, 3, bytes.fromhex('616263'))
    # (void return — call succeeding is the assertion)
    # owner(string): 0x20, 3, "abc" -> 555
    r = harness.call(app, "owner(string)", 32, 3, bytes.fromhex('616263'))
    assert r.abi_return == 555
    # content(string): 0x20, 3, "abc" -> 0x00
    r = harness.call(app, "content(string)", 32, 3, bytes.fromhex('616263'))
    assert r.abi_return == 0
    # setContent(string,bytes32): 0x40, 333, 3, "def" ->
    r = harness.call(app, "setContent(string,bytes32)", 64, 333, 3, bytes.fromhex('646566'))
    # (void return — call succeeding is the assertion)
    # setAddr(string,address): 0x40, 124, 3, "def" ->
    r = harness.call(app, "setAddr(string,address)", 64, 124, 3, bytes.fromhex('646566'))
    # (void return — call succeeding is the assertion)
    # setSubRegistrar(string,address): 0x40, 125, 3, "def" ->
    r = harness.call(app, "setSubRegistrar(string,address)", 64, 125, 3, bytes.fromhex('646566'))
    # (void return — call succeeding is the assertion)
    # content(string): 0x20, 3, "def" -> 333
    r = harness.call(app, "content(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 333
    # addr(string): 0x20, 3, "def" -> 124
    r = harness.call(app, "addr(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 124
    # subRegistrar(string): 0x20, 3, "def" -> 125
    r = harness.call(app, "subRegistrar(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 125
    # disown(string,address): 0x40, 0x124, 3, "def" ->
    r = harness.call(app, "disown(string,address)", 64, 292, 3, bytes.fromhex('646566'))
    # (void return — call succeeding is the assertion)
    # owner(string): 0x20, 3, "def" -> 0
    r = harness.call(app, "owner(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 0
    # content(string): 0x20, 3, "def" -> 0
    r = harness.call(app, "content(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 0
    # addr(string): 0x20, 3, "def" -> 0
    r = harness.call(app, "addr(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 0
    # subRegistrar(string): 0x20, 3, "def" -> 0
    r = harness.call(app, "subRegistrar(string)", 32, 3, bytes.fromhex('646566'))
    assert r.abi_return == 0

def test_base64(harness):
    """externalContracts/base64.sol"""
    app = harness.compile_and_deploy("externalContracts/base64.sol")
    # encode_inline_asm(bytes): 0x20, 0 -> 0x20, 0
    r = harness.call(app, "encode_inline_asm(bytes)", 32, 0)
    assert tuple(r.abi_return) == (32, 0)
    # encode_inline_asm(bytes): 0x20, 1, "f" -> 0x20, 4, "Zg=="
    r = harness.call(app, "encode_inline_asm(bytes)", 32, 1, bytes.fromhex('66'))
    assert r.abi_return == 'Zg=='
    # encode_inline_asm(bytes): 0x20, 2, "fo" -> 0x20, 4, "Zm8="
    r = harness.call(app, "encode_inline_asm(bytes)", 32, 2, bytes.fromhex('666f'))
    assert r.abi_return == 'Zm8='
    # encode_inline_asm(bytes): 0x20, 3, "foo" -> 0x20, 4, "Zm9v"
    r = harness.call(app, "encode_inline_asm(bytes)", 32, 3, bytes.fromhex('666f6f'))
    assert r.abi_return == 'Zm9v'
    # encode_inline_asm(bytes): 0x20, 4, "foob" -> 0x20, 8, "Zm9vYg=="
    r = harness.call(app, "encode_inline_asm(bytes)", 32, 4, bytes.fromhex('666f6f62'))
    assert r.abi_return == 'Zm9vYg=='
    # encode_inline_asm(bytes): 0x20, 5, "fooba" -> 0x20, 8, "Zm9vYmE="
    r = harness.call(app, "encode_inline_asm(bytes)", 32, 5, bytes.fromhex('666f6f6261'))
    assert r.abi_return == 'Zm9vYmE='
    # encode_inline_asm(bytes): 0x20, 6, "foobar" -> 0x20, 8, "Zm9vYmFy"
    r = harness.call(app, "encode_inline_asm(bytes)", 32, 6, bytes.fromhex('666f6f626172'))
    assert r.abi_return == 'Zm9vYmFy'
    # encode_no_asm(bytes): 0x20, 0 -> 0x20, 0
    r = harness.call(app, "encode_no_asm(bytes)", 32, 0)
    assert tuple(r.abi_return) == (32, 0)
    # encode_no_asm(bytes): 0x20, 1, "f" -> 0x20, 4, "Zg=="
    r = harness.call(app, "encode_no_asm(bytes)", 32, 1, bytes.fromhex('66'))
    assert r.abi_return == 'Zg=='
    # encode_no_asm(bytes): 0x20, 2, "fo" -> 0x20, 4, "Zm8="
    r = harness.call(app, "encode_no_asm(bytes)", 32, 2, bytes.fromhex('666f'))
    assert r.abi_return == 'Zm8='
    # encode_no_asm(bytes): 0x20, 3, "foo" -> 0x20, 4, "Zm9v"
    r = harness.call(app, "encode_no_asm(bytes)", 32, 3, bytes.fromhex('666f6f'))
    assert r.abi_return == 'Zm9v'
    # encode_no_asm(bytes): 0x20, 4, "foob" -> 0x20, 8, "Zm9vYg=="
    r = harness.call(app, "encode_no_asm(bytes)", 32, 4, bytes.fromhex('666f6f62'))
    assert r.abi_return == 'Zm9vYg=='
    # encode_no_asm(bytes): 0x20, 5, "fooba" -> 0x20, 8, "Zm9vYmE="
    r = harness.call(app, "encode_no_asm(bytes)", 32, 5, bytes.fromhex('666f6f6261'))
    assert r.abi_return == 'Zm9vYmE='
    # encode_no_asm(bytes): 0x20, 6, "foobar" -> 0x20, 8, "Zm9vYmFy"
    r = harness.call(app, "encode_no_asm(bytes)", 32, 6, bytes.fromhex('666f6f626172'))
    assert r.abi_return == 'Zm9vYmFy'
    # encode_inline_asm_large()
    r = harness.call(app, "encode_inline_asm_large()")
    # (void return — call succeeding is the assertion)
    # encode_no_asm_large()
    r = harness.call(app, "encode_no_asm_large()")
    # (void return — call succeeding is the assertion)

def test_deposit_contract(harness):
    """externalContracts/deposit_contract.sol"""
    app = harness.compile_and_deploy("externalContracts/deposit_contract.sol")
    # supportsInterface(bytes4): 0x0 -> 0
    r = harness.call(app, "supportsInterface(bytes4)", 0)
    assert r.abi_return == 0
    # supportsInterface(bytes4): 0xffffffff00000000000000000000000000000000000000000000000000000000 -> false # defined to be false by ERC-165 #
    r = harness.call(app, "supportsInterface(bytes4)", 0xffffffff00000000000000000000000000000000000000000000000000000000)
    # TODO: verify expected: false # defined to be false by ERC-165 #
    assert not r.reverted
    # supportsInterface(bytes4): 0x01ffc9a700000000000000000000000000000000000000000000000000000000 -> true # ERC-165 id #
    r = harness.call(app, "supportsInterface(bytes4)", 0x1ffc9a700000000000000000000000000000000000000000000000000000000)
    # TODO: verify expected: true # ERC-165 id #
    assert not r.reverted
    # supportsInterface(bytes4): 0x8564090700000000000000000000000000000000000000000000000000000000 -> true # the deposit interface id #
    r = harness.call(app, "supportsInterface(bytes4)", 0x8564090700000000000000000000000000000000000000000000000000000000)
    # TODO: verify expected: true # the deposit interface id #
    assert not r.reverted
    # get_deposit_root() -> 0xd70a234731285c6804c2a4f56711ddb8c82c99740f207854891028af34e27e5e
    r = harness.call(app, "get_deposit_root()")
    assert r.abi_return == 97265174396505314209556402511040080631145938316814330575766876638812984999518
    # get_deposit_count() -> 0x20, 8, 0 # TODO: check balance and logs after each deposit #
    r = harness.call(app, "get_deposit_count()")
    # TODO: verify expected: 0x20 | 8 | 0 # TODO: check balance and logs after each deposit #
    assert not r.reverted
    # deposit(bytes,bytes,bytes,bytes32), 32 ether: 0 -> FAILURE # Empty input #
    r = harness.call(app, "deposit(bytes,bytes,bytes,bytes32)", 0, payment_wei=32000000000000000000, expect_revert=True)
    assert r.reverted
    # get_deposit_root() -> 0xd70a234731285c6804c2a4f56711ddb8c82c99740f207854891028af34e27e5e
    r = harness.call(app, "get_deposit_root()")
    assert r.abi_return == 97265174396505314209556402511040080631145938316814330575766876638812984999518
    # get_deposit_count() -> 0x20, 8, 0
    r = harness.call(app, "get_deposit_count()")
    assert tuple(r.abi_return) == (32, 8, 0)
    # deposit(bytes,bytes,bytes,bytes32), 1 ether: 0x80, 0xe0, 0x120, 0xaa4a8d0b7d9077248630f1a4701ae9764e42271d7f22b7838778411857fd349e, 0x30, 0x933ad9491b62059dd065b560d256d8957a8c402cc6e8d8ee7290ae11e8f73292, 0x67a8811c397529dac52ae1342ba58c9500000000000000000000000000000000, 0x20, 0x00f50428677c60f997aadeab24aabf7fceaef491c96a52b463ae91f95611cf71, 0x60, 0xa29d01cc8c6296a8150e515b5995390ef841dc18948aa3e79be6d7c1851b4cbb, 0x5d6ff49fa70b9c782399506a22a85193151b9b691245cebafd2063012443c132, 0x4b6c36debaedefb7b2d71b0503ffdc00150aaffd42e63358238ec888901738b8 -> # txhash: 0x7085c586686d666e8bb6e9477a0f0b09565b2060a11f1c4209d3a52295033832 #
    r = harness.call(app, "deposit(bytes,bytes,bytes,bytes32)", 128, 224, 288, 0xaa4a8d0b7d9077248630f1a4701ae9764e42271d7f22b7838778411857fd349e, 48, 0x933ad9491b62059dd065b560d256d8957a8c402cc6e8d8ee7290ae11e8f73292, 0x67a8811c397529dac52ae1342ba58c9500000000000000000000000000000000, 32, 0xf50428677c60f997aadeab24aabf7fceaef491c96a52b463ae91f95611cf71, 96, 0xa29d01cc8c6296a8150e515b5995390ef841dc18948aa3e79be6d7c1851b4cbb, 0x5d6ff49fa70b9c782399506a22a85193151b9b691245cebafd2063012443c132, 0x4b6c36debaedefb7b2d71b0503ffdc00150aaffd42e63358238ec888901738b8, payment_wei=1000000000000000000)
    # TODO: verify expected: # txhash: 0x7085c586686d666e8bb6e9477a0f0b09565b2060a11f1c4209d3a52295033832 #
    assert not r.reverted
    # get_deposit_root() -> 0x2089653123d9c721215120b6db6738ba273bbc5228ac093b1f983badcdc8a438
    r = harness.call(app, "get_deposit_root()")
    assert r.abi_return == 14716767603733094437689435645873038451717577593199503049703782010867344254008
    # get_deposit_count() -> 0x20, 8, 0x0100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "get_deposit_count()")
    assert tuple(r.abi_return) == (32, 8, 452312848583266388373324160190187140051835877600158453279131187530910662656)
    # deposit(bytes,bytes,bytes,bytes32), 32 ether: 0x80, 0xe0, 0x120, 0xdbd986dc85ceb382708cf90a3500f500f0a393c5ece76963ac3ed72eccd2c301, 0x30, 0xb2ce0f79f90e7b3a113ca5783c65756f96c4b4673c2b5c1eb4efc22280259441, 0x06d601211e8866dc5b50dc48a244dd7c00000000000000000000000000000000, 0x20, 0x00344b6c73f71b11c56aba0d01b7d8ad83559f209d0a4101a515f6ad54c89771, 0x60, 0x945caaf82d18e78c033927d51f452ebcd76524497b91d7a11219cb3db6a1d369, 0x7595fc095ce489e46b2ef129591f2f6d079be4faaf345a02c5eb133c072e7c56, 0x0c6c3617eee66b4b878165c502357d49485326bc6b31bc96873f308c8f19c09d -> # txhash: 0x404d8e109822ce448e68f45216c12cb051b784d068fbe98317ab8e50c58304ac #
    r = harness.call(app, "deposit(bytes,bytes,bytes,bytes32)", 128, 224, 288, 0xdbd986dc85ceb382708cf90a3500f500f0a393c5ece76963ac3ed72eccd2c301, 48, 0xb2ce0f79f90e7b3a113ca5783c65756f96c4b4673c2b5c1eb4efc22280259441, 0x6d601211e8866dc5b50dc48a244dd7c00000000000000000000000000000000, 32, 0x344b6c73f71b11c56aba0d01b7d8ad83559f209d0a4101a515f6ad54c89771, 96, 0x945caaf82d18e78c033927d51f452ebcd76524497b91d7a11219cb3db6a1d369, 0x7595fc095ce489e46b2ef129591f2f6d079be4faaf345a02c5eb133c072e7c56, 0xc6c3617eee66b4b878165c502357d49485326bc6b31bc96873f308c8f19c09d, payment_wei=32000000000000000000)
    # TODO: verify expected: # txhash: 0x404d8e109822ce448e68f45216c12cb051b784d068fbe98317ab8e50c58304ac #
    assert not r.reverted
    # get_deposit_root() -> 0x40255975859377d912c53aa853245ebd939bdd2b33a28e084babdcc1ed8238ee
    r = harness.call(app, "get_deposit_root()")
    assert r.abi_return == 29014013074531673165507518994136311552691550960539730258963015614797346650350
    # get_deposit_count() -> 0x20, 8, 0x0200000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "get_deposit_count()")
    assert tuple(r.abi_return) == (32, 8, 904625697166532776746648320380374280103671755200316906558262375061821325312)

def test_prbmath_signed(harness):
    """externalContracts/prbmath_signed.sol"""
    app = harness.compile_and_deploy("externalContracts/prbmath_signed.sol")
    # div(int256,int256): 3141592653589793238, 88714123 -> 35412542528203691288251815328
    r = harness.call(app, "div(int256,int256)", 0x2b992ddfa23249d6, 88714123)
    assert r.abi_return == 35412542528203691288251815328
    # exp(int256): 3141592653589793238 -> 23140692632779268978
    r = harness.call(app, "exp(int256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 23140692632779268978
    # exp2(int256): 3141592653589793238 -> 8824977827076287620
    r = harness.call(app, "exp2(int256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 8824977827076287620
    # gm(int256,int256): 3141592653589793238, 88714123 -> 16694419339601
    r = harness.call(app, "gm(int256,int256)", 0x2b992ddfa23249d6, 88714123)
    assert r.abi_return == 16694419339601
    # log10(int256): 3141592653589793238 -> 4971498726941338506
    r = harness.call(app, "log10(int256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 4971498726941338506
    # log2(int256): 3141592653589793238 -> 1651496129472318782
    r = harness.call(app, "log2(int256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 1651496129472318782
    # mul(int256,int256): 3141592653589793238, 88714123 -> 278703637
    r = harness.call(app, "mul(int256,int256)", 0x2b992ddfa23249d6, 88714123)
    assert r.abi_return == 278703637
    # pow(int256,uint256): 3141592653589793238, 5 -> 306019684785281453040
    r = harness.call(app, "pow(int256,uint256)", 0x2b992ddfa23249d6, 5)
    assert r.abi_return == 306019684785281453040
    # sqrt(int256): 3141592653589793238 -> 1772453850905516027
    r = harness.call(app, "sqrt(int256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 1772453850905516027
    # benchmark(int256): 3141592653589793238 -> 998882724338592125, 1000000000000000000, 1000000000000000000
    r = harness.call(app, "benchmark(int256)", 0x2b992ddfa23249d6)
    assert tuple(r.abi_return) == (998882724338592125, 1000000000000000000, 1000000000000000000)

def test_prbmath_unsigned(harness):
    """externalContracts/prbmath_unsigned.sol"""
    app = harness.compile_and_deploy("externalContracts/prbmath_unsigned.sol")
    # div(uint256,uint256): 3141592653589793238, 88714123 -> 35412542528203691288251815328
    r = harness.call(app, "div(uint256,uint256)", 0x2b992ddfa23249d6, 88714123)
    assert r.abi_return == 35412542528203691288251815328
    # exp(uint256): 3141592653589793238 -> 23140692632779268978
    r = harness.call(app, "exp(uint256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 23140692632779268978
    # exp2(uint256): 3141592653589793238 -> 8824977827076287620
    r = harness.call(app, "exp2(uint256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 8824977827076287620
    # gm(uint256,uint256): 3141592653589793238, 88714123 -> 16694419339601
    r = harness.call(app, "gm(uint256,uint256)", 0x2b992ddfa23249d6, 88714123)
    assert r.abi_return == 16694419339601
    # log10(uint256): 3141592653589793238 -> 0x44fe4fc084a52b8a
    r = harness.call(app, "log10(uint256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 4971498726941338506
    # log2(uint256): 3141592653589793238 -> 1651496129472318782
    r = harness.call(app, "log2(uint256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 1651496129472318782
    # mul(uint256,uint256): 3141592653589793238, 88714123 -> 278703637
    r = harness.call(app, "mul(uint256,uint256)", 0x2b992ddfa23249d6, 88714123)
    assert r.abi_return == 278703637
    # pow(uint256,uint256): 3141592653589793238, 5 -> 306019684785281453040
    r = harness.call(app, "pow(uint256,uint256)", 0x2b992ddfa23249d6, 5)
    assert r.abi_return == 306019684785281453040
    # sqrt(uint256): 3141592653589793238 -> 1772453850905516027
    r = harness.call(app, "sqrt(uint256)", 0x2b992ddfa23249d6)
    assert r.abi_return == 1772453850905516027
    # benchmark(uint256): 3141592653589793238 -> 998882724338592125, 1000000000000000000, 1000000000000000000
    r = harness.call(app, "benchmark(uint256)", 0x2b992ddfa23249d6)
    assert tuple(r.abi_return) == (998882724338592125, 1000000000000000000, 1000000000000000000)

def test_ramanujan_pi(harness):
    """externalContracts/ramanujan_pi.sol"""
    app = harness.compile_and_deploy("externalContracts/ramanujan_pi.sol")
    # prb_pi() -> 3141592656369545286
    r = harness.call(app, "prb_pi()")
    assert r.abi_return == 3141592656369545286

def test_snark(harness):
    """externalContracts/snark.sol"""
    app = harness.compile_and_deploy("externalContracts/snark.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
    # g() -> true
    r = harness.call(app, "g()")
    assert r.abi_return is True
    # pair() -> true
    r = harness.call(app, "pair()")
    assert r.abi_return is True
    # verifyTx() -> true
    r = harness.call(app, "verifyTx()")
    assert r.abi_return is True

def test_strings(harness):
    """externalContracts/strings.sol"""
    app = harness.compile_and_deploy("externalContracts/strings.sol")
    # toSlice(string): 0x20, 11, "hello world" -> 11, 0xa0
    r = harness.call(app, "toSlice(string)", 32, 11, bytes.fromhex('68656c6c6f20776f726c64'))
    assert tuple(r.abi_return) == (11, 160)
    # roundtrip(string): 0x20, 11, "hello world" -> 0x20, 11, "hello world"
    r = harness.call(app, "roundtrip(string)", 32, 11, bytes.fromhex('68656c6c6f20776f726c64'))
    assert r.abi_return == 'hello world'
    # utf8len(string): 0x20, 16, "\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83" -> 4 # Input: "😃😃😃😃" #
    r = harness.call(app, "utf8len(string)", 32, 16, bytes.fromhex('f09f9883f09f9883f09f9883f09f9883'))
    # TODO: verify expected: 4 # Input: "😃😃😃😃" #
    assert not r.reverted
    # multiconcat(string,uint256): 0x40, 3, 11, "hello world" -> 0x20, 0x58, 0x68656c6c6f20776f726c6468656c6c6f20776f726c6468656c6c6f20776f726c, 0x6468656c6c6f20776f726c6468656c6c6f20776f726c6468656c6c6f20776f72, 49027192869463622675296414541903001712009715982962058146354235762728281047040 # concatenating 3 times #
    r = harness.call(app, "multiconcat(string,uint256)", 64, 3, 11, bytes.fromhex('68656c6c6f20776f726c64'))
    # TODO: verify expected: 0x20 | 0x58 | 0x68656c6c6f20776f726c6468656c6c6f20776f726c6468656c6c6f20776f726c | 0x6468656c6c6f20776f726c6468656c6c6f20776f726c6468656c6c6f20776f72 | 49027192869463622675296414541903001712009715982962058146354235762728281047040 # concatenating 3 times #
    assert not r.reverted
    # benchmark(string,bytes32): 0x40, 0x0842021, 8, "solidity" -> 0x2020
    r = harness.call(app, "benchmark(string,bytes32)", 64, 8658977, 8, bytes.fromhex('736f6c6964697479'))
    assert r.abi_return == 8224
