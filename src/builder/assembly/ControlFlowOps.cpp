/// @file ControlFlowOps.cpp
/// Yul control-flow translation: `if`, `for`, `break`, `continue`,
/// `leave`, `switch`. Extracted from StatementOps.cpp; the
/// `buildStatement` dispatcher there delegates each constructor's
/// variant arm to one of these.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <libsolutil/Numeric.h>

namespace puyasol::builder
{

void AssemblyBuilder::buildIfStatement(
	solidity::yul::If const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_node.debugData);

	// Check if body is a revert-only block (common SafeCast/require pattern).
	// Emitting assert(NOT(cond)) directly avoids puya DCE of if(cond){assert(false)}.
	bool isRevertBody = false;
	for (auto const& stmt : _node.body.statements)
	{
		if (auto const* exprStmt = std::get_if<solidity::yul::ExpressionStatement>(&stmt))
		{
			if (auto const* funcCall = std::get_if<solidity::yul::FunctionCall>(&exprStmt->expression))
			{
				if (getFunctionName(funcCall->functionName) == "revert")
					isRevertBody = true;
			}
		}
	}

	if (isRevertBody)
	{
		// Emit assert(NOT(condition)) — avoids DCE of if(cond){assert(false)}
		auto cond = ensureBool(buildExpression(*_node.condition), loc);
		auto notCond = awst::makeNot(std::move(cond), loc);

		auto stmt = awst::makeExpressionStatement(awst::makeAssert(std::move(notCond), loc, "revert"), loc);
		_out.push_back(std::move(stmt));
	}
	else
	{
		// Original IfElse path for non-revert if-bodies
		auto cond = ensureBool(buildExpression(*_node.condition), loc);

		auto ifBlock = awst::makeBlock(loc);
		for (auto const& innerStmt: _node.body.statements)
			buildStatement(innerStmt, ifBlock->body);

		_out.push_back(awst::makeIfElse(
			std::move(cond), std::move(ifBlock), nullptr, loc));
	}
}

void AssemblyBuilder::buildForLoop(
	solidity::yul::ForLoop const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_node.debugData);
	// Translate pre block
	for (auto const& preStmt: _node.pre.statements)
		buildStatement(preStmt, _out);

	// Building the condition may produce pending statements —
	// e.g. a side-effecting Yul function call inside the
	// condition (`for {} eq(i, sideeffect()) {} {}`). Those must
	// run before EVERY condition check, including the first. Pull
	// them out here so they don't silently leak into the loop
	// body, where the first check would see stale values.
	size_t pendingBefore = m_pendingStatements.size();
	auto cond = ensureBool(buildExpression(*_node.condition), loc);
	std::vector<std::shared_ptr<awst::Statement>> condStmts;
	for (size_t i = pendingBefore; i < m_pendingStatements.size(); ++i)
		condStmts.push_back(std::move(m_pendingStatements[i]));
	m_pendingStatements.resize(pendingBefore);

	// Set post statements so `continue` can emit them
	auto* savedPost = m_forLoopPost;
	m_forLoopPost = &_node.post.statements;

	auto body = awst::makeBlock(loc);
	for (auto const& bodyStmt: _node.body.statements)
		buildStatement(bodyStmt, body->body);
	// Post statements at end of body (normal iteration path)
	for (auto const& postStmt: _node.post.statements)
		buildStatement(postStmt, body->body);

	m_forLoopPost = savedPost;

	if (condStmts.empty())
	{
		// Pure condition — plain `while (cond) { body; post }`.
		_out.push_back(awst::makeWhileLoop(std::move(cond), std::move(body), loc));
	}
	else
	{
		// Side-effecting condition — restructure as
		//   while (true) { <cond-stmts>; if (!cond) break; body; post }
		// so the condition's side effects run before every check.
		auto outerBody = awst::makeBlock(loc);
		for (auto& cs: condStmts)
			outerBody->body.push_back(std::move(cs));
		auto breakBlock = awst::makeBlock(loc);
		breakBlock->body.push_back(awst::makeLoopExit(loc));
		outerBody->body.push_back(awst::makeIfElse(
			awst::makeNot(std::move(cond), loc),
			std::move(breakBlock), nullptr, loc));
		for (auto& s: body->body)
			outerBody->body.push_back(std::move(s));
		_out.push_back(awst::makeWhileLoop(
			awst::makeTrue(loc), std::move(outerBody), loc));
	}
}

void AssemblyBuilder::buildBreakStatement(
	solidity::yul::Break const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	_out.push_back(awst::makeLoopExit(makeLoc(_node.debugData)));
}

