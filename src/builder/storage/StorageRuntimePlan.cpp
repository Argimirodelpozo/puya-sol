#include "builder/storage/StorageRuntimePlan.h"

#include "builder/ProgramAnalysis.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/codec/EvmValueCodec.h"

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
		for (auto const& member: structure->members(nullptr))
		{
			if (typeUsesHashedSlots(member.type))
				return true;
			// The full AVM account needs a shadow word when solc packs it with
			// another member. This applies inside structs/fixed arrays too, not
			// just to top-level state declarations checked below.
			auto const* underlying = codec::underlyingType(member.type);
			if (dynamic_cast<solidity::frontend::AddressType const*>(underlying)
				|| dynamic_cast<solidity::frontend::ContractType const*>(underlying))
				for (auto const& other: structure->members(nullptr))
					if (other.name != member.name && structure->storageOffsetsOfMember(other.name).first
						== structure->storageOffsetsOfMember(member.name).first) return true;
		}
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
	result.solidityLayout.computeLayout(_contract, _typeMapper);

	auto const& slotCallables = _typeMapper.analysis().callablesWithStorageSlotAccess;
	// solc creation/deployed graphs include modifiers and are closed over
	// library/free references. Only fall back to source walks without a graph.
	if (_typeMapper.analysis().hasContractReachability(_contract.id()))
		for (int64_t callableId: slotCallables)
			if (_typeMapper.analysis().isCallableReachable(
				_contract.id(), callableId))
			{
				result.usesSlotAccess = true;
				break;
			}
	if (!_typeMapper.analysis().hasContractReachability(_contract.id()))
	{
		forEachDefinedFunction(_contract, [&](auto const* function) {
			if (function->isImplemented() && slotCallables.count(function->id()))
				result.usesSlotAccess = true;
		});
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
			if (base)
				for (auto const* modifier: base->functionModifiers())
					if (modifier && modifier->isImplemented() && slotCallables.count(modifier->id()))
						result.usesSlotAccess = true;
	}
	result.requiresSparseSlots = result.usesSlotAccess;

	// Packed addresses use a keccak-derived shadow slot for their high bytes.
	for (auto const& variable: result.solidityLayout.variables())
	{
		// A fixed array can start in the dense region while its valid elements
		// extend far beyond it. Solc's full-width storage span is authoritative;
		// testing only the declaration's first slot wrongly removes the sparse path.
		if (variable.slot >= solidity::u256(kEvmDenseSlotLimit)
			|| (variable.solType && variable.solType->storageSize()
				> solidity::u256(kEvmDenseSlotLimit) - variable.slot))
			result.requiresSparseSlots = true;
		if (variable.wtype == awst::WType::accountType() && variable.byteSize == 20)
			if (auto const* slot = result.solidityLayout.getSlotInfo(variable.slot);
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
			// Transient declarations have their own scratch-backed word space.
			if (typeUsesHashedSlots(variable->annotation().type))
				result.requiresSparseSlots = true;
		}
	}

	return result;
}

} // namespace puyasol::builder
