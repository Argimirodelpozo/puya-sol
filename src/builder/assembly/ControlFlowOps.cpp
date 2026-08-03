/// @file ControlFlowOps.cpp
/// Yul control-flow translation: `if`, `for`, `break`, `continue`,
/// `leave`, `switch`. Extracted from StatementOps.cpp; the
/// `buildStatement` dispatcher there delegates each constructor's
/// variant arm to one of these.

#include "builder/assembly/AssemblyBuilder.h"

#include <libsolutil/Numeric.h>

namespace puyasol::builder
{

void AssemblyBuilder::drainPendingStatements(
	std::vector<std::shared_ptr<awst::Statement>>& _out, size_t _from)
{
	for (size_t i = _from; i < m_pendingStatements.size(); ++i)
		_out.push_back(std::move(m_pendingStatements[i]));
	m_pendingStatements.resize(_from);
}

void AssemblyBuilder::buildIfStatement(
	solidity::yul::If const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_node.debugData);

	// Revert-ONLY body (SafeCast/require pattern): `if cond { revert(...) }`
	// collapses to assert(!cond) to avoid puya DCE. Require the body to be
	// EXACTLY a single revert — the old code set the flag if ANY top-level
	// statement was a revert, dropping everything else (a preceding
	// conditional `leave`, or the `mstore(selector) mstore(data)` that builds
	// a custom-error payload), so `if c { if x { leave } revert() }` reverted
	// even when x was set.
	bool isRevertBody = false;
	if (_node.body.statements.size() == 1)
	{
		if (auto const* exprStmt = std::get_if<solidity::yul::ExpressionStatement>(
				&_node.body.statements[0]))
			if (auto const* funcCall = std::get_if<solidity::yul::FunctionCall>(&exprStmt->expression))
				if (getFunctionName(funcCall->functionName) == "revert")
					isRevertBody = true;
	}

	// Condition may produce pending statements; drain before the if (same as buildForLoop).
	if (isRevertBody)
	{
		size_t pendingBefore = m_pendingStatements.size();
		auto cond = ensureBool(buildExpression(*_node.condition), loc);
		drainPendingStatements(_out, pendingBefore);
		auto notCond = awst::makeNot(std::move(cond), loc);

		_out.push_back(awst::makeExpressionStatement(awst::makeAssert(std::move(notCond), loc, "revert"), loc));
	}
	else
	{
		size_t pendingBefore = m_pendingStatements.size();
		auto cond = ensureBool(buildExpression(*_node.condition), loc);
		drainPendingStatements(_out, pendingBefore);

		auto ifBlock = awst::makeBlock(loc);
		// Don't latch m_haltEmitted for a conditional body: post-block code stays reachable.
		bool savedHalt = m_haltEmitted;
		for (auto const& innerStmt: _node.body.statements)
			buildStatement(innerStmt, ifBlock->body);
		m_haltEmitted = savedHalt;
		// Memory-content constants recorded inside the conditional body must not
		// fold in code after the if.
		invalidateMemConstants();

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
	for (auto const& preStmt: _node.pre.statements)
		buildStatement(preStmt, _out);

	// Cond/body re-execute per iteration: memory-content constants from before
	// the loop (or its pre) must not fold inside it.
	invalidateMemConstants();

	// Condition may produce pending statements (e.g. sideeffect() inside cond);
	// they must run before every check, not leak into the body.
	size_t pendingBefore = m_pendingStatements.size();
	auto cond = ensureBool(buildExpression(*_node.condition), loc);
	std::vector<std::shared_ptr<awst::Statement>> condStmts;
	for (size_t i = pendingBefore; i < m_pendingStatements.size(); ++i)
		condStmts.push_back(std::move(m_pendingStatements[i]));
	m_pendingStatements.resize(pendingBefore);

	auto* savedPost = m_forLoopPost;
	m_forLoopPost = &_node.post.statements;

	auto body = awst::makeBlock(loc);
	bool savedHalt = m_haltEmitted; // loop body halts are conditional
	for (auto const& bodyStmt: _node.body.statements)
		buildStatement(bodyStmt, body->body);
	// Post statements at end of body (normal iteration path)
	for (auto const& postStmt: _node.post.statements)
		buildStatement(postStmt, body->body);
	m_haltEmitted = savedHalt;

	m_forLoopPost = savedPost;
	// Body/post recordings must not survive the loop.
	invalidateMemConstants();

	if (condStmts.empty())
	{
		_out.push_back(awst::makeWhileLoop(std::move(cond), std::move(body), loc));
	}
	else
	{
		// Side-effecting condition: while(true){ <cond-stmts>; if(!cond) break; body; post }
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
	// Yul `continue` jumps to the post expression, not the condition.
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
	// Inlined Yul functions are wrapped in `while true {…break}`;
	// `leave` breaks out. Outside an inlined function it's a no-op.
	if (m_inlineDepth > 0)
		_out.push_back(awst::makeLoopExit(makeLoc(_node.debugData)));
}

void AssemblyBuilder::buildSwitchStatement(
	solidity::yul::Switch const& _node,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_node.debugData);
	size_t pendingBefore = m_pendingStatements.size();
	auto switchExpr = buildExpression(*_node.expression);
	drainPendingStatements(_out, pendingBefore);

	// Widen a uint64-natured scrutinee. Several builtins return uint64 by this
	// codebase's "returns uint64; consumer coerces" convention (returndatasize,
	// gas, timestamp) — but the case labels below are built as 256-bit Yul
	// values, and puya rejects the pair outright with "Switch cases types
	// mismatch with value to match". The switch IS the consumer, so it coerces
	// here; biguint then takes the normalised 32-byte match path used by every
	// other scrutinee. `switch returndatasize()` is Gnosis GPv2SafeERC20's
	// non-standard-ERC20 probe, vendored by Aave and CoW.
	if (switchExpr && switchExpr->wtype && switchExpr->wtype->name() != "bool")
		switchExpr = ensureBiguint(std::move(switchExpr), loc);

	// AVM `match` does exact byte comparison; ARC4 uint256 decodes to 32-byte biguint.
	// Normalize both scrutinee and case constants to 32-byte big-endian BytesConstants.
	bool useBytesMatch = switchExpr->wtype
		&& switchExpr->wtype->name() == "biguint";
	bool useBoolMatch = switchExpr->wtype
		&& switchExpr->wtype->name() == "bool";

	auto switchNode = std::make_shared<awst::Switch>();
	switchNode->sourceLocation = loc;

	if (useBytesMatch)
	{
		auto cast = awst::makeAsBytes(switchExpr, loc);
		// uint512 mapping → 64-byte biguint; zero-extend to ≥32 then take last 32.
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

	bool savedHalt = m_haltEmitted; // switch-case halts are conditional
	for (auto const& yulCase: _node.cases)
	{
		// Each case body starts fresh: recordings from a SIBLING case (translated
		// just before) never execute on this case's path.
		invalidateMemConstants();
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
	m_haltEmitted = savedHalt;
	// Case-body recordings are conditional — must not fold after the switch.
	invalidateMemConstants();

	_out.push_back(std::move(switchNode));
}

} // namespace puyasol::builder
