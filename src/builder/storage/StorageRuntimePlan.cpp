#include "builder/storage/StorageRuntimePlan.h"

#include "builder/ProgramAnalysis.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/EvmLayoutMode.h"

#include <libsolidity/ast/Types.h>

#include <functional>

namespace puyasol::builder
{

namespace
{

bool typeUsesHashedSlots(solidity::frontend::Type const* _type)
{
	if (!_type)
		return true;
	if (dynamic_cast<solidity::frontend::MappingType const*>(_type))
		return true;
	if (auto const* array = dynamic_cast<solidity::frontend::ArrayType const*>(_type))
		return array->isDynamicallySized() || typeUsesHashedSlots(array->baseType());
	if (auto const* structure = dynamic_cast<solidity::frontend::StructType const*>(_type))
	{
		for (auto const& member: structure->structDefinition().members())
			if (member && typeUsesHashedSlots(member->type()))
				return true;
	}
	return false;
}

} // namespace

StorageRuntimePlan StorageRuntimePlan::analyze(
	solidity::frontend::ContractDefinition const& _contract,
	TypeMapper& _typeMapper)
{
	StorageRuntimePlan result;
	result.evmLayout = _typeMapper.profile().evmStorageLayout;
	result.dispatchLayout.computeLayout(
		_contract, _typeMapper,
		result.evmLayout
			? StorageLayoutSource::SolidityCanonical
			: StorageLayoutSource::LegacyDispatch);

	auto const& asmCallables = _typeMapper.analysis().callablesWithStorageAssembly;
	forEachDefinedFunction(_contract, [&](auto const* _function) {
		if (_function->isImplemented() && asmCallables.count(_function->id()))
			result.containsInlineAssembly = true;
	});
	// solc's per-contract graph also reaches free/library functions that are
	// emitted as host-bound methods. Their assembly accesses this contract's
	// storage and therefore requires this contract's default-layout dispatcher.
	if (_typeMapper.analysis().hasContractReachability(_contract.id()))
		for (int64_t callableId: asmCallables)
			if (_typeMapper.analysis().isFunctionReachable(
				_contract.id(), callableId))
			{
				result.containsInlineAssembly = true;
				break;
			}
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		if (base)
			for (auto const* modifier: base->functionModifiers())
				if (modifier && modifier->isImplemented()
					&& asmCallables.count(modifier->id()))
					result.containsInlineAssembly = true;
	result.requiresSparseSlots = result.containsInlineAssembly;

	// Packed addresses use a keccak-derived shadow slot for their high bytes.
	for (auto const& variable: result.dispatchLayout.variables())
	{
		if (variable.slot >= solidity::u256(kEvmDenseSlotLimit))
			result.requiresSparseSlots = true;
		if (variable.wtype == awst::WType::accountType() && variable.byteSize == 20)
			if (auto const* slot = result.dispatchLayout.getSlotInfo(variable.slot);
				slot && slot->variableIndices.size() > 1)
				result.requiresSparseSlots = true;
	}

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (!base)
			continue;
		for (auto const* variable: base->stateVariables())
		{
			if (!variable || variable->isConstant() || variable->immutable())
				continue;
			if (variable->referenceLocation()
				== solidity::frontend::VariableDeclaration::Location::Transient)
				result.requiresSparseSlots = true;
			if (typeUsesHashedSlots(variable->annotation().type))
				result.requiresSparseSlots = true;
		}
	}

	return result;
}

} // namespace puyasol::builder
