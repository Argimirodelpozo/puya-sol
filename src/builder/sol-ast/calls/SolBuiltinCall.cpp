#include "builder/sol-ast/calls/SolBuiltinCall.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

SolBuiltinCall::SolBuiltinCall(
	eb::BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _call,
	std::string _builtinName)
	: SolFunctionCall(_ctx, _call),
	  m_builtinName(std::move(_builtinName))
{
}

std::shared_ptr<awst::Expression> SolBuiltinCall::toAwst()
{
	// blockhash is AVM-specific, handle separately
	if (m_builtinName == "blockhash")
		return handleBlockhash();

	// All other builtins: delegate to BuiltinCallableRegistry
	eb::BuiltinCallableRegistry registry;
	std::vector<std::shared_ptr<awst::Expression>> args;
	for (auto const& arg: m_call.arguments())
		args.push_back(buildExpr(*arg));

	auto result = registry.tryCall(m_ctx, m_builtinName, args, m_loc);
	if (result)
		return result->resolve();

	Logger::instance().warning("unhandled builtin: " + m_builtinName, m_loc);
	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

std::shared_ptr<awst::Expression> SolBuiltinCall::handleBlockhash()
{
	Logger::instance().warning(
		"blockhash() mapped to AVM 'block BlkSeed'. "
		"AVM has no block hash — BlkSeed (VRF output) is used as the closest equivalent. "
		"Not cryptographically equivalent to EVM blockhash.", m_loc);

	auto blockNum = buildExpr(*m_call.arguments()[0]);
	blockNum = TypeCoercion::implicitNumericCast(
		std::move(blockNum), awst::WType::uint64Type(), m_loc);

	auto e = std::make_shared<awst::IntrinsicCall>();
	e->sourceLocation = m_loc;
	e->wtype = awst::WType::bytesType();
	e->opCode = "block";
	e->immediates = {std::string("BlkSeed")};
	e->stackArgs.push_back(std::move(blockNum));

	// Cast to the target type (bytes32 or biguint)
	if (m_wtype && m_wtype != awst::WType::bytesType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = m_wtype;
		cast->expr = std::move(e);
		return cast;
	}
	return e;
}

} // namespace puyasol::builder::sol_ast
