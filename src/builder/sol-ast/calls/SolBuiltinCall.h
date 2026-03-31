#pragma once

#include "builder/sol-ast/SolFunctionCall.h"
#include "builder/sol-eb/BuiltinCallables.h"

namespace puyasol::builder::sol_ast
{

/// Built-in function calls: keccak256, sha256, addmod, mulmod, gasleft,
/// selfdestruct, blockhash, ecrecover.
///
/// Delegates to BuiltinCallableRegistry for most, handles blockhash inline
/// (AVM-specific: maps to block BlkSeed).
class SolBuiltinCall: public SolFunctionCall
{
public:
	SolBuiltinCall(
		eb::BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _call,
		std::string _builtinName);

	std::shared_ptr<awst::Expression> toAwst() override;

private:
	std::string m_builtinName;

	std::shared_ptr<awst::Expression> handleBlockhash();
};

} // namespace puyasol::builder::sol_ast
