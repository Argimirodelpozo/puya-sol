#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

#include <set>
#include <vector>

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

	// ── buildSubroutineCall phases ──────────────────────────────────────
	/// Storage-ref-pointer result: reconstitute IndexExpression (or pass through slot handles / bytes box-key returns).
	std::shared_ptr<awst::Expression> wrapStorageRefResult(
		std::shared_ptr<awst::Expression> _result,
		solidity::frontend::FunctionDefinition const* _funcDef);
	/// Param wtypes for coercion + the mapping/slot storage-ref index sets.
	void collectSubroutineParamTypes(
		solidity::frontend::FunctionDefinition const& _funcDef,
		std::vector<awst::WType const*>& paramTypes,
		std::set<size_t>& mappingStorageParamIndices,
		std::set<size_t>& evmSlotRefParamIndices,
		std::set<size_t>& blobOffsetParamIndices);
	/// Box-key prefix for a mapping/storage-ref argument.
	std::shared_ptr<awst::Expression> extractMappingKeyPrefix(
		solidity::frontend::Expression const& argExpr);
	/// Build all call args (using-for receiver first) with EVM left-to-right effect sequencing.
	void buildSequencedArgs(
		std::shared_ptr<awst::SubroutineCallExpression> const& call,
		solidity::frontend::FunctionDefinition const* _funcDef,
		bool _isUsingForCall,
		std::vector<awst::WType const*> const& paramTypes,
		std::set<size_t> const& mappingStorageParamIndices,
		std::set<size_t> const& evmSlotRefParamIndices,
		std::set<size_t> const& blobOffsetParamIndices);
	/// Companion byte offset for an offset-convention struct-ref argument.
	std::shared_ptr<awst::Expression> offsetForArg(
		solidity::frontend::Expression const* argExpr);
	/// Append the companion uint64 offset args (handle-model dual handle).
	void appendStructRefOffsetArgs(
		std::shared_ptr<awst::SubroutineCallExpression> const& call,
		solidity::frontend::FunctionDefinition const& _funcDef,
		bool _isUsingForCall);

	/// Helper to build return type from a FunctionDefinition.
	awst::WType const* returnTypeFrom(
		solidity::frontend::FunctionDefinition const* _funcDef);
};

} // namespace puyasol::builder::sol_ast
