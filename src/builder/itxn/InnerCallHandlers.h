#pragma once

#include "builder/sol-eb/NodeBuilder.h"

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

	/// THE payment choke point: PaymentTxn create-fields for `{value:}` legs
	/// and transfer/send. Maps EVM-profile receivers (xchain A(E) routing,
	/// contract-value → escrow). Public: SolExternalCall's typed
	/// `{value:}` leg builds through it too.
	static std::shared_ptr<awst::Expression> buildPaymentTransaction(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
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

	/// .call{value: X}("") (empty/absent data) → bare payment
	static std::unique_ptr<InstanceBuilder> handleCallWithValue(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::Expression> _amount,
		awst::SourceLocation const& _loc);

	/// .call{value:V}(abi.encodeCall(fn, args)) → inner app call.
	/// Non-null `_callValue` prepends a PaymentTxn in the SAME inner group
	/// (msg.value = preceding payment's Amount on the callee side).
	static std::unique_ptr<InstanceBuilder> handleCallWithEncodeCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		solidity::frontend::FunctionCall const& _encodeCallExpr,
		std::shared_ptr<awst::Expression> _callValue,
		awst::SourceLocation const& _loc);

	/// .call{value:V}(abi.encodeWithSignature/WithSelector(...)) → typed inner call.
	static std::unique_ptr<InstanceBuilder> handleCallWithSignatureArgs(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		solidity::frontend::FunctionCall const& _encodeExpr,
		bool _isSignature,
		std::shared_ptr<awst::Expression> _callValue,
		awst::SourceLocation const& _loc);

	/// Submit typed inner app call; returns (true, LastLog[4:]) tuple.
	/// Non-null `_callValue` → [PaymentTxn, ApplicationCall] group submit.
	static std::unique_ptr<InstanceBuilder> submitTypedAppCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _receiver,
		std::shared_ptr<awst::TupleExpression> _argsTuple,
		std::shared_ptr<awst::Expression> _callValue,
		awst::SourceLocation const& _loc);

	/// .call{value:V}(rawBytes) → inner app call; splits [selector, rest] as ApplicationArgs.
	/// The three abi.encode* self-call forms, normalised: fnName + optional full signature string (encodeWithSignature), optional …
	struct SelfEncodeForm
	{
		std::string fnName;
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

	/// .staticcall(data) for precompile addresses 0x01–0x09.
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

	static std::shared_ptr<awst::Expression> addressToAppId(
		std::shared_ptr<awst::Expression> _receiver,
		awst::SourceLocation const& _loc);

public:
	/// Encode one call argument to its ApplicationArgs bytes. THE single ARC4
	/// arg encoder for BOTH the typed `c.f(...)` path (SolExternalCall) and the
	/// `.call(abi.encodeCall/encodeWith*)` inner-call shapes. When both are
	/// known, `_sourceSolType` and `_paramSolType` select the Solidity implicit
	/// conversion before transport encoding. `_paramSolType` then drives exact
	/// biguint width, uint64 pad-to-width, and the dynamic-bytes length header;
	/// it is nullptr for
	/// type-less shapes (encodeWithSelector/Signature), which fall back to
	/// backing-width encoding. The two paths used to carry separate copies that
	/// DRIFTED (inner: biguint always 32B, bare itob, no array/struct encode) —
	/// a latent revert class on inner calls with sub-256 uintN/array args.
	static std::shared_ptr<awst::Expression> encodeArgToBytes(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _arg,
		solidity::frontend::Type const* _sourceSolType,
		solidity::frontend::Type const* _paramSolType,
		awst::SourceLocation const& _loc);

	/// Build the EVM contract-profile transport once for every outgoing call
	/// shape: ApplicationArgs[0] is the 4-byte Solidity selector and [1] is one
	/// canonical ABI body. Argument conversions are driven by the declared
	/// parameter types when available and aggregate layout recurses in the
	/// shared EVM encoder.
	static std::shared_ptr<awst::TupleExpression> buildEvmApplicationArgs(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _selector,
		std::vector<solidity::frontend::ASTPointer<
			solidity::frontend::Expression const>> const& _args,
		std::vector<solidity::frontend::Type const*> const& _paramTypes,
		awst::SourceLocation const& _loc);

	/// Canonical argument body shared by EVM-profile method calls and
	/// constructor creation (which has no selector).
	static std::shared_ptr<awst::Expression> encodeEvmArgumentBody(
		ContractContext& _ctx,
		std::vector<solidity::frontend::ASTPointer<
			solidity::frontend::Expression const>> const& _args,
		std::vector<solidity::frontend::Type const*> const& _paramTypes,
		awst::SourceLocation const& _loc);

	/// Submit-then-CAPTURE: push `__itxn_log_N = itxn LastLog` into pre-effects
	/// right after a submit and return the temp var. Result reads MUST go through
	/// this, never a live `itxn LastLog` — the itxn context is a single register,
	/// so several inner calls built inside ONE statement (a tuple of calls,
	/// nested call args) all flush their submits first and live reads would all
	/// see the LAST call's log. (Same capture discipline as CreatedApplicationID
	/// in SolNewExpression.)
	static std::shared_ptr<awst::Expression> captureLastLog(
		ContractContext& _ctx, awst::SourceLocation const& _loc);

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
