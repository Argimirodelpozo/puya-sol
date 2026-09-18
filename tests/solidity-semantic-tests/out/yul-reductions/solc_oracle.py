"""Official solc/EVM oracle for the new Yul word-math regression matrix."""
import json
from pathlib import Path
import subprocess
from web3 import Web3, EthereumTesterProvider

root = Path('/home/argi/AlgorandFoundation/SideProjects/puya-sol/puya-sol')
source = root / 'tests/solidity-semantic-tests/tests/puyasolRegression/contracts/yul_word_reductions.sol'
solc = '/home/argi/.solcx/solc-v0.8.34'
mask = 2**256 - 1
rows = []

def expected_words(x, n):
    shift = min(n, 256)
    signed = x - 2**256 if x >= 2**255 else x
    low_bits = min(256, 8 * (n + 1))
    low = x & (2**low_bits - 1)
    extended = (low - 2**low_bits if low >= 2**(low_bits - 1) else low) & mask
    return [(x << shift) & mask, x >> shift, (signed >> shift) & mask, extended]

for via_ir in (False, True):
    request = {'language': 'Solidity', 'sources': {source.name: {'content': source.read_text()}},
               'settings': {'viaIR': via_ir, 'optimizer': {'enabled': True}, 'evmVersion': 'cancun',
                            'outputSelection': {'*': {'*': ['abi', 'evm.bytecode.object']}}}}
    result = json.loads(subprocess.run([solc, '--standard-json'], input=json.dumps(request),
                                      text=True, capture_output=True, check=True).stdout)
    assert not [e for e in result.get('errors', []) if e['severity'] == 'error']
    data = result['contracts'][source.name]['YulWordReductions']
    evm = Web3(EthereumTesterProvider())
    sender = evm.eth.accounts[0]
    factory = evm.eth.contract(abi=data['abi'], bytecode=data['evm']['bytecode']['object'])
    receipt = evm.eth.wait_for_transaction_receipt(factory.constructor().transact({'from': sender}))
    assert receipt.status == 1
    app = evm.eth.contract(address=receipt.contractAddress, abi=data['abi'])

    def check(name, args, expected):
        actual = getattr(app.functions, name)(*args).call({'from': sender})
        assert actual == expected, (via_ir, name, args, actual, expected)
        rows.append({'via_ir': via_ir, 'method': name, 'args': args, 'result': actual})

    for x in (0, 17, 2**255, mask):
        check('largeSignextend', [x], [x, x])
    for x in (0, 0x80, 2**255, mask):
        for n in (0, 7, 31, 255, 256, 2**64, mask):
            expected = expected_words(x, n)
            check('words', [x, n], expected)
            check('typed', [x, n], expected[:3])
    for n in (0, 8, 256, 2**64):
        for op, expected in enumerate(expected_words(mask, n)):
            check('ordered', [mask, n, op], [expected, 21])
    check('literals', [], [v.ljust(32, b'\0') for v in (b'abc', b'\0\x01\xff\0', b'\0"\\\xff', b'')])

print(json.dumps({'solc': subprocess.check_output([solc, '--version'], text=True).strip(),
                  'checks': len(rows), 'rows': rows}, default=lambda value: value.hex(), indent=2))
