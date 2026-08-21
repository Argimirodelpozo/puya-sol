#pragma once

/// @file StorageRefPointer.h
/// Detection for storage-ref pointer functions — internal functions returning
/// `T storage`. puya's Lvalue union is closed (no call-result lvalue), so
/// such functions return only the uint64 index; call sites reconstitute
/// `IndexExpression(<stateVar>, <call>)` as the lvalue.

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include "builder/ProgramAnalysis.h"

#include <cstdint>
#include <set>
#include <vector>

namespace puyasol::builder
{

/// True if `_t` is a MappingType, or any array/struct that (recursively)
/// contains one. Determines whether a `storage` ref travels as a bytes
/// box-key vs an AWST-mapped value. Must be consistent across AWSTBuilder,
/// SolInternalCall, FunctionBuilder, PublicGetterBuilder, and
/// storageRefPointerReturn, or callee writes land under the wrong key.
/// Defined here (lowest storage-ref header); AWSTBuilder.h re-exports it.
inline bool containsMappingType(
	solidity::frontend::Type const* _t,
	std::set<int64_t>* _visiting = nullptr)
{
	if (!_t) return false;
	if (dynamic_cast<solidity::frontend::MappingType const*>(_t)) return true;
	if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		return containsMappingType(arr->baseType(), _visiting);
	if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(_t))
	{
		// A RECURSIVE struct (`struct Node { Node[] kids; }`) would otherwise
		// recurse forever. Callers that only ever see storage-ref types never hit
		// one, so this was latent until the predicate started running over every
		// struct state var (StorageMapper::shouldUseBoxStorage).
		std::set<int64_t> owned;
		if (!_visiting) _visiting = &owned;
		if (!_visiting->insert(st->structDefinition().id()).second)
			return false;                       // already on the current path
		for (auto const& member: st->members(nullptr))
			if (containsMappingType(member.type, _visiting))
				return true;
		return false;
	}
	return false;
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
	if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		return !arr->isByteArrayOrString() && arr->isDynamicallyEncoded();
	return false;
}

/// If `_func` is a storage-ref pointer function — an implemented internal
/// function returning a single `T storage` via `return <holder>[<idx>]` or
/// (mapping-of-struct) a named-return assignment, with no `.slot :=` assembly —
/// returns the IndexAccess. The holder may be a state variable (array/slot:
/// call site reconstitutes IndexExpression) or a `storage` param (mapping:
/// box-key pass-through). Returns nullptr otherwise.
inline solidity::frontend::IndexAccess const* storageRefPointerReturn(
	solidity::frontend::FunctionDefinition const* _func)
{
	using namespace solidity::frontend;
	if (!_func || !_func->isImplemented())
		return nullptr;
	if (_func->returnParameters().size() != 1)
		return nullptr;
	if (_func->returnParameters()[0]->referenceLocation()
		!= VariableDeclaration::Location::Storage)
		return nullptr;

	// Collect every `return` statement and note any inline assembly.
	struct ReturnFinder: ASTConstVisitor
	{
		std::vector<Return const*> returns;
		bool sawAssembly = false;
		bool visit(Return const& _r) override
		{
			returns.push_back(&_r);
			return true;
		}
		bool visit(InlineAssembly const&) override
		{
			sawAssembly = true;
			return true;
		}
	} finder;
	_func->body().accept(finder);

	// `.slot :=` assembly variant handled elsewhere (returns biguint slot).
	// Require exactly one return — branching returns can't reduce to one base.
	if (finder.sawAssembly)
		return nullptr;

	// `<holder>[<index>]` comes from an explicit return or a named-return
	// assignment (V4 shape: `position = self[positionKey]` with no return stmt).
	Expression const* refExpr = nullptr;
	bool namedReturnForm = false;
	if (finder.returns.size() == 1 && finder.returns[0]->expression())
		refExpr = finder.returns[0]->expression();
	else if (finder.returns.empty() && !_func->returnParameters()[0]->name().empty())
	{
		struct AssignFinder: ASTConstVisitor
		{
			VariableDeclaration const* target = nullptr;
			Expression const* rhs = nullptr;
			bool visit(Assignment const& _a) override
			{
				if (auto const* lhs =
						dynamic_cast<Identifier const*>(&_a.leftHandSide()))
					if (lhs->annotation().referencedDeclaration == target)
						rhs = &_a.rightHandSide();
				return true;
			}
		} af;
		af.target = _func->returnParameters()[0].get();
		_func->body().accept(af);
		refExpr = af.rhs;
		namedReturnForm = true;
	}
	if (!refExpr)
		return nullptr;

	auto const* indexAccess = dynamic_cast<IndexAccess const*>(refExpr);
	if (!indexAccess)
		return nullptr;

	// Mapping holder → element is box-keyed (callee yields bytes key, no reconstitution).
	// Array/slot holder → returns uint64 index; caller reconstitutes IndexExpression.
	bool const holderIsMapping = dynamic_cast<MappingType const*>(
		indexAccess->baseExpression().annotation().type) != nullptr;

	auto const* baseId = dynamic_cast<Identifier const*>(
		&indexAccess->baseExpression());
	if (!baseId)
		return nullptr;
	auto const* baseVar = dynamic_cast<VariableDeclaration const*>(
		baseId->annotation().referencedDeclaration);
	if (!baseVar)
		return nullptr;

	// Named-return form is supported only for a mapping holder (box-key
	// pass-through); array/slot reconstitution needs an explicit return.
	if (namedReturnForm)
		return holderIsMapping ? indexAccess : nullptr;

	// Explicit return: state-var holder → array/slot reconstitution or direct mapping.
	// Non-state-var holder only accepted when it's a mapping (e.g. `return self[k]`
	// where `self` is a `mapping(K=>V) storage` param — box-key pass-through).
	if (baseVar->isStateVariable())
		return indexAccess;
	if (holderIsMapping)
		return indexAccess;
	return nullptr;
}

/// True if the storage-ref pointer function's return is box-keyed (bytes prefix)
/// rather than a uint64 index: holder is a mapping, or the returned struct
/// has nested mappings. Use everywhere the old containsMappingType(returnType)
/// gate stood so plain-struct mapping elements (e.g. V4 Position.State) are
/// still box-keyed.
inline bool storageRefReturnIsBytesKeyed(
	solidity::frontend::FunctionDefinition const* _func)
{
	using namespace solidity::frontend;
	auto const* ix = storageRefPointerReturn(_func);
	if (!ix)
		return false;
	if (dynamic_cast<MappingType const*>(ix->baseExpression().annotation().type))
		return true;
	return containsMappingType(_func->returnParameters()[0]->type());
}

} // namespace puyasol::builder
