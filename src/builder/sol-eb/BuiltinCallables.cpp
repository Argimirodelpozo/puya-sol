/// @file BuiltinCallables.cpp
/// Solidity builtin function implementations via the builder pattern.

#include "builder/sol-eb/BuiltinCallables.h"
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

static void emitModByZeroCheck(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> const& _modulus,
	awst::SourceLocation const& _loc)
{
	// assert(modulus != 0, "modulo by zero") — prevents optimizer from eliminating
	auto zero = awst::makeBiguintConstant("0", _loc);

	auto cmp = awst::makeNumericCompare(_modulus, awst::NumericComparison::Ne, std::move(zero), _loc);

	auto stmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "modulo by zero"), _loc);
	_ctx.prePendingStatements.push_back(std::move(stmt));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleMulmod(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 3) return nullptr;

	auto x = promoteToBiguint(std::move(_args[0]), _loc);
	auto y = promoteToBiguint(std::move(_args[1]), _loc);
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

	auto x = promoteToBiguint(std::move(_args[0]), _loc);
	auto y = promoteToBiguint(std::move(_args[1]), _loc);
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
		_ctx.prePendingStatements.push_back(std::move(submitStmt));
	}

	// EVM selfdestruct halts — emit return so subsequent statements don't execute.
	auto retStmt = awst::makeReturnStatement(nullptr, _loc);
	_ctx.prePendingStatements.push_back(std::move(retStmt));

	auto vc = awst::makeVoidConstant(_loc);
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(vc));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleGasleft(
	ContractContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& /*_args*/,
	awst::SourceLocation const& _loc)
{
	auto e = awst::makeGlobal(std::string("OpcodeBudget"), awst::WType::uint64Type(), _loc);
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

	auto toBytes = [&](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
		if (expr->wtype != awst::WType::bytesType())
		{
			auto cast = awst::makeAsBytes(std::move(expr), _loc);
			return cast;
		}
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

	// Names must be unique per call: all prePending statements flush before any
	// reads lower, so `ecrecover(a)==ecrecover(b)` with a shared name would have
	// both sides read the SECOND call's v/result.
	int ecTmpId = (awst::NameGen::next("BuiltinCallables.s_ecrecoverTmpCounter") + 1);
	std::string vTmpName = "__ecrecover_v_" + std::to_string(ecTmpId);
	auto vTmpTarget = awst::makeVarExpression(vTmpName, awst::WType::uint64Type(), _loc);
	auto vAssign = awst::makeAssignmentStatement(vTmpTarget, std::move(vUint), _loc);
	_ctx.prePendingStatements.push_back(std::move(vAssign));

	auto readV = [&]() -> std::shared_ptr<awst::Expression> {
		auto r = awst::makeVarExpression(vTmpName, awst::WType::uint64Type(), _loc);
		return r;
	};

	auto mkU64 = [&](std::string const& _val) {
		auto c = awst::makeIntegerConstant(_val, _loc);
		return c;
	};

	// recovery_id = v>=27 ? v-27 : 0 (unguarded v-27 underflows for v<27).
	auto vGte27 = awst::makeNumericCompare(readV(), awst::NumericComparison::Gte, mkU64("27"), _loc);

	auto vMinus27 = awst::makeUInt64BinOp(readV(), awst::UInt64BinaryOperator::Sub, mkU64("27"), _loc);

	auto recIdCond = awst::makeConditional(
		std::move(vGte27), std::move(vMinus27), mkU64("0"),
		awst::WType::uint64Type(), _loc);
	// Clamp: &1 so ecdsa opcode sees 0 or 1 for any v (e.g. 29).
	auto recIdClamp = awst::makeUInt64BinOp(std::move(recIdCond), awst::UInt64BinaryOperator::BitAnd, mkU64("1"), _loc);
	std::shared_ptr<awst::Expression> recoveryId = std::move(recIdClamp);

	// ecdsa_pk_recover Secp256k1 → (pubkey_x: bytes, pubkey_y: bytes)
	auto tupleType = _ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}
	);

	auto ecdsaRecover = awst::makeIntrinsicCall("ecdsa_pk_recover", tupleType, _loc);
	ecdsaRecover->immediates.push_back("Secp256k1");
	ecdsaRecover->stackArgs.push_back(std::move(msgHash));
	ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
	ecdsaRecover->stackArgs.push_back(std::move(r));
	ecdsaRecover->stackArgs.push_back(std::move(s));

	// Unique per call — see vTmpName comment.
	std::string tmpName = "__ecrecover_result_" + std::to_string(ecTmpId);
	auto tmpTarget = awst::makeVarExpression(tmpName, tupleType, _loc);

	auto assignTuple = awst::makeAssignmentStatement(tmpTarget, std::move(ecdsaRecover), _loc);
	_ctx.prePendingStatements.push_back(std::move(assignTuple));

	auto tupleRead0 = awst::makeVarExpression(tmpName, tupleType, _loc);
	auto pubkeyX = awst::makeTupleItem(std::move(tupleRead0), 0, awst::WType::bytesType(), _loc);

	auto tupleRead1 = awst::makeVarExpression(tmpName, tupleType, _loc);
	auto pubkeyY = awst::makeTupleItem(std::move(tupleRead1), 1, awst::WType::bytesType(), _loc);

	// concat(pubkey_x, pubkey_y) → 64 bytes
	auto pubkeyConcat = awst::makeConcat(std::move(pubkeyX), std::move(pubkeyY), _loc);

	// keccak256(pubkey) → 32 bytes
	auto hash = awst::makeKeccak256(std::move(pubkeyConcat), _loc);

	// extract3(hash, 12, 20) → last 20 bytes = Ethereum address
	auto addr20 = awst::makeExtract(std::move(hash), 12, 20, _loc);

	// Left-pad to 32 bytes: concat(bzero(12), addr20) → bytes32 form
	auto paddedAddr = awst::makeLeftPad(std::move(addr20), 12, _loc);

	// EVM ecrecover returns address(0) for invalid v; we always run ecdsa_pk_recover
	// and mask to bzero(32) when v ∉ {27,28}.
	auto isValidV = [&]() -> std::shared_ptr<awst::Expression> {
		auto gte = awst::makeNumericCompare(readV(), awst::NumericComparison::Gte, mkU64("27"), _loc);

		auto lte = awst::makeNumericCompare(readV(), awst::NumericComparison::Lte, mkU64("28"), _loc);

		auto andOp = awst::makeBoolBinOp(std::move(gte), awst::BinaryBooleanOperator::And, std::move(lte), _loc);
		return andOp;
	};

	auto maskedAddr = awst::makeConditional(
		isValidV(), std::move(paddedAddr), awst::makeBzero(32, _loc),
		awst::WType::bytesType(), _loc);

	auto addrCast = awst::makeAsAccount(std::move(maskedAddr), _loc);

	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(addrCast));
}

} // namespace puyasol::builder::eb
