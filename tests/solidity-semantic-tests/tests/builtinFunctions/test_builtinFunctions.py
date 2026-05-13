"""Tests for the builtinFunctions category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_assignment_to_const_var_involving_keccak(harness):
    """builtinFunctions/contracts/assignment_to_const_var_involving_keccak.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/assignment_to_const_var_involving_keccak.sol")
    # f() -> 0x4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 35286403120855365962805127237049809881669876751651884979611909062921250761797

def test_blobhash(harness):
    """builtinFunctions/contracts/blobhash.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/blobhash.sol")
    # f() -> 0x0100000000000000000000000000000000000000000000000000000000000001
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 452312848583266388373324160190187140051835877600158453279131187530910662657
    # g() -> 0x0100000000000000000000000000000000000000000000000000000000000002
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 452312848583266388373324160190187140051835877600158453279131187530910662658
    # h() -> 0x00
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 0

def test_blobhash_shadow_resolution(harness):
    """builtinFunctions/contracts/blobhash_shadow_resolution.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/blobhash_shadow_resolution.sol")
    # f() -> 0x03
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3

def test_blockhash(harness):
    """builtinFunctions/contracts/blockhash.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/blockhash.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_blockhash_shadow_resolution(harness):
    """builtinFunctions/contracts/blockhash_shadow_resolution.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/blockhash_shadow_resolution.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_erc7201_equivalent_solidity_spec(harness):
    """builtinFunctions/contracts/erc7201_equivalent_solidity_spec.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_equivalent_solidity_spec.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True
    # builtinOutput() -> 0x183a6125c38840424c4a85fa12bab2ab606c4b6d0e7cc73c0c06ba5300eab500
    r = harness.call(app, "builtinOutput()")
    assert as_int(r.abi_return) == 10958655983261152271848436692291137275443024275653522991983264966744321209600

def test_erc7201_equivalent_solidity_spec_comptime(harness):
    """builtinFunctions/contracts/erc7201_equivalent_solidity_spec_comptime.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_equivalent_solidity_spec_comptime.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_layout_specifier_slot_match_comptime(harness):
    """builtinFunctions/contracts/erc7201_layout_specifier_slot_match_comptime.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_layout_specifier_slot_match_comptime.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_overflow_expression(harness):
    """builtinFunctions/contracts/erc7201_overflow_expression.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_overflow_expression.sol")
    # f() -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_erc7201_param_abi_encode(harness):
    """builtinFunctions/contracts/erc7201_param_abi_encode.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_abi_encode.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True
    # builtinOutput() -> -14651554186193368082021334953908208762193027200365752719897746810709432803072
    r = harness.call(app, "builtinOutput()")
    assert as_int(r.abi_return) in (-14651554186193368082021334953908208762193027200365752719897746810709432803072, 101140535051122827341549650054779699091076957465274811319559837197203696836864)

def test_erc7201_param_array_string_literal(harness):
    """builtinFunctions/contracts/erc7201_param_array_string_literal.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_array_string_literal.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_locations(harness):
    """builtinFunctions/contracts/erc7201_param_locations.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_locations.sol")
    # storageVar() -> true
    r = harness.call(app, "storageVar()")
    assert bool(as_int(r.abi_return)) is True
    # constStorageVar() -> true
    r = harness.call(app, "constStorageVar()")
    assert bool(as_int(r.abi_return)) is True
    # memoryVar() -> true
    r = harness.call(app, "memoryVar()")
    assert bool(as_int(r.abi_return)) is True
    # calldataParam(string): 0x20, 12, "example.main" -> true
    r = harness.call(app, "calldataParam(string)", 'example.main')
    assert bool(as_int(r.abi_return)) is True
    # calldataSlice(bytes): 0x20, 12, "example.main" -> true
    r = harness.call(app, "calldataSlice(bytes)", 'example.main')
    assert bool(as_int(r.abi_return)) is True
    # literalParam() -> true
    r = harness.call(app, "literalParam()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_pure_function(harness):
    """builtinFunctions/contracts/erc7201_param_pure_function.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_pure_function.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_string_concat(harness):
    """builtinFunctions/contracts/erc7201_param_string_concat.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_string_concat.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_string_literal_with_escaped_chars(harness):
    """builtinFunctions/contracts/erc7201_param_string_literal_with_escaped_chars.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_string_literal_with_escaped_chars.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_ternary_operator(harness):
    """builtinFunctions/contracts/erc7201_param_ternary_operator.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_ternary_operator.sol")
    # simple() -> true
    r = harness.call(app, "simple()")
    assert bool(as_int(r.abi_return)) is True
    # compounded(bool,bool): false, true -> true
    r = harness.call(app, "compounded(bool,bool)", False, True)
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_unicode_string_literal(harness):
    """builtinFunctions/contracts/erc7201_param_unicode_string_literal.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_unicode_string_literal.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_unicode_string_variable(harness):
    """builtinFunctions/contracts/erc7201_param_unicode_string_variable.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_unicode_string_variable.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_with_zero_last_byte_of_inner_hash(harness):
    """builtinFunctions/contracts/erc7201_param_with_zero_last_byte_of_inner_hash.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_with_zero_last_byte_of_inner_hash.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_erc7201_param_with_zero_last_byte_of_inner_hash_comptime(harness):
    """builtinFunctions/contracts/erc7201_param_with_zero_last_byte_of_inner_hash_comptime.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/erc7201_param_with_zero_last_byte_of_inner_hash_comptime.sol")
    # builtinMatchesSolidityImplementation() -> true
    r = harness.call(app, "builtinMatchesSolidityImplementation()")
    assert bool(as_int(r.abi_return)) is True

def test_function_types_sig(harness):
    """builtinFunctions/contracts/function_types_sig.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/function_types_sig.sol")
    # `.selector` returns a bytes4 (4 raw bytes); the original EVM-flat
    # fixture left-aligns each into a 32-byte word, but AVM returns the
    # 4-byte selector directly.
    assert bytes(harness.call(app, "f()").abi_return) == bytes.fromhex("26121ff0")
    assert bytes(harness.call(app, "g()").abi_return) == bytes.fromhex("4bb8a92a")
    assert bytes(harness.call(app, "h()").abi_return) == bytes.fromhex("4bb8a92a")
    assert bytes(harness.call(app, "i()").abi_return) == bytes.fromhex("0c55699c")

