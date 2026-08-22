/// @file BuiltinCallables.cpp
/// Solidity builtin function implementations via the builder pattern.

#include "builder/sol-eb/BuiltinCallables.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/SecpRangeCheck.h"
#include "awst/NameGen.h"
#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-types/TypeMapper.h"

namespace puyasol::builder::eb
{

/// Minimal InstanceBuilder for builtin return values (no Solidity-type semantics needed).
class GenericInstanceBuilder: public InstanceBuilder
{
public:
	GenericInstanceBuilder(ContractContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr))
	{
	}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

BuiltinCallableRegistry::BuiltinCallableRegistry()
{
	registerHandler("keccak256", &handleKeccak256);
	registerHandler("sha256", &handleSha256);
	registerHandler("mulmod", &handleMulmod);
	registerHandler("addmod", &handleAddmod);
	registerHandler("gasleft", &handleGasleft);
	registerHandler("selfdestruct", &handleSelfdestruct);
	registerHandler("ecrecover", &handleEcrecover);
}

void BuiltinCallableRegistry::registerHandler(std::string _name, CallHandler _handler)
{
	m_handlers[std::move(_name)] = std::move(_handler);
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::tryCall(
	ContractContext& _ctx,
	std::string const& _name,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc) const
{
	auto it = m_handlers.find(_name);
	if (it != m_handlers.end())
		return it->second(_ctx, _args, _loc);
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleKeccak256(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	auto call = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
	for (auto& arg: _args)
		call->stackArgs.push_back(std::move(arg));
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(call));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleSha256(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	auto call = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), _loc);
	for (auto& arg: _args)
		call->stackArgs.push_back(std::move(arg));
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(call));
}

// Force `_e` to evaluate NOW as a pre-statement (returns a var read), unless
// it is a trivially-duplicable leaf. Used to sequence the left operands of
// mulmod/addmod before the modulus zero-check — Solidity evaluates the three
// args left-to-right, but the check (a pre-statement referencing the modulus)
// would otherwise run before x/y, which are embedded inline in the result.
static std::shared_ptr<awst::Expression> materializeNow(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _e,
	awst::SourceLocation const& _loc)
{
	if (!_e
		|| dynamic_cast<awst::VarExpression const*>(_e.get())
		|| dynamic_cast<awst::IntegerConstant const*>(_e.get())
		|| dynamic_cast<awst::SingleEvaluation const*>(_e.get()))
		return _e;
	std::string nm = "__modarg_"
		+ std::to_string(awst::NameGen::next("BuiltinCallables.s_modArgCounter") + 1);
	auto const* wt = _e->wtype;
	_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(nm, wt, _loc), std::move(_e), _loc));
	return awst::makeVarExpression(nm, wt, _loc);
}

static void emitModByZeroCheck(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> const& _modulus,
	awst::SourceLocation const& _loc)
{
	// assert(modulus != 0, "modulo by zero") — prevents optimizer from eliminating
	auto zero = awst::makeBiguintConstant("0", _loc);

	auto cmp = awst::makeNumericCompare(_modulus, awst::NumericComparison::Ne, std::move(zero), _loc);

	auto stmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "modulo by zero"), _loc);
	_ctx.preEffects().push_back(std::move(stmt));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleMulmod(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 3) return nullptr;

	// Left-to-right: force x then y to evaluate BEFORE the modulus zero-check
	// (a pre-statement) — else the check runs first and a side-effecting arg
	// mis-orders vs Solidity.
	auto x = materializeNow(_ctx, promoteToBiguint(std::move(_args[0]), _loc), _loc);
	auto y = materializeNow(_ctx, promoteToBiguint(std::move(_args[1]), _loc), _loc);
	// Modulus referenced twice (assert + mod); eval-once for side-effecting args.
	auto z = awst::makeEvalOnce(promoteToBiguint(std::move(_args[2]), _loc), _loc);
	emitModByZeroCheck(_ctx, z, _loc);

	auto mul = awst::makeBigUIntBinOp(std::move(x), awst::BigUIntBinaryOperator::Mult, std::move(y), _loc);

	auto mod = awst::makeBigUIntBinOp(std::move(mul), awst::BigUIntBinaryOperator::Mod, std::move(z), _loc);

	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(mod));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleAddmod(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 3) return nullptr;

	auto x = materializeNow(_ctx, promoteToBiguint(std::move(_args[0]), _loc), _loc);
	auto y = materializeNow(_ctx, promoteToBiguint(std::move(_args[1]), _loc), _loc);
	auto z = awst::makeEvalOnce(promoteToBiguint(std::move(_args[2]), _loc), _loc);
	emitModByZeroCheck(_ctx, z, _loc);

	auto add = awst::makeBigUIntBinOp(std::move(x), awst::BigUIntBinaryOperator::Add, std::move(y), _loc);

	auto mod = awst::makeBigUIntBinOp(std::move(add), awst::BigUIntBinaryOperator::Mod, std::move(z), _loc);

	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(mod));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleSelfdestruct(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	// AVM: send remaining balance via inner pay (CloseRemainderTo).
	// Post-Cancun EVM selfdestruct only sends funds — no DeleteApplication needed.
	if (!_args.empty())
	{
		auto beneficiary = std::move(_args[0]);

		// Get current app address for the Sender field
		auto appAddr = awst::makeGlobal(std::string("CurrentApplicationAddress"), awst::WType::accountType(), _loc);

		static awst::WInnerTransactionFields s_payFieldsType(1); // pay
		auto create = awst::makeCreateInnerTransaction(&s_payFieldsType, _loc);

		auto typeVal = awst::makeOne(_loc); // pay

		auto feeVal = awst::makeZero(_loc);

		auto amountVal = awst::makeZero(_loc); // CloseRemainderTo sends everything

		create->fields["TypeEnum"] = std::move(typeVal);
		create->fields["Fee"] = std::move(feeVal);
		create->fields["Receiver"] = std::move(appAddr);
		create->fields["Amount"] = std::move(amountVal);
		create->fields["CloseRemainderTo"] = std::move(beneficiary);

		static awst::WInnerTransaction s_payTxnType(1);
		auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
		submit->itxns.push_back(std::move(create));

		auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
		_ctx.preEffects().push_back(std::move(submitStmt));
	}

	// EVM selfdestruct halts — emit return so subsequent statements don't execute.
	auto retStmt = awst::makeReturnStatement(nullptr, _loc);
	_ctx.preEffects().push_back(std::move(retStmt));

	auto vc = awst::makeVoidConstant(_loc);
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(vc));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleGasleft(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& /*_args*/,
	awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::GasLeft, _ctx.typeMapper.profile(), _loc);
	auto e = awst::makeAsBiguint(
		awst::makeItob(awst::makeGlobal(
			std::string("OpcodeBudget"), awst::WType::uint64Type(), _loc), _loc), _loc);
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(e));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleEcrecover(
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

	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(addrCast));
}

} // namespace puyasol::builder::eb
