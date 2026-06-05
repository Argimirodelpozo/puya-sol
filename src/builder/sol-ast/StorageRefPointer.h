#pragma once

/// @file StorageRefPointer.h
/// Detection for "storage-ref pointer functions" — internal functions that
/// return a `T storage` reference.
///
/// puya's `Lvalue` union (VarExpression | FieldExpression | IndexExpression
/// | TupleExpression | StorageExpression) is closed: a storage location is
/// always one of those structural nodes, never a call result. A `callsub`
/// can only hand back a value copy, so a storage pointer cannot survive a
/// real subroutine return.
///
/// Such a function is therefore compiled to return only the uint64 *index*
/// of the storage location, and each call site reconstitutes the location
/// as `IndexExpression(<stateVar>, <call>)` — a real lvalue node. The
/// function body (including any guards / local computation) still runs as
/// an ordinary subroutine; only the index value crosses the return.

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <cstdint>
#include <set>
#include <vector>

namespace puyasol::builder
{

/// True if `_t` is a MappingType, or any array (possibly nested) whose element
/// type eventually contains a MappingType, or a struct with such a member. Used
/// to decide which `storage` references travel as runtime bytes key-prefixes (the
/// box-key scheme) vs ordinary AWST-mapped values. Must agree across every call
/// site (AWSTBuilder, SolInternalCall, FunctionBuilder, PublicGetterBuilder, and
/// storageRefPointerReturn below) or callee writes land under the wrong key.
/// (Defined here, the lowest storage-ref header; AWSTBuilder.h re-exports it.)
inline bool containsMappingType(solidity::frontend::Type const* _t)
{
	if (!_t) return false;
	if (dynamic_cast<solidity::frontend::MappingType const*>(_t)) return true;
	if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		return containsMappingType(arr->baseType());
	if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(_t))
	{
		for (auto const& member: st->members(nullptr))
			if (containsMappingType(member.type))
				return true;
		return false;
	}
	return false;
}

/// Registry of struct StructDefinition ids that are used as a mapping VALUE type
/// anywhere in the program (populated source-unit-wide at the start of
/// AWSTBuilder::build). A struct-storage-ref of such a type denotes a mapping
/// element — a box keyed at runtime — so it must travel as a box-key (bytes),
/// not by value. A plain state-var/local struct (never a mapping value) travels
/// by value (the existing copy + write-back path).
///
/// COMPILE-TIME ONLY: this is a type classifier, not a storage-location decision.
/// All storage lives in main; this never implies operating on another program's
/// boxes. The scan is source-unit-wide purely because the mapping DECLARATION may
/// appear in a different contract's source than the library that defines methods
/// on the value struct (V4: the orchestrator's source declares mapping(=>Position.
/// State); the Position library's source defines update) — both compile against
/// the same single main storage. Single-threaded, one-compile-per-process, so a
/// process-wide static is safe.
inline std::set<int64_t>& boxKeyedStructRegistry()
{
	static std::set<int64_t> registry;
	return registry;
}

/// Walk `_t` and record into the box-keyed-struct registry every struct type that
/// appears as a mapping VALUE within it (recursively through arrays, nested
/// mappings, and struct members). `_seen` guards recursive struct types.
inline void collectMappingValueStructs(
	solidity::frontend::Type const* _t,
	std::set<int64_t>& _out,
	std::set<solidity::frontend::Type const*>& _seen)
{
	using namespace solidity::frontend;
	if (!_t || !_seen.insert(_t).second) return;
	if (auto const* m = dynamic_cast<MappingType const*>(_t))
	{
		// The value type is box-keyed if it is a struct (possibly inside an array).
		Type const* vu = m->valueType();
		while (auto const* va = dynamic_cast<ArrayType const*>(vu)) vu = va->baseType();
		if (auto const* vs = dynamic_cast<StructType const*>(vu))
			_out.insert(vs->structDefinition().id());
		collectMappingValueStructs(m->valueType(), _out, _seen); // nested mappings / struct members
	}
	else if (auto const* a = dynamic_cast<ArrayType const*>(_t))
		collectMappingValueStructs(a->baseType(), _out, _seen);
	else if (auto const* s = dynamic_cast<StructType const*>(_t))
		for (auto const& member: s->members(nullptr))
			collectMappingValueStructs(member.type, _out, _seen);
}

