#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <functional>
#include <memory>
#include <string>

namespace puyasol::builder::eb
{

/// Handles address.call/staticcall/delegatecall/transfer/send patterns.
class InnerCallHandlers
{
public:
	/// Fund a newly created inner app via payment to itxn CreatedApplicationID.
	/// Called after inner app creation when {value: X} is specified.
	static void fundCreatedApp(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// Try to handle an address member call.
	/// Returns nullptr if not handled (falls through to old code).
	static std::unique_ptr<InstanceBuilder> tryHandleAddressCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::string const& _memberName,
		solidity::frontend::FunctionCall const& _callNode,
		std::shared_ptr<awst::Expression> _callValue,
		solidity::frontend::Expression const& _baseExpr,
		awst::SourceLocation const& _loc);

private:
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

	/// .call{value: X}("") → payment
	static std::unique_ptr<InstanceBuilder> handleCallWithValue(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// .call(abi.encodeCall(fn, args)) → inner app call
	static std::unique_ptr<InstanceBuilder> handleCallWithEncodeCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		solidity::frontend::FunctionCall const& _encodeCallExpr,
		awst::SourceLocation const& _loc);

	/// .call(abi.encodeWithSignature/WithSelector(...)) → typed inner call
	/// (sha512_256 selector + per-arg ARC4 ApplicationArgs)
	static std::unique_ptr<InstanceBuilder> handleCallWithSignatureArgs(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		solidity::frontend::FunctionCall const& _encodeExpr,
		bool _isSignature,
		awst::SourceLocation const& _loc);

	/// Shared tail: submit the typed inner app call and produce the
	/// (true, AVM-framed returndata) tuple (LastLog minus ARC4 prefix).
	static std::unique_ptr<InstanceBuilder> submitTypedAppCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::TupleExpression> _argsTuple,
		awst::SourceLocation const& _loc);

	/// .call(rawBytes) → inner app call with raw bytes as ApplicationArgs[0]
	static std::unique_ptr<InstanceBuilder> handleCallWithRawData(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _dataBytes,
		awst::SourceLocation const& _loc);

	/// .staticcall(data) on precompile address
	static std::unique_ptr<InstanceBuilder> handleStaticCallPrecompile(
		ContractContext& _ctx,
		uint64_t _precompileAddr,
		std::shared_ptr<awst::Expression> _inputData,
		awst::SourceLocation const& _loc);

	/// .delegatecall(...) → stub
	static std::unique_ptr<InstanceBuilder> handleDelegatecall(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	// Helpers
	static std::shared_ptr<awst::Expression> buildPaymentTransaction(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> makeBoolBytesTuple(
		bool _success,
		std::shared_ptr<awst::Expression> _data,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> makeBoolBytesTupleEmpty(
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> addressToAppId(
		std::shared_ptr<awst::Expression> _receiver,
		awst::SourceLocation const& _loc);

	/// Convert an ARC4 argument to bytes for inner txn ApplicationArgs.
	static std::shared_ptr<awst::Expression> encodeArgToBytes(
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

public:
	/// Build the ARC4 method selector string from a FunctionDefinition —
	/// THE canonical selector form (ARC4 type names, return suffix, "void"
	/// for none): what routers dispatch on, fn-pointer slots store, and
	/// f.selector / type(I).interfaceId expose.
	static std::string buildMethodSelector(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const* _func);

	/// Overload from a name + FunctionType (public-state-var getters have
	/// no FunctionDefinition; their signature derives from the getter's
	/// FunctionType parameter/return lists).
	static std::string buildMethodSelector(
		ContractContext& _ctx,
		std::string const& _name,
		solidity::frontend::FunctionType const& _funcType);

	static std::shared_ptr<awst::IntrinsicCall> makeExtract(
		std::shared_ptr<awst::Expression> _source, int _offset, int _length,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::IntrinsicCall> makeConcat(
		std::shared_ptr<awst::Expression> _a, std::shared_ptr<awst::Expression> _b,
		awst::SourceLocation const& _loc);

};

} // namespace puyasol::builder::eb
