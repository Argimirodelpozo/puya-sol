#pragma once

/// @file StorageRefPointer.h
/// Detection for storage-ref pointer functions — internal functions returning
/// `T storage`. puya's Lvalue union is closed (no call-result lvalue), so
/// such functions return only the uint64 index; call sites reconstitute
/// `IndexExpression(<stateVar>, <call>)` as the lvalue.

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <cstdint>
#include <set>
#include <vector>

namespace puyasol::builder
{

/// True if `_t` is a MappingType, or any array/struct that (recursively)
/// contains one. Determines whether a `storage` ref travels as a bytes
/// box-key vs an AWST-mapped value. Must be consistent across AWSTBuilder,
/// SolInternalCall, ContractBuilder, and return plans, or callee writes land
/// under the wrong key.
/// Defined here (lowest storage-ref header); AWSTBuilder.h re-exports it.
inline bool containsMappingType(
	solidity::frontend::Type const* _t)
{
	// Solc's predicate requires a nameable type. Literal/magic types can
	// reach conversion checks too, but cannot contain stored mappings.
	return _t && _t->nameable() && _t->containsNestedMapping();
}

/// A nonrecursive, storage-only wrapper with exactly one struct member has
/// the same solc storage extent as that member at (0, 0). Default holder format
/// 2 represents that wrapper transparently: it adds neither an ARC4 offset
/// header nor a logical holder step. Nominal WType identities remain distinct.
inline solidity::frontend::StructType const* transparentMappingWrapper(
	solidity::frontend::Type const* _type)
{
	using namespace solidity::frontend;
	auto const* outer = dynamic_cast<StructType const*>(_type);
	if (!outer || outer->recursive()
		|| !containsMappingType(outer)) return nullptr;
	auto const& members = outer->structDefinition().members();
	if (members.size() != 1) return nullptr;
	auto const* inner = dynamic_cast<StructType const*>(members.front()->type());
	if (!inner) return nullptr;
	auto const& [slot, offset] = outer->storageOffsetsOfMember(members.front()->name());
	return slot == 0 && offset == 0 && outer->storageSize() == inner->storageSize()
		? inner : nullptr;
}

/// A variable-size array anywhere in the stored aggregate makes its serialized
/// value dynamic. Use solc's array/member facts without asking for an ABI type:
/// structs containing internal functions have no ABI interface. Mappings have
/// their own keyed representation and are handled by containsMappingType.
inline bool hasDynamicStorageShape(solidity::frontend::Type const* _t)
{
	using namespace solidity::frontend;
	std::vector<Type const*> pending{_t};
	std::set<int64_t> visited;
	while (!pending.empty())
	{
		auto const* type = pending.back();
		pending.pop_back();
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
		{
			if (array->isDynamicallySized()) return true;
			pending.push_back(array->baseType());
		}
		else if (auto const* structure = dynamic_cast<StructType const*>(type);
			structure && visited.insert(structure->structDefinition().id()).second)
			for (auto const& member: structure->members(nullptr))
				pending.push_back(member.type);
	}
	return false;
}

} // namespace puyasol::builder
