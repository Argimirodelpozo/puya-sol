#include "builder/sol-ast/calls/SolBuiltinCall.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTUtils.h>

#include <sstream>

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

	// blobhash(n) — EIP-4844 transaction blob hash. AVM has no blob
	// transactions, so return bytes32(0) and warn.
	if (m_builtinName == "blobhash")
	{
		Logger::instance().warning(
			"blobhash() has no AVM equivalent — returning bytes32(0).", m_loc);
		auto bc = std::make_shared<awst::BytesConstant>();
		bc->sourceLocation = m_loc;
		bc->wtype = m_ctx.typeMapper.createType<awst::BytesWType>(32);
		bc->encoding = awst::BytesEncoding::Base16;
		bc->value = std::vector<uint8_t>(32, 0);
		return bc;
	}

	// ripemd160(bytes memory) — AVM has no RIPEMD-160 opcode. Return the
	// 20-byte zero hash as a stub. Tests that treat the digest as opaque
	// (e.g. comparing with another ripemd160 call on the same input) still
	// compile, though most cryptographic uses will produce wrong output.
	if (m_builtinName == "ripemd160")
	{
		Logger::instance().warning(
			"ripemd160() has no AVM equivalent — returning bytes20(0); "
			"cryptographic uses will misbehave.", m_loc);
		auto bc = std::make_shared<awst::BytesConstant>();
		bc->sourceLocation = m_loc;
		bc->wtype = m_ctx.typeMapper.createType<awst::BytesWType>(20);
		bc->encoding = awst::BytesEncoding::Base16;
		bc->value = std::vector<uint8_t>(20, 0);
		return bc;
	}

	// erc7201 — evaluate the ERC-7201 namespace slot at compile time when
	// possible (the Solidity front-end does the heavy lifting via
	// erc7201CompileTimeValue).  Emit the resulting u256 as a biguint
	// IntegerConstant; anything non-constant is unreachable because
	// `erc7201(<non-literal>)` isn't allowed.
	if (m_builtinName == "erc7201")
	{
		if (auto slotOpt = solidity::frontend::erc7201CompileTimeValue(m_call))
		{
			std::ostringstream oss;
			oss << *slotOpt;
			auto ic = std::make_shared<awst::IntegerConstant>();
			ic->sourceLocation = m_loc;
			ic->wtype = awst::WType::biguintType();
			ic->value = oss.str();
			return ic;
		}
		Logger::instance().warning(
			"erc7201() with a non-constant argument is not supported; "
			"returning 0", m_loc);
		auto ic = std::make_shared<awst::IntegerConstant>();
		ic->sourceLocation = m_loc;
		ic->wtype = awst::WType::biguintType();
		ic->value = "0";
		return ic;
	}

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