/// A `T storage` reference is BOX-KEYED — it travels as a bytes box-key prefix and
/// the holder writes box storage directly — when T is a mapping, an array/struct
/// that CONTAINS a mapping, or a plain struct that is itself used as a mapping
/// VALUE type (e.g. V4 Position.State, which has no nested mappings but is the
/// value of mapping(bytes32 => Position.State)). A plain struct that is NOT a
/// mapping value (a state-var/local struct passed to a library function) is NOT
/// box-keyed — it travels by value (the copy + write-back path), so widening to
/// "any struct" would wrongly box-key those and read empty boxes (regressing the
/// using-for/library/struct tests). This is the gate every storage-ref PARAM/RETURN
/// site must use instead of bare `containsMappingType`. Apply only to storage refs
/// (referenceLocation == Storage), which the call sites already establish.
inline bool isBoxKeyedStorageRef(solidity::frontend::Type const* _t)
{
	if (containsMappingType(_t)) return true;
	if (auto const* s = dynamic_cast<solidity::frontend::StructType const*>(_t))
		return boxKeyedStructRegistry().count(s->structDefinition().id()) > 0;
	return false;
}

/// If `_func` is a storage-ref pointer function — an implemented internal
/// function returning a single `T storage` via either an explicit
/// `return <holder>[<idx>];` statement OR (for the mapping-of-struct case) a
/// named-return assignment `<namedReturn> = <holder>[<idx>];`, with no `.slot :=`
/// inline assembly — returns that `IndexAccess` (its base holder and index
/// sub-expression are reachable from it). The base `<holder>` may be a state
/// variable (array/slot refs reconstitute `IndexExpression(stateVar, idx)` at the
/// call site) or, for the mapping-of-struct case only, a `storage` param/local
/// (e.g. `self[k]` where `self` is a `mapping(K=>Struct) storage` param — the
/// box-key is passed through, the caller binds it as a struct-storage-ref).
/// Returns nullptr otherwise.
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

	// The `.slot :=` assembly variant is handled separately (its return
	// type maps to a biguint slot). Require exactly one return — branching
	// returns into different containers can't reduce to one base.
	if (finder.sawAssembly)
		return nullptr;

	// The reference expression `<holder>[<index>]` comes from either an explicit
	// `return <holder>[<idx>];` or a named-return assignment
	// `<named> = <holder>[<idx>];` with no explicit return (the Uniswap V4
	// `position = self[positionKey];` shape: `returns (T storage position)`).
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

	// Is the holder a mapping? Then the element is box-keyed: the callee yields the
	// bytes box-key directly and the caller binds it as a storage ref (no
	// reconstitution). An array/slot holder instead returns a uint64 index that the
	// caller reconstitutes as IndexExpression(holder, idx) — which requires an
	// explicit `return holder[idx];` and a state-variable holder.
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

	// Explicit return: a state-variable holder is the original case (array/slot
	// reconstitution + direct state mappings). A non-state-var holder is accepted
	// only when it is a mapping — e.g. a `mapping(K=>V) storage` param `self` in
	// `return self[k]` — as box-key pass-through (no state variable required).
	if (baseVar->isStateVariable())
		return indexAccess;
	if (holderIsMapping)
		return indexAccess;
	return nullptr;
}

/// For a storage-ref pointer function (storageRefPointerReturn != nullptr), true if
/// the reference is BOX-KEYED (a bytes prefix) rather than a uint64 array/slot index:
/// the holder is a mapping, or the returned struct itself carries nested mappings
/// (the latter preserves the original mapping-of-struct behaviour). Callers use this
/// to choose the bytes-vs-uint64 return type and the pass-through-vs-reconstitution
/// path; it must be consulted everywhere the old `containsMappingType(returnType)`
/// gate stood, so plain-struct mapping elements (e.g. V4 Position.State, which has no
/// nested mappings) are still box-keyed.
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
