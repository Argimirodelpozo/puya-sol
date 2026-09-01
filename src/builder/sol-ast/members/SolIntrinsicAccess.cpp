/// @file SolIntrinsicAccess.cpp
/// msg.sender, block.timestamp, block.prevrandao, block.difficulty, etc.
/// Registry shape: exact (base, member) rows dispatch to one handler each; a handler returning nullptr falls through to …

#include "builder/sol-ast/members/SolIntrinsicAccess.h"
#include "builder/contract/RouterConditions.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/SelectorSemantics.h"
#include "builder/sol-intrinsics/IntrinsicMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace puyasol::builder::sol_ast
{

namespace
{

// An EVM ABI boundary gives Solidity one 160-bit address namespace.  Values
// decoded from calldata are zero-extended from 20 bytes, so ambient caller
// identity must use the same representation; otherwise storing an address
// argument and later indexing by msg.sender can never hit the same slot for
// an Algorand user account (whose native sender is 32 bytes).
// Non-EVM abi: fall through to IntrinsicMapper's standard mapping.
std::shared_ptr<awst::Expression> buildEvmMsgSender(
	eb::ContractContext& ctx, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	if (ctx.typeMapper.profile().contractAbi != builder::ContractAbi::Evm)
		return nullptr;
	auto sender = awst::makeTxn(
		"Sender", awst::WType::accountType(), loc);
	auto low160 = awst::makeExtractLastN(std::move(sender), 20, loc);
	std::shared_ptr<awst::Expression> projected = awst::makeAsAccount(
		awst::makeLeftPadToN(std::move(low160), 32, loc), loc);
	// xchain account model: a caller that presented a VERIFIED owner claim
	// (ApplicationArgs[2], asserted against the derived LogicSig address at
	// the entry arm) IS that 20-byte EVM identity. The low-20 projection of
	// the raw sender survives only as the unclaimed-caller compatibility
	// shim (deploy/creator paths).
	if (ctx.typeMapper.profile().xchainAccounts)
	{
		auto hasClaim = awst::makeNumericCompare(
			awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gte,
			awst::makeIntegerConstant("3", loc), loc);
		auto claimed = awst::makeAsAccount(
			awst::makeLeftPadToN(awst::makeAppArg(2, loc), 32, loc), loc);
		return awst::makeConditional(
			std::move(hasClaim), std::move(claimed), std::move(projected),
			awst::WType::accountType(), loc);
	}
	return projected;
}

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

// block.prevrandao / block.difficulty → block BlkSeed (Round - 2).
// difficulty == prevrandao post-Paris (same EVM opcode); one lowering.
std::shared_ptr<awst::Expression> buildBlockRandao(
	eb::ContractContext& ctx, Context&, std::string const& member,
	awst::SourceLocation const& loc)
{
	builder::EvmFeaturePolicy::report(
		member == "difficulty" ? builder::EvmFeature::BlockDifficulty
			: builder::EvmFeature::BlockPrevrandao,
		ctx.typeMapper.profile(), loc);

	// Round - 2, clamped: uint64 Sub panics on underflow and the first
	// rounds of a fresh chain (create at round 1) would hard-panic.
	auto round = awst::makeGlobal(std::string("Round"), awst::WType::uint64Type(), loc);
	auto isEarly = awst::makeNumericCompare(
		awst::makeGlobal(std::string("Round"), awst::WType::uint64Type(), loc),
		awst::NumericComparison::Lt,
		awst::makeIntegerConstant("2", loc), loc);
	auto prevRound = awst::makeConditional(
		std::move(isEarly),
		awst::makeZero(loc),
		awst::makeUInt64BinOp(
			std::move(round), awst::UInt64BinaryOperator::Sub,
			awst::makeIntegerConstant("2", loc), loc),
		awst::WType::uint64Type(), loc);

	auto blockSeed = awst::makeBlock(
		"BlkSeed", std::move(prevRound), awst::WType::bytesType(), loc);

	auto cast = awst::makeAsBiguint(std::move(blockSeed), loc);
	return cast;
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

// msg.sig is the routed ARC-4 selector in compatibility mode. Under
// --evm-selectors the explicit transport map recovers the corresponding
// Solidity selector while preserving the outer selector across internal calls.
std::shared_ptr<awst::Expression> buildMsgSig(
	eb::ContractContext& ctx, Context&, std::string const&,
	awst::SourceLocation const& loc)
{
	if (!builder::SelectorSemantics::enabled(ctx.typeMapper))
		return awst::makeAppArg(
			0, loc, ctx.typeMapper.createType<awst::BytesWType>(4));

	auto hasSelector = awst::makeNumericCompare(
		awst::makeTxn(
			std::string("NumAppArgs"), awst::WType::uint64Type(), loc),
		awst::NumericComparison::Gt, awst::makeZero(loc), loc);
	auto raw = awst::makeConditional(
		std::move(hasSelector), awst::makeAppArg(0, loc),
		awst::makeBytesConstant(std::vector<uint8_t>(4, 0), loc),
		awst::WType::bytesType(), loc);
	auto selector = builder::SelectorSemantics::runtimeSelector(
		ctx, std::move(raw), loc);
	return awst::makeReinterpretCast(
		std::move(selector),
		ctx.typeMapper.createType<awst::BytesWType>(4), loc);
}

// msg.data → concatenate ApplicationArgs[0..15] (selector + ARC4 args).
// ARC4 args are already left-padded; result approximates EVM head encoding
// for scalar args. Bare calls return bzero(0). Cap at 16 slots (AVM hard limit).
std::shared_ptr<awst::Expression> buildMsgData(
	eb::ContractContext& ctx, Context& scope, std::string const&,
	awst::SourceLocation const& loc)
{
	// EVM calldata is empty during construction (ctor args in initcode, not calldata).
	// AVM runs ctor as __postInit with ApplicationArgs; return empty to match
	// (various/create_calldata asserts msg.data.length == 0 in the ctor).
	if (scope.isInConstructor())
		return awst::makeBytesConstant({}, loc);

	auto numAppArgs = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), loc);

	auto zero = awst::makeZero(loc);

	auto hasData = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Gt, std::move(zero), loc);

	// Concatenate slots 0..15; absent slots contribute bzero(0).
	std::shared_ptr<awst::Expression> calldataConcat;
	for (int slot = 0; slot < 16; ++slot)
	{
		auto slotIdx = awst::makeIntegerConstant(slot, loc);

		auto numArgsCheck = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), loc);

		auto slotIdxCmp = awst::makeIntegerConstant(slot, loc);

		auto slotPresent = awst::makeNumericCompare(std::move(numArgsCheck), awst::NumericComparison::Gt, std::move(slotIdxCmp), loc);

		std::shared_ptr<awst::Expression> slotBytes =
			awst::makeAppArg(slot, loc);
		if (slot == 0)
			slotBytes = builder::SelectorSemantics::runtimeSelector(
				ctx, std::move(slotBytes), loc);

		auto slotChoice = awst::makeConditional(
			std::move(slotPresent), std::move(slotBytes),
			awst::makeBzero(0, loc),
			awst::WType::bytesType(), loc);

		if (!calldataConcat)
		{
			calldataConcat = std::move(slotChoice);
		}
		else
			calldataConcat = awst::makeConcat(std::move(calldataConcat), std::move(slotChoice), loc);
	}

	return awst::makeConditional(
		std::move(hasData), std::move(calldataConcat),
		awst::makeBzero(0, loc),
		awst::WType::bytesType(), loc);
}

