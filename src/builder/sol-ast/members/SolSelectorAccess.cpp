/// @file SolSelectorAccess.cpp
/// f.selector, E.selector → keccak256("Name(type1,...)")[:4].
/// Migrated from MemberAccessBuilder.cpp lines 139-360.

#include "builder/sol-ast/members/SolSelectorAccess.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolSelectorAccess::makeSelectorExpr(std::string const& _sig)
{
	// sha512_256 ARC-4 selector (TEAL `method "sig"`), per the project-wide
	// convention (routers, encodeCall, events, custom errors, external
	// fn-pointers). Was keccak256(sig)[0:4] — a selector nothing on the
	// AVM dispatches on. NOTE: type(I).interfaceId stays solc-side keccak
	// (XOR of EVM selectors), so `f.selector ^ g.selector == interfaceId`
	// no longer holds; ERC-165 code comparing interfaceId constants is
	// unaffected (both sides come from solc).
	return awst::makeMethodConstant(_sig, awst::WType::bytesType(), m_loc);
}

std::string SolSelectorAccess::resolveSignature(Expression const& _expr)
{
	auto const* ft = dynamic_cast<FunctionType const*>(_expr.annotation().type);
	if (ft)
	{
		try { return ft->externalSignature(); }
		catch (...) {}
	}
	if (auto const* id = dynamic_cast<Identifier const*>(&_expr))
	{
		if (auto const* fd = dynamic_cast<FunctionDefinition const*>(
				id->annotation().referencedDeclaration))
		{
			std::string s = fd->name() + "(";
			bool first = true;
			for (auto const& p: fd->parameters())
			{
				if (!first) s += ",";
				s += p->type()->canonicalName();
				first = false;
			}
			return s + ")";
		}
	}
	if (auto const* ma = dynamic_cast<MemberAccess const*>(&_expr))
	{
		auto const* mft = dynamic_cast<FunctionType const*>(ma->annotation().type);
		if (mft)
		{
			try { return mft->externalSignature(); }
			catch (...) {}
		}
	}
	return {};
}

std::shared_ptr<awst::Expression> SolSelectorAccess::toAwst()
{
	auto const& baseExpr = baseExpression();
	auto const* baseType = baseExpr.annotation().type;
	std::string sig;

	// Evaluate base expression for side effects before computing selector.
	// Walk through MemberAccess chain to find the innermost expression.
	{
		Expression const* inner = &baseExpr;
		if (auto const* tuple = dynamic_cast<TupleExpression const*>(inner))
			if (tuple->components().size() == 1 && tuple->components()[0])
				inner = tuple->components()[0].get();
		while (auto const* ma = dynamic_cast<MemberAccess const*>(inner))
			inner = &ma->expression();

		// Ternary: (cond ? f : g).selector — return different selectors per branch
		if (auto const* cond = dynamic_cast<Conditional const*>(inner))
		{
			std::string trueSig = resolveSignature(cond->trueExpression());
			std::string falseSig = resolveSignature(cond->falseExpression());
			if (!trueSig.empty())
			{
				auto condition = buildExpr(cond->condition());
				auto condStmt = awst::makeExpressionStatement(condition, m_loc);
				m_ctx.prePendingStatements.push_back(std::move(condStmt));

				if (trueSig == falseSig)
					return makeSelectorExpr(trueSig);

				auto ternCond = buildExpr(cond->condition());
				return awst::makeConditional(
					std::move(ternCond),
					makeSelectorExpr(trueSig),
					makeSelectorExpr(falseSig.empty() ? trueSig : falseSig),
					awst::WType::bytesType(), m_loc);
			}
		}
		// General: h().f.selector — evaluate h() for side effects
		else if (!dynamic_cast<Identifier const*>(inner))
		{
			auto innerVal = buildExpr(*inner);
			if (innerVal && innerVal->wtype != awst::WType::voidType())
			{
				auto stmt = awst::makeExpressionStatement(std::move(innerVal), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(stmt));
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
				sig = funcType->externalSignature();
			}
			catch (...)
			{
				// Ternary distribution: (c ? f : g).selector
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
				// Fallback: try identifier
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
		// Function pointer variable: extract selector from bytes representation.
		// puya-sol encodes external fn-ptrs as 12 bytes = itob(appId, 8) + selector(4).
		if (funcType && funcType->kind() == FunctionType::Kind::External)
		{
			auto base = buildExpr(baseExpr);
			// Coerce to bytes if typed as bytes[12].
			if (base && base->wtype && base->wtype->kind() == awst::WTypeKind::Bytes)
				base = awst::makeAsBytes(std::move(base), m_loc);
			if (base && base->wtype == awst::WType::bytesType())
			{
				// selector is bytes 8..12 of the 12-byte encoding.
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

	// Event selectors are bytes32 (the EVM topic shape): emit the FULL
	// 32-byte sha512_256(sig) — its first 4 bytes equal the ARC-28 log
	// prefix our emit path writes, so prefix comparisons stay coherent.
	if (bytesWType && bytesWType->length().has_value() && *bytesWType->length() == 32)
	{
		auto hash = awst::makeIntrinsicCall(
			"sha512_256", awst::WType::bytesType(), m_loc);
		hash->stackArgs.push_back(awst::makeUtf8BytesConstant(sig, m_loc));
		return awst::makeReinterpretCast(std::move(hash), targetType, m_loc);
	}

	// Function selectors: 4-byte sha512_256 ARC-4 selector — see
	// makeSelectorExpr.
	auto selector = awst::makeMethodConstant(sig, awst::WType::bytesType(), m_loc);
	if (targetType && targetType != awst::WType::bytesType())
		return awst::makeReinterpretCast(std::move(selector), targetType, m_loc);
	return selector;
}

} // namespace puyasol::builder::sol_ast
