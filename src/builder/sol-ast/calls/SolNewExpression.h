#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// new bytes(N), new T[](N), new Contract(...).
/// Handles object creation expressions.
class SolNewExpression: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// new bytes(N) → bzero(N)
	std::shared_ptr<awst::Expression> handleNewBytes();
	/// new T[](N) → NewArray with N default elements or runtime loop
	std::shared_ptr<awst::Expression> handleNewArray();
};

} // namespace puyasol::builder::sol_ast
