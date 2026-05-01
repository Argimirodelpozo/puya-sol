#include "builder/sol-ast/calls/SolRequireAssert.h"
#include "builder/sol-intrinsics/IntrinsicMapper.h"

namespace puyasol::builder::sol_ast
{

SolRequireAssert::SolRequireAssert(
	eb::BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _call)
	: SolFunctionCall(_ctx, _call)
{
}

std::shared_ptr<awst::Expression> SolRequireAssert::toAwst()
{
	auto const& args = m_call.arguments();
	std::shared_ptr<awst::Expression> condition;
	std::optional<std::string> message;

	if (!args.empty())
		condition = buildExpr(*args[0]);

	if (args.size() > 1)
	{
		// Custom error constructor: require(cond, Errors.Foo(args...))
		bool isCustomError = false;
		if (auto const* errorCall = dynamic_cast<solidity::frontend::FunctionCall const*>(args[1].get()))
		{
			auto const& errExpr = errorCall->expression();
			if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&errExpr))
			{
				message = ma->memberName();
				isCustomError = true;
			}
			else if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&errExpr))
			{
				message = id->name();
				isCustomError = true;
			}
			// Solidity evaluates require's args eagerly — even on the
			// success path. Build each error-arg expression so its side
			// effects (e.g. an inner call that short-circuits via Yul
			// return) actually run.
			if (isCustomError)
				for (auto const& a : errorCall->arguments())
					(void)buildExpr(*a);
		}
		if (!isCustomError)
		{
			auto msgExpr = buildExpr(*args[1]);
			if (auto const* sc = dynamic_cast<awst::StringConstant const*>(msgExpr.get()))
				message = sc->value;
			else
				message = "assertion failed";
		}
	}

	return IntrinsicMapper::createAssert(std::move(condition), std::move(message), m_loc);
}

} // namespace puyasol::builder::sol_ast