def test_iterated_keccak256_with_bytes(harness):
    """builtinFunctions/contracts/iterated_keccak256_with_bytes.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/iterated_keccak256_with_bytes.sol")
    # foo() -> 0xb338eefce206f9f57b83aa738deecd5326dc4b72dd81ee6a7c621a6facb7acdc
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 81064592765372817159845741028275376000365320033790514016917613221788490640604

def test_keccak256(harness):
    """builtinFunctions/contracts/keccak256.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256.sol")
    # f(int256): 4 -> 0x8a35acfbc15ff81a39ae7d344fd709f28e8600b4aa8c65c6b64bfe7fe36bd19b
    r = harness.call(app, "f(int256)", 4)
    assert as_int(r.abi_return) == 62514009886607029107290561805838585334079798074568712924583230797734656856475
    # f(int256): 5 -> 0x036b6384b5eca791c62761152d0c79bb0604c104a5fb6f4eb0703f3154bb3db0
    r = harness.call(app, "f(int256)", 5)
    assert as_int(r.abi_return) == 1546678032441257452667456735582814959992782782816731922691272282333561699760
    # f(int256): -1 -> 0xa9c584056064687e149968cbab758a3376d22aedc6a55823d1b3ecbee81b8fb9
    r = harness.call(app, "f(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) == 76789851457802156565283866979031212934421734113360677815664780851587518795705

def test_keccak256_empty(harness):
    """builtinFunctions/contracts/keccak256_empty.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256_empty.sol")
    # f() -> 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 89477152217924674838424037953991966239322087453347756267410168184682657981552

def test_keccak256_multiple_arguments(harness):
    """builtinFunctions/contracts/keccak256_multiple_arguments.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256_multiple_arguments.sol")
    # foo(uint256,uint256,uint256): 0xa, 0xc, 0xd -> 0xbc740a98aae5923e8f04c9aa798c9ee82f69e319997699f2782c40828db9fd81
    r = harness.call(app, "foo(uint256,uint256,uint256)", 10, 12, 13)
    assert as_int(r.abi_return) == 85239842926541264634154666327463972906709059378906552613586341771123147537793

def test_keccak256_multiple_arguments_with_numeric_literals(harness):
    """builtinFunctions/contracts/keccak256_multiple_arguments_with_numeric_literals.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256_multiple_arguments_with_numeric_literals.sol")
    # foo(uint256,uint16): 0xa, 0xc -> 0x88acd45f75907e7c560318bc1a5249850a0999c4896717b1167d05d116e6dbad
    r = harness.call(app, "foo(uint256,uint16)", 10, 12)
    assert as_int(r.abi_return) == 61819910846267543446242086161023601988610643930522389071654317431727835241389

def test_keccak256_multiple_arguments_with_string_literals(harness):
    """builtinFunctions/contracts/keccak256_multiple_arguments_with_string_literals.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256_multiple_arguments_with_string_literals.sol")
    # foo() -> 0x41b1a0649752af1b28b3dc29a1556eee781e4a4c3a1f7f53f90fa834de098c4d
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 29714174079724412745887019504253973571029824035614949642323418802670541573197
    # bar(uint256,uint16): 0xa, 0xc -> 0x6990f36476dc412b1c4baa48e2d9f4aa4bb313f61fda367c8fdbbb2232dc6146
    r = harness.call(app, "bar(uint256,uint16)", 10, 12)
    assert as_int(r.abi_return) == 47748954911445452833847828877350899715607415561591670347703179290839474790726

def test_keccak256_packed(harness):
    """builtinFunctions/contracts/keccak256_packed.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256_packed.sol")
    # f(int256): 4 -> 0xd270285b9966fefc715561efcd09d5b6a8deb15596f7c53cb4a1bb73aa55ac3a
    r = harness.call(app, "f(int256)", 4)
    assert as_int(r.abi_return) == 95183863613105289674943871047709809424547296864956777631899359142068901751866
    # f(int256): 5 -> 0xf2f92566c5653600c1e527a7073e5d881576d12bb51887c0b8f3e1f81865b03d
    r = harness.call(app, "f(int256)", 5)
    assert as_int(r.abi_return) == 109899912411597832212822893111608843524663982160387972078328824211481649590333
    # f(int256): -1 -> 0xbc78b45e0db67af5af72e4ab62757c67aefa7388cdf0c4e74f8b5fe9dd5d9d13
    r = harness.call(app, "f(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) == 85248082031449023985059491939699956408088110354102027088226743004047620283667

def test_keccak256_packed_complex_types(harness):
    """builtinFunctions/contracts/keccak256_packed_complex_types.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256_packed_complex_types.sol")
    # f() -> 0xba4f20407251e4607cd66b90bfea19ec6971699c03e4a4f3ea737d5818ac27ae, 0xba4f20407251e4607cd66b90bfea19ec6971699c03e4a4f3ea737d5818ac27ae, 0xe7490fade3a8e31113ecb6c0d2635e28a6f5ca8359a57afe914827f41ddf0848
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (84269993347964014300195658947572255396004753318263724057427059822633029478318, 84269993347964014300195658947572255396004753318263724057427059822633029478318, 104613356072704699328120257376527735614470975369668734659467872912728506959944)

def test_keccak256_with_bytes(harness):
    """builtinFunctions/contracts/keccak256_with_bytes.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/keccak256_with_bytes.sol")
    # foo() -> true
    r = harness.call(app, "foo()")
    assert bool(as_int(r.abi_return)) is True

def test_msg_sig(harness):
    """builtinFunctions/contracts/msg_sig.sol

    `msg.sig` returns the 4-byte ABI method selector. Puya uses ARC4
    selectors (which include the return type in the hash), so the value
    differs from the EVM-keccak256("foo(uint256)") form — assert that
    the returned selector is consistent across two invocations rather
    than against a hard-coded EVM value.
    """
    app = harness.compile_and_deploy("builtinFunctions/contracts/msg_sig.sol")
    sel = bytes(harness.call(app, "foo(uint256)", 0).abi_return)
    assert len(sel) == 4
    # Second call gets the same selector.
    assert bytes(harness.call(app, "foo(uint256)", 1).abi_return) == sel


def test_msg_sig_after_internal_call_is_same(harness):
    """builtinFunctions/contracts/msg_sig_after_internal_call_is_same.sol

    `msg.sig` after an internal call still returns the OUTER selector;
    we assert internal consistency rather than the EVM-specific value.
    """
    app = harness.compile_and_deploy("builtinFunctions/contracts/msg_sig_after_internal_call_is_same.sol")
    sel = bytes(harness.call(app, "foo(uint256)", 0).abi_return)
    assert len(sel) == 4
    assert bytes(harness.call(app, "foo(uint256)", 1).abi_return) == sel

def test_ripemd160(harness):
    """builtinFunctions/contracts/ripemd160.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/ripemd160.sol")
    # f(int256): 4 -> 0x1b0f3c404d12075c68c938f9f60ebea4f74941a0000000000000000000000000
    r = harness.call(app, "f(int256)", 4)
    assert as_int(r.abi_return) == 12239365456053725440107558875761931117347152855322617053615694768895724355584
    # f(int256): 5 -> 0xee54aa84fc32d8fed5a5fe160442ae84626829d9000000000000000000000000
    r = harness.call(app, "f(int256)", 5)
    assert as_int(r.abi_return) == 107800049998410314181947434501784187786654500865778811479236083403822943174656
    # f(int256): -1 -> 0x1cf4e77f5966e13e109703cd8a0df7ceda7f3dc3000000000000000000000000
    r = harness.call(app, "f(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) == 13097468180871836274597881871755309280971053328054043821123942875702197485568

def test_ripemd160_empty(harness):
    """builtinFunctions/contracts/ripemd160_empty.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/ripemd160_empty.sol")
    # f() -> 0x9c1185a5c5e9fc54612808977ee8f548b2258d31000000000000000000000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 70591763180588889921896472592087647508930935365384853188857905717740272877568

def test_ripemd160_packed(harness):
    """builtinFunctions/contracts/ripemd160_packed.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/ripemd160_packed.sol")
    # f(int256): 4 -> 0xf93175303eba2a7b372174fc9330237f5ad202fc000000000000000000000000
    r = harness.call(app, "f(int256)", 4)
    assert as_int(r.abi_return) == 112713283608413432366500292079636390015042877224965778699306835103129784025088
    # f(int256): 5 -> 0x04f4fc112e2bfbe0d38f896a46629e08e2fcfad5000000000000000000000000
    r = harness.call(app, "f(int256)", 5)
    assert as_int(r.abi_return) == 2242101781399935236999115622957774579424822401483030762403486566236383346688
    # f(int256): -1 -> 0xc0a2e4b1f3ff766a9a0089e7a410391730872495000000000000000000000000
    r = harness.call(app, "f(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) == 87131874548254851242104262105679177295925122029417861264957203483662101774336

def test_sha256(harness):
    """builtinFunctions/contracts/sha256.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/sha256.sol")
    # f(int256): 4 -> 0xe38990d0c7fc009880a9c07c23842e886c6bbdc964ce6bdd5817ad357335ee6f
    r = harness.call(app, "f(int256)", 4)
    assert as_int(r.abi_return) == 102918074156479767208844353797675673170264177419479145455589118040061966151279
    # f(int256): 5 -> 0x96de8fc8c256fa1e1556d41af431cace7dca68707c78dd88c3acab8b17164c47
    r = harness.call(app, "f(int256)", 5)
    assert as_int(r.abi_return) == 68240159698054048912945530158929160304440341641269126469902820990734598294599
    # f(int256): -1 -> 0xaf9613760f72635fbdb44a5a0a63c39f12af30f950a6ee5c971be188e89c4051
    r = harness.call(app, "f(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) == 79419909877869412302011273272600157910097194791702522154213193972579280109649

def test_sha256_empty(harness):
    """builtinFunctions/contracts/sha256_empty.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/sha256_empty.sol")
    # f() -> 0xe3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 102987336249554097029535212322581322789799900648198034993379397001115665086549

def test_sha256_packed(harness):
    """builtinFunctions/contracts/sha256_packed.sol"""
    app = harness.compile_and_deploy("builtinFunctions/contracts/sha256_packed.sol")
    # f(int256): 4 -> 0x804e0d7003cfd70fc925dc103174d9f898ebb142ecc2a286da1abd22ac2ce3ac
    r = harness.call(app, "f(int256)", 4)
    assert as_int(r.abi_return) == 58033951432328784014309100941220065668419038229569888230017781326780325225388
    # f(int256): 5 -> 0xe94921945f9068726c529a290a954f412bcac53184bb41224208a31edbf63cf0
    r = harness.call(app, "f(int256)", 5)
    assert as_int(r.abi_return) == 105518105313395515086322545934905310180852738469466176175217259920264724364528
    # f(int256): -1 -> 0xf14def4d07cd185ddd8b10a81b2238326196a38867e6e6adbcc956dc913488c7
    r = harness.call(app, "f(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) == 109145095326669468812651857352188286659179238190865846698410948873810387175623