void AssemblyBuilder::buildContinueStatement(
	solidity::yul::Continue const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// In Yul, `continue` jumps to the for-loop's post expression,
	// not the condition. Emit post statements before LoopContinue.
	if (m_forLoopPost)
	{
		for (auto const& postStmt: *m_forLoopPost)
			buildStatement(postStmt, _out);
	}
	_out.push_back(awst::makeLoopContinue(makeLoc(_node.debugData)));
}

void AssemblyBuilder::buildLeaveStatement(
	solidity::yul::Leave const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Leave = early exit from a Yul function. We inline Yul
	// functions wrapped in `while true { … break }`, so a
	// `leave` is just a break out of that wrapper loop.
	// Outside of an inlined function it has no meaningful
	// translation — emit a no-op (Solidity wouldn't even
	// parse this case).
	if (m_inlineDepth > 0)
		_out.push_back(awst::makeLoopExit(makeLoc(_node.debugData)));
}

void AssemblyBuilder::buildSwitchStatement(
	solidity::yul::Switch const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_node.debugData);
	auto switchExpr = buildExpression(*_node.expression);

	// Build AWST Switch node from Yul switch cases.
	// AVM `match` does exact byte comparison. Yul values are u256 (32 bytes),
	// so ARC4 uint256 parameters decode to 32-byte biguint. We must ensure
	// both the switch expression and case constants use the same 32-byte
	// encoding. Cast switch expr to bytes and use 32-byte BytesConstants.
	bool useBytesMatch = switchExpr->wtype
		&& switchExpr->wtype->name() == "biguint";
	bool useBoolMatch = switchExpr->wtype
		&& switchExpr->wtype->name() == "bool";

	auto switchNode = std::make_shared<awst::Switch>();
	switchNode->sourceLocation = loc;

	if (useBytesMatch)
	{
		auto cast = awst::makeAsBytes(switchExpr, loc);

		// Normalise to a fixed 32-byte big-endian encoding —
		// biguint ABI decoding may produce 64-byte values (our
		// uint512 mapping) but the case constants below are 32
		// bytes. Zero-extend to at least 32 bytes, then extract
		// the last 32.
		auto bor = awst::makeZeroExtendToN(std::move(cast), 32, loc);

		auto lenCall = awst::makeLen(bor, loc);

		auto minus = awst::makeUInt64BinOp(std::move(lenCall),
			awst::UInt64BinaryOperator::Sub,
			awst::makeIntegerConstant("32", loc), loc);

		auto width = awst::makeIntegerConstant("32", loc);

		auto extract = awst::makeExtract3(std::move(bor), std::move(minus), std::move(width), loc);
		switchNode->value = std::move(extract);
	}
	else
	{
		switchNode->value = switchExpr;
	}

	for (auto const& yulCase: _node.cases)
	{
		if (!yulCase.value)
		{
			auto caseBlock = awst::makeBlock(makeLoc(yulCase.debugData));
			for (auto const& stmt: yulCase.body.statements)
				buildStatement(stmt, caseBlock->body);
			switchNode->defaultCase = std::move(caseBlock);
		}
		else
		{
			auto caseBlock = awst::makeBlock(makeLoc(yulCase.debugData));
			for (auto const& stmt: yulCase.body.statements)
				buildStatement(stmt, caseBlock->body);

			if (useBytesMatch
				&& yulCase.value->kind == solidity::yul::LiteralKind::Number)
			{
				// 32-byte big-endian BytesConstant matching ARC4 uint256 encoding
				auto const& val = yulCase.value->value.value();
				auto be = solidity::toBigEndian(val);
				switchNode->cases.emplace_back(
					awst::makeBytesConstant(
						std::vector<uint8_t>(be.begin(), be.end()),
						makeLoc(yulCase.value->debugData)),
					std::move(caseBlock));
			}
			else if (useBoolMatch
				&& yulCase.value->kind == solidity::yul::LiteralKind::Number)
			{
				// Convert numeric case to BoolConstant for bool switch
				auto const& val = yulCase.value->value.value();
				switchNode->cases.emplace_back(
					awst::makeBoolConstant(val != 0, makeLoc(yulCase.value->debugData)),
					std::move(caseBlock));
			}
			else
			{
				auto caseVal = buildLiteral(*yulCase.value);
				switchNode->cases.emplace_back(
					std::move(caseVal), std::move(caseBlock));
			}
		}
	}

	_out.push_back(std::move(switchNode));
}

} // namespace puyasol::builder
