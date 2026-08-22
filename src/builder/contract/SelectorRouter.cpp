#include "builder/contract/SelectorRouter.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

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

	// isBareCall: pass empty bytes; else pass ApplicationArgs[0].
	auto makeCall = [&](std::string const& _name,
		solidity::frontend::FunctionDefinition const* _func,
		bool _isBareCall)
		-> std::shared_ptr<awst::Statement>
	{
		// Solidity's typed fallback form
		//
		//   fallback(bytes calldata) external returns (bytes memory)
		//
		// returns RAW EVM returndata.  Keep that value on the subroutine edge and
		// publish it through the same structured-log carrier low-level inner calls
		// consume.  Treating every fallback as void discarded the value entirely;
		// callers then observed successful calls with empty returndata.
		bool const returnsBytes = _func && _func->isFallback()
			&& !_func->returnParameters().empty();
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{_name},
			returnsBytes ? awst::WType::bytesType() : awst::WType::voidType(), _loc);
		if (_func && _func->parameters().size() == 1) // fallback takes `bytes calldata _input`
		{
			std::shared_ptr<awst::Expression> argExpr;
			if (_isBareCall)
			{
				// No calldata in bare calls — pass empty bytes
				argExpr = awst::makeBytesConstant({}, _loc);
			}
			else
			{
				argExpr = awst::makeAppArg(0, _loc);
			}

			awst::pushCallArg(call->args, std::move(argExpr));
		}

		if (!returnsBytes)
			return awst::makeExpressionStatement(std::move(call), _loc);

		auto log = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
		log->stackArgs.push_back(awst::makeConcat(
			awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, _loc),
			std::move(call), _loc));
		return awst::makeExpressionStatement(std::move(log), _loc);
	};

	auto makeTrueLit = [&]() {
		return awst::makeTrue(_loc);
	};

	auto makeReturnTrue = [&]() -> std::shared_ptr<awst::Statement> {
		auto r = awst::makeReturnStatement(makeTrueLit(), _loc);
		return r;
	};

	auto makeIsNoOp = [&]() -> std::shared_ptr<awst::Expression> {
		auto onCompletion = awst::makeTxn(std::string("OnCompletion"), awst::WType::uint64Type(), _loc);
		return awst::makeNumericCompare(
			std::move(onCompletion), awst::NumericComparison::Eq, awst::makeZero(_loc), _loc);
	};

	// Step 1: bare NoOp call (NumAppArgs==0).
	{
		auto numAppArgs = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), _loc);

		auto zero = awst::makeZero(_loc);

		auto isBareCall = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Eq, std::move(zero), _loc);

		auto isBareNoOp = awst::makeBoolBinOp(
			std::move(isBareCall), awst::BinaryBooleanOperator::And, makeIsNoOp(), _loc);

		auto bareBlock = awst::makeBlock(_loc);
		if (_receiveFunc)
			bareBlock->body.push_back(makeCall("__receive", _receiveFunc, true));
		else if (_fallbackFunc)
			bareBlock->body.push_back(makeCall("__fallback", _fallbackFunc, true));
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
		dispatchBlock->body.push_back(makeCall("__fallback", _fallbackFunc, false));

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
