/// @file BuiltinCallables.cpp
/// Solidity builtin function implementations via the builder pattern.

#include "builder/sol-eb/BuiltinCallables.h"
#include "builder/sol-eb/SolIntegerBuilder.h"

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
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->opCode = "keccak256";
	call->wtype = awst::WType::bytesType();
	for (auto& arg: _args)
		call->stackArgs.push_back(std::move(arg));
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(call));
}

std::unique_ptr<InstanceBuilder> BuiltinCallableRegistry::handleSha256(
	BuilderContext& _ctx,
	std::vector<std::shared_ptr<awst::Expression>>& _args,
	awst::SourceLocation const& _loc)
{
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->opCode = "sha256";
	call->wtype = awst::WType::bytesType();
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

	auto itob = std::make_shared<awst::IntrinsicCall>();
	itob->sourceLocation = _loc;
	itob->wtype = awst::WType::bytesType();
	itob->opCode = "itob";
	itob->stackArgs.push_back(std::move(_expr));

	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::biguintType();
	cast->expr = std::move(itob);
	return cast;
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
		auto appAddr = std::make_shared<awst::IntrinsicCall>();
		appAddr->sourceLocation = _loc;
		appAddr->wtype = awst::WType::accountType();
		appAddr->opCode = "global";
		appAddr->immediates = {std::string("CurrentApplicationAddress")};

		static awst::WInnerTransactionFields s_payFieldsType(1); // pay
		auto create = std::make_shared<awst::CreateInnerTransaction>();
		create->sourceLocation = _loc;
		create->wtype = &s_payFieldsType;

		auto typeVal = std::make_shared<awst::IntegerConstant>();
		typeVal->sourceLocation = _loc;
		typeVal->wtype = awst::WType::uint64Type();
		typeVal->value = "1"; // pay

		auto feeVal = std::make_shared<awst::IntegerConstant>();
		feeVal->sourceLocation = _loc;
		feeVal->wtype = awst::WType::uint64Type();
		feeVal->value = "0";

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

		auto submitStmt = std::make_shared<awst::ExpressionStatement>();
		submitStmt->sourceLocation = _loc;
		submitStmt->expr = std::move(submit);
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
	auto e = std::make_shared<awst::IntrinsicCall>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::uint64Type();
	e->opCode = "global";
	e->immediates = {std::string("OpcodeBudget")};
	return std::make_unique<GenericInstanceBuilder>(_ctx, std::move(e));
}

} // namespace puyasol::builder::eb
