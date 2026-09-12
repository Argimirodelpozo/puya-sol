#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/abi/AbiSelectorCalldataBuilder.h"
#include "builder/abi/EvmAbiDecode.h"
#include "builder/abi/EvmAbiEncode.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-ast/CallOperands.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <stdexcept>

namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeValuesAsEvmAbi(
	ContractContext& ctx,
	std::vector<solidity::frontend::Type const*> const& types,
	std::vector<std::shared_ptr<awst::Expression>> values,
	awst::SourceLocation const& loc)
{
	return abi::encodeEvmAbi(ctx.typeMapper, types, std::move(values), loc, ctx.preEffects());
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::handleDecode(
	ContractContext& ctx, solidity::frontend::FunctionCall const& call,
	awst::SourceLocation const& loc)
{
	auto const* target = ctx.typeMapper.map(call.annotation().type);
	if (!target || call.arguments().size() != 2)
		throw std::logic_error("Invalid solc ABI decode annotation");
	// Capture the payload and its effects ONCE, before any bounds/leaf reads.
	auto data = sol_ast::CallOperands::evaluate(ctx, *call.arguments()[0], loc);
	if (data->wtype != awst::WType::bytesType())
		data = awst::makeAsBytes(std::move(data), loc);
	std::vector<solidity::frontend::Type const*> types;
	if (auto const* tuple = dynamic_cast<solidity::frontend::TupleType const*>(call.annotation().type))
		types = tuple->components();
	else types.push_back(call.annotation().type);
	return abi::decodeEvmAbi(ctx.typeMapper, std::move(data), types, target, loc, ctx.preEffects());
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::decodeArc4(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	solidity::frontend::Expression const& _dataNode,
	awst::SourceLocation const& _loc)
{
	auto const* targetType = _ctx.typeMapper.map(_callNode.annotation().type);
	if (!targetType)
		throw std::logic_error("Invalid solc ARC4 decode annotation");
	auto data = sol_ast::CallOperands::evaluate(_ctx, _dataNode, _loc);
	if (data->wtype != awst::WType::bytesType())
		data = awst::makeAsBytes(std::move(data), _loc);

	auto const* tupleType = dynamic_cast<solidity::frontend::TupleType const*>(
		_callNode.annotation().type);
	awst::WType const* wireType = nullptr;
	if (tupleType)
	{
		std::vector<awst::WType const*> components;
		for (auto const* component: tupleType->components())
		{
			auto const* wireComponent =
				_ctx.typeMapper.mapSolTypeToARC4(component);
			if (!wireComponent)
			{
				Logger::instance().error(
					"type is not representable in ARC4 decoding", _loc);
				return awst::makeBytesConstant({}, _loc);
			}
			components.push_back(wireComponent);
		}
		wireType = _ctx.typeMapper.createType<awst::ARC4Tuple>(
			std::move(components));
	}
	else
		wireType = _ctx.typeMapper.mapSolTypeToARC4(
			_callNode.annotation().type);
	if (!wireType)
	{
		Logger::instance().error(
			"type is not representable in ARC4 decoding", _loc);
		return awst::makeBytesConstant({}, _loc);
	}
	auto wire = awst::makeARC4FromBytes(
		std::move(data), wireType, _loc, /*validate=*/true);
	if (awst::structurallyEquivalent(wireType, targetType))
		return wire;
	return awst::makeARC4Decode(std::move(wire), targetType, _loc);
}

namespace
{

std::shared_ptr<awst::Expression> arc4EncodeAtWireTypes(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>> _values,
	std::vector<awst::WType const*> _wireTypes,
	awst::SourceLocation const& _loc)
{
	if (_values.size() != _wireTypes.size())
		throw std::logic_error("ARC4 encoding value/type arity mismatch");
	for (size_t i = 0; i < _values.size(); ++i)
		if (!_values[i] || !_wireTypes[i])
			throw std::logic_error("Missing ARC4 encoding value/type");
	if (_values.empty())
		return awst::makeBytesConstant({}, _loc);

	if (_values.size() == 1)
	{
		auto value = std::move(_values.front());
		if (awst::structurallyEquivalent(value->wtype, _wireTypes.front()))
			return awst::makeAsBytes(std::move(value), _loc);
		return awst::makeAsBytes(
			awst::makeARC4Encode(std::move(value), _wireTypes.front(), _loc), _loc);
	}

	// Puya's tuple codec sees only UIntEncoding(N), not the signed alias. A
	// negative intN is held as a wider two's-complement native integer and would
	// therefore look like an overflow when encoded recursively. Pre-encode signed
	// members so makeARC4Encode can trim them to their declared wire width.
	for (size_t i = 0; i < _values.size(); ++i)
		if (auto const* integer =
				dynamic_cast<awst::ARC4UIntN const*>(_wireTypes[i]);
			integer && integer->isSigned()
			&& !awst::structurallyEquivalent(_values[i]->wtype, _wireTypes[i]))
			_values[i] = awst::makeARC4Encode(
				std::move(_values[i]), _wireTypes[i], _loc);

	std::vector<awst::WType const*> nativeTypes;
	nativeTypes.reserve(_values.size());
	for (auto const& value: _values)
		nativeTypes.push_back(value->wtype);
	auto const* nativeTuple = _ctx.typeMapper.createType<awst::WTuple>(
		std::move(nativeTypes));
	auto tuple = awst::makeTupleExpression(nativeTuple, _loc);
	tuple->items = std::move(_values);
	auto const* wireTuple = _ctx.typeMapper.createType<awst::ARC4Tuple>(
		std::move(_wireTypes));
	return awst::makeAsBytes(
		awst::makeARC4Encode(std::move(tuple), wireTuple, _loc), _loc);
}

} // namespace

std::shared_ptr<awst::Expression> AbiEncoderBuilder::arc4EncodeSolidityArgs(
	ContractContext& _ctx,
	std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const& _args,
	awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Expression>> values;
	std::vector<awst::WType const*> wireTypes;
	values.reserve(_args.size());
	wireTypes.reserve(_args.size());
	for (auto const& argument: _args)
	{
		auto const* sourceType = argument->annotation().type;
		auto const* concreteType = sourceType;
		if (concreteType)
			if (auto const* mobile = concreteType->mobileType())
				concreteType = mobile;
		auto const* nativeType = _ctx.typeMapper.map(concreteType);
		auto const* wireType = _ctx.typeMapper.mapSolTypeToARC4(concreteType);
		if (!nativeType || !wireType)
		{
			Logger::instance().error(
				"type is not representable in ARC4 encoding", _loc);
			return awst::makeBytesConstant({}, _loc);
		}

		auto value = sol_ast::CallOperands::evaluate(_ctx, *argument, _loc);
		if (value->wtype != nativeType)
			value = builder::ConversionPlan{
				sourceType,
				concreteType,
				nativeType,
				builder::ConversionPlan::Context::AbiArgument}.emit(
					std::move(value), _loc);
		values.push_back(std::move(value));
		wireTypes.push_back(wireType);
	}

	return arc4EncodeAtWireTypes(
		_ctx, std::move(values), std::move(wireTypes), _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::arc4EncodeValues(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>> _vals,
	awst::SourceLocation const& _loc)
{
	std::vector<awst::WType const*> arc4Types;
	arc4Types.reserve(_vals.size());
	for (auto const& val : _vals)
		arc4Types.push_back(_ctx.typeMapper.mapToARC4Type(val->wtype));
	return arc4EncodeAtWireTypes(
		_ctx, std::move(_vals), std::move(arc4Types), _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeArgsAsEvmAbi(
	ContractContext& _ctx,
	std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const& _args,
	size_t _first,
	awst::SourceLocation const& _loc, bool _packed)
{
	std::vector<solidity::frontend::Type const*> types;
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = _first; i < _args.size(); ++i)
	{
		auto const* sourceType = _args[i]->annotation().type;
		auto const* type = sourceType;
		// Solc leaves literals as rational/string-literal pseudo-types. Its
		// mobile type is the concrete ABI type Solidity assigns at this call.
		if (type)
			if (auto const* mobile = type->mobileType())
				type = mobile;
		types.push_back(type);
		auto value = sol_ast::CallOperands::evaluate(_ctx, *_args[i], _loc);
		// solc's abiEncodingFunctionStringLiteral encodes raw literal bytes,
		// including non-UTF8 hex literals. Its mobile string type selects the
		// wire layout, not an ordinary (UTF8-checked) implicit string conversion.
		if (type)
			if (auto const* target = _ctx.typeMapper.map(type);
				target && value->wtype != target)
				value = dynamic_cast<solidity::frontend::StringLiteralType const*>(sourceType)
					? awst::makeReinterpretCast(std::move(value), target, _loc)
					: builder::ConversionPlan{
					sourceType, type, target,
					builder::ConversionPlan::Context::AbiArgument}.emit(
						std::move(value), _loc);
		values.push_back(std::move(value));
	}
	return _packed
		? abi::encodePackedEvmAbi(_ctx.typeMapper, types, std::move(values), _loc, _ctx.preEffects())
		: encodeValuesAsEvmAbi(_ctx, types, std::move(values), _loc);
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::build(
	ContractContext& ctx, solidity::frontend::FunctionCall const& call,
	awst::SourceLocation const& loc)
{
	using Kind = solidity::frontend::FunctionType::Kind;
	auto const* type = dynamic_cast<solidity::frontend::FunctionType const*>(call.expression().annotation().type);
	if (!type) throw std::logic_error("ABI call has no solc function type");
	switch (type->kind())
	{
	case Kind::ABIEncode: return encodeArgsAsEvmAbi(ctx, call.arguments(), 0, loc);
	case Kind::ABIEncodePacked: return encodeArgsAsEvmAbi(ctx, call.arguments(), 0, loc, true);
	case Kind::ABIEncodeCall: return handleEncodeCall(ctx, call, loc);
	case Kind::ABIEncodeWithSelector: return handleEncodeWithSelector(ctx, call, loc);
	case Kind::ABIEncodeWithSignature: return handleEncodeWithSignature(ctx, call, loc);
	case Kind::ABIDecode: return handleDecode(ctx, call, loc);
	default: throw std::logic_error("Unexpected solc ABI builtin kind");
	}
}

} // namespace puyasol::builder::eb
