/// @file SolBytesConcat.cpp
/// bytes.concat(a, b, ...) and string.concat(a, b, ...).
/// Migrated from FunctionCallBuilder.cpp lines 3270-3321.

#include "builder/sol-ast/calls/SolBytesConcat.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolBytesConcat::toAwst()
{
	auto const& args = m_call.arguments();

	if (args.empty())
		return awst::makeBytesConstant({}, m_loc);

	auto toBytes = [this](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
		if (expr->wtype == awst::WType::bytesType()
			|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
			return expr;
		auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), m_loc);
		return cast;
	};

	auto result = toBytes(buildExpr(*args[0]));
	for (size_t i = 1; i < args.size(); ++i)
		result = awst::makeConcat(std::move(result), toBytes(buildExpr(*args[i])), m_loc);

	return result;
}

} // namespace puyasol::builder::sol_ast
