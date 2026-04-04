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
	auto sigConst = std::make_shared<awst::BytesConstant>();
	sigConst->sourceLocation = m_loc;
	sigConst->wtype = awst::WType::bytesType();
	sigConst->encoding = awst::BytesEncoding::Utf8;
	sigConst->value = std::vector<uint8_t>(_sig.begin(), _sig.end());

	auto keccak = std::make_shared<awst::IntrinsicCall>();
	keccak->sourceLocation = m_loc;
	keccak->wtype = awst::WType::bytesType();
	keccak->opCode = "keccak256";
	keccak->stackArgs.push_back(std::move(sigConst));

	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = m_loc;
	zero->wtype = awst::WType::uint64Type();
	zero->value = "0";
	auto four = std::make_shared<awst::IntegerConstant>();
	four->sourceLocation = m_loc;
	four->wtype = awst::WType::uint64Type();
	four->value = "4";

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = m_loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(keccak));
	extract->stackArgs.push_back(std::move(zero));
	extract->stackArgs.push_back(std::move(four));
	return extract;
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
				auto condStmt = std::make_shared<awst::ExpressionStatement>();
				condStmt->sourceLocation = m_loc;
				condStmt->expr = condition;
				m_ctx.prePendingStatements.push_back(std::move(condStmt));

				if (trueSig == falseSig)
					return makeSelectorExpr(trueSig);

				auto ternCond = buildExpr(cond->condition());
				auto ternary = std::make_shared<awst::ConditionalExpression>();
				ternary->sourceLocation = m_loc;
				ternary->wtype = awst::WType::bytesType();
				ternary->condition = std::move(ternCond);
				ternary->trueExpr = makeSelectorExpr(trueSig);
				ternary->falseExpr = makeSelectorExpr(falseSig.empty() ? trueSig : falseSig);
				return ternary;
			}
		}
		// General: h().f.selector — evaluate h() for side effects
		else if (!dynamic_cast<Identifier const*>(inner))
		{
			auto innerVal = buildExpr(*inner);
			if (innerVal && innerVal->wtype != awst::WType::voidType())
			{
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = m_loc;
				stmt->expr = std::move(innerVal);
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
						auto ternary = std::make_shared<awst::ConditionalExpression>();
						ternary->sourceLocation = m_loc;
						ternary->wtype = awst::WType::bytesType();
						ternary->condition = std::move(condition);
						ternary->trueExpr = makeSelectorExpr(trueSig);
						ternary->falseExpr = makeSelectorExpr(falseSig);

						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = m_loc;
						cast->wtype = awst::WType::biguintType();
						cast->expr = std::move(ternary);
						return cast;
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

	if (sig.empty()) return nullptr;

	Logger::instance().debug("selector: " + sig, m_loc);

	auto keccak = std::make_shared<awst::IntrinsicCall>();
	keccak->sourceLocation = m_loc;
	keccak->wtype = awst::WType::bytesType();
	keccak->opCode = "keccak256";
	auto sigBytes = std::make_shared<awst::BytesConstant>();
	sigBytes->sourceLocation = m_loc;
	sigBytes->wtype = awst::WType::bytesType();
	sigBytes->encoding = awst::BytesEncoding::Utf8;
	sigBytes->value = std::vector<uint8_t>(sig.begin(), sig.end());
	keccak->stackArgs.push_back(std::move(sigBytes));

	auto* targetType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
	auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(targetType);
	if (bytesWType && bytesWType->length().has_value() && *bytesWType->length() == 4)
	{
		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = m_loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract";
		extract->immediates = {0, 4};
		extract->stackArgs.push_back(std::move(keccak));

		auto padded = std::make_shared<awst::ReinterpretCast>();
		padded->sourceLocation = m_loc;
		padded->wtype = targetType;
		padded->expr = std::move(extract);
		return padded;
	}
	else if (targetType != awst::WType::bytesType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = targetType;
		cast->expr = std::move(keccak);
		return cast;
	}
	return keccak;
}

} // namespace puyasol::builder::sol_ast
