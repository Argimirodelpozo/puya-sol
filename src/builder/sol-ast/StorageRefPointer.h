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

#include <vector>

namespace puyasol::builder
{

/// If `_func` is a storage-ref pointer function — an implemented internal
/// function returning a single `T storage` via exactly one
/// `return <stateVar>[<idx>];` statement, with no `.slot :=` inline
/// assembly — returns that return statement's `IndexAccess` (its base
/// state-variable and index sub-expression are reachable from it).
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
	if (finder.returns.size() != 1 || !finder.returns[0]->expression())
		return nullptr;

	// The return expression must be `<stateVariable>[<index>]`.
	auto const* indexAccess = dynamic_cast<IndexAccess const*>(
		finder.returns[0]->expression());
	if (!indexAccess)
		return nullptr;
	auto const* baseId = dynamic_cast<Identifier const*>(
		&indexAccess->baseExpression());
	if (!baseId)
		return nullptr;
	auto const* baseVar = dynamic_cast<VariableDeclaration const*>(
		baseId->annotation().referencedDeclaration);
	if (!baseVar || !baseVar->isStateVariable())
		return nullptr;
	return indexAccess;
}

} // namespace puyasol::builder
