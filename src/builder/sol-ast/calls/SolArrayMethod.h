#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// array.push(val), array.push(), and array.pop().
/// Handles both box-backed (state variable) and in-memory arrays.
class SolArrayMethod: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Handle push/pop on box-backed dynamic arrays.
	std::shared_ptr<awst::Expression> handleBoxArray(
		std::string const& _memberName,
		solidity::frontend::Expression const& _baseExpr,
		solidity::frontend::VariableDeclaration const& _varDecl);

	/// Handle push/pop on in-memory arrays.
	std::shared_ptr<awst::Expression> handleMemoryArray(
		std::string const& _memberName,
		solidity::frontend::Expression const& _baseExpr);

	/// Handle push/pop on a dynamic array field of a storage struct via
	/// a copy-on-write pattern (read struct → mutate tmp → write back).
	std::shared_ptr<awst::Expression> handleStructFieldArrayMethod(
		std::string const& _memberName,
		solidity::frontend::MemberAccess const& _fieldAccess,
		solidity::frontend::VariableDeclaration const& _structVar);
};

} // namespace puyasol::builder::sol_ast
