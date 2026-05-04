/// @file UrosSplitter.cpp

#include "splitter/UrosSplitter.h"

#include "Logger.h"

namespace puyasol::splitter
{

namespace
{

/// Stub body for a method/subroutine of the given return type.
/// Returns a Block containing a single ReturnStatement with a default value
/// of `_ret`. Default is what makeDefaultValue would produce, but inline so
/// we don't pull in StorageMapper here.
std::shared_ptr<awst::Block> makeStubBody(
	awst::WType const* _ret, awst::SourceLocation const& _loc)
{
	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = _loc;

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

/// Find the primary deployable Contract in `_roots` (skip libraries / abstract).
/// Returns the first non-abstract Contract; nullptr if none.
std::shared_ptr<awst::Contract> findPrimaryContract(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	for (auto const& r : _roots)
		if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
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
