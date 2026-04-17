/// @file BuiltinCallables.cpp
/// Solidity builtin function implementations via the builder pattern.

#include "builder/sol-eb/BuiltinCallables.h"
#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-types/TypeMapper.h"

namespace puyasol::builder::eb
{

/// Simple InstanceBuilder that just wraps an expression — used for builtin results
/// where we don't need a full Solidity-type-aware builder.
class GenericInstanceBuilder: public InstanceBuilder
{
public:
	GenericInstanceBuilder(BuilderContext& _ctx, std::shared_ptr<awst::Expression> _expr)
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
	BuilderContext& _ctx,
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
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	auto call = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
	for (auto& arg: _args)
		call->stackArgs.push_back(std::move(arg));
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(call));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleSha256(
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	auto call = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), _loc);
	for (auto& arg: _args)
		call->stackArgs.push_back(std::move(arg));
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(call));
}

std::shared_ptr<awst::Expression> BuiltinCallableRegistry::promoteToBigUInt(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	if (_expr->wtype == awst::WType::biguintType())
		return _expr;

	auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
	itob->stackArgs.push_back(std::move(_expr));

	auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
	return cast;
}

static void emitModByZeroCheck(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> const& _modulus,
	awst::SourceLocation const& _loc)
{
	// assert(modulus != 0, "modulo by zero") — prevents optimizer from eliminating
	auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());

	auto cmp = awst::makeNumericCompare(_modulus, awst::NumericComparison::Ne, std::move(zero), _loc);

	auto stmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "modulo by zero"), _loc);
	_ctx.prePendingStatements.push_back(std::move(stmt));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleMulmod(
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 3) return nullptr;

	auto x = promoteToBigUInt(std::move(_args[0]), _loc);
	auto y = promoteToBigUInt(std::move(_args[1]), _loc);
	auto z = promoteToBigUInt(std::move(_args[2]), _loc);

	// EVM reverts on mod by zero
	emitModByZeroCheck(_ctx, z, _loc);

	auto mul = std::make_shared<awst::BigUIntBinaryOperation>();
	mul->sourceLocation = _loc;
	mul->wtype = awst::WType::biguintType();
	mul->left = std::move(x);
	mul->right = std::move(y);
	mul->op = awst::BigUIntBinaryOperator::Mult;

	auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
	mod->sourceLocation = _loc;
	mod->wtype = awst::WType::biguintType();
	mod->left = std::move(mul);
	mod->right = std::move(z);
	mod->op = awst::BigUIntBinaryOperator::Mod;

	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(mod));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleAddmod(
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	if (_args.size() != 3) return nullptr;

	auto x = promoteToBigUInt(std::move(_args[0]), _loc);
	auto y = promoteToBigUInt(std::move(_args[1]), _loc);
	auto z = promoteToBigUInt(std::move(_args[2]), _loc);

	// EVM reverts on mod by zero
	emitModByZeroCheck(_ctx, z, _loc);

	auto add = std::make_shared<awst::BigUIntBinaryOperation>();
	add->sourceLocation = _loc;
	add->wtype = awst::WType::biguintType();
	add->left = std::move(x);
	add->right = std::move(y);
	add->op = awst::BigUIntBinaryOperator::Add;

	auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
	mod->sourceLocation = _loc;
	mod->wtype = awst::WType::biguintType();
	mod->left = std::move(add);
	mod->right = std::move(z);
	mod->op = awst::BigUIntBinaryOperator::Mod;

	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(mod));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleSelfdestruct(
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	// selfdestruct(addr) on AVM:
	// Send remaining balance to addr via inner payment (CloseRemainderTo).
	// Post-Cancun EVM selfdestruct only sends funds without deleting the contract,
	// so we don't require DeleteApplication OnCompletion.
	if (!_args.empty())
	{
		auto beneficiary = std::move(_args[0]);

		// Get current app address for the Sender field
		auto appAddr = awst::makeIntrinsicCall("global", awst::WType::accountType(), _loc);
		appAddr->immediates = {std::string("CurrentApplicationAddress")};

		static awst::WInnerTransactionFields s_payFieldsType(1); // pay
		auto create = std::make_shared<awst::CreateInnerTransaction>();
		create->sourceLocation = _loc;
		create->wtype = &s_payFieldsType;

		auto typeVal = std::make_shared<awst::IntegerConstant>();
		typeVal->sourceLocation = _loc;
		typeVal->wtype = awst::WType::uint64Type();
		typeVal->value = "1"; // pay

		auto feeVal = awst::makeIntegerConstant("0", _loc);

		auto amountVal = std::make_shared<awst::IntegerConstant>();
		amountVal->sourceLocation = _loc;
		amountVal->wtype = awst::WType::uint64Type();
		amountVal->value = "0"; // amount=0, CloseRemainderTo sends everything

		create->fields["TypeEnum"] = std::move(typeVal);
		create->fields["Fee"] = std::move(feeVal);
		create->fields["Receiver"] = std::move(appAddr);
		create->fields["Amount"] = std::move(amountVal);
		create->fields["CloseRemainderTo"] = std::move(beneficiary);

		static awst::WInnerTransaction s_payTxnType(1);
		auto submit = std::make_shared<awst::SubmitInnerTransaction>();
		submit->sourceLocation = _loc;
		submit->wtype = &s_payTxnType;
		submit->itxns.push_back(std::move(create));

		auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
		_ctx.prePendingStatements.push_back(std::move(submitStmt));
	}

	// Terminate execution after selfdestruct — on EVM, selfdestruct halts
	// execution so any code after it (e.g. assert(false)) is dead code.
	// Emit return to prevent subsequent statements from executing.
	auto retStmt = std::make_shared<awst::ReturnStatement>();
	retStmt->sourceLocation = _loc;
	retStmt->value = nullptr;
	_ctx.prePendingStatements.push_back(std::move(retStmt));

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = _loc;
	vc->wtype = awst::WType::voidType();
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(vc));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleGasleft(
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& /*_args*/,
	awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntrinsicCall("global", awst::WType::uint64Type(), _loc);
	e->immediates = {std::string("OpcodeBudget")};
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(e));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleEcrecover(
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	// ecrecover(bytes32 hash, uint8 v, bytes32 r, bytes32 s) → address
	if (_args.size() != 4) return nullptr;

	auto msgHash = std::move(_args[0]);
	auto v = std::move(_args[1]);
	auto r = std::move(_args[2]);
	auto s = std::move(_args[3]);

	// Ensure args are bytes for ecdsa_pk_recover
	// hash, r, s should be bytes32 → bytes
	auto toBytes = [&](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
		if (expr->wtype != awst::WType::bytesType())
		{
			auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
			return cast;
		}
		return expr;
	};
	msgHash = toBytes(std::move(msgHash));
	r = toBytes(std::move(r));
	s = toBytes(std::move(s));

	// v is uint8 (27 or 28). Normalise to uint64 so we can reference it
	// multiple times and clamp into the valid recovery range. We persist
	// v in a temp var because ConditionalExpression duplicates operands
	// in the serialised AWST.
	std::shared_ptr<awst::Expression> vUint;
	if (v->wtype == awst::WType::uint64Type() || v->wtype == awst::WType::boolType())
	{
		vUint = std::move(v);
	}
	else
	{
		// biguint v → bytes → btoi
		auto vBytes = awst::makeReinterpretCast(std::move(v), awst::WType::bytesType(), _loc);
		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		btoi->stackArgs.push_back(std::move(vBytes));
		vUint = std::move(btoi);
	}

	// Stash v in a temp so we can read it multiple times.
	std::string vTmpName = "__ecrecover_v";
	auto vTmpTarget = awst::makeVarExpression(vTmpName, awst::WType::uint64Type(), _loc);
	auto vAssign = std::make_shared<awst::AssignmentStatement>();
	vAssign->sourceLocation = _loc;
	vAssign->target = vTmpTarget;
	vAssign->value = std::move(vUint);
	_ctx.prePendingStatements.push_back(std::move(vAssign));

	auto readV = [&]() -> std::shared_ptr<awst::Expression> {
		auto r = awst::makeVarExpression(vTmpName, awst::WType::uint64Type(), _loc);
		return r;
	};

	auto mkU64 = [&](std::string const& _val) {
		auto c = awst::makeIntegerConstant(_val, _loc);
		return c;
	};

	// Recovery id: if v ∈ {27,28} then v-27 else 0. The conditional is
	// important because unguarded `v-27` underflows for v < 27 and crashes.
	auto vGte27 = awst::makeNumericCompare(readV(), awst::NumericComparison::Gte, mkU64("27"), _loc);

	auto vMinus27 = awst::makeUInt64BinOp(readV(), awst::UInt64BinaryOperator::Sub, mkU64("27"), _loc);

	auto recIdCond = std::make_shared<awst::ConditionalExpression>();
	recIdCond->sourceLocation = _loc;
	recIdCond->wtype = awst::WType::uint64Type();
	recIdCond->condition = std::move(vGte27);
	recIdCond->trueExpr = std::move(vMinus27);
	recIdCond->falseExpr = mkU64("0");
	// Clamp further: `recovery_id & 1` so the ecdsa opcode doesn't see an
	// out-of-range value when v is e.g. 29. Combined with the outer result
	// masking this keeps the TEAL valid for any v.
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

	// Store result in temp var
	std::string tmpName = "__ecrecover_result";
	auto tmpTarget = awst::makeVarExpression(tmpName, tupleType, _loc);

	auto assignTuple = std::make_shared<awst::AssignmentStatement>();
	assignTuple->sourceLocation = _loc;
	assignTuple->target = tmpTarget;
	assignTuple->value = std::move(ecdsaRecover);
	_ctx.prePendingStatements.push_back(std::move(assignTuple));

	// Extract pubkey_x and pubkey_y
	auto tupleRead0 = awst::makeVarExpression(tmpName, tupleType, _loc);
	auto pubkeyX = std::make_shared<awst::TupleItemExpression>();
	pubkeyX->sourceLocation = _loc;
	pubkeyX->wtype = awst::WType::bytesType();
	pubkeyX->base = std::move(tupleRead0);
	pubkeyX->index = 0;

	auto tupleRead1 = awst::makeVarExpression(tmpName, tupleType, _loc);
	auto pubkeyY = std::make_shared<awst::TupleItemExpression>();
	pubkeyY->sourceLocation = _loc;
	pubkeyY->wtype = awst::WType::bytesType();
	pubkeyY->base = std::move(tupleRead1);
	pubkeyY->index = 1;

	// concat(pubkey_x, pubkey_y) → 64 bytes
	auto pubkeyConcat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyX));
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyY));

	// keccak256(pubkey) → 32 bytes
	auto hash = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
	hash->stackArgs.push_back(std::move(pubkeyConcat));

	// extract3(hash, 12, 20) → last 20 bytes = Ethereum address
	auto off12 = awst::makeIntegerConstant("12", _loc);
	auto len20 = awst::makeIntegerConstant("20", _loc);
	auto addr20 = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	addr20->stackArgs.push_back(std::move(hash));
	addr20->stackArgs.push_back(std::move(off12));
	addr20->stackArgs.push_back(std::move(len20));

	// Left-pad to 32 bytes: concat(bzero(12), addr20) → bytes32 form
	auto pad12 = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	auto twelve = awst::makeIntegerConstant("12", _loc);
	pad12->stackArgs.push_back(std::move(twelve));

	auto paddedAddr = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	paddedAddr->stackArgs.push_back(std::move(pad12));
	paddedAddr->stackArgs.push_back(std::move(addr20));

	// Solidity's ecrecover returns address(0) when v is not 27 or 28
	// (EVM precompile returns empty data for malformed input; Solidity
	// converts that to the zero address). We can't short-circuit the
	// ecdsa_pk_recover opcode itself, so always compute it and mask the
	// result to bzero(32) when v was out of range.
	auto isValidV = [&]() -> std::shared_ptr<awst::Expression> {
		auto gte = awst::makeNumericCompare(readV(), awst::NumericComparison::Gte, mkU64("27"), _loc);

		auto lte = awst::makeNumericCompare(readV(), awst::NumericComparison::Lte, mkU64("28"), _loc);

		auto andOp = std::make_shared<awst::BooleanBinaryOperation>();
		andOp->sourceLocation = _loc;
		andOp->wtype = awst::WType::boolType();
		andOp->left = std::move(gte);
		andOp->op = awst::BinaryBooleanOperator::And;
		andOp->right = std::move(lte);
		return andOp;
	};

	auto zero32 = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	zero32->stackArgs.push_back(mkU64("32"));

	auto maskedAddr = std::make_shared<awst::ConditionalExpression>();
	maskedAddr->sourceLocation = _loc;
	maskedAddr->wtype = awst::WType::bytesType();
	maskedAddr->condition = isValidV();
	maskedAddr->trueExpr = std::move(paddedAddr);
	maskedAddr->falseExpr = std::move(zero32);

	// Cast to account type (address return type)
	auto addrCast = awst::makeReinterpretCast(std::move(maskedAddr), awst::WType::accountType(), _loc);

	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(addrCast));
}

} // namespace puyasol::builder::eb
