#pragma once

#include <map>

#include "builder/sol-ast/SolFunctionCall.h"

#include <set>
#include <vector>

namespace puyasol::builder::sol_ast
{
class ResolvedLValue;

/// Internal function calls: direct calls, library calls, free functions,
/// super calls, base internal calls, using-for directive calls.
/// Builds a SubroutineCallExpression targeting the resolved function.
class SolInternalCall: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Interior field paths requested for reference params of THIS call
	/// (param index → path, enclosing box wtype); see BuildArtifacts::PathSpecialization.
	std::map<size_t, std::pair<std::vector<std::string>, awst::WType const*>> m_pathSpecs;
	std::map<size_t, std::shared_ptr<ResolvedLValue>> m_writeBacks;
	/// Resolve an identifier-based function call target.
	std::shared_ptr<awst::Expression> resolveIdentifierCall(
		solidity::frontend::Identifier const& _ident);

	/// Resolve a member-access-based function call target.
	std::shared_ptr<awst::Expression> resolveMemberAccessCall(
		solidity::frontend::MemberAccess const& _memberAccess);

	/// All function-valued expressions use the same argument lowering and
	/// dispatch path. Only proven-stable direct initializers are specialized.
	std::shared_ptr<awst::Expression> buildFunctionPointerCall(
		solidity::frontend::Expression const& _callee,
		solidity::frontend::FunctionType const& _type);

	/// Build the SubroutineCallExpression with arguments and type coercion.
	std::shared_ptr<awst::Expression> buildSubroutineCall(
		awst::SubroutineTarget _target,
		awst::WType const* _returnType,
		solidity::frontend::FunctionDefinition const* _funcDef);

	// ── buildSubroutineCall phases ──────────────────────────────────────
	/// Storage-ref-pointer result: reconstitute IndexExpression (or pass through slot handles / bytes box-key returns).
	std::shared_ptr<awst::Expression> wrapStorageRefResult(
		std::shared_ptr<awst::Expression> _result,
		solidity::frontend::FunctionDefinition const* _funcDef);
	/// Box-key prefix for a mapping/storage-ref argument.
	std::shared_ptr<awst::Expression> extractMappingKeyPrefix(
		solidity::frontend::Expression const& argExpr);
	/// Bind source operands in the selected solc codegen's evaluation order.
	void buildSequencedArgs(
		std::vector<awst::CallArg>& args,
		solidity::frontend::FunctionDefinition const* _funcDef,
		awst::SubroutineTarget const* target = nullptr);
	/// One physical binding for a key-plus-byte-offset struct reference.
	std::pair<std::shared_ptr<awst::Expression>, std::shared_ptr<awst::Expression>> bindBoxedReference(
		solidity::frontend::Expression const& argExpr);
	awst::WType const* returnTypeFrom(solidity::frontend::FunctionDefinition const* _funcDef);

};

} // namespace puyasol::builder::sol_ast
