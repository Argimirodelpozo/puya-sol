/// @file SolSelectorAccess.cpp
/// f.selector, E.selector → keccak256("Name(type1,...)")[:4].
/// Migrated from MemberAccessBuilder.cpp lines 139-360.

#include "builder/sol-ast/members/SolSelectorAccess.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolSelectorAccess::makeSelectorExpr(std::string const& _sig)
{
	// ARC-4 sha512_256 selector (TEAL `method "sig"`); was keccak256(sig)[0:4].
	// type(I).interfaceId stays keccak (EVM XOR) so `f.selector ^ g.selector == interfaceId`
	// no longer holds, but ERC-165 comparisons of interfaceId constants are unaffected.
	return awst::makeMethodConstant(_sig, awst::WType::bytesType(), m_loc);
}

// Function selectors use the ARC-4 canonical sig (ARC4 type names + return, "void" if none),
// matching the router and fn-pointer slots (InnerCallHandlers::buildMethodSelector).
// Events/errors use their no-return form (ARC-28 emit prefix / custom-error revert payload).
std::string SolSelectorAccess::canonicalSelectorSig(FunctionType const& _ft)
{
	if (_ft.kind() == FunctionType::Kind::Error)
	{
		try { return _ft.externalSignature(); }
		catch (...) { return {}; }
	}
	if (_ft.hasDeclaration())
	{
		if (auto const* fd = dynamic_cast<FunctionDefinition const*>(&_ft.declaration()))
			return eb::InnerCallHandlers::buildMethodSelector(m_ctx, fd);
		// Public-state-var getter has no FunctionDefinition; derive from FunctionType.
		if (dynamic_cast<VariableDeclaration const*>(&_ft.declaration()))
			return eb::InnerCallHandlers::buildMethodSelector(
				m_ctx, _ft.declaration().name(), _ft);
	}
	try { return _ft.externalSignature(); }
	catch (...) { return {}; }
}

std::string SolSelectorAccess::resolveSignature(Expression const& _expr)
{
	auto const* ft = dynamic_cast<FunctionType const*>(_expr.annotation().type);
	if (ft)
	{
		auto sig = canonicalSelectorSig(*ft);
		if (!sig.empty()) return sig;
	}
	if (auto const* id = dynamic_cast<Identifier const*>(&_expr))
	{
		if (auto const* fd = dynamic_cast<FunctionDefinition const*>(
				id->annotation().referencedDeclaration))
			return eb::InnerCallHandlers::buildMethodSelector(m_ctx, fd);
	}
	if (auto const* ma = dynamic_cast<MemberAccess const*>(&_expr))
	{
		auto const* mft = dynamic_cast<FunctionType const*>(ma->annotation().type);
		if (mft)
		{
			auto sig = canonicalSelectorSig(*mft);
			if (!sig.empty()) return sig;
		}
	}
	return {};
}