using MemberHandler = std::shared_ptr<awst::Expression> (*)(
	eb::ContractContext&, Context&, std::string const&,
	awst::SourceLocation const&);

struct MemberEntry
{
	char const* base;
	char const* member;
	MemberHandler fn;
};

constexpr MemberEntry kIntrinsicMembers[] = {
	{"msg", "sender", buildEvmMsgSender},
	{"msg", "value", buildMsgValue},
	{"msg", "sig", buildMsgSig},
	{"msg", "data", buildMsgData},
	{"block", "chainid", buildBlockChainId},
	{"block", "basefee", buildBlockFeeZero},
	{"block", "blobbasefee", buildBlockFeeZero},
	{"block", "gaslimit", buildBlockGasLimit},
	{"block", "prevrandao", buildBlockRandao},
	{"block", "difficulty", buildBlockRandao},
	{"block", "coinbase", buildBlockCoinbase},
	{"tx", "origin", buildTxOrigin},
	{"tx", "gasprice", buildTxGasPrice},
};

} // anonymous namespace

std::shared_ptr<awst::Expression> SolIntrinsicAccess::toAwst()
{
	auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpression());
	if (!baseId) return nullptr;

	std::string baseName = baseId->name();
	std::string member = memberName();

	for (auto const& entry: kIntrinsicMembers)
		if (baseName == entry.base && member == entry.member)
		{
			if (auto result = entry.fn(m_ctx, m_scope, member, m_loc))
				return result;
			break; // handler declined (e.g. msg.sender outside EVM abi) — fall through
		}

	// Fall through to IntrinsicMapper for standard intrinsics (block.timestamp, etc.)
	auto intrinsic = builder::IntrinsicMapper::tryMapMemberAccess(baseName, member, m_loc);
	if (intrinsic)
	{
		auto* solType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (intrinsic->wtype == awst::WType::uint64Type()
			&& solType == awst::WType::biguintType())
			return awst::makeAsBiguint(
				awst::makeItob(std::move(intrinsic), m_loc), m_loc);
		if (intrinsic->wtype == awst::WType::bytesType()
			&& solType == awst::WType::biguintType())
		{
			auto cast = awst::makeAsBiguint(std::move(intrinsic), m_loc);
			return cast;
		}
		return intrinsic;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast
