#include "builder/SelectorSemantics.h"

#include "builder/SolcFacts.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <set>

namespace puyasol::builder
{

using namespace solidity::frontend;

bool SelectorSemantics::enabled(TypeMapper const& _typeMapper)
{
	return _typeMapper.profile().evmSelectors;
}

std::shared_ptr<awst::Expression> SelectorSemantics::functionSelector(
	eb::ContractContext& _ctx,
	FunctionType const& _function,
	std::string const& _arc4Signature,
	awst::SourceLocation const& _loc)
{
	if (!enabled(_ctx.typeMapper))
		return awst::makeMethodConstant(
			_arc4Signature, awst::WType::bytesType(), _loc);
	return awst::makeBytesConstant(
		SolcFacts::externalSelector(_function), _loc,
		awst::BytesEncoding::Base16, awst::WType::bytesType());
}

std::shared_ptr<awst::Expression> SelectorSemantics::signatureSelector(
	eb::ContractContext& _ctx,
	std::string const& _signature,
	awst::SourceLocation const& _loc)
{
	if (!enabled(_ctx.typeMapper))
		return awst::makeMethodConstant(
			_signature, awst::WType::bytesType(), _loc);
	return awst::makeBytesConstant(
		SolcFacts::externalSelector(_signature), _loc,
		awst::BytesEncoding::Base16, awst::WType::bytesType());
}

std::shared_ptr<awst::Expression> SelectorSemantics::eventSelector(
	eb::ContractContext& _ctx,
	std::string const& _signature,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	if (enabled(_ctx.typeMapper))
		return awst::makeBytesConstant(
			SolcFacts::signatureHash(_signature), _loc,
			awst::BytesEncoding::Base16, _targetType);
	auto hash = awst::makeIntrinsicCall(
		"sha512_256", awst::WType::bytesType(), _loc);
	hash->stackArgs.push_back(awst::makeUtf8BytesConstant(_signature, _loc));
	return awst::makeReinterpretCast(std::move(hash), _targetType, _loc);
}

std::string SelectorSemantics::eventSignature(
	eb::ContractContext& _ctx, EventDefinition const& _event)
{
	auto arc4Name = [&](Type const* type) -> std::string {
		auto const* w = _ctx.typeMapper.map(type);
		if (w == awst::WType::biguintType()) return "uint256";
		if (w == awst::WType::uint64Type()) return "uint64";
		if (w == awst::WType::boolType()) return "bool";
		if (w == awst::WType::accountType()) return "address";
		if (w == awst::WType::bytesType()) return "byte[]";
		if (w == awst::WType::stringType()) return "string";
		if (auto const* bw = dynamic_cast<awst::BytesWType const*>(w))
			return bw->length().has_value()
				? "byte[" + std::to_string(*bw->length()) + "]" : "byte[]";
		return type ? type->toString(true) : std::string{};
	};
	std::string result = _event.name() + "(";
	for (size_t i = 0; i < _event.parameters().size(); ++i)
	{
		if (i) result += ",";
		result += arc4Name(_event.parameters()[i]->type());
	}
	return result + ")";
}

std::vector<SelectorRoute> SelectorSemantics::routes(eb::ContractContext& _ctx)
{
	std::vector<SelectorRoute> result;
	if (!enabled(_ctx.typeMapper))
		return result;

	std::set<std::string> seen;
	std::vector<ContractDefinition const*> contracts;
	if (_ctx.currentContract)
		contracts.push_back(_ctx.currentContract);
	else
		contracts = _ctx.selectorContracts;
	for (auto const* contract: contracts)
		for (auto const& [_, function]: contract->interfaceFunctionList(true))
		{
			if (!function) continue;
			std::string route;
			if (function->hasDeclaration())
			{
				auto const& declaration = function->declaration();
				if (auto const* fd = dynamic_cast<FunctionDefinition const*>(&declaration))
					route = eb::InnerCallHandlers::buildMethodSelector(_ctx, fd);
				else if (auto const* vd = dynamic_cast<VariableDeclaration const*>(&declaration))
					route = eb::InnerCallHandlers::buildMethodSelector(
						_ctx, vd->name(), *function);
			}
			if (route.empty() || !seen.insert(route).second) continue;
			result.push_back({
				std::move(route), SolcFacts::externalSelector(*function)});
		}
	return result;
}

std::shared_ptr<awst::Expression> SelectorSemantics::runtimeSelector(
	eb::ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _routingSelector,
	awst::SourceLocation const& _loc)
{
	if (!enabled(_ctx.typeMapper))
		return _routingSelector;
	return translateRuntimeSelector(
		std::move(_routingSelector), routes(_ctx), _loc);
}

std::shared_ptr<awst::Expression> SelectorSemantics::translateRuntimeSelector(
	std::shared_ptr<awst::Expression> _routingSelector,
	std::vector<SelectorRoute> const& _routes,
	awst::SourceLocation const& _loc)
{
	if (_routes.empty())
		return _routingSelector;
	if (_routingSelector->wtype != awst::WType::bytesType())
		_routingSelector = awst::makeAsBytes(std::move(_routingSelector), _loc);
	auto route = awst::makeEvalOnce(std::move(_routingSelector), _loc);
	std::shared_ptr<awst::Expression> result = route;
	for (auto it = _routes.rbegin(); it != _routes.rend(); ++it)
	{
		auto matches = awst::makeBytesComparison(
			route, awst::EqualityComparison::Eq,
			awst::makeMethodConstant(
				it->arc4Signature, awst::WType::bytesType(), _loc),
			_loc);
		auto semantic = awst::makeBytesConstant(
			it->soliditySelector, _loc, awst::BytesEncoding::Base16,
			awst::WType::bytesType());
		result = awst::makeConditional(
			std::move(matches), std::move(semantic), std::move(result),
			awst::WType::bytesType(), _loc);
	}
	return result;
}

} // namespace puyasol::builder
