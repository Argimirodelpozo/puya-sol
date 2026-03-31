#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// Internal function calls: direct calls, library calls, free functions,
/// super calls, base internal calls, using-for directive calls.
/// Builds a SubroutineCallExpression targeting the resolved function.
class SolInternalCall: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Resolve an identifier-based function call target.
	std::shared_ptr<awst::Expression> resolveIdentifierCall(
		solidity::frontend::Identifier const& _ident);

	/// Resolve a member-access-based function call target.
	std::shared_ptr<awst::Expression> resolveMemberAccessCall(
		solidity::frontend::MemberAccess const& _memberAccess);

	/// Resolve a function pointer cast pattern: _castToView(fn)(args).
	std::shared_ptr<awst::Expression> resolveFunctionPointerCast(
		solidity::frontend::FunctionCall const& _innerCall);

	/// Build the SubroutineCallExpression with arguments and type coercion.
	std::shared_ptr<awst::Expression> buildSubroutineCall(
		awst::SubroutineTarget _target,
		awst::WType const* _returnType,
		solidity::frontend::FunctionDefinition const* _funcDef,
		bool _isUsingForCall);

	/// Helper to build return type from a FunctionDefinition.
	awst::WType const* returnTypeFrom(
		solidity::frontend::FunctionDefinition const* _funcDef);
};

} // namespace puyasol::builder::sol_ast
