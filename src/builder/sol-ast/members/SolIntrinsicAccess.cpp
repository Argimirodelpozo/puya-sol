/// @file SolIntrinsicAccess.cpp
/// msg.sender, block.timestamp, block.prevrandao, block.difficulty, etc.
/// Resolved solc MagicType/member pairs select the environment lowering.

#include "builder/sol-ast/members/SolIntrinsicAccess.h"
#include "builder/contract/RouterConditions.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/SelectorSemantics.h"
#include "builder/codec/ByteSlice.h"
#include "builder/contract/SelectorRouter.h"
#include <stdexcept>
#include "builder/sol-types/TypeMapper.h"
#include "builder/XchainAccounts.h"
#include "builder/BuildArtifacts.h"
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
// Native ARC4 keeps the complete Algorand sender.
std::shared_ptr<awst::Expression> buildEvmMsgSenderInline(
	eb::ContractContext& ctx,
	builder::TargetProfile::XchainAccounts const& xc,
	awst::SourceLocation const& loc);


/// The claim-verifying msg.sender expression (xchain profile), inlined
/// once into the shared __evm_sender method.
std::shared_ptr<awst::Expression> buildEvmMsgSenderInline(
	eb::ContractContext& ctx,
	builder::TargetProfile::XchainAccounts const& xc,
	awst::SourceLocation const& loc)
{
	auto sender = awst::makeTxn(
		"Sender", awst::WType::accountType(), loc);
	auto low160 = awst::makeExtractLastN(std::move(sender), 20, loc);
	std::shared_ptr<awst::Expression> projected = awst::makeAsAccount(
		awst::makeLeftPadToN(std::move(low160), 32, loc), loc);
	{
		auto argCountOk = awst::makeNumericCompare(
			awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gte,
			awst::makeIntegerConstant("3", loc), loc);
		auto lenOk = awst::makeNumericCompare(
			awst::makeLen(awst::makeAppArg(2, loc), loc),
			awst::NumericComparison::Eq,
			awst::makeIntegerConstant("20", loc), loc);
		auto shapeOk = awst::makeBoolBinOp(
			std::move(argCountOk), awst::BinaryBooleanOperator::And,
			std::move(lenOk), loc);
		auto derivedOk = awst::makeBytesComparison(
			awst::makeAsBytes(
				builder::xchain::derivedAccount(
					xc, awst::makeAppArg(2, loc), loc), loc),
			awst::EqualityComparison::Eq,
			awst::makeAsBytes(
				awst::makeTxn("Sender", awst::WType::accountType(), loc), loc),
			loc);
		auto isClaim = awst::makeBoolBinOp(
			std::move(shapeOk), awst::BinaryBooleanOperator::And,
			std::move(derivedOk), loc);
		auto claimed = awst::makeAsAccount(
			awst::makeLeftPadToN(awst::makeAppArg(2, loc), 32, loc), loc);
		return awst::makeConditional(
			std::move(isClaim), std::move(claimed), std::move(projected),
			awst::WType::accountType(), loc);
	}
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
		awst::SourceLocation const& loc) { return SolIntrinsicAccess::sender(ctx, loc); }},
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

std::shared_ptr<awst::Expression> SolIntrinsicAccess::sender(
	eb::ContractContext& ctx,
	awst::SourceLocation const& loc)
{
	if (ctx.typeMapper.profile().contractAbi != builder::ContractAbi::Evm)
		return awst::makeTxn("Sender", awst::WType::accountType(), loc);
	auto sender = awst::makeTxn(
		"Sender", awst::WType::accountType(), loc);
	auto low160 = awst::makeExtractLastN(std::move(sender), 20, loc);
	std::shared_ptr<awst::Expression> projected = awst::makeAsAccount(
		awst::makeLeftPadToN(std::move(low160), 32, loc), loc);
	// xchain account model: a caller that presented a valid owner claim
	// (ApplicationArgs[2]: 20 bytes whose derived LogicSig address IS the
	// sender) is that EVM identity. The check is fully SELF-VERIFYING at
	// the read site — arity alone must not gate it, because __postInit and
	// ARC-4-routed calls legitimately carry 3+ args that are NOT claims
	// (a multi-arg ctor once adopted its own second argument as the
	// minting identity). The hash comparison cannot pass accidentally.
	// The low-20 projection of the raw sender survives only as the
	// unclaimed-caller compatibility shim (deploy/creator paths).
	if (auto const& xc = ctx.typeMapper.profile().xchainAccounts)
	{
		// Root subroutines (library / free functions, currentContract unset)
		// cannot invoke a contract instance method: inline the claim check
		// there (Permit2's PermitHash library reads msg.sender). Contract
		// methods share the memoized __evm_sender below.
		if (!ctx.currentContract)
			return buildEvmMsgSenderInline(ctx, *xc, loc);
		// One contract method per contract: the claim check (two app-arg
		// reads, a sha512_256, a compare) was inlined at EVERY msg.sender use
		// — 39 copies in CTFExchange.
		auto& arts = ctx.typeMapper.artifacts();
		std::string const key = "environment:msg.sender";
		auto found = arts.contract().helpers.find(key);
		std::string name = found != arts.contract().helpers.end()
			? found->second : std::string();
		if (name.empty())
		{
			name = "__evm_sender";
			arts.contract().helpers[key] = name;
			// cref is stamped when the pending methods are attached.
			auto method = awst::ContractMethod(
				"", name, awst::WType::accountType(), {}, loc);
			method.body->body.push_back(awst::makeReturnStatement(
				buildEvmMsgSenderInline(ctx, *xc, loc), loc));
			arts.contract().pendingHelpers.push_back(std::move(method));
		}
		return awst::makeSubroutineCall(
			awst::InstanceMethodTarget{name}, awst::WType::accountType(), loc);
	}
	return projected;
}

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
