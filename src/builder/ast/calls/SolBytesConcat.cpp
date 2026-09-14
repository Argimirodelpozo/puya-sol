/// @file SolBytesConcat.cpp
/// bytes.concat(a, b, ...) and string.concat(a, b, ...).

#include "builder/ast/calls/SolBytesConcat.h"

#include "builder/types/TypeMapper.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolBytesConcat::toAwst()
{
	auto args = CallOperands::build(m_ctx, m_call, m_loc);

	auto toBytes = [this](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
		if (expr->wtype == awst::WType::bytesType()
			|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
			return expr;
		auto cast = awst::makeAsBytes(std::move(expr), m_loc);
		return cast;
	};

	auto result = args.empty() ? awst::makeBytesConstant({}, m_loc) : toBytes(std::move(args[0]));
	for (size_t i = 1; i < args.size(); ++i)
		result = awst::makeConcat(std::move(result), toBytes(std::move(args[i])), m_loc);

	// `string.concat` returns `string memory`, but the concat intrinsic is
	// labelled plain `bytes` — identical at runtime, different WType. puya
	// type-checks assignment target vs value and rejects the whole program
	// ("assignment target type differs from expression value type") the moment
	// the result is bound to a string-typed local. Label it as the call's
	// DECLARED type so every consumer agrees, not just the paths that happen to
	// run their own width/fixup (returns already did; locals did not).
	if (auto const* declared = m_ctx.typeMapper.map(m_call.annotation().type))
		if (declared == awst::WType::stringType() && result->wtype != declared)
			result = awst::makeReinterpretCast(std::move(result), declared, m_loc);

	return result;
}

} // namespace puyasol::builder::sol_ast
