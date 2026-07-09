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
	/// Pay `amount` to the address of the just-created inner app (for `{value: X}`).
	static void fundCreatedApp(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

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

	/// .call{value: X}() → payment
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

	/// .call(abi.encodeWithSignature/WithSelector(...)) → typed inner call.
	static std::unique_ptr<InstanceBuilder> handleCallWithSignatureArgs(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		solidity::frontend::FunctionCall const& _encodeExpr,
		bool _isSignature,
		awst::SourceLocation const& _loc);

	/// Submit typed inner app call; returns (true, LastLog[4:]) tuple.
	static std::unique_ptr<InstanceBuilder> submitTypedAppCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::TupleExpression> _argsTuple,
		awst::SourceLocation const& _loc);

	/// .call(rawBytes) → inner app call; splits [selector, rest] as ApplicationArgs.
	static std::unique_ptr<InstanceBuilder> handleCallWithRawData(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _dataBytes,
		awst::SourceLocation const& _loc);

	/// .staticcall(data) for precompile addresses 0x01–0x09.
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

public:
	/// Encode one call argument to its ApplicationArgs bytes. THE single ARC4
	/// arg encoder for BOTH the typed `c.f(...)` path (SolExternalCall) and the
	/// `.call(abi.encodeCall/encodeWith*)` inner-call shapes. `_paramSolType`
	/// is the callee's DECLARED param type when known (drives exact biguint
	/// width, uint64 pad-to-width, dynamic-bytes length header); nullptr for
	/// type-less shapes (encodeWithSelector/Signature), which fall back to
	/// backing-width encoding. The two paths used to carry separate copies that
	/// DRIFTED (inner: biguint always 32B, bare itob, no array/struct encode) —
	/// a latent revert class on inner calls with sub-256 uintN/array args.
	static std::shared_ptr<awst::Expression> encodeArgToBytes(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _arg,
		solidity::frontend::Type const* _paramSolType,
		awst::SourceLocation const& _loc);

	/// Canonical ARC4 selector string from a FunctionDefinition
	/// (routers dispatch on this; fn-pointer slots and f.selector expose it).
	static std::string buildMethodSelector(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const* _func);

	/// Overload for public-state-var getters (no FunctionDefinition).
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

/// Canonical ARC4 ABI type name for a NESTED position (struct field / array element): exact bit width,
/// signedness PRESERVED (nested int8 = "int8", not "uint8"), recursing structs/arrays. Verified against
/// puya's emitted `method "..."` signatures — reuse anywhere a cross-contract selector must match the
/// callee's published ABI (e.g. SolExternalCall typed calls + the .call() path).
std::string nestedArc4Name(ContractContext& _ctx, solidity::frontend::Type const* _type);

/// Canonical ARC4 ABI type name for a TOP-LEVEL param position (selector computation): scalars
/// collapse to "uint64"/"uintN" (signedness dropped, matching what puya registers), enums →
/// "uint64", aggregates recurse via nestedArc4Name, exotics (fn pointers, contracts) fall back to
/// the ARC4-type mapping the callee publishes. THE single param namer — SolExternalCall's typed
/// `c.f(...)` path and the `.call(abi.encodeCall(...))` inner-call path both use it; keeping two
/// copies in lockstep by hand is where the enum uint8-vs-uint64 selector bug came from.
std::string solTypeToArc4ParamName(ContractContext& _ctx, solidity::frontend::Type const* _type);

/// Return-position variant: SIGNED integer returns are named "uint256" (full two's complement,
/// see intSelectorReturnName); everything else as solTypeToArc4ParamName.
std::string solTypeToArc4ReturnName(ContractContext& _ctx, solidity::frontend::Type const* _type);

} // namespace puyasol::builder::eb
