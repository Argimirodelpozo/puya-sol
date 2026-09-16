/// @file BuiltinCallables.cpp
/// Solidity builtin value lowering, selected by solc function kind.

#include "builder/eb/BuiltinCallables.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "builder/lowering/itxn/Precompile.h"
#include "awst/NameGen.h"
#include "builder/eb/BigUIntMathHelpers.h"
#include "builder/types/TypeMapper.h"

namespace puyasol::builder::eb
{
namespace
{
std::shared_ptr<awst::Expression> buildEcrecover(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	// ecrecover(bytes32 hash, uint8 v, bytes32 r, bytes32 s) → address.
	if (_args.size() != 4) return nullptr;

	auto msgHash = std::move(_args[0]);
	auto v = std::move(_args[1]);
	auto r = std::move(_args[2]);
	auto s = std::move(_args[3]);

	// Normalise each operand to a 32-byte word. uint64 (e.g. a literal 0 arg)
	// can't reinterpret to bytes (not bytes-backed) — itob + left-pad; biguint is
	// minimal-length — ARC4-encode to a full uint256 word; bytes[N] reinterprets.
	auto toBytes = [&](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
		if (expr->wtype == awst::WType::uint64Type())
			return awst::makeLeftPad(awst::makeItob(std::move(expr), _loc), 24, _loc);
		if (expr->wtype == awst::WType::biguintType())
			return awst::makeAsBytes(
				awst::makeARC4Encode(std::move(expr),
					_ctx.typeMapper.createType<awst::ARC4UIntN>(256), _loc),
				_loc);
		if (expr->wtype != awst::WType::bytesType())
			return awst::makeAsBytes(std::move(expr), _loc);
		return expr;
	};
	msgHash = toBytes(std::move(msgHash));
	r = toBytes(std::move(r));
	s = toBytes(std::move(s));

	// Normalise v to uint64; persist in a temp (ConditionalExpression duplicates operands in AWST).
	std::shared_ptr<awst::Expression> vUint;
	if (v->wtype == awst::WType::uint64Type() || v->wtype == awst::WType::boolType())
	{
		vUint = std::move(v);
	}
	else
	{
		// biguint v → bytes → btoi
		auto vBytes = awst::makeAsBytes(std::move(v), _loc);
		vUint = awst::makeBtoi(std::move(vBytes), _loc);
	}

	// Names must be unique per call: all pre-effects flush before any
	// reads lower, so `ecrecover(a)==ecrecover(b)` with a shared name would have
	// both sides read the SECOND call's v/result.
	int ecTmpId = (awst::NameGen::next("BuiltinCallables.s_ecrecoverTmpCounter") + 1);
	std::string vTmpName = "__ecrecover_v_" + std::to_string(ecTmpId);
	auto vTmpTarget = awst::makeVarExpression(vTmpName, awst::WType::uint64Type(), _loc);
	auto vAssign = awst::makeAssignmentStatement(vTmpTarget, std::move(vUint), _loc);
	_ctx.preEffects().push_back(std::move(vAssign));

	auto readV = [&]() -> std::shared_ptr<awst::Expression> {
		auto r = awst::makeVarExpression(vTmpName, awst::WType::uint64Type(), _loc);
		return r;
	};

	auto mkU64 = [&](std::string const& _val) {
		auto c = awst::makeIntegerConstant(_val, _loc);
		return c;
	};

	// Persist r/s in temps: each is read by the validity checks AND the recover call.
	std::string rTmpName = "__ecrecover_r_" + std::to_string(ecTmpId);
	std::string sTmpName = "__ecrecover_s_" + std::to_string(ecTmpId);
	_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(rTmpName, awst::WType::bytesType(), _loc), std::move(r), _loc));
	_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(sTmpName, awst::WType::bytesType(), _loc), std::move(s), _loc));
	auto readR = [&]() { return awst::makeVarExpression(rTmpName, awst::WType::bytesType(), _loc); };
	auto readS = [&]() { return awst::makeVarExpression(sTmpName, awst::WType::bytesType(), _loc); };

	// recovery_id = v>=27 ? v-27 : 0 (unguarded v-27 underflows for v<27).
	auto vGte27 = awst::makeNumericCompare(readV(), awst::NumericComparison::Gte, mkU64("27"), _loc);

	auto vMinus27 = awst::makeUInt64BinOp(readV(), awst::UInt64BinaryOperator::Sub, mkU64("27"), _loc);

	auto recIdCond = awst::makeConditional(
		std::move(vGte27), std::move(vMinus27), mkU64("0"),
		awst::WType::uint64Type(), _loc);
	// Clamp: &1 so ecdsa opcode sees 0 or 1 for any v (e.g. 29).
	auto recIdClamp = awst::makeUInt64BinOp(std::move(recIdCond), awst::UInt64BinaryOperator::BitAnd, mkU64("1"), _loc);
	std::shared_ptr<awst::Expression> recoveryId = std::move(recIdClamp);

	// EVM ecrecover returns address(0) for v ∉ {27,28}, r ∉ [1,N-1], s ∉ [1,N-1]
	// (N = secp256k1 group order). AVM ecdsa_pk_recover PANICS on such inputs, so
	// gate the opcode itself behind the checkable conditions and yield zero without
	// running it. (Residue: an in-range r whose x-coordinate isn't on the curve
	// still panics where EVM returns 0 — not checkable without the recover itself.)
	auto isValid = [&]() -> std::shared_ptr<awst::Expression> {
		auto andOp = [&](std::shared_ptr<awst::Expression> a, std::shared_ptr<awst::Expression> b) {
			return awst::makeBoolBinOp(std::move(a), awst::BinaryBooleanOperator::And, std::move(b), _loc);
		};
		// v is uint8-typed at the language level, so the uint64 window check is
		// exact here; the raw-calldata lowerings must validate the full word.
		auto cond = andOp(
			awst::makeNumericCompare(readV(), awst::NumericComparison::Gte, mkU64("27"), _loc),
			awst::makeNumericCompare(readV(), awst::NumericComparison::Lte, mkU64("28"), _loc));
		cond = andOp(std::move(cond),
			builder::secp256k1RangeCondition(readR, readS, _loc));
		return cond;
	};

	// ecdsa_pk_recover Secp256k1 → (pubkey_x: bytes, pubkey_y: bytes) — built
	// INSIDE the conditional's true branch so invalid inputs never execute it.
	auto tupleType = _ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}
	);

	auto ecdsaRecover = awst::makeIntrinsicCall("ecdsa_pk_recover", tupleType, _loc);
	ecdsaRecover->immediates.push_back("Secp256k1");
	ecdsaRecover->stackArgs.push_back(std::move(msgHash));
	ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
	ecdsaRecover->stackArgs.push_back(readR());
	ecdsaRecover->stackArgs.push_back(readS());

	// Tuple read twice (x, y) — eval-once so the opcode runs a single time.
	auto tupleOnce = awst::makeEvalOnce(std::move(ecdsaRecover), _loc);
	auto pubkeyX = awst::makeTupleItem(tupleOnce, 0, awst::WType::bytesType(), _loc);
	auto pubkeyY = awst::makeTupleItem(tupleOnce, 1, awst::WType::bytesType(), _loc);

	// keccak256(pubkey_x ++ pubkey_y)[12:32] left-padded = Ethereum address word
	auto pubkeyConcat = awst::makeConcat(std::move(pubkeyX), std::move(pubkeyY), _loc);
	auto hash = awst::makeKeccak256(std::move(pubkeyConcat), _loc);
	auto addr20 = awst::makeExtract(std::move(hash), 12, 20, _loc);
	auto paddedAddr = awst::makeLeftPad(std::move(addr20), 12, _loc);

	auto maskedAddr = awst::makeConditional(
		isValid(), std::move(paddedAddr), awst::makeBzero(32, _loc),
		awst::WType::bytesType(), _loc);

	auto addrCast = awst::makeAsAccount(std::move(maskedAddr), _loc);

	return addrCast;
}

} // namespace

