#pragma once

#include "awst/Node.h"
#include <libsolidity/ast/AST.h>
#include <functional>

namespace puyasol::builder::eb { class ContractContext; }

namespace puyasol::builder::sol_ast
{

/// Evaluate operands in solc-codegen order, then arrange values in formal
/// parameter order. Encoding and physical reference binding remain the caller's.
class CallOperands
{
public:
	using Expr = std::shared_ptr<awst::Expression>;
	using LowerArgument = std::function<Expr(solidity::frontend::Expression const&, size_t)>;

	static std::vector<size_t> argumentOrder(
		solidity::frontend::FunctionCall const& call, bool viaIR);
	static std::vector<Expr> build(eb::ContractContext& ctx,
		solidity::frontend::FunctionCall const& call, awst::SourceLocation const& loc,
		LowerArgument const& lower = {});
	/// Internal-call parameters, including the bound receiver in solc-codegen order.
	static std::vector<Expr> buildParameters(eb::ContractContext& ctx,
		solidity::frontend::FunctionCall const& call, awst::SourceLocation const& loc,
		LowerArgument const& lower = {});
	static Expr evaluate(eb::ContractContext& ctx,
		solidity::frontend::Expression const& source, awst::SourceLocation const& loc,
		std::function<Expr()> const& lower = {});
};

} // namespace puyasol::builder::sol_ast