std::shared_ptr<awst::Expression> SolSelectorAccess::toAwst()
{
	auto const& baseExpr = baseExpression();
	auto const* baseType = baseExpr.annotation().type;
	std::string sig;

	// Walk to innermost expression for side-effect evaluation.
	{
		Expression const* inner = &baseExpr;
		if (auto const* tuple = dynamic_cast<TupleExpression const*>(inner))
			if (tuple->components().size() == 1 && tuple->components()[0])
				inner = tuple->components()[0].get();
		while (auto const* ma = dynamic_cast<MemberAccess const*>(inner))
			inner = &ma->expression();

		// Ternary: (cond ? f : g).selector → conditional on branches
		if (auto const* cond = dynamic_cast<Conditional const*>(inner))
		{
			std::string trueSig = resolveSignature(cond->trueExpression());
			std::string falseSig = resolveSignature(cond->falseExpression());
			if (!trueSig.empty())
			{
				// Build the condition ONCE — the old code built it twice (a
				// discarded side-effect statement AND the conditional), so a
				// side-effecting `bump() ? this.f : this.g` ran twice.
				auto condition = buildExpr(cond->condition());

				if (trueSig == falseSig)
				{
					// Selector is branch-independent; run the condition only
					// for its side effects.
					m_ctx.preEffects().push_back(
						awst::makeExpressionStatement(std::move(condition), m_loc));
					return makeSelectorExpr(trueSig);
				}

				return awst::makeConditional(
					std::move(condition),
					makeSelectorExpr(trueSig),
					makeSelectorExpr(falseSig.empty() ? trueSig : falseSig),
					awst::WType::bytesType(), m_loc);
			}
		}
		// General case: evaluate inner (e.g. h() in h().f.selector) for side effects
		else if (!dynamic_cast<Identifier const*>(inner))
		{
			auto innerVal = buildExpr(*inner);
			if (innerVal && innerVal->wtype != awst::WType::voidType())
			{
				auto stmt = awst::makeExpressionStatement(std::move(innerVal), m_loc);
				m_ctx.preEffects().push_back(std::move(stmt));
			}
		}
	}

	FunctionType const* funcType = nullptr;
	if (auto const* ft = dynamic_cast<FunctionType const*>(baseType))
		funcType = ft;
	else if (auto const* typeType = dynamic_cast<TypeType const*>(baseType))
		funcType = dynamic_cast<FunctionType const*>(typeType->actualType());

	if (funcType)
	{
		if (funcType->kind() == FunctionType::Kind::Event)
		{
			auto const* eventDef = dynamic_cast<EventDefinition const*>(
				&funcType->declaration());
			if (eventDef)
			{
				sig = eventDef->name() + "(";
				bool first = true;
				for (auto const& param: eventDef->parameters())
				{
					if (!first) sig += ",";
					sig += param->type()->canonicalName();
					first = false;
				}
				sig += ")";
			}
		}
		else
		{
			try
			{
				sig = canonicalSelectorSig(*funcType);
				if (sig.empty())
					throw std::runtime_error("unresolved selector signature");
			}
			catch (...)
			{
				// Ternary distribution fallback: (c ? f : g).selector
				if (auto const* cond = dynamic_cast<Conditional const*>(&baseExpr))
				{
					std::string trueSig = resolveSignature(cond->trueExpression());
					std::string falseSig = resolveSignature(cond->falseExpression());

					if (!trueSig.empty() && !falseSig.empty())
					{
						auto condition = buildExpr(cond->condition());
						auto ternary = awst::makeConditional(
							std::move(condition),
							makeSelectorExpr(trueSig),
							makeSelectorExpr(falseSig),
							awst::WType::bytesType(), m_loc);

						return awst::makeAsBiguint(std::move(ternary), m_loc);
					}
				}
				// Fallback: resolve from identifier declaration
				if (auto const* ident = dynamic_cast<Identifier const*>(&baseExpr))
				{
					if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(
							ident->annotation().referencedDeclaration))
					{
						sig = funcDef->name() + "(";
						bool first = true;
						for (auto const& param: funcDef->parameters())
						{
							if (!first) sig += ",";
							sig += param->type()->canonicalName();
							first = false;
						}
						sig += ")";
					}
				}
				if (sig.empty())
					Logger::instance().warning("could not resolve function selector", m_loc);
			}
		}
	}

	if (sig.empty())
	{
		// External fn-pointer: 12-byte encoding = itob(appId,8) ++ selector(4).
		if (funcType && funcType->kind() == FunctionType::Kind::External)
		{
			auto base = buildExpr(baseExpr);
			if (base && base->wtype && base->wtype->kind() == awst::WTypeKind::Bytes)
				base = awst::makeAsBytes(std::move(base), m_loc);
			if (base && base->wtype == awst::WType::bytesType())
			{
				// selector is bytes 8..12
				auto extract = awst::makeExtract(std::move(base), 8, 4, m_loc);

				auto* targetType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
				return awst::makeReinterpretCast(
					std::move(extract),
					targetType ? targetType : awst::WType::bytesType(),
					m_loc);
			}
		}
		return nullptr;
	}

	Logger::instance().debug("selector: " + sig, m_loc);

	auto* targetType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
	auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(targetType);

	// Event selectors are bytes32 (EVM topic shape): full 32-byte sha512_256(sig).
	// First 4 bytes match the ARC-28 log prefix, so prefix comparisons stay coherent.
	if (bytesWType && bytesWType->length().has_value() && *bytesWType->length() == 32)
	{
		auto hash = awst::makeIntrinsicCall(
			"sha512_256", awst::WType::bytesType(), m_loc);
		hash->stackArgs.push_back(awst::makeUtf8BytesConstant(sig, m_loc));
		return awst::makeReinterpretCast(std::move(hash), targetType, m_loc);
	}

	// Function selectors: 4-byte ARC-4 sha512_256 (see makeSelectorExpr).
	auto selector = awst::makeMethodConstant(sig, awst::WType::bytesType(), m_loc);
	if (targetType && targetType != awst::WType::bytesType())
		return awst::makeReinterpretCast(std::move(selector), targetType, m_loc);
	return selector;
}

} // namespace puyasol::builder::sol_ast
