"""Check the exact reference-return fixture on solc's EVM backends."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from web3 import Web3, EthereumTesterProvider

ROOT = Path('/home/argi/AlgorandFoundation/SideProjects/puya-sol/puya-sol')
SOURCE = ROOT / 'tests/solidity-semantic-tests/tests/puyasolRegression/contracts/returned_memory_identity.sol'
checks = {
    'ReturnedMemoryIdentity': [
        ('direct', (), 9), ('throughReturn', (), 9),
        ('retainedAlias', (), [7, 9]), ('localReturnedAlias', (), [9, 9]),
        ('duplicateReturn', (), [9, 9, 9]), ('defaults', (), [9, 0, 7, 0]),
        ('libraryAlias', (), 9), ('libraryCopy', (), [5, 7, 9]),
        ('libraryPairCopy', (), [5, 9, 5]), ('publicAlias', (), 9),
        ('publicPointer', (), [9, 9, 5]), ('directDestination', (), [9, 1]),
        ('argumentOnce', (), [9, 1]), ('modifierAlias', (), [9, 9]),
        ('conditionalFreshTuple', (False,), [5, 8]), ('conditionalFreshTuple', (True,), [9, 9]),
        ('conditionalTuple', (False,), [5, 9, 9]), ('conditionalTuple', (True,), [9, 7, 9]),
        ('conditional', (False,), [5, 9]), ('conditional', (True,), [9, 9]),
        ('dispatch', (False,), [5, 9]), ('dispatch', (True,), [9, 7]),
        ('publicIdentity', ((17,),), (17,)),
    ],
    'ReturnedMemoryArrays': [
        ('arrays', (), [9, 9, 7]), ('nested', (), 9), ('bytesAlias', (), bytes.fromhex('01ff03')),
    ],
}
count = 0
records = []
for via_ir in (False, True):
    request = {'language': 'Solidity', 'sources': {'fixture.sol': {'content': SOURCE.read_text()}},
               'settings': {'viaIR': via_ir, 'optimizer': {'enabled': True, 'runs': 200},
                            'evmVersion': 'cancun', 'outputSelection': {'*': {'*': [
                                'abi', 'evm.bytecode.object', 'evm.bytecode.linkReferences']}}}}
    compiled = subprocess.run(['/home/argi/.solcx/solc-v0.8.34', '--standard-json'],
                              input=json.dumps(request), text=True, capture_output=True, check=True)
    result = json.loads(compiled.stdout)
    errors = [error for error in result.get('errors', []) if error['severity'] == 'error']
    assert not errors, errors
    contracts = result['contracts']['fixture.sol']
    evm = Web3(EthereumTesterProvider())
    sender = evm.eth.accounts[0]
    libraries = {}

    def deploy(name):
        artifact = contracts[name]
        bytecode = artifact['evm']['bytecode']['object']
        for source, references in artifact['evm']['bytecode']['linkReferences'].items():
            for library, positions in references.items():
                address = libraries[library][2:]
                for position in positions:
                    assert position['length'] == 20
                    offset = position['start'] * 2
                    bytecode = bytecode[:offset] + address + bytecode[offset + 40:]
        factory = evm.eth.contract(abi=artifact['abi'], bytecode=bytecode)
        receipt = evm.eth.wait_for_transaction_receipt(factory.constructor().transact({'from': sender}))
        assert receipt.status == 1
        return evm.eth.contract(address=receipt.contractAddress, abi=artifact['abi'])

    libraries['MemoryIdentityLibrary'] = deploy('MemoryIdentityLibrary').address
    for name, cases in checks.items():
        app = deploy(name)
        for method, args, expected in cases:
            actual = getattr(app.functions, method)(*args).call({'from': sender})
            assert actual == expected, (via_ir, name, method, args, actual, expected)
            records.append({'via_ir': via_ir, 'contract': name, 'method': method, 'args': args,
                            'expected': expected, 'actual': actual})
            count += 1
    print(json.dumps({'engine': 'solc-evm', 'via_ir': via_ir, 'checks': sum(map(len, checks.values())),
                      'status': 'passed'}), flush=True)
print(f'{count} EVM oracle checks passed', flush=True)
if len(sys.argv) > 1:
    report = {'solc': subprocess.check_output(['/home/argi/.solcx/solc-v0.8.34', '--version'], text=True).strip(),
              'source': str(SOURCE.relative_to(ROOT)), 'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              'passed': count, 'checks': records}
    Path(sys.argv[1]).write_text(json.dumps(report, indent=2, default=lambda value: '0x' + value.hex()) + '\n')
