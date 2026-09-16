#include "builder/ast/calls/SolRequireAssert.h"
#include "builder/ast/calls/RevertBlob.h"
#include "builder/solc/SolcFacts.h"

namespace puyasol::builder::sol_ast
{

SolRequireAssert::SolRequireAssert(
	eb::ContractContext& ctx, solidity::frontend::FunctionCall const& call)
	: SolFunctionCall(ctx, call)
{
}

std::shared_ptr<awst::Expression> SolRequireAssert::toAwst()
{
	using namespace solidity::frontend;
	auto condition = CallOperands::evaluate(m_ctx, *arguments().at(0), m_loc);
	RevertPayload payload;
	if (arguments().size() > 1)
	{
		// The result type, not identifier/member syntax, distinguishes an
		// error constructor from an ordinary message-producing function.
		auto const* magic = dynamic_cast<MagicType const*>(arguments()[1]->annotation().type);
		if (magic && magic->kind() == MagicType::Kind::Error)
			payload = RevertPayload(m_ctx, dynamic_cast<FunctionCall const&>(SolcFacts::unparenthesized(*arguments()[1])), m_loc);
		else
			payload = RevertPayload(m_ctx, CallOperands::evaluate(m_ctx, *arguments()[1], m_loc), m_loc);
	}
	auto const* type = dynamic_cast<FunctionType const*>(m_call.expression().annotation().type);
	if (type && type->kind() == FunctionType::Kind::Assert)
		payload.blob = awst::makeBytesConstant(panicRevertBlobBytes(0x01), m_loc);

	if (!payload.blob)
		return awst::makeAssert(std::move(condition), m_loc);

	// All source operands are already evaluated, even when the condition is
	// true. Only payload logging is conditional. Keep the native assert out of
	// the branch for Puya's explicit-assert accounting.
	if (auto const* constant = dynamic_cast<awst::BoolConstant const*>(condition.get()))
	{
		if (constant->value) return awst::makeVoidConstant(m_loc);
		m_ctx.preEffects().push_back(makeRevertLogStmt(std::move(payload.blob), m_loc));
	}
	else
	{
		auto log = awst::makeBlock(m_loc);
		log->body.push_back(makeRevertLogStmt(std::move(payload.blob), m_loc));
		m_ctx.preEffects().push_back(awst::makeIfElse(
			awst::makeNot(condition, m_loc), std::move(log), nullptr, m_loc));
	}
	auto check = awst::makeAssert(std::move(condition), m_loc, std::move(payload.message));
	check->isExplicit = false;
	return check;
}

} // namespace puyasol::builder::sol_ast
