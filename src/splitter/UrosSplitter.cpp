/// @file UrosSplitter.cpp

#include "splitter/UrosSplitter.h"

#include "Logger.h"

namespace puyasol::splitter
{

namespace
{

/// ABI selector for the orchestrator's `dispatch()byte[]` method.
/// sha512_256("dispatch()byte[]")[:4] = 0x617da41d.
constexpr uint8_t DISPATCH_SELECTOR[4] = {0x61, 0x7d, 0xa4, 0x1d};

/// AVM TypeEnum constant for ApplicationCall (`appl`).
constexpr int APPL_TYPE_ENUM = 6;

/// Build `txn GroupIndex` reading the current txn's group index.
std::shared_ptr<awst::IntrinsicCall> txnGroupIndex(awst::SourceLocation const& _loc)
{
	auto n = awst::makeIntrinsicCall("txn", awst::WType::uint64Type(), _loc);
	n->immediates = {std::string("GroupIndex")};
	return n;
}

/// Build `txn GroupIndex + 1` (uint64).
std::shared_ptr<awst::Expression> nextGroupIndex(awst::SourceLocation const& _loc)
{
	return awst::makeUInt64BinOp(
		txnGroupIndex(_loc),
		awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant("1", _loc, awst::WType::uint64Type()),
		_loc);
}

/// Build the four "the orc" guard assertions and prepend them to a stub
/// body. Each split method's stub validates that the IMMEDIATELY-NEXT
/// transaction in the group is `<TMPL_UROS_ORCH_APP_ID>.dispatch()` —
/// otherwise the call reverts. Without this guard, calling a stubbed
/// method directly would silently return a default value, fooling the
/// caller into thinking a state mutation occurred.
///
/// Asserts emitted, in order:
///   1. `Txn.GroupIndex + 1 < Global.GroupSize`        (next txn exists)
///   2. `gtxn[next].TypeEnum == 6 (appl)`              (next is app call)
///   3. `gtxn[next].ApplicationID == TMPL_UROS_ORCH_APP_ID`
///                                                     (the right orch)
///   4. `gtxn[next].ApplicationArgs[0] == 0x617da41d`  (calling dispatch)
std::vector<std::shared_ptr<awst::Statement>> makeOrcGuardStatements(
	awst::SourceLocation const& _loc)
{
	using awst::makeIntrinsicCall;
	using awst::makeIntegerConstant;
	using awst::makeNumericCompare;
	using awst::makeAssert;
	using awst::makeExpressionStatement;

	std::vector<std::shared_ptr<awst::Statement>> guards;

	// 1. next_idx < Global.GroupSize
	{
		auto groupSize = makeIntrinsicCall("global", awst::WType::uint64Type(), _loc);
		groupSize->immediates = {std::string("GroupSize")};
		auto cond = makeNumericCompare(
			nextGroupIndex(_loc),
			awst::NumericComparison::Lt,
			std::move(groupSize),
			_loc);
		guards.push_back(makeExpressionStatement(
			makeAssert(std::move(cond), _loc, std::string("uros: no orch txn after stub")),
			_loc));
	}

	// 2. gtxns TypeEnum == 6 (appl)
	{
		auto typeEnum = makeIntrinsicCall("gtxns", awst::WType::uint64Type(), _loc);
		typeEnum->immediates = {std::string("TypeEnum")};
		typeEnum->stackArgs.push_back(nextGroupIndex(_loc));
		auto cond = makeNumericCompare(
			std::move(typeEnum),
			awst::NumericComparison::Eq,
			makeIntegerConstant(std::to_string(APPL_TYPE_ENUM), _loc, awst::WType::uint64Type()),
			_loc);
		guards.push_back(makeExpressionStatement(
			makeAssert(std::move(cond), _loc, std::string("uros: next txn not appl")),
			_loc));
	}

	// 3. gtxns ApplicationID == TemplateVar(UROS_ORCH_APP_ID)
	{
		auto appId = makeIntrinsicCall("gtxns", awst::WType::uint64Type(), _loc);
		appId->immediates = {std::string("ApplicationID")};
		appId->stackArgs.push_back(nextGroupIndex(_loc));

		auto orchTmpl = std::make_shared<awst::TemplateVar>();
		orchTmpl->sourceLocation = _loc;
		orchTmpl->wtype = awst::WType::uint64Type();
		// puya doesn't prepend template_vars_prefix to TemplateVar.name
		// — it expects the full prefixed key, then matches against
		// options.template_variables (which IS prefix-prepended). So we
		// store the fully-qualified "TMPL_UROS_ORCH_APP_ID" here.
		orchTmpl->name = "TMPL_UROS_ORCH_APP_ID";

		auto cond = makeNumericCompare(
			std::move(appId), awst::NumericComparison::Eq, std::move(orchTmpl), _loc);
		guards.push_back(makeExpressionStatement(
			makeAssert(std::move(cond), _loc, std::string("uros: wrong orch app")),
			_loc));
	}

	// 4. gtxnsa ApplicationArgs 0 == 0x617da41d (dispatch selector)
	{
		auto appArg0 = makeIntrinsicCall("gtxnsa", awst::WType::bytesType(), _loc);
		appArg0->immediates = {std::string("ApplicationArgs"), 0};
		appArg0->stackArgs.push_back(nextGroupIndex(_loc));

		std::vector<uint8_t> selBytes(
			DISPATCH_SELECTOR, DISPATCH_SELECTOR + sizeof(DISPATCH_SELECTOR));
		auto selConst = awst::makeBytesConstant(std::move(selBytes), _loc);

		auto cmp = std::make_shared<awst::BytesComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(appArg0);
		cmp->op = awst::EqualityComparison::Eq;
		cmp->rhs = std::move(selConst);

		guards.push_back(makeExpressionStatement(
			makeAssert(std::move(cmp), _loc, std::string("uros: not dispatch selector")),
			_loc));
	}

	return guards;
}

/// Stub body for a method/subroutine of the given return type.
/// Returns a Block containing the orc-guard asserts followed by a single
/// ReturnStatement with a default value of `_ret`.
std::shared_ptr<awst::Block> makeStubBody(
	awst::WType const* _ret, awst::SourceLocation const& _loc)
{
	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = _loc;

	// Inject the four "the orc" guards before the default-value return so
	// direct calls to a stubbed method revert instead of silently no-oping.
	for (auto& g : makeOrcGuardStatements(_loc))
		block->body.push_back(std::move(g));

	std::shared_ptr<awst::Expression> retVal;
	if (!_ret || _ret == awst::WType::voidType())
	{
		// void: bare `return;`
		auto ret = awst::makeReturnStatement(nullptr, _loc);
		block->body.push_back(std::move(ret));
		return block;
	}

	if (_ret == awst::WType::uint64Type())
		retVal = awst::makeIntegerConstant("0", _loc, awst::WType::uint64Type());
	else if (_ret == awst::WType::biguintType())
		retVal = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());
	else if (_ret == awst::WType::boolType())
		retVal = awst::makeBoolConstant(false, _loc);
	else if (_ret == awst::WType::accountType())
	{
		// 32-byte zero, reinterpret as account.
		std::vector<uint8_t> zeros(32, 0);
		auto bc = awst::makeBytesConstant(std::move(zeros), _loc);
		retVal = awst::makeReinterpretCast(std::move(bc), awst::WType::accountType(), _loc);
	}
	else
	{
		// bytes / strings / aggregates / ARC4 — empty bytes value, reinterpret
		// to the declared return type. puya accepts an empty-bytes literal
		// reinterpreted as most aggregate types as a "default zero".
		auto bc = awst::makeBytesConstant(std::vector<uint8_t>{}, _loc);
		retVal = awst::makeReinterpretCast(std::move(bc), _ret, _loc);
	}

