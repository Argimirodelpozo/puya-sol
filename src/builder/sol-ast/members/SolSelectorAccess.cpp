/// @file SolSelectorAccess.cpp
/// Policy-selected function/error/event selector lowering.
/// Migrated from MemberAccessBuilder.cpp lines 139-360.

#include "builder/sol-ast/members/SolSelectorAccess.h"
#include "builder/SelectorSemantics.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/itxn/InnerCallHandlers.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolSelectorAccess::makeSelectorExpr(std::string const& _sig)
{
	return builder::SelectorSemantics::signatureSelector(m_ctx, _sig, m_loc);
}

// Compatibility mode exposes the ARC-4 canonical signature (ARC4 type names +
// return) so selector values match the router. --evm-selectors instead retains
// solc's external signature here; makeSelectorExpr hashes it with keccak.
std::string SolSelectorAccess::canonicalSelectorSig(FunctionType const& _ft)
{
	if (builder::SelectorSemantics::enabled(m_ctx.typeMapper))
	{
		try { return _ft.externalSignature(); }
		catch (...) { return {}; }
	}
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
		{
			if (builder::SelectorSemantics::enabled(m_ctx.typeMapper))
				if (auto const* ft = fd->functionType(false))
					return ft->externalSignature();
			return eb::InnerCallHandlers::buildMethodSelector(m_ctx, fd);
		}
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
			try { sig = funcType->externalSignature(); }
			catch (...) {}
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
		// External fn-pointer: the public selector is bytes 8..12 in both
		// layouts; --evm-selectors appends a separate ARC-4 routing field.
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

	// Event emission remains ARC-28. The observable Event.selector value is
	// sha512_256 in compatibility mode and Solidity's keccak topic under the flag.
	if (bytesWType && bytesWType->length().has_value() && *bytesWType->length() == 32)
		return builder::SelectorSemantics::eventSelector(
			m_ctx, sig, targetType, m_loc);

	// Prefer solc's externalIdentifier when the FunctionType survives; signature
	// hashing remains only for distributed/fallback selector shapes.
	auto selector = funcType
		? builder::SelectorSemantics::functionSelector(
			m_ctx, *funcType, sig, m_loc)
		: makeSelectorExpr(sig);
	if (targetType && targetType != awst::WType::bytesType())
		return awst::makeReinterpretCast(std::move(selector), targetType, m_loc);
	return selector;
}

} // namespace puyasol::builder::sol_ast
