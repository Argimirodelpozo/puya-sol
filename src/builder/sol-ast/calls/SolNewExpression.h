#pragma once

#include "builder/sol-ast/SolFunctionCall.h"
#include <set>
#include <string>

namespace puyasol::builder::sol_ast
{

/// new bytes(N), new T[](N), new Contract(...).
/// Handles object creation expressions.
class SolNewExpression: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

	/// Child contract names referenced by `new C()`. Used to generate
	/// the .tmpl file mapping template variable names to child bytecode.
	static std::set<std::string> const& childContracts() { return s_childContracts; }
	static void resetChildContracts() { s_childContracts.clear(); }

private:
	std::shared_ptr<awst::Expression> handleNewBytes();
	std::shared_ptr<awst::Expression> handleNewArray();

	static std::set<std::string> s_childContracts;
};

} // namespace puyasol::builder::sol_ast
