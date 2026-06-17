#include "builder/contract/SelectorRouter.h"

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
	//   NumAppArgs==0 → bare call → receive/fallback + return true
	//   else → __did_match = ARC4Router() (assignment → can_exit_early=False)
	//          if !__did_match → fallback; __did_match = true
	//          return __did_match

	// isBareCall: pass empty bytes; else pass ApplicationArgs[0].
	auto makeCall = [&](std::string const& _name,
		solidity::frontend::FunctionDefinition const* _func,
		bool _isBareCall)
		-> std::shared_ptr<awst::Statement>
	{
		auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{_name}, awst::WType::voidType(), _loc);
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

		auto stmt = awst::makeExpressionStatement(call, _loc);
		return stmt;
	};

	auto makeTrueLit = [&]() {
		return awst::makeTrue(_loc);
	};

	auto makeReturnTrue = [&]() -> std::shared_ptr<awst::Statement> {
		auto r = awst::makeReturnStatement(makeTrueLit(), _loc);
		return r;
	};

	// Step 1: bare call (NumAppArgs==0).
	{
		auto numAppArgs = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), _loc);

		auto zero = awst::makeZero(_loc);

		auto isBareCall = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Eq, std::move(zero), _loc);

		auto bareBlock = awst::makeBlock(_loc);
		if (_receiveFunc)
			bareBlock->body.push_back(makeCall("__receive", _receiveFunc, true));
		else if (_fallbackFunc)
			bareBlock->body.push_back(makeCall("__fallback", _fallbackFunc, true));
		bareBlock->body.push_back(makeReturnTrue());

		_body.body.push_back(awst::makeIfElse(
			std::move(isBareCall), std::move(bareBlock), nullptr, _loc));
	}

	// Step 2: run ARC4 router; assignment → can_exit_early=False.
	std::string matchVarName = "__did_match_routing";
	{
		auto matchVar = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto routerExpr = awst::makeARC4Router(awst::WType::boolType(), _loc);

		auto assignMatch = awst::makeAssignmentStatement(std::move(matchVar), std::move(routerExpr), _loc);
		_body.body.push_back(std::move(assignMatch));
	}

	// Step 3: no-match + fallback exists → call fallback.
	if (_fallbackFunc)
	{
		auto matchVarRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto notMatch = awst::makeNot(std::move(matchVarRead), _loc);

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
