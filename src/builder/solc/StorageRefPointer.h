#pragma once

/// @file StorageRefPointer.h
/// Detection for storage-ref pointer functions — internal functions returning
/// `T storage`. puya's Lvalue union is closed (no call-result lvalue), so
/// such functions return only the uint64 index; call sites reconstitute
/// `IndexExpression(<stateVar>, <call>)` as the lvalue.

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>
#include "builder/solc/SolcFwd.h"

#include "builder/context/ProgramAnalysis.h"

#include <cstdint>
#include <set>
#include <vector>

namespace puyasol::builder
{

/// True when `_t`, or any type reachable through its arrays and struct
/// members, satisfies `_pred`. Shared traversal behind the storage-only-family
/// predicates below.
///
/// A RECURSIVE struct (`struct Node { Node[] kids; }`) would otherwise recurse
/// forever. Callers that only ever see storage-ref types never hit one, so this
/// was latent until the predicates started running over every struct state var
/// (StorageMapper::shouldUseBoxStorage).
template <typename Predicate>
inline bool typeContains(
	solidity::frontend::Type const* _t,
	Predicate const& _pred,
	std::set<int64_t>* _visiting = nullptr)
{
	if (!_t) return false;
	if (_pred(_t)) return true;
	if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		return typeContains(arr->baseType(), _pred, _visiting);
	if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(_t))
	{
		std::set<int64_t> owned;
		if (!_visiting) _visiting = &owned;
		if (!_visiting->insert(st->structDefinition().id()).second)
			return false;                       // already on the current path
		for (auto const& member: st->members(nullptr))
			if (typeContains(member.type, _pred, _visiting))
				return true;
		return false;
	}
	return false;
}

/// True if `_t` is a MappingType, or any array/struct that (recursively)
/// contains one. Determines whether a `storage` ref travels as a bytes
/// box-key vs an AWST-mapped value. Must be consistent across AWSTBuilder,
/// SolInternalCall, FunctionBuilder, PublicGetterBuilder, and
/// storageRefPointerReturn, or callee writes land under the wrong key.
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
	return typeContains(_t, [](solidity::frontend::Type const* t) {
		auto const* array = dynamic_cast<solidity::frontend::ArrayType const*>(t);
		return array && array->isDynamicallySized();
	});
}

/// Cached provenance; no repeated AST scans at signatures, returns or callers.
inline solidity::frontend::IndexAccess const* storageRefPointerReturn(
	solidity::frontend::FunctionDefinition const* _func,
	ProgramAnalysis const& _analysis)
{
	return _analysis.storageReturnFacts(_func).indexedReturn;
}

/// True if the storage-ref pointer function's return is box-keyed (bytes prefix)
/// rather than a uint64 index: holder is a mapping, or the returned struct
/// has nested mappings. Use everywhere the old containsMappingType(returnType)
/// gate stood so plain-struct mapping elements (e.g. V4 Position.State) are
/// still box-keyed.
inline bool storageRefReturnIsBytesKeyed(
	solidity::frontend::FunctionDefinition const* _func,
	ProgramAnalysis const& _analysis)
{
	return _analysis.storageReturnFacts(_func).bytesKeyed;
}

inline bool storageRefReturnUsesSlot(
	solidity::frontend::FunctionDefinition const* _func,
	ProgramAnalysis const& _analysis)
{
	return _analysis.storageReturnFacts(_func).slotHandle;
}

} // namespace puyasol::builder
