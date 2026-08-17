#include "builder/sol-ast/calls/SolBuiltinCall.h"
#include "builder/builtin/Ripemd160Builder.h"
#include "builder/BuildArtifacts.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTUtils.h>

#include <sstream>

namespace puyasol::builder::sol_ast
{

SolBuiltinCall::SolBuiltinCall(
	eb::ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _call,
	std::string _builtinName)
	: SolFunctionCall(_ctx, _call),
	  m_builtinName(std::move(_builtinName))
{
}

std::shared_ptr<awst::Expression> SolBuiltinCall::toAwst()
{
	// Preserve the argument's effects, then issue the centralized hard error.
	if (m_builtinName == "blockhash")
		return handleBlockhash();

	// AVM has no blob transaction context. A block seed is not a blob hash and
	// must not be substituted merely to make an EVM test harness pass.
	if (m_builtinName == "blobhash")
	{
		builder::EvmFeaturePolicy::report(
			builder::EvmFeature::BlobHash, m_ctx.typeMapper.profile(), m_loc);
		(void) buildExpr(*m_call.arguments()[0]);
		return awst::makeBytesConstant(
			std::vector<uint8_t>(32, 0), m_loc, awst::BytesEncoding::Base16,
			m_ctx.typeMapper.createType<awst::BytesWType>(32));
	}

	// ripemd160: no AVM opcode; lower to synthesized __builtin_ripemd160
	// (Ripemd160Builder). Compile-time fold for empty input.
	if (m_builtinName == "ripemd160")
	{
		// Fold empty-input ripemd160 (0x9c1185a5c5e9fc54612808977ee8f548b2258d31);
		// test suite pins this value.
		if (m_call.arguments().size() == 1)
		{
			auto const* arg = m_call.arguments()[0].get();
			bool isEmpty = false;
			if (auto const* strLit = dynamic_cast<solidity::frontend::Literal const*>(arg))
			{
				if ((strLit->token() == solidity::frontend::Token::StringLiteral
					|| strLit->token() == solidity::frontend::Token::HexStringLiteral)
					&& strLit->value().empty())
					isEmpty = true;
			}
			if (isEmpty)
			{
				std::vector<uint8_t> emptyDigest = {
					0x9c, 0x11, 0x85, 0xa5, 0xc5, 0xe9, 0xfc, 0x54,
					0x61, 0x28, 0x08, 0x97, 0x7e, 0xe8, 0xf5, 0x48,
					0xb2, 0x25, 0x8d, 0x31
				};
				return awst::makeBytesConstant(
					std::move(emptyDigest), m_loc, awst::BytesEncoding::Base16,
					m_ctx.typeMapper.createType<awst::BytesWType>(20));
			}
		}
		// Request the helper only after constant folding has declined.
		m_ctx.typeMapper.artifacts().needsRipemd160 = true;
		auto arg = buildExpr(*m_call.arguments()[0]);
		auto* bytes20Type = m_ctx.typeMapper.createType<awst::BytesWType>(20);
		auto call = awst::makeSubroutineCall(awst::SubroutineID{builtin::ripemd160SubroutineId()}, awst::WType::bytesType(), m_loc);
		// Coerce non-bytes arg to bytes (non-bytes shapes unexpected per type rules).
		if (arg && arg->wtype != awst::WType::bytesType())
			arg = awst::makeAsBytes(std::move(arg), m_loc);
		awst::pushCallArg(call->args, "data", std::move(arg));
		// Reinterpret to bytes20.
		return awst::makeReinterpretCast(std::move(call), bytes20Type, m_loc);
	}

	// erc7201: slot = keccak256(encode(keccak256(bytes(id))-1)) & ~0xff.
	// Compile-time literal → fold via erc7201CompileTimeValue.
	if (m_builtinName == "erc7201")
	{
		if (auto slotOpt = solidity::frontend::erc7201CompileTimeValue(m_call))
		{
			std::ostringstream oss;
			oss << *slotOpt;
			auto ic = awst::makeIntegerConstant(oss.str(), m_loc, awst::WType::biguintType());
			return ic;
		}

		// Runtime implementation.
		auto idExpr = buildExpr(*m_call.arguments()[0]);
		// Cast string → bytes if needed.
		if (idExpr && idExpr->wtype != awst::WType::bytesType())
		{
			auto cast = awst::makeAsBytes(std::move(idExpr), m_loc);
			idExpr = std::move(cast);
		}

		// h1 = keccak256(id)
		auto h1 = awst::makeKeccak256(idExpr, m_loc);

		// h1_int = biguint(h1)
		auto h1Int = awst::makeAsBiguint(std::move(h1), m_loc);

		// minus1 = h1_int - 1
		auto one = awst::makeOne(m_loc, awst::WType::biguintType());

		auto sub = awst::makeBigUIntBinOp(std::move(h1Int), awst::BigUIntBinaryOperator::Sub, std::move(one), m_loc);

		// minus1_bytes = 32-byte BE via b|(sub, bzero(32))
		auto minusBytesCast = awst::makeAsBytes(std::move(sub), m_loc);

		auto minus1Bytes = awst::makeBytesBinOp(
			awst::makeBzero(32, m_loc),
			awst::BytesBinaryOperator::BitOr,
			std::move(minusBytesCast),
			m_loc);

		// h2 = keccak256(minus1_bytes)
		auto h2 = awst::makeKeccak256(std::move(minus1Bytes), m_loc);

		// Top 31 bytes of h2
		auto top31Start = awst::makeZero(m_loc);

		auto top31Len = awst::makeIntegerConstant("31", m_loc);

		auto top31 = awst::makeExtract3(std::move(h2), std::move(top31Start), std::move(top31Len), m_loc);
		// Concat with 0x00 to zero the last byte.
		auto zeroByte = awst::makeBytesConstant({0}, m_loc);

		auto masked = awst::makeConcat(std::move(top31), std::move(zeroByte), m_loc);

		// Cast to biguint
		auto result = awst::makeAsBiguint(std::move(masked), m_loc);
		return result;
	}

	// All other builtins: delegate to BuiltinCallableRegistry
	eb::BuiltinCallableRegistry registry;
	std::vector<std::shared_ptr<awst::Expression>> args;
	for (auto const& arg: m_call.arguments())
		args.push_back(buildExpr(*arg));

	auto result = registry.tryCall(m_ctx, m_builtinName, args, m_loc);
	if (result)
		return result->resolve();

	Logger::instance().error("unhandled builtin: " + m_builtinName, m_loc);
	auto vc = awst::makeVoidConstant(m_loc);
	return vc;
}

std::shared_ptr<awst::Expression> SolBuiltinCall::handleBlockhash()
{
	builder::EvmFeaturePolicy::report(
		builder::EvmFeature::BlockHash, m_ctx.typeMapper.profile(), m_loc);

	// Preserve argument effects, but return only a type-correct poison value;
	// the policy error prevents it from reaching TEAL.
	(void) buildExpr(*m_call.arguments()[0]);
	auto e = awst::makeBytesConstant(std::vector<uint8_t>(32, 0), m_loc);

	// Cast to the target type (bytes32 or biguint)
	if (m_wtype && m_wtype != awst::WType::bytesType())
	{
		auto cast = awst::makeReinterpretCast(std::move(e), m_wtype, m_loc);
		return cast;
	}
	return e;
}

} // namespace puyasol::builder::sol_ast
