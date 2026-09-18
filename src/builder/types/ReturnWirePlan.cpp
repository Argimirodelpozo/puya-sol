#include "builder/types/ReturnWirePlan.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/SolIntType.h"
#include "builder/codec/Arc4Defaults.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

awst::WType const* abiReturnNativeType(
	TypeMapper& types, solidity::frontend::Type const* solType)
{
	if (auto integer = SolIntType::fromSolOrEnum(solType); integer && integer->isSigned)
		return awst::WType::biguintType();
	return types.map(solType);
}

ReturnWireElem planReturnElement(
	TypeMapper& types,
	solidity::frontend::Type const* solType,
	awst::WType const* nativeType)
{
	ReturnWireElem item;
	item.nativeType = item.wireType = nativeType;
	auto integer = SolIntType::fromSolOrEnum(solType);
	if (nativeType == awst::WType::biguintType())
	{
		item.isSigned = integer && integer->isSigned;
		item.bits = integer ? integer->bits : 256u;
		item.wireType = types.createType<awst::ARC4UIntN>(
			static_cast<int>(item.isSigned ? 256u : item.bits));
		item.encoded = true;
	}
	else if (integer && !integer->isSigned && integer->bits < 64)
	{
		// Unsigned sub-word: mask the native uint64 to the declared width and
		// publish that width (arc4.uintN) like solc's ABI, instead of uint64.
		item.masked = true;
		item.bits = integer->bits;
		item.wireType = types.createType<awst::ARC4UIntN>(static_cast<int>(item.bits));
		item.encoded = true;
	}
	return item;
}

FunctionReturnPlan const& TypeMapper::functionReturnPlan(
	solidity::frontend::FunctionDefinition const& function)
{
	using solidity::frontend::VariableDeclaration;
	if (auto it = m_returnPlans.find(function.id()); it != m_returnPlans.end())
		return it->second;

	FunctionReturnPlan plan;
	auto const& returns = function.returnParameters();
	std::vector<awst::WType const*> nativeTypes, wireTypes;
	std::vector<std::string> names;
	bool allNamed = true;
	auto const& storageReturns = analysis().storageReturnFacts(&function);
	for (auto const& parameter: returns)
	{
		auto const* native = function.isPartOfExternalInterface()
			? abiReturnNativeType(*this, parameter->type()) : map(parameter->type());
		bool const storage = parameter->referenceLocation() == VariableDeclaration::Location::Storage;
		if (storage && (profile().evmStorageLayout || storageReturns.slotHandle))
			native = awst::WType::biguintType();
		else if (storage && storageReturns.bytesKeyed)
			native = awst::WType::bytesType();
		else if (returns.size() == 1 && storageReturns.indexedReturn)
			native = awst::WType::uint64Type();
		plan.elements.push_back(planReturnElement(*this, parameter->type(), native));
		nativeTypes.push_back(native);
		wireTypes.push_back(plan.elements.back().wireType);
		names.push_back(parameter->name());
		allNamed &= !parameter->name().empty();
	}
	if (returns.empty())
		plan.nativeType = plan.wireType = awst::WType::voidType();
	else if (returns.size() == 1)
	{
		plan.nativeType = nativeTypes.front();
		plan.wireType = wireTypes.front();
	}
	else
	{
		// Solidity return tuples are positional. Retain optional AWST field
		// names only for a fully named list; repeated empty names are invalid.
		plan.nativeType = allNamed
			? createType<awst::WTuple>(std::move(nativeTypes), std::move(names), function.name() + "Return")
			: createType<awst::WTuple>(std::move(nativeTypes));
		plan.wireType = createType<awst::WTuple>(std::move(wireTypes));
	}
	plan.internalType = plan.nativeType;
	plan.internalElements = plan.elements;
	std::vector<awst::WType const*> internalTypes;
	bool pointers = false;
	for (size_t i = 0; i < returns.size(); ++i)
	{
		auto& element = plan.internalElements[i];
		if (returns[i]->referenceLocation() == VariableDeclaration::Location::Memory
			&& analysis().memoryPointerDeclarations.contains(returns[i]->id()))
		{
			element = planReturnElement(*this, returns[i]->type(), awst::WType::uint64Type());
			pointers = true;
		}
		internalTypes.push_back(element.nativeType);
	}
	if (pointers) plan.internalType = internalTypes.size() == 1 ? internalTypes.front()
		: createType<awst::WTuple>(std::move(internalTypes));
	return m_returnPlans.emplace(function.id(), std::move(plan)).first->second;
}

} // namespace puyasol::builder
