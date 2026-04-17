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
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(expr);
		return cast;
	};

	auto result = toBytes(buildExpr(*args[0]));
	for (size_t i = 1; i < args.size(); ++i)
	{
		auto arg = toBytes(buildExpr(*args[i]));
		auto concat = std::make_shared<awst::IntrinsicCall>();
		concat->sourceLocation = m_loc;
		concat->wtype = awst::WType::bytesType();
		concat->opCode = "concat";
		concat->stackArgs.push_back(std::move(result));
		concat->stackArgs.push_back(std::move(arg));
		result = std::move(concat);
	}

	return result;
}

} // namespace puyasol::builder::sol_ast