	auto ret = awst::makeReturnStatement(std::move(retVal), _loc);
	block->body.push_back(std::move(ret));
	return block;
}

/// Deep-clone a ContractMethod's args + signature; replace the body with a
/// stub matching its declared return type.
awst::ContractMethod cloneStubbed(awst::ContractMethod const& _m)
{
	awst::ContractMethod stub;
	stub.sourceLocation = _m.sourceLocation;
	stub.args = _m.args;
	stub.returnType = _m.returnType;
	stub.documentation = _m.documentation;
	stub.inlineOpt = _m.inlineOpt;
	stub.pure = _m.pure;
	stub.cref = _m.cref;
	stub.memberName = _m.memberName;
	stub.arc4MethodConfig = _m.arc4MethodConfig;
	stub.body = makeStubBody(_m.returnType, _m.sourceLocation);
	return stub;
}

/// Build the synthetic `__delegate_update()void` method that admits
/// OnCompletion=UpdateApplication on the contract. Both main and helper get
/// one — the orchestrator's swap dance targets this selector to splice in
/// the alternate program bytes.
///
/// Body is empty (just a bare return). No sender check is enforced here:
/// the security model defers to the orchestrator pattern (only the
/// orchestrator knows the right `__codebox_*` payloads). Hardening this
/// to `assert(Txn.Sender == TMPL_UROS_ORCHESTRATOR)` is straightforward
/// follow-up work, but adds template-var plumbing.
awst::ContractMethod makeDelegateUpdateMethod(
	std::string const& _cref, awst::SourceLocation const& _loc)
{
	awst::ContractMethod m;
	m.sourceLocation = _loc;
	m.returnType = awst::WType::voidType();
	m.cref = _cref;
	m.memberName = "__delegate_update";

	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = _loc;
	block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	m.body = block;

	awst::ARC4ABIMethodConfig cfg;
	cfg.sourceLocation = _loc;
	// 4 = OnCompletionAction.UpdateApplication
	cfg.allowedCompletionTypes = {4};
	cfg.create = 3;  // ARC4CreateOption::Disallow — never created via this method
	cfg.name = "__delegate_update";
	cfg.readonly = false;
	m.arc4MethodConfig = cfg;
	return m;
}

/// Find the primary deployable Contract in `_roots`.
///
/// AAVE V4 contract files routinely hold a base + a deployable
/// derived contract (e.g. `AccessManagerEnumerable.sol` defines
/// `AccessManager` then `AccessManagerEnumerable`). AWST emits the
/// base first because it's needed for linearization, but the deploy
/// target is the LAST Contract — that's the one that gets the bare
/// filename's appName + the canonical compilation_set entry.
///
/// Solidity's convention (and puya-sol's emission) is "deployable
/// last", so iterate roots in reverse and return the first Contract.
std::shared_ptr<awst::Contract> findPrimaryContract(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	for (auto it = _roots.rbegin(); it != _roots.rend(); ++it)
		if (auto c = std::dynamic_pointer_cast<awst::Contract>(*it))
			return c;
	return nullptr;
}

/// Shallow clone a Contract — copies all fields and methods but preserves
/// shared_ptr identity for the AWST blocks that aren't being modified.
std::shared_ptr<awst::Contract> shallowCloneContract(
	awst::Contract const& _src)
{
	auto out = std::make_shared<awst::Contract>();
	out->sourceLocation = _src.sourceLocation;
	out->id = _src.id;
	out->name = _src.name;
	out->description = _src.description;
	out->methodResolutionOrder = _src.methodResolutionOrder;
	out->approvalProgram = _src.approvalProgram;
	out->clearProgram = _src.clearProgram;
	out->methods = _src.methods;
	out->appState = _src.appState;
	out->stateTotals = _src.stateTotals;
	out->reservedScratchSpace = _src.reservedScratchSpace;
	out->avmVersion = _src.avmVersion;
	return out;
}

} // namespace

