"""Official solc execution of raw-length preservation and typed clearing."""
import json
from pathlib import Path
import subprocess
from web3 import Web3, EthereumTesterProvider

root = Path('/home/argi/AlgorandFoundation/SideProjects/puya-sol/puya-sol')
fixtures = root / 'tests/solidity-semantic-tests/tests/puyasolRegression/contracts'
solc = '/home/argi/.solcx/solc-v0.8.34'
sources = {name + '.sol': {'content': (fixtures / (name + '.sol')).read_text()}
           for name in ('raw_array_storage', 'named_scalar_slots')}
checks = [('rootRoundtrip', [], 8), ('memberRoundtrip', [], [9, 7, 8]),
          ('hiddenTail', [], [29, 29]), ('clearTail', [False], 0), ('clearTail', [True], 0),
          ('packedRoundtrip', [], 9), ('nestedRoundtrip', [], 18),
          ('mappingRoundtrip', [], 19), ('literalRoot', [], 8), ('runtimeRoot', [0], 8)]
rows = []
for via_ir in (False, True):
    request = {'language': 'Solidity', 'sources': sources, 'settings': {
        'viaIR': via_ir, 'optimizer': {'enabled': True}, 'evmVersion': 'cancun',
        'outputSelection': {'*': {'*': ['abi', 'evm.bytecode.object']}}}}
    result = json.loads(subprocess.run([solc, '--standard-json'], input=json.dumps(request),
                                      text=True, capture_output=True, check=True).stdout)
    assert not [e for e in result.get('errors', []) if e['severity'] == 'error'], result
    evm = Web3(EthereumTesterProvider())
    sender = evm.eth.accounts[0]
    for source, contract, cases in [('raw_array_storage', 'RawArrayStorage', checks),
                                    ('named_scalar_slots', 'NamedScalarSlots',
                                     [('scalarSlots', [], [7, 9, True, 33]), ('metadata', [], 2)])]:
        data = result['contracts'][source + '.sol'][contract]
        factory = evm.eth.contract(abi=data['abi'], bytecode=data['evm']['bytecode']['object'])
        receipt = evm.eth.wait_for_transaction_receipt(factory.constructor().transact({'from': sender}))
        assert receipt.status == 1
        app = evm.eth.contract(address=receipt.contractAddress, abi=data['abi'])
        for name, args, expected in cases:
            actual = getattr(app.functions, name)(*args).call({'from': sender})
            assert actual == expected, (via_ir, name, args, actual, expected)
            rows.append(dict(via_ir=via_ir, contract=contract, method=name, args=args, result=actual))
print(json.dumps(dict(solc=subprocess.check_output([solc, '--version'], text=True).strip(),
                      checks=len(rows), rows=rows), indent=2))
