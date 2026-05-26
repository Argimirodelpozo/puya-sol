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
		// No fallback/receive: use the normal pattern `return ARC4Router()`
		// which triggers puya's can_exit_early=True (rejects on no selector match).
		auto routerExpr = awst::makeARC4Router(awst::WType::boolType(), _loc);

		auto routerReturn = awst::makeReturnStatement(routerExpr, _loc);
		_body.body.push_back(routerReturn);
		return;
	}

	// Custom dispatch for fallback/receive.
	// Pattern:
	//   if (NumAppArgs == 0) {
	//     if (receive) call receive; else call fallback;
	//     return true;
	//   }
	//   __did_match = ARC4Router();
	//   if (!__did_match) {
	//     call fallback;  // or reject if no fallback
	//     __did_match = true;
	//   }
	//   return __did_match;
	//
	// Using ARC4Router as an assignment value forces puya's
	// can_exit_early=False mode, so the router returns false on no-match
	// instead of calling err.

	// isBareCall=true → pass empty bytes as the fallback argument
	// isBareCall=false → pass ApplicationArgs[0] (the unmatched data)
	auto makeCall = [&](std::string const& _name,
		solidity::frontend::FunctionDefinition const* _func,
		bool _isBareCall)
		-> std::shared_ptr<awst::Statement>
	{
		auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{_name}, awst::WType::voidType(), _loc);
		// If the function takes a bytes parameter, pass the calldata.
		// Fallback may take `bytes calldata _input`.
		if (_func && _func->parameters().size() == 1)
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

	// Step 1: Bare call check (NumAppArgs == 0).
	// Call receive/fallback and return true — no selector to match.
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

	// Step 2: Non-bare call — run the ARC4 router.
	// Assign result to var (triggers can_exit_early=False in puya).
	std::string matchVarName = "__did_match_routing";
	{
		auto matchVar = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto routerExpr = awst::makeARC4Router(awst::WType::boolType(), _loc);

		auto assignMatch = awst::makeAssignmentStatement(std::move(matchVar), std::move(routerExpr), _loc);
		_body.body.push_back(std::move(assignMatch));
	}

	// Step 3: If no match AND fallback exists, call fallback.
	if (_fallbackFunc)
	{
		auto matchVarRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto notMatch = awst::makeNot(std::move(matchVarRead), _loc);

		auto dispatchBlock = awst::makeBlock(_loc);
		dispatchBlock->body.push_back(makeCall("__fallback", _fallbackFunc, false));

		// Set __did_match = true so the approval returns true.
		auto matchVarWrite = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

		auto assignTrue = awst::makeAssignmentStatement(std::move(matchVarWrite), makeTrueLit(), _loc);
		dispatchBlock->body.push_back(std::move(assignTrue));

		_body.body.push_back(awst::makeIfElse(
			std::move(notMatch), std::move(dispatchBlock), nullptr, _loc));
	}

	// Step 4: return __did_match_routing
	auto finalRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), _loc);

	auto retStmt = awst::makeReturnStatement(std::move(finalRead), _loc);
	_body.body.push_back(std::move(retStmt));
}

} // namespace puyasol::builder
