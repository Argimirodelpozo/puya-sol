/// @file SolIntrinsicAccess.cpp
/// msg.sender, block.timestamp, block.prevrandao, block.difficulty, etc.
/// Resolved solc MagicType/member pairs select the environment lowering.

#include "builder/ast/members/SolIntrinsicAccess.h"
#include "builder/contract/RouterConditions.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/codec/ByteSlice.h"
#include "builder/contract/SelectorRouter.h"
#include <stdexcept>
#include "builder/types/TypeMapper.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace puyasol::builder::sol_ast
{

namespace
{

// An explicitly configured EVM chain id is exact for replay. Otherwise use
// GenesisHash as the AVM-native network identity instead of a plausible
// fixed integer shared by every Algorand network.
std::shared_ptr<awst::Expression> buildBlockChainId(
	eb::ContractContext& ctx, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	auto const& profile = ctx.typeMapper.profile();
	builder::EvmFeaturePolicy::report(
		builder::EvmFeature::BlockChainId, profile, loc);
	if (profile.evmChainId)
		return awst::makeIntegerConstant(
			*profile.evmChainId, loc, awst::WType::biguintType());
	return awst::makeAsBiguint(
		awst::makeGlobal("GenesisHash", awst::WType::bytesType(), loc), loc);
}

// block.basefee / block.blobbasefee → 0.
// AVM has a flat per-txn fee (~1000 microAlgos); no EIP-1559 or blob pricing.
std::shared_ptr<awst::Expression> buildBlockFeeZero(
	eb::ContractContext& ctx, Context&, std::string const& member,
	awst::SourceLocation const& loc)
{
	builder::EvmFeaturePolicy::report(
		member == "basefee" ? builder::EvmFeature::BlockBaseFee
			: builder::EvmFeature::BlockBlobBaseFee,
		ctx.typeMapper.profile(), loc);
	auto zero = awst::makeZero(loc, awst::WType::biguintType());
	return zero;
}

// Use an explicit replay value when supplied; otherwise the group's TOTAL
// pooled app-call budget (GroupSize x MaxAppProgramCost=700) — constant
// within an execution like EVM's block-level value, unlike the shrinking
// OpcodeBudget remainder. Never invent a loop-friendly gas sentinel.
std::shared_ptr<awst::Expression> buildBlockGasLimit(
	eb::ContractContext& ctx, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	auto const& profile = ctx.typeMapper.profile();
	builder::EvmFeaturePolicy::report(
		builder::EvmFeature::BlockGasLimit, profile, loc);
	if (profile.evmBlockGasLimit)
		return awst::makeIntegerConstant(
			*profile.evmBlockGasLimit, loc, awst::WType::biguintType());
	return awst::makeAsBiguint(
		awst::makeItob(awst::makeUInt64BinOp(
			awst::makeGlobal("GroupSize", awst::WType::uint64Type(), loc),
			awst::UInt64BinaryOperator::Mult,
			awst::makeIntegerConstant("700", loc), loc), loc), loc);
}

// EVM randomness is unavailable. The opted-in native seed mapping is anchored
// to transaction validity, so submission/simulation timing cannot invalidate it.
std::shared_ptr<awst::Expression> buildBlockRandao(
	eb::ContractContext& ctx, Context&, std::string const& member,
	awst::SourceLocation const& loc)
{
	return builder::buildBlockSeed(
		member == "difficulty" ? builder::EvmFeature::BlockDifficulty : builder::EvmFeature::BlockPrevrandao,
		ctx.typeMapper.profile(), loc);
}

std::shared_ptr<awst::Expression> buildBlockCoinbase(
	eb::ContractContext& ctx, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	auto const& profile = ctx.typeMapper.profile();
	builder::EvmFeaturePolicy::report(
		builder::EvmFeature::BlockCoinbase, profile, loc);
	std::vector<uint8_t> value(32, 0);
	if (profile.evmCoinbase)
	{
		auto b20 = builder::decodeEvmCoinbase20(*profile.evmCoinbase);
		std::copy(b20.begin(), b20.end(), value.begin() + 12);
	}
	return awst::makeAsAccount(
		awst::makeBytesConstant(std::move(value), loc), loc);
}

std::shared_ptr<awst::Expression> buildTxOrigin(
	eb::ContractContext& ctx, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	builder::EvmFeaturePolicy::report(
		builder::EvmFeature::TxOrigin, ctx.typeMapper.profile(), loc);
	// Type-correct poison value; the logged error prevents emission.
	return awst::makeTxn("Sender", awst::WType::accountType(), loc);
}

std::shared_ptr<awst::Expression> buildTxGasPrice(
	eb::ContractContext& ctx, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	builder::EvmFeaturePolicy::report(
		builder::EvmFeature::TxGasPrice, ctx.typeMapper.profile(), loc);
	return awst::makeAsBiguint(
		awst::makeItob(awst::makeTxn(
			"Fee", awst::WType::uint64Type(), loc), loc), loc);
}

// msg.value → GroupIndex > 0 ? gtxns Amount[GroupIndex-1] : 0
std::shared_ptr<awst::Expression> buildMsgValue(
	eb::ContractContext&, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	// Promote uint64 → biguint (Solidity msg.value is uint256).
	auto itob = awst::makeItob(builder::makeMsgValueAmount(loc), loc);
	return awst::makeAsBiguint(std::move(itob), loc);
}

std::shared_ptr<awst::Expression> buildMsgData(
	eb::ContractContext& ctx, Context& scope, std::string const&,
	awst::SourceLocation const& loc)
{
	if (scope.isInConstructor()) return awst::makeBytesConstant({}, loc);
	if (ctx.typeMapper.profile().contractAbi == builder::ContractAbi::Evm)
		return builder::reconstructCalldata(builder::CalldataTransport::SplitEvm, loc);
	return builder::reconstructCalldata(builder::CalldataTransport::Arc4Arguments, loc,
		builder::SelectorSemantics::runtimeSelector(ctx, awst::makeAppArg(0, loc), loc));
}

std::shared_ptr<awst::Expression> buildMsgSig(
	eb::ContractContext& ctx, Context& scope, std::string const&,
	awst::SourceLocation const& loc)
{
	// solc: the first four calldata bytes, right-zero-padded. Constructors
	// have no calldata; empty/short fallback inputs are valid.
	auto data = buildMsgData(ctx, scope, {}, loc);
	return awst::makeReinterpretCast(builder::readPaddedBytes(ctx.typeMapper, std::move(data),
		awst::makeZero(loc), awst::makeIntegerConstant(4, loc), loc),
		ctx.typeMapper.createType<awst::BytesWType>(4), loc);
}

std::shared_ptr<awst::Expression> buildBlockClock(
	eb::ContractContext&, Context&, std::string const& member, awst::SourceLocation const& loc)
{
	return awst::makeAsBiguint(awst::makeItob(awst::makeGlobal(
		member == "number" ? "Round" : "LatestTimestamp", awst::WType::uint64Type(), loc), loc), loc);
}

using MemberHandler = std::shared_ptr<awst::Expression> (*)(
	eb::ContractContext&, Context&, std::string const&,
	awst::SourceLocation const&);

struct MemberEntry
{
	solidity::frontend::MagicType::Kind kind;
	char const* member;
	MemberHandler fn;
};

using MagicKind = solidity::frontend::MagicType::Kind;
constexpr MemberEntry kIntrinsicMembers[] = {
	{MagicKind::Message, "sender", [](eb::ContractContext& ctx, Context&, std::string const&,
		awst::SourceLocation const& loc) { return buildMessageSender(ctx.typeMapper, loc); }},
	{MagicKind::Message, "value", buildMsgValue},
	{MagicKind::Message, "sig", buildMsgSig},
	{MagicKind::Message, "data", buildMsgData},
	{MagicKind::Block, "chainid", buildBlockChainId},
	{MagicKind::Block, "number", buildBlockClock},
	{MagicKind::Block, "timestamp", buildBlockClock},
	{MagicKind::Block, "basefee", buildBlockFeeZero},
	{MagicKind::Block, "blobbasefee", buildBlockFeeZero},
	{MagicKind::Block, "gaslimit", buildBlockGasLimit},
	{MagicKind::Block, "prevrandao", buildBlockRandao},
	{MagicKind::Block, "difficulty", buildBlockRandao},
	{MagicKind::Block, "coinbase", buildBlockCoinbase},
	{MagicKind::Transaction, "origin", buildTxOrigin},
	{MagicKind::Transaction, "gasprice", buildTxGasPrice},
};

} // anonymous namespace

std::shared_ptr<awst::Expression> SolIntrinsicAccess::toAwst()
{
	auto const* magic = dynamic_cast<solidity::frontend::MagicType const*>(baseExpression().annotation().type);
	if (!magic) throw std::logic_error("Intrinsic receiver has no solc MagicType");
	for (auto const& entry: kIntrinsicMembers)
		if (magic->kind() == entry.kind && memberName() == entry.member)
			return entry.fn(m_ctx, m_scope, memberName(), m_loc);
	throw std::logic_error("Unsupported solc intrinsic member");
}

} // namespace puyasol::builder::sol_ast
