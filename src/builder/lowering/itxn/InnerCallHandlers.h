#pragma once

#include "builder/eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder::eb
{

/// Handles address.call/staticcall/delegatecall/transfer/send patterns.
class InnerCallHandlers
{
public:
	/// Try to handle an address member call; nullptr if not handled.
	static std::unique_ptr<InstanceBuilder> tryHandleAddressCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::string const& _memberName,
		solidity::frontend::FunctionCall const& _callNode,
		std::shared_ptr<awst::Expression> _callValue,
		solidity::frontend::Expression const& _baseExpr,
		awst::SourceLocation const& _loc);

private:
	/// Capture source values before any ABI encoder snapshots mutable carriers.
	static std::vector<std::shared_ptr<awst::Expression>> lowerArguments(
		ContractContext& ctx,
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const& args,
		std::vector<solidity::frontend::Type const*> const& paramTypes,
		awst::SourceLocation const& loc, bool reinterpret = false);

	/// .transfer(amount)
	static std::unique_ptr<InstanceBuilder> handleTransfer(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// .send(amount)
	static std::unique_ptr<InstanceBuilder> handleSend(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// .call{value: X}("") → payment plus receive/fallback for applications.
	static std::unique_ptr<InstanceBuilder> handleCallWithValue(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// Shared tail of every inner app call: the appl itxn (ApplicationArgs =
	/// `_argsTuple`, omitted when null so the callee sees empty calldata)
	/// grouped behind the optional payment, submitted as a pre-effect; result
	/// = (true, LastLog[4:]).
	static std::unique_ptr<InstanceBuilder> submitAppCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _argsTuple,
		std::shared_ptr<awst::Expression> _callValue,
		awst::SourceLocation const& _loc);

	/// Exact signature/declaration facts and unevaluated operands of a self call.
	struct SelfEncodeForm
	{
		std::string sigString;
		solidity::frontend::FunctionDefinition const* refFunc = nullptr;
		solidity::frontend::Expression const* targetIdentityExpr = nullptr;
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> resolvedArgs;
	};
	static SelfEncodeForm parseSelfEncodeForm(
		solidity::frontend::FunctionCall const& encCall,
		solidity::frontend::MemberAccess const* encMA);
	static solidity::frontend::FunctionDefinition const* resolveSelfCallOverload(
		ContractContext& _ctx,
		SelfEncodeForm const& form);
	static std::unique_ptr<InstanceBuilder> emitDirectSelfCall(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const& targetFunc,
		SelfEncodeForm const& form,
		std::string const& encodeName,
		bool staticCall,
		awst::SourceLocation const& _loc);

	/// `.call/.staticcall(data)` router (self-call rewrites, visible encoders, precompiles, self fallback, empty-data folds, raw data).
	static std::unique_ptr<InstanceBuilder> handleCallWithData(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::string const& _memberName,
		solidity::frontend::FunctionCall const& _callNode,
		std::shared_ptr<awst::Expression> _callValue,
		solidity::frontend::Expression const& _baseExpr,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleCallWithRawData(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _dataBytes,
		std::shared_ptr<awst::Expression> _callValue,
		awst::SourceLocation const& _loc);

	/// `t.call("")` with NO value: EVM still EXECUTES the callee (receive, or
	/// fallback when no receive exists). Zero-arg inner app call — the EVM
	/// entry router's NumAppArgs==0 arm is exactly that dispatch.
	static std::unique_ptr<InstanceBuilder> handleCallWithEmptyData(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		awst::SourceLocation const& _loc);

	/// .staticcall(data) for precompile addresses 0x01–0x0a; unsupported ones fail.
	static std::unique_ptr<InstanceBuilder> handleStaticCallPrecompile(
		ContractContext& _ctx,
		uint64_t _precompileAddr,
		std::shared_ptr<awst::Expression> _inputData,
		awst::SourceLocation const& _loc);

	/// .delegatecall(...) → explicit runtime failure when reached.
	static std::unique_ptr<InstanceBuilder> handleDelegatecall(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	// Helpers

	static std::shared_ptr<awst::Expression> makeBoolBytesTuple(
		bool _success,
		std::shared_ptr<awst::Expression> _data,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> makeBoolBytesTupleEmpty(
		awst::SourceLocation const& _loc);

public:
	/// Canonical ARC4 selector string from a FunctionDefinition
	/// (routers always dispatch on this; compatibility-mode selector expressions
	/// and the routing field of external function pointers also expose it).
	static std::string buildMethodSelector(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const* _func);

	/// Overload for public-state-var getters (no FunctionDefinition).
	static std::string buildMethodSelector(
		ContractContext& _ctx,
		std::string const& _name,
		solidity::frontend::FunctionType const& _funcType);

};

/// Name the emitted parameter wire plan when only a solc type is available.
std::string solTypeToArc4ParamName(ContractContext& _ctx, solidity::frontend::Type const* _type);

/// Name the emitted return wire plan (including signed-return promotion).
std::string solTypeToArc4ReturnName(ContractContext& _ctx, solidity::frontend::Type const* _type);

} // namespace puyasol::builder::eb
