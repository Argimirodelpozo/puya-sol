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
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// Try to handle an address member call.
	/// Returns nullptr if not handled (falls through to old code).
	static std::unique_ptr<InstanceBuilder> tryHandleAddressCall(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::string const& _memberName,
		solidity::frontend::FunctionCall const& _callNode,
		std::shared_ptr<awst::Expression> _callValue,
		solidity::frontend::Expression const& _baseExpr,
		awst::SourceLocation const& _loc);

private:
	/// .transfer(amount)
	static std::unique_ptr<InstanceBuilder> handleTransfer(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// .send(amount)
	static std::unique_ptr<InstanceBuilder> handleSend(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// .call{value: X}("") → payment
	static std::unique_ptr<InstanceBuilder> handleCallWithValue(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// .call(abi.encodeCall(fn, args)) → inner app call
	static std::unique_ptr<InstanceBuilder> handleCallWithEncodeCall(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		solidity::frontend::FunctionCall const& _encodeCallExpr,
		awst::SourceLocation const& _loc);

	/// .call(rawBytes) → inner app call with raw bytes as ApplicationArgs[0]
	static std::unique_ptr<InstanceBuilder> handleCallWithRawData(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _dataBytes,
		awst::SourceLocation const& _loc);

	/// .staticcall(data) on precompile address
	static std::unique_ptr<InstanceBuilder> handleStaticCallPrecompile(
		BuilderContext& _ctx,
		uint64_t _precompileAddr,
		std::shared_ptr<awst::Expression> _inputData,
		awst::SourceLocation const& _loc);

	/// .delegatecall(...) → stub
	static std::unique_ptr<InstanceBuilder> handleDelegatecall(
		BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	// Helpers
	static std::shared_ptr<awst::Expression> buildPaymentTransaction(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> makeUint64(
		std::string _value, awst::SourceLocation const& _loc);

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

	/// Build the ARC4 method selector string from a FunctionDefinition.
	static std::string buildMethodSelector(
		BuilderContext& _ctx,
		solidity::frontend::FunctionDefinition const* _func);

	static std::shared_ptr<awst::IntrinsicCall> makeExtract(
		std::shared_ptr<awst::Expression> _source, int _offset, int _length,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::IntrinsicCall> makeConcat(
		std::shared_ptr<awst::Expression> _a, std::shared_ptr<awst::Expression> _b,
		awst::SourceLocation const& _loc);

	/// Left-pad bytes to N using bzero+concat+extract3.
	static std::shared_ptr<awst::Expression> leftPadToN(
		std::shared_ptr<awst::Expression> _expr, int _n,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
