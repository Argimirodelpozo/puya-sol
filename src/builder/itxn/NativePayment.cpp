#include "builder/itxn/NativePayment.h"
#include "awst/NameGen.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/XchainAccounts.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder
{
namespace
{

std::shared_ptr<awst::Expression> applicationEscrow(
	std::shared_ptr<awst::Expression> _appId, awst::SourceLocation const& _loc)
{
	static awst::WTuple s_addressResult({awst::WType::bytesType(), awst::WType::boolType()});
	auto checked = std::make_shared<awst::CheckedMaybe>();
	checked->expr = awst::makeAppParamsGet("AppAddress",
		awst::makeAsUInt64(std::move(_appId), _loc), &s_addressResult, _loc);
	checked->wtype = awst::WType::bytesType();
	checked->sourceLocation = _loc;
	checked->comment = "payment target application does not exist";
	return awst::makeAsAccount(std::move(checked), _loc);
}

struct NativeReceiver
{
	std::shared_ptr<awst::Expression> account;
	std::shared_ptr<awst::Expression> appId; // zero for an ordinary account
};

NativeReceiver paymentReceiver(
	TargetProfile const& _profile, std::shared_ptr<awst::Expression> _receiver,
	awst::SourceLocation const& _loc)
{
	// Preserve the app identity established by typed call/new lowering. Its
	// escrow is a native account, not a projected EVM identity requiring opt-in.
	if (_receiver->wtype == awst::WType::applicationType())
	{
		auto appId = awst::makeEvalOnce(awst::makeAsUInt64(std::move(_receiver), _loc), _loc);
		return {applicationEscrow(appId, _loc), std::move(appId)};
	}

	if (!_profile.xchainAccounts && _profile.contractAbi == ContractAbi::Evm)
		EvmFeaturePolicy::report(EvmFeature::NativeValueTransfer, _profile, _loc);

	auto receiver = TypeCoercion::coerceForAssignment(
		std::move(_receiver), awst::WType::accountType(), _loc);
	auto bytes = awst::makeEvalOnce(awst::makeAsBytes(std::move(receiver), _loc), _loc);
	// Solidity contract values can be stored/passed as bzero24 ++ appId.
	// Resolve that convention to an escrow, never pay the keyless encoding.
	// Zero is still the zero address, not an application identity.
	auto appId = awst::makeEvalOnce(awst::makeConditional(
		awst::makeBytesComparison(awst::makeExtract(bytes, 0, 24, _loc),
			awst::EqualityComparison::Eq, awst::makeBzero(24, _loc), _loc),
		awst::makeWord32ToUInt64(bytes, _loc), awst::makeZero(_loc),
		awst::WType::uint64Type(), _loc), _loc);
	auto account = awst::makeConditional(
		awst::makeNumericCompare(appId, awst::NumericComparison::Ne, awst::makeZero(_loc), _loc),
		applicationEscrow(appId, _loc),
		xchain::mapPaymentReceiver(_profile, awst::makeAsAccount(bytes, _loc), _loc),
		awst::WType::accountType(), _loc);
	return {std::move(account), std::move(appId)};
}

std::shared_ptr<awst::CreateInnerTransaction> paymentFields(
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	static awst::WInnerTransactionFields s_paymentFields(1);
	auto create = awst::makeCreateInnerTransaction(&s_paymentFields, _loc);
	create->fields["TypeEnum"] = awst::makeOne(_loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["Receiver"] = std::move(_receiver);
	create->fields["Amount"] = std::move(_amount);
	return create;
}

} // namespace

std::shared_ptr<awst::CreateInnerTransaction> buildNativePayment(
	TargetProfile const& _profile,
	std::vector<std::shared_ptr<awst::Statement>>& _preEffects,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	auto receiver = paymentReceiver(_profile, std::move(_receiver), _loc);
	auto amount = TypeCoercion::checkedAmountToUint64(_preEffects, std::move(_amount), _loc);
	return paymentFields(std::move(receiver.account), std::move(amount), _loc);
}

std::shared_ptr<awst::Statement> buildNativeTransfer(
	TargetProfile const& _profile,
	std::vector<std::shared_ptr<awst::Statement>>& _preEffects,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	auto receiver = paymentReceiver(_profile, std::move(_receiver), _loc);
	auto amount = TypeCoercion::checkedAmountToUint64(_preEffects, std::move(_amount), _loc);
	auto block = awst::makeBlock(_loc);
	// A shared field can contain nested SingleEvaluation nodes (e.g. amount
	// narrowing). Materialize before the branch so every definition dominates
	// BOTH submits; first evaluating it in one arm leaves the other uninitialized.
	auto pin = [&](std::shared_ptr<awst::Expression> value, char const* tag) {
		auto name = std::string("__native_payment_") + tag + "_"
			+ std::to_string(awst::NameGen::next("NativePayment.transfer"));
		auto read = awst::makeVarExpression(name, value->wtype, _loc);
		block->body.push_back(awst::makeAssignmentStatement(read, std::move(value), _loc));
		return read;
	};
	amount = pin(std::move(amount), "amount");
	receiver.account = pin(std::move(receiver.account), "receiver");
	receiver.appId = pin(std::move(receiver.appId), "app");
	auto payment = paymentFields(std::move(receiver.account), std::move(amount), _loc);
	static awst::WInnerTransaction s_payTxn(1), s_appTxn(6);
	auto payOnly = awst::makeSubmitInnerTransaction(&s_payTxn, _loc);
	payOnly->itxns.push_back(payment);

	static awst::WInnerTransactionFields s_appFields(6);
	auto call = awst::makeCreateInnerTransaction(&s_appFields, _loc);
	call->fields["TypeEnum"] = awst::makeIntegerConstant(6, _loc);
	call->fields["Fee"] = awst::makeZero(_loc);
	call->fields["ApplicationID"] = awst::makeAsApplication(receiver.appId, _loc);
	call->fields["OnCompletion"] = awst::makeZero(_loc);
	// No ApplicationArgs: the callee dispatches receive/fallback. Grouping the
	// payment immediately before it supplies msg.value and makes rejection atomic.
	auto payAndCall = awst::makeSubmitInnerTransaction(&s_appTxn, _loc);
	payAndCall->itxns = {std::move(payment), std::move(call)};
	auto appBranch = awst::makeBlock(_loc);
	appBranch->body.push_back(awst::makeExpressionStatement(std::move(payAndCall), _loc));
	auto accountBranch = awst::makeBlock(_loc);
	accountBranch->body.push_back(awst::makeExpressionStatement(std::move(payOnly), _loc));
	block->body.push_back(awst::makeIfElse(
		awst::makeNumericCompare(receiver.appId, awst::NumericComparison::Ne,
			awst::makeZero(_loc), _loc),
		std::move(appBranch), std::move(accountBranch), _loc));
	return block;
}

std::shared_ptr<awst::CreateInnerTransaction> buildNativeClose(
	TargetProfile const& _profile,
	std::vector<std::shared_ptr<awst::Statement>>& _preEffects,
	std::shared_ptr<awst::Expression> _beneficiary,
	awst::SourceLocation const& _loc)
{
	auto create = buildNativePayment(_profile, _preEffects,
		std::move(_beneficiary), awst::makeZero(_loc), _loc);
	create->fields["CloseRemainderTo"] = std::move(create->fields["Receiver"]);
	create->fields["Receiver"] = awst::makeGlobal(
		std::string("CurrentApplicationAddress"), awst::WType::accountType(), _loc);
	return create;
}

} // namespace puyasol::builder
