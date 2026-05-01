#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder
{
class TypeMapper;
}

namespace puyasol::builder::sol_ast
{

/// Inline assembly block — delegates to AssemblyBuilder.
/// Reads function-level params/return type from the enclosing FunctionContext.
class SolInlineAssembly: public SolStatement
{
public:
	SolInlineAssembly(BlockContext& _blk,
		solidity::frontend::InlineAssembly const& _node,
		awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	solidity::frontend::InlineAssembly const& m_node;
};

} // namespace puyasol::builder::sol_ast