std::shared_ptr<awst::Expression> buildBuiltinCall(
	ContractContext& ctx,
	solidity::frontend::FunctionType::Kind kind,
	std::vector<std::shared_ptr<awst::Expression>> args,
	awst::SourceLocation const& loc)
{
	using Kind = solidity::frontend::FunctionType::Kind;
	switch (kind)
	{
	case Kind::KECCAK256:
	case Kind::SHA256:
	{
		auto call = awst::makeIntrinsicCall(
			kind == Kind::KECCAK256 ? "keccak256" : "sha256", awst::WType::bytesType(), loc);
		call->stackArgs = std::move(args);
		return call;
	}
	case Kind::AddMod:
	case Kind::MulMod:
	{
		if (args.size() != 3) return nullptr;
		auto x = promoteToBiguint(std::move(args[0]), loc);
		auto y = promoteToBiguint(std::move(args[1]), loc);
		auto z = awst::makeEvalOnce(promoteToBiguint(std::move(args[2]), loc), loc);
		// Keep the zero check even if the arithmetic result is unused.
		ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(
			z, awst::NumericComparison::Ne, awst::makeBiguintConstant("0", loc), loc),
			loc, "modulo by zero"), loc);
		auto value = awst::makeBigUIntBinOp(std::move(x),
			kind == Kind::AddMod ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Mult,
			std::move(y), loc);
		return awst::makeBigUIntBinOp(std::move(value), awst::BigUIntBinaryOperator::Mod, std::move(z), loc);
	}
	case Kind::GasLeft:
		EvmFeaturePolicy::report(EvmFeature::GasLeft, ctx.typeMapper.profile(), loc);
		return promoteToBiguint(awst::makeGlobal("OpcodeBudget", awst::WType::uint64Type(), loc), loc);
	case Kind::Selfdestruct:
	{
		// Post-Cancun selfdestruct sends the balance without deleting the app.
		if (!args.empty())
		{
			auto create = buildNativeClose(ctx.typeMapper.profile(), ctx.preEffects(), std::move(args[0]), loc);
			static awst::WInnerTransaction payTxnType(1);
			auto submit = awst::makeSubmitInnerTransaction(&payTxnType, loc);
			submit->itxns.push_back(std::move(create));
			ctx.queuePreExpression(std::move(submit), loc);
		}
		ctx.queuePreEffect(awst::makeReturnStatement(nullptr, loc));
		return awst::makeVoidConstant(loc);
	}
	case Kind::ECRecover:
		return buildEcrecover(ctx, args, loc);
	default:
		return nullptr;
	}
}

} // namespace puyasol::builder::eb
