#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder
{
class TypeMapper;
}

namespace puyasol::builder::sol_ast
{

/// Inline assembly block — delegates to AssemblyBuilder.
class SolInlineAssembly: public SolStatement
{
public:
	SolInlineAssembly(StatementContext& _ctx,
		solidity::frontend::InlineAssembly const& _node,
		awst::SourceLocation _loc,
		std::vector<std::pair<std::string, awst::WType const*>> const& _functionParams,
		awst::WType const* _returnType,
		std::map<std::string, unsigned> const& _functionParamBitWidths);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	solidity::frontend::InlineAssembly const& m_node;
	std::vector<std::pair<std::string, awst::WType const*>> const& m_functionParams;
	awst::WType const* m_returnType;
	std::map<std::string, unsigned> const& m_functionParamBitWidths;
};

} // namespace puyasol::builder::sol_ast
