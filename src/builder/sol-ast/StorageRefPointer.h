#pragma once

/// @file StorageRefPointer.h
/// Detection for storage-ref pointer functions — internal functions returning
/// `T storage`. puya's Lvalue union is closed (no call-result lvalue), so
/// such functions return only the uint64 index; call sites reconstitute
/// `IndexExpression(<stateVar>, <call>)` as the lvalue.

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>
#include "builder/sol-types/SolcFwd.h"

#include "builder/ProgramAnalysis.h"

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

/// True if `_t` is an INTERNAL function pointer, or any array/struct that
/// (recursively) holds one. The other storage-only family: like mappings these
/// have no ABI interface type, so solc's `isDynamicallyEncoded()` asserts
/// rather than answering (`solAssert(interfaceType(false).get(), "")`,
/// Types.cpp StructType::isDynamicallyEncoded).
/// `struct S { function() returns (uint) x; }` held as a state var aborted the
/// compiler outright until this was peeled.
///
/// Kind::External is deliberately EXCLUDED: those are the one function kind
/// solc gives an interface type (FunctionType::interfaceType), they encode as a
/// static 24-byte value, and the ABI predicate answers them fine. Matching them
/// here would silently reclassify structs that merely hold a callback.
inline bool containsInternalFunctionType(
	solidity::frontend::Type const* _t,
	std::set<int64_t>* _visiting = nullptr)
{
	return typeContains(_t, [](solidity::frontend::Type const* t) {
		auto const* fn = dynamic_cast<solidity::frontend::FunctionType const*>(t);
		return fn && fn->kind() != solidity::frontend::FunctionType::Kind::External;
	}, _visiting);
}

/// True when the `T storage` ref must travel as a bytes box-key prefix (the handle):
/// T is a mapping, contains a mapping, a struct used as a mapping value (e.g. V4
/// Position.State), OR a struct large enough to be ALWAYS box-backed (handle-model
/// Stage 1a). The bytes key is the box this aggregate lives in, so passing it to any
/// callee — library, free, OR contract method — writes through the shared box instead
/// of a lost value-copy.
///
/// The always-boxed gate is `storageSizeUpperBound() >= 4` slots (≥128B): such a struct
/// is boxed regardless of its name (shouldUseBoxStorage: estimatedBytes ≥ 128 > 128−nameLen
/// for any name), so the type-only predicate here AGREES with the var-level boxing decision —
/// no mismatch. Smaller structs are name-length-dependent (app-global) and still travel by
/// value + write-back; widening to *those* wrongly box-keys app-global storage (the
/// using-for/library/struct regression — hence the size gate, not "any struct").
/// Use this instead of bare containsMappingType at every storage-ref param/return site
/// (caller ensures referenceLocation==Storage).
inline bool isBoxKeyedStorageRef(
	solidity::frontend::Type const* _t,
	ProgramAnalysis const& _analysis)
{
	if (containsMappingType(_t)) return true;
	if (auto const* s = dynamic_cast<solidity::frontend::StructType const*>(_t))
	{
		auto id = s->structDefinition().id();
		if (_analysis.boxKeyedStructs.count(id) > 0)
			return true;
		// Passed by ref somewhere → boxed (shouldUseBoxStorage) → travels as a box-key handle.
		// Targeted (only ref-passed types) so never-ref-passed structs keep app-global (Stage 1b;
		// boxing EVERY struct regressed delete/asm/modifier/recursive paths).
		if (_analysis.refPassedStructs.count(id) > 0)
			return true;
		// Always-boxed (≥128B) structs: type-only size matches the var-level box decision.
		try { if (s->storageSizeUpperBound() >= 4) return true; } catch (...) {}
		return false;
	}
	// Every recursively dynamic non-bytes array is unconditionally box-backed
	// by StorageMapper, so its storage-ref representation is the box key too.
	// This uses solc's recursive ABI-shape fact and therefore covers scalar,
	// struct, and mixed fixed/dynamic ranks without enumerating leaf shapes.
	// Internal-fn-pointer check first: an element with no ABI interface type
	// aborts isDynamicallyEncoded() instead of answering it, and such an array
	// is fixed-size anyway, so it is not box-backed on this rule.
	if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		return !arr->isByteArrayOrString() && !containsInternalFunctionType(arr)
			&& arr->isDynamicallyEncoded();
	return false;
}

/// Cached provenance; no repeated AST scans at signatures, returns or callers.
inline solidity::frontend::IndexAccess const* storageRefPointerReturn(
	solidity::frontend::FunctionDefinition const* _func,
	ProgramAnalysis const& _analysis)
{
	auto found = _func ? _analysis.storageReferenceReturns.find(_func->id())
		: _analysis.storageReferenceReturns.end();
	return found == _analysis.storageReferenceReturns.end()
		? nullptr : found->second.indexedReturn;
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
	auto found = _func ? _analysis.storageReferenceReturns.find(_func->id())
		: _analysis.storageReferenceReturns.end();
	return found != _analysis.storageReferenceReturns.end() && found->second.bytesKeyed;
}

inline bool storageRefReturnUsesSlot(
	solidity::frontend::FunctionDefinition const* _func,
	ProgramAnalysis const& _analysis)
{
	auto found = _func ? _analysis.storageReferenceReturns.find(_func->id())
		: _analysis.storageReferenceReturns.end();
	return found != _analysis.storageReferenceReturns.end() && found->second.slotHandle;
}

} // namespace puyasol::builder
