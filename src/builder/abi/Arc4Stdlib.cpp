/// @file Arc4Stdlib.cpp
/// ARC4 codec facade from libs/AVM.sol, implemented without solc extensions.

#include "builder/abi/Arc4Stdlib.h"

#include "builder/abi/AbiEncoderBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTUtils.h>
#include <libsolidity/ast/Types.h>

#include <string>
#include <string_view>

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

namespace
{

/// Match the resolved declaration, not the spelling at the call site. This
/// supports named imports, renamed imports, and module aliases while avoiding
/// interception of an unrelated user library that happens to be named ARC4.
FunctionDefinition const* resolvedArc4Function(
	MemberAccess const& _memberAccess,
	std::string_view _name)
{
	auto const* function = dynamic_cast<FunctionDefinition const*>(
		_memberAccess.annotation().referencedDeclaration);
	if (!function || function->name() != _name
		|| !Arc4Stdlib::isFacadeFunction(*function))
		return nullptr;
	return function;
}

FunctionCall const* asAbiEncodeEnvelope(Expression const& _expression)
{
	auto const* expression = resolveOuterUnaryTuples(&_expression);
	auto const* call = dynamic_cast<FunctionCall const*>(expression);
	if (!call)
		return nullptr;
	auto const* functionType = dynamic_cast<FunctionType const*>(
		call->expression().annotation().type);
	if (!functionType || functionType->kind() != FunctionType::Kind::ABIEncode)
		return nullptr;
	return call;
}

FunctionCall const* asArc4DecodeEnvelope(Expression const& _expression)
{
	auto const* expression = resolveOuterUnaryTuples(&_expression);
	auto const* call = dynamic_cast<FunctionCall const*>(expression);
	if (!call)
		return nullptr;
	auto const* memberAccess = dynamic_cast<MemberAccess const*>(&call->expression());
	if (!memberAccess || !resolvedArc4Function(*memberAccess, "decode"))
		return nullptr;
	return call;
}

std::shared_ptr<awst::Expression> invalidEnvelope(
	std::string _message,
	awst::SourceLocation const& _loc)
{
	Logger::instance().error(std::move(_message), _loc);
	return awst::makeBytesConstant({}, _loc);
}

} // namespace

bool Arc4Stdlib::isFacadeFunction(FunctionDefinition const& _function)
{
	auto const* owner = _function.annotation().contract;
	if (!owner || !owner->isLibrary() || owner->name() != "ARC4"
		|| _function.sourceUnitName() != "libs/AVM.sol"
		|| (_function.name() != "encode" && _function.name() != "decode")
		|| _function.visibility() != Visibility::Internal
		|| _function.stateMutability() != StateMutability::Pure
		|| _function.parameters().size() != 1
		|| _function.returnParameters().size() != 1)
		return false;
	auto isBytes = [](VariableDeclaration const& _parameter) {
		auto const* array = dynamic_cast<ArrayType const*>(_parameter.type());
		return array && array->isByteArray();
	};
	return isBytes(*_function.parameters().front())
		&& isBytes(*_function.returnParameters().front());
}

std::optional<std::shared_ptr<awst::Expression>> Arc4Stdlib::tryHandleCall(
	ContractContext& _ctx,
	MemberAccess const& _memberAccess,
	FunctionCall const& _call,
	awst::SourceLocation const& _loc)
{
	if (resolvedArc4Function(_memberAccess, "encode"))
	{
		if (_call.arguments().size() != 1)
			return invalidEnvelope(
				"ARC4.encode expects exactly one abi.encode(...) type envelope", _loc);
		auto const* envelope = asAbiEncodeEnvelope(*_call.arguments().front());
		if (!envelope)
			return invalidEnvelope(
				"ARC4.encode must be written as ARC4.encode(abi.encode(...))", _loc);
		return AbiEncoderBuilder::arc4EncodeSolidityArgs(
			_ctx, envelope->arguments(), _loc);
	}

	if (resolvedArc4Function(_memberAccess, "decode"))
		return invalidEnvelope(
			"ARC4.decode must be used as the input to abi.decode, for example "
			"abi.decode(ARC4.decode(data), (uint64, address))",
			_loc);

	return std::nullopt;
}

std::optional<std::shared_ptr<awst::Expression>> Arc4Stdlib::tryHandleDecodeEnvelope(
	ContractContext& _ctx,
	FunctionCall const& _abiDecodeCall,
	awst::SourceLocation const& _loc)
{
	if (_abiDecodeCall.arguments().empty())
		return std::nullopt;
	auto const* envelope = asArc4DecodeEnvelope(
		*_abiDecodeCall.arguments().front());
	if (!envelope)
		return std::nullopt;
	if (envelope->arguments().size() != 1)
		return invalidEnvelope(
			"ARC4.decode expects exactly one bytes argument", _loc);
	return AbiEncoderBuilder::decodeArc4(
		_ctx, _abiDecodeCall, *envelope->arguments().front(), _loc);
}

} // namespace puyasol::builder::eb
