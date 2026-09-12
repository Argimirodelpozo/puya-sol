/// @file AsmScan.h
/// One inline-assembly shape query that depends on Yul external references:
/// recognising the OpenZeppelin StorageSlot pointer-cast idiom.

#pragma once

#include "builder/ProgramAnalysis.h"

#include <libsolidity/ast/AST.h>
#include <libyul/AST.h>

#include <optional>

namespace puyasol::builder
{

/// Prove the modifier-free, one-statement pointer cast `r.slot := p.slot`.
/// The wrapper field must have the exact solc type and storage extent of p,
/// at slot/byte offset zero. This permits eliding the BODY, never its operands.
/// Assigning a storage pointer rebinds it; assigning the wrapper field writes
/// the referent. Prepared once in ProgramAnalysis, not rediscovered by callers.
inline std::optional<StorageReferenceReturnFacts::PointerAlias> storagePointerAliasParam(
	solidity::frontend::FunctionDefinition const& _func)
{
	using namespace solidity::frontend;
	if (!_func.isImplemented() || !_func.modifiers().empty() || _func.returnParameters().size() != 1)
		return std::nullopt;
	auto const& rp = _func.returnParameters()[0];
	if (!rp || rp->referenceLocation() != VariableDeclaration::Location::Storage)
		return std::nullopt;
	auto const* st = dynamic_cast<StructType const*>(rp->type());
	if (!st || st->structDefinition().members().size() != 1)
		return std::nullopt;
	auto const& field = st->structDefinition().members()[0];

	auto const& stmts = _func.body().statements();
	if (stmts.size() != 1)
		return std::nullopt;
	auto const* asmStmt = dynamic_cast<InlineAssembly const*>(stmts[0].get());
	if (!asmStmt)
		return std::nullopt;
	auto const& root = asmStmt->operations().root();
	if (root.statements.size() != 1)
		return std::nullopt;
	auto const* assign = std::get_if<solidity::yul::Assignment>(&root.statements[0]);
	if (!assign || assign->variableNames.size() != 1)
		return std::nullopt;
	auto const* rhs = std::get_if<solidity::yul::Identifier>(assign->value.get());
	if (!rhs)
		return std::nullopt;

	// Both sides must be `.slot` external references: LHS the return param,
	// RHS one of the storage parameters.
	Declaration const* lhsDecl = nullptr;
	Declaration const* rhsDecl = nullptr;
	for (auto const& [yulId, extInfo]: asmStmt->annotation().externalReferences)
	{
		if (extInfo.suffix != "slot")
			continue;
		if (yulId == &assign->variableNames[0])
			lhsDecl = extInfo.declaration;
		else if (yulId == rhs)
			rhsDecl = extInfo.declaration;
	}
	if (!lhsDecl || lhsDecl->id() != rp->id() || !rhsDecl)
		return std::nullopt;

	for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
	{
		auto const& p = _func.parameters()[pi];
		if (!p || p->id() != rhsDecl->id())
			continue;
		if (p->referenceLocation() != VariableDeclaration::Location::Storage)
			return std::nullopt;
		auto const* fieldType = field->type();
		if (auto const* reference = dynamic_cast<ReferenceType const*>(fieldType))
			fieldType = reference->withLocation(DataLocation::Storage, true);
		if (!fieldType || !p->type() || *fieldType != *p->type()
			|| st->storageOffsetsOfMember(field->name()) != std::pair<solidity::u256, unsigned>{0, 0}
			|| st->storageSize() != p->type()->storageSize())
			return std::nullopt;
		return StorageReferenceReturnFacts::PointerAlias{pi, field->name()};
	}
	return std::nullopt;
}

} // namespace puyasol::builder
