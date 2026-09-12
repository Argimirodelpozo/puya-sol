#include "builder/contract/SelectorRouter.h"
#include "builder/contract/RouterConditions.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> reconstructCalldata(
	CalldataTransport transport, awst::SourceLocation const& loc,
	std::shared_ptr<awst::Expression> selector)
{
	auto count = awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc);
	auto data = selector ? std::move(selector) : awst::makeAppArg(0, loc);
	// Explicit native compatibility: values remain in their ARC4 wire format.
	int const slots = transport == CalldataTransport::Arc4Arguments ? 16 : 2;
	for (int i = 1; i < slots; ++i)
	{
		auto present = awst::makeNumericCompare(count,
			transport == CalldataTransport::Arc4Fallback ? awst::NumericComparison::Eq : awst::NumericComparison::Gte,
			awst::makeIntegerConstant(i + 1, loc), loc);
		data = awst::makeConcat(std::move(data), awst::makeConditional(
			std::move(present), awst::makeAppArg(i, loc), awst::makeBytesConstant({}, loc),
			awst::WType::bytesType(), loc), loc);
	}
	return awst::makeConditional(awst::makeNumericCompare(count,
		awst::NumericComparison::Gt, awst::makeZero(loc), loc),
		std::move(data), awst::makeBytesConstant({}, loc), awst::WType::bytesType(), loc);
}

void emitReturnLog(std::shared_ptr<awst::Expression> payload,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out)
{
	auto log = awst::makeIntrinsicCall("log", awst::WType::voidType(), loc);
	log->stackArgs.push_back(awst::makeConcat(
		awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, loc), std::move(payload), loc));
	out.push_back(awst::makeExpressionStatement(std::move(log), loc));
}

void emitFallbackCall(solidity::frontend::FunctionDefinition const& function,
	std::string const& method, std::shared_ptr<awst::Expression> calldata,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out)
{
	bool const returnsBytes = !function.returnParameters().empty();
	auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{method},
		returnsBytes ? awst::WType::bytesType() : awst::WType::voidType(), loc);
	if (!function.parameters().empty()) awst::pushCallArg(call->args, std::move(calldata));
	if (returnsBytes) emitReturnLog(std::move(call), loc, out);
	else
	{
		out.push_back(awst::makeExpressionStatement(std::move(call), loc));
		// A void handler can emit events, but its final event is not returndata.
		emitReturnLog(awst::makeBytesConstant({}, loc), loc, out);
	}
}

void emitSelectorDispatch(
	awst::Block& _body,
	solidity::frontend::FunctionDefinition const* _fallbackFunc,
	solidity::frontend::FunctionDefinition const* _receiveFunc,
	awst::SourceLocation const& _loc)
{
	if (!_fallbackFunc && !_receiveFunc)
	{
		// No fallback/receive: `return ARC4Router()` → puya can_exit_early=True.
		auto routerExpr = awst::makeARC4Router(awst::WType::boolType(), _loc);

		auto routerReturn = awst::makeReturnStatement(routerExpr, _loc);
		_body.body.push_back(routerReturn);
		return;
	}

	// Custom dispatch (fallback/receive present):
	//   NumAppArgs==0 && OnCompletion==NoOp → receive/fallback + return true
	//   else → __did_match = ARC4Router() (assignment → can_exit_early=False)
	//          if !__did_match && OnCompletion==NoOp → fallback; __did_match = true
	//          return __did_match
	//
	// Both fallback arms are NoOp-only: EVM's receive/fallback exist for plain
	// calls, and an unchecked arm would approve lifecycle txns (Delete/Update/
	// CloseOut) that dodge the router's per-method OnCompletion gating by
	// arriving bare or with an unmatched selector.

	auto makeTrueLit = [&]() {
		return awst::makeTrue(_loc);
	};

	auto makeReturnTrue = [&]() -> std::shared_ptr<awst::Statement> {
		auto r = awst::makeReturnStatement(makeTrueLit(), _loc);
		return r;
	};

	auto makeIsNoOp = [&]() { return isNoOpCall(_loc); };

	// Step 1: bare NoOp call (NumAppArgs==0).
	{
		auto numAppArgs = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), _loc);

		auto zero = awst::makeZero(_loc);

		auto isBareCall = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Eq, std::move(zero), _loc);

		auto isBareNoOp = awst::makeBoolBinOp(
			std::move(isBareCall), awst::BinaryBooleanOperator::And, makeIsNoOp(), _loc);

		auto bareBlock = awst::makeBlock(_loc);
		if (_receiveFunc)
			emitFallbackCall(*_receiveFunc, "__receive", awst::makeBytesConstant({}, _loc), _loc, bareBlock->body);
		else if (_fallbackFunc)
			emitFallbackCall(*_fallbackFunc, "__fallback", awst::makeBytesConstant({}, _loc), _loc, bareBlock->body);
		bareBlock->body.push_back(makeReturnTrue());

		_body.body.push_back(awst::makeIfElse(
			std::move(isBareNoOp), std::move(bareBlock), nullptr, _loc));
	}

	// Step 2: run ARC4 router; assignment → can_exit_early=False.
	std::string matchVarName = "__did_match_routing";
	{
		auto matchVar = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto routerExpr = awst::makeARC4Router(awst::WType::boolType(), _loc);

		auto assignMatch = awst::makeAssignmentStatement(std::move(matchVar), std::move(routerExpr), _loc);
		_body.body.push_back(std::move(assignMatch));
	}

	// Step 3: no-match + NoOp + fallback exists → call fallback.
	if (_fallbackFunc)
	{
		auto matchVarRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto notMatch = awst::makeBoolBinOp(
			awst::makeNot(std::move(matchVarRead), _loc),
			awst::BinaryBooleanOperator::And, makeIsNoOp(), _loc);

		auto dispatchBlock = awst::makeBlock(_loc);
		emitFallbackCall(*_fallbackFunc, "__fallback", reconstructCalldata(CalldataTransport::Arc4Fallback, _loc),
			_loc, dispatchBlock->body);

		auto matchVarWrite = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto assignTrue = awst::makeAssignmentStatement(std::move(matchVarWrite), makeTrueLit(), _loc);
		dispatchBlock->body.push_back(std::move(assignTrue));

		_body.body.push_back(awst::makeIfElse(
			std::move(notMatch), std::move(dispatchBlock), nullptr, _loc));
	}

	auto finalRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

	auto retStmt = awst::makeReturnStatement(std::move(finalRead), _loc);
	_body.body.push_back(std::move(retStmt));
}

} // namespace puyasol::builder
