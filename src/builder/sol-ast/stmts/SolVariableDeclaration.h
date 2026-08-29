#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

/// Variable declaration: type name = expr; or (type1 a, type2 b) = expr;
/// Handles initializers, storage aliases, function pointers, constant tracking,
/// new-array size upgrading, and tuple destructuring.
class SolVariableDeclaration: public SolStatement
{
public:
	SolVariableDeclaration(BlockContext& _blk,
		solidity::frontend::VariableDeclarationStatement const& _node,
		awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	// ── toAwst binding rungs (SolVariableDeclaration.cpp) ───────────────
	bool tryCalldataSlicePointerBinding(
		solidity::frontend::VariableDeclaration const& decl,
		solidity::frontend::Expression const* initialValue,
		std::vector<std::shared_ptr<awst::Statement>>& result);
	bool trySlotModeStoragePointer(
		solidity::frontend::VariableDeclaration const& decl,
		solidity::frontend::Expression const* initialValue,
		std::vector<std::shared_ptr<awst::Statement>>& result);
	std::shared_ptr<awst::Expression> buildInitValue(
		solidity::frontend::VariableDeclaration const& decl,
		solidity::frontend::Expression const* initialValue,
		awst::WType const*& type,
		std::shared_ptr<awst::Expression> const& target,
		bool& earlyExit);
	bool tryStorageAliasBinding(
		solidity::frontend::VariableDeclaration const& decl,
		std::shared_ptr<awst::Expression>& value,
		solidity::frontend::Expression const* initialValue,
		std::vector<std::shared_ptr<awst::Statement>>& result);
	bool tryMemoryAliasBinding(
		solidity::frontend::VariableDeclaration const& decl,
		solidity::frontend::Expression const* initialValue,
		std::shared_ptr<awst::Expression>& value,
		awst::WType const* type,
		std::vector<std::shared_ptr<awst::Statement>>& result);
	bool tryBlobOffsetBinding(
		solidity::frontend::VariableDeclaration const& decl,
		solidity::frontend::Expression const* initialValue,
		std::shared_ptr<awst::Expression>& value,
		awst::WType const* type,
		std::vector<std::shared_ptr<awst::Statement>>& result);
	bool tryAsmAggregateInit(
		solidity::frontend::VariableDeclaration const& decl,
		solidity::frontend::Expression const* initialValue,
		std::shared_ptr<awst::Expression>& value,
		awst::WType const* type,
		std::vector<std::shared_ptr<awst::Statement>>& result);
	void emitDefaultDeclaration(
		solidity::frontend::VariableDeclaration const& decl,
		std::shared_ptr<awst::Expression> target,
		std::shared_ptr<awst::Expression> value,
		awst::WType const* type,
		solidity::frontend::Expression const* initialValue,
		std::vector<std::shared_ptr<awst::Statement>>& result);
	void buildTupleDestructuring(
		solidity::frontend::Expression const* initialValue,
		std::vector<std::shared_ptr<awst::Statement>>& result);

	solidity::frontend::VariableDeclarationStatement const& m_node;
};

} // namespace puyasol::builder::sol_ast