UrosSplitter::Result UrosSplitter::split(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots,
	std::set<std::string> const& _splitNames)
{
	Result out;

	auto primary = findPrimaryContract(_roots);
	if (!primary)
	{
		Logger::instance().warning("--uros-splitter: no primary Contract found in AWST; nothing to split");
		out.mainRoots = _roots;
		return out;
	}

	// Determine which names are actually present.
	std::set<std::string> present;
	for (auto const& m : primary->methods)
		present.insert(m.memberName);

	std::set<std::string> applied;
	for (auto const& n : _splitNames)
	{
		if (present.count(n))
			applied.insert(n);
		else
			Logger::instance().warning(
				"--uros-splitter: '" + n + "' not found in contract '"
				+ primary->name + "', skipping");
	}
	out.appliedNames.assign(applied.begin(), applied.end());

	if (applied.empty())
	{
		Logger::instance().warning("--uros-splitter: no matching methods to split; emitting only the main contract");
		out.mainRoots = _roots;
		return out;
	}

	// Build mainContract (split methods → stubs).
	auto mainContract = shallowCloneContract(*primary);
	for (auto& m : mainContract->methods)
	{
		if (applied.count(m.memberName))
			m = cloneStubbed(m);
	}
	// Append the synthetic UpdateApplication-admitting method that the
	// orchestrator's dance targets when swapping in the helper bytes.
	mainContract->methods.push_back(
		makeDelegateUpdateMethod(primary->id, primary->sourceLocation));

	// Build helperContract (kept methods → stubs, split methods → real).
	auto helperContract = shallowCloneContract(*primary);
	helperContract->name = primary->name + "__split";
	helperContract->id = primary->id + "__split";
	// puya's resolve_contract_method walks `[id, ...method_resolution_order]`
	// and matches on `(method.cref == mro_entry)`. Methods carry the ORIGINAL
	// cref (the source's contract id, e.g. "/.../Smoke.sol.Smoke"), so we
	// prepend the original id to the MRO. Without this, helper methods are
	// invisible to the resolver and puya asserts on `m is not None`.
	helperContract->methodResolutionOrder.insert(
		helperContract->methodResolutionOrder.begin(), primary->id);
	// Stub the constructor too — helper is never created, only swapped in
	// via UpdateApplication, which doesn't run the constructor.
	{
		awst::ContractMethod stubbedApproval;
		stubbedApproval.sourceLocation = primary->approvalProgram.sourceLocation;
		stubbedApproval.args = primary->approvalProgram.args;
		stubbedApproval.returnType = primary->approvalProgram.returnType;
		stubbedApproval.cref = primary->approvalProgram.cref + "__split";
		stubbedApproval.memberName = primary->approvalProgram.memberName;
		stubbedApproval.body = std::make_shared<awst::Block>();
		stubbedApproval.body->sourceLocation = primary->approvalProgram.sourceLocation;
		// Approval body for helper: single dispatch over kept methods (stubs)
		// and split methods (real bodies). Use the original program body so
		// the dispatch table stays the same; per-method bodies were already
		// flipped above. Keep `approvalProgram.body` shared with the source
		// — modifications happen in `methods`.
		stubbedApproval.body = primary->approvalProgram.body;
		helperContract->approvalProgram = stubbedApproval;
	}
	// Helper keeps every method's real body (no stubbing). Stubbing the
	// kept methods saves bytes but breaks split-method bodies that call
	// kept methods internally — e.g. Tornado's verifyProof calls
	// verifyingKey() (an internal helper). puya's resolver fails with
	// "unable to resolve function reference" if the target isn't in
	// `methods`. The size win from --uros-splitter comes from MAIN
	// shrinking; helper carrying the full surface is fine because the
	// helper is compiled separately and lives in its own app-side box,
	// not deployed standalone.
	// Helper also needs a __delegate_update method — the dance's revert step
	// (UpdateApplication on main with main's original bytes) lands while
	// main's *current* approval is the helper's bytes. Without an admitting
	// branch on the helper, the revert would fail and the whole atomic dance
	// would unwind.
	helperContract->methods.push_back(
		makeDelegateUpdateMethod(primary->id, primary->sourceLocation));

	// Both root sets keep all subroutines unchanged. Some of those are
	// reachable from split methods, others from kept methods. A future
	// optimisation pass could call-graph-analyse and prune dead
	// subroutines per side; for the prototype we duplicate.
	for (auto const& r : _roots)
	{
		if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
		{
			if (c.get() == primary.get())
			{
				out.mainRoots.push_back(mainContract);
				out.helperRoots.push_back(helperContract);
			}
			else
			{
				// Other contracts (libraries, abstract, sibling deployables)
				// are passed through to both root sets unchanged.
				out.mainRoots.push_back(r);
				out.helperRoots.push_back(r);
			}
		}
		else
		{
			// Subroutine root — duplicate into both sets.
			out.mainRoots.push_back(r);
			out.helperRoots.push_back(r);
		}
	}

	Logger::instance().info(
		"--uros-splitter: applied to " + std::to_string(applied.size())
		+ " method(s) of '" + primary->name + "'");

	return out;
}

} // namespace puyasol::splitter
