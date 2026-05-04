/// @file UrosSplitter.cpp

#include "splitter/UrosSplitter.h"

#include "Logger.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <fstream>

namespace puyasol::splitter
{

namespace fs = boost::filesystem;
using njson = nlohmann::ordered_json;

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

// Forward decl — needed because makeDefaultValue recurses into struct
// fields, which themselves can be any wtype.
std::shared_ptr<awst::Expression> makeDefaultValue(
	awst::WType const* _t, awst::SourceLocation const& _loc);

std::shared_ptr<awst::Expression> makeDefaultValue(
	awst::WType const* _t, awst::SourceLocation const& _loc)
{
	if (!_t || _t == awst::WType::voidType())
		return nullptr;
	if (_t == awst::WType::uint64Type())
		return awst::makeIntegerConstant("0", _loc, awst::WType::uint64Type());
	if (_t == awst::WType::biguintType())
		return awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());
	if (_t == awst::WType::boolType())
		return awst::makeBoolConstant(false, _loc);
	if (_t == awst::WType::accountType())
	{
		std::vector<uint8_t> zeros(32, 0);
		auto bc = awst::makeBytesConstant(std::move(zeros), _loc);
		return awst::makeReinterpretCast(std::move(bc), awst::WType::accountType(), _loc);
	}
	// ARC4Struct (incl. puya-sol's synthesized `<method>Return` structs
	// for multi-value Solidity returns): build a NewStruct with each
	// field's default. Empty-bytes reinterpret-cast doesn't work for
	// these — puya rejects with "unsupported type cast (from: bytes,
	// to: <synthName>)".
	if (auto const* sct = dynamic_cast<awst::ARC4Struct const*>(_t))
	{
		auto ns = std::make_shared<awst::NewStruct>();
		ns->sourceLocation = _loc;
		ns->wtype = _t;
		for (auto const& [fname, ftype] : sct->fields())
		{
			auto fv = makeDefaultValue(ftype, _loc);
			if (!fv)
				fv = awst::makeReinterpretCast(
					awst::makeBytesConstant({}, _loc), ftype, _loc);
			ns->values[fname] = std::move(fv);
		}
		return ns;
	}
	// WTuple: puya-sol synthesizes these for multi-return Solidity
	// methods (`getAccess` returns a 4-tuple → `getAccessReturn`
	// WTuple). Build a TupleExpression with each component's default.
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_t))
	{
		auto te = std::make_shared<awst::TupleExpression>();
		te->sourceLocation = _loc;
		te->wtype = _t;
		for (auto const* ft : tup->types())
		{
			auto fv = makeDefaultValue(ft, _loc);
			if (!fv)
				fv = awst::makeReinterpretCast(
					awst::makeBytesConstant({}, _loc), ft, _loc);
			te->items.push_back(std::move(fv));
		}
		return te;
	}
	// Fallback: empty bytes reinterpret. Works for bytes / strings /
	// ARC4UIntN / dynamic arrays etc. — puya treats an empty literal as
	// the canonical "zero" for those types.
	auto bc = awst::makeBytesConstant(std::vector<uint8_t>{}, _loc);
	return awst::makeReinterpretCast(std::move(bc), _t, _loc);
}

/// Name of the shared orc-guard helper method. Each split contract
/// (main + every chunk) gets a private instance method by this name
/// that performs the 4 orc-guards. Stubbed methods call into it
/// instead of inlining all 4 guards — collapses ~150B of guard code
/// per stub down to a ~10B callsub. For AME this saves ~3 KB on main.
constexpr char ORC_GUARD_NAME[] = "__uros_orc_guard";

awst::ContractMethod makeOrcGuardMethod(
	std::string const& _cref, awst::SourceLocation const& _loc)
{
	awst::ContractMethod m;
	m.sourceLocation = _loc;
	m.cref = _cref;
	m.memberName = ORC_GUARD_NAME;
	m.returnType = awst::WType::voidType();
	// arc4MethodConfig stays nullopt — not exposed via ABI dispatch.
	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = _loc;
	for (auto& g : makeOrcGuardStatements(_loc))
		block->body.push_back(std::move(g));
	// Bare `return;` to satisfy puya's terminator requirement on
	// void-returning subroutines.
	block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	m.body = std::move(block);
	return m;
}

/// Stub body: `this.__uros_orc_guard(); return <default>;`
std::shared_ptr<awst::Block> makeStubBody(
	awst::WType const* _ret, awst::SourceLocation const& _loc)
{
	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = _loc;

	// `this.__uros_orc_guard()` — InstanceMethodTarget call to the
	// shared helper. The contract owning this stub method must also
	// carry the helper (added by split() to mainContract + each chunk).
	auto callExpr = std::make_shared<awst::SubroutineCallExpression>();
	callExpr->sourceLocation = _loc;
	callExpr->wtype = awst::WType::voidType();
	callExpr->target = awst::InstanceMethodTarget{ORC_GUARD_NAME};
	block->body.push_back(awst::makeExpressionStatement(std::move(callExpr), _loc));

	auto retVal = makeDefaultValue(_ret, _loc);
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
	std::vector<std::set<std::string>> const& _splitGroups)
{
	Result out;

	auto primary = findPrimaryContract(_roots);
	if (!primary)
	{
		Logger::instance().warning("--uros-splitter: no primary Contract found in AWST; nothing to split");
		out.mainRoots = _roots;
		return out;
	}

	// Map memberName → index in primary->methods, for fast lookup.
	std::set<std::string> present;
	for (auto const& m : primary->methods)
		present.insert(m.memberName);

	// Filter each group to names actually present in the primary
	// contract, AND check no name appears in two groups (every split
	// method must belong to exactly one chunk).
	std::vector<std::vector<std::string>> appliedPerGroup(_splitGroups.size());
	std::set<std::string> seenAcrossGroups;
	for (size_t gi = 0; gi < _splitGroups.size(); ++gi)
	{
		for (auto const& n : _splitGroups[gi])
		{
			if (!present.count(n))
			{
				Logger::instance().warning(
					"--uros-splitter: '" + n + "' not found in contract '"
					+ primary->name + "', skipping");
				continue;
			}
			if (!seenAcrossGroups.insert(n).second)
			{
				Logger::instance().error(
					"--uros-splitter: '" + n + "' appears in multiple chunk "
					"groups — every split method must belong to exactly one chunk");
				return out;
			}
			appliedPerGroup[gi].push_back(n);
		}
	}
	std::set<std::string> appliedAll(seenAcrossGroups);

	if (appliedAll.empty())
	{
		Logger::instance().warning("--uros-splitter: no matching methods to split; emitting only the main contract");
		out.mainRoots = _roots;
		return out;
	}

	// Build mainContract: every split method (across all groups) is
	// stubbed. Non-split methods keep their real bodies. Plus a
	// synthetic __delegate_update to admit the dance's swap-in.
	auto mainContract = shallowCloneContract(*primary);
	for (auto& m : mainContract->methods)
	{
		if (appliedAll.count(m.memberName))
			m = cloneStubbed(m);
	}
	mainContract->methods.push_back(
		makeDelegateUpdateMethod(primary->id, primary->sourceLocation));
	mainContract->methods.push_back(
		makeOrcGuardMethod(primary->id, primary->sourceLocation));

	// Build N chunk contracts. Each chunk_i:
	//  - same surface as main (same selectors, same state schema)
	//  - REAL bodies for its group's methods
	//  - STUB bodies for split methods OUTSIDE its group (so the chunk
	//    is small while still carrying the dispatch table)
	//  - REAL bodies for non-split methods (they may be called
	//    internally by the chunk's split methods — same constraint as
	//    the original single-helper design)
	//  - synthetic __delegate_update to admit step 3's restore
	for (size_t gi = 0; gi < _splitGroups.size(); ++gi)
	{
		std::set<std::string> myMethods(
			appliedPerGroup[gi].begin(), appliedPerGroup[gi].end());

		auto chunkContract = shallowCloneContract(*primary);
		std::string suffix = "__chunk_" + std::to_string(gi);
		chunkContract->name = primary->name + suffix;
		chunkContract->id = primary->id + suffix;
		// Methods carry the ORIGINAL cref — prepend primary->id so puya's
		// resolve_contract_method (walks [id, ...mro], matches cref) can
		// find them.
		chunkContract->methodResolutionOrder.insert(
			chunkContract->methodResolutionOrder.begin(), primary->id);

		for (auto& m : chunkContract->methods)
		{
			// Stub the methods in OTHER chunks' groups; keep this chunk's
			// real bodies and all non-split methods real.
			bool isSplitButNotMine =
				appliedAll.count(m.memberName) && !myMethods.count(m.memberName);
			if (isSplitButNotMine)
				m = cloneStubbed(m);
		}
		chunkContract->methods.push_back(
			makeDelegateUpdateMethod(primary->id, primary->sourceLocation));
		chunkContract->methods.push_back(
			makeOrcGuardMethod(primary->id, primary->sourceLocation));

		// Build this chunk's full root set: all roots, with the primary
		// contract substituted for chunkContract.
		Chunk chunk;
		for (auto const& r : _roots)
		{
			if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
			{
				if (c.get() == primary.get())
					chunk.roots.push_back(chunkContract);
				else
					chunk.roots.push_back(r);
			}
			else
			{
				chunk.roots.push_back(r);
			}
		}
		chunk.appliedNames = std::move(appliedPerGroup[gi]);
		out.chunks.push_back(std::move(chunk));
	}

	// mainRoots: replace primary with mainContract; pass all other
	// roots through unchanged.
	for (auto const& r : _roots)
	{
		if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
		{
			if (c.get() == primary.get())
				out.mainRoots.push_back(mainContract);
			else
				out.mainRoots.push_back(r);
		}
		else
		{
			out.mainRoots.push_back(r);
		}
	}

	Logger::instance().info(
		"--uros-splitter: " + std::to_string(out.chunks.size())
		+ " chunk(s) carrying " + std::to_string(appliedAll.size())
		+ " method(s) of '" + primary->name + "'");

	return out;
}

std::vector<UrosSplitter::ChunkPaths> UrosSplitter::emitChunkAwsts(
	std::string const& _outputDir,
	Result const& _result,
	int _optimizationLevel,
	bool _outputIr,
	int64_t _orchAppId)
{
	std::vector<ChunkPaths> paths;
	paths.reserve(_result.chunks.size());

	for (size_t ci = 0; ci < _result.chunks.size(); ++ci)
	{
		auto const& chunk = _result.chunks[ci];
		ChunkPaths p;
		p.dir = (fs::path(_outputDir) / "__uros_split"
			/ ("chunk_" + std::to_string(ci))).string();
		fs::create_directories(p.dir);

		json::AWSTSerializer serializer;
		auto chunkJson = serializer.serialize(chunk.roots);
		p.awstPath = (fs::path(p.dir) / "awst.json").string();
		{
			std::ofstream out(p.awstPath);
			out << chunkJson.dump(2) << std::endl;
			Logger::instance().info("Wrote: " + p.awstPath);
		}

		std::vector<std::string> chunkContractNames;
		for (auto const& r : chunk.roots)
			if (auto const* c = dynamic_cast<awst::Contract const*>(r.get()))
			{
				chunkContractNames.push_back(c->id);
				// The chunk-renamed contract is identified by the
				// "__chunk_" infix in its name (set by split() above).
				if (p.contractName.empty()
					&& c->name.find("__chunk_") != std::string::npos)
					p.contractName = c->name;
			}

		p.optionsPath = (fs::path(p.dir) / "options.json").string();
		std::set<std::string> noChildren;
		// Chunks emit orc-guards (which reference TMPL_UROS_ORCH_APP_ID)
		// for the methods they stub out (i.e. methods belonging to OTHER
		// chunks). Declare the template var so puya doesn't reject.
		std::map<std::string, int64_t> chunkTemplateVars;
		chunkTemplateVars["UROS_ORCH_APP_ID"] = _orchAppId;
		if (chunkContractNames.size() <= 1)
		{
			std::string nm = chunkContractNames.empty() ? "" : chunkContractNames[0];
			json::OptionsWriter::write(
				p.optionsPath, nm, p.dir,
				_optimizationLevel, _outputIr, noChildren, chunkTemplateVars);
		}
		else
		{
			json::OptionsWriter::writeMultiple(
				p.optionsPath, chunkContractNames, p.dir,
				_optimizationLevel, _outputIr, noChildren, chunkTemplateVars);
		}

		paths.push_back(std::move(p));
	}
	return paths;
}

namespace
{
std::string readHexFile(fs::path const& _p)
{
	if (!fs::exists(_p)) return {};
	std::ifstream f(_p.string(), std::ios::binary);
	std::vector<uint8_t> bytes(
		(std::istreambuf_iterator<char>(f)),
		std::istreambuf_iterator<char>());
	std::string hex;
	hex.reserve(bytes.size() * 2);
	for (auto b : bytes)
	{
		char buf[3];
		snprintf(buf, sizeof(buf), "%02x", b);
		hex += buf;
	}
	return hex;
}
}

int UrosSplitter::compileChunksAndEmitDeployTemplate(
	std::string const& _outputDir,
	std::string const& _mainBareName,
	Result const& _result,
	std::vector<ChunkPaths> const& _chunkPaths,
	std::string const& _puyaPath,
	std::string const& _logLevel)
{
	for (size_t ci = 0; ci < _chunkPaths.size(); ++ci)
	{
		Logger::instance().info(
			"Invoking puya backend for --uros-splitter chunk_"
			+ std::to_string(ci) + "...");
		runner::PuyaRunner chunkRunner;
		chunkRunner.setPuyaPath(_puyaPath);
		int rc = chunkRunner.run(
			_chunkPaths[ci].awstPath, _chunkPaths[ci].optionsPath, _logLevel);
		if (rc != 0)
		{
			Logger::instance().error(
				"--uros-splitter: chunk_" + std::to_string(ci)
				+ " puya run failed");
			return rc;
		}
	}

	njson tmpl = njson::object();
	tmpl["main_contract"] = _mainBareName;
	tmpl["main_approval_hex"] = readHexFile(
		fs::path(_outputDir) / (_mainBareName + ".approval.bin"));
	tmpl["main_clear_hex"] = readHexFile(
		fs::path(_outputDir) / (_mainBareName + ".clear.bin"));

	njson chunksArr = njson::array();
	for (size_t ci = 0; ci < _result.chunks.size(); ++ci)
	{
		njson c = njson::object();
		c["name"] = _chunkPaths[ci].contractName;
		c["methods"] = _result.chunks[ci].appliedNames;
		c["approval_hex"] = readHexFile(
			fs::path(_chunkPaths[ci].dir)
			/ (_chunkPaths[ci].contractName + ".approval.bin"));
		c["clear_hex"] = readHexFile(
			fs::path(_chunkPaths[ci].dir)
			/ (_chunkPaths[ci].contractName + ".clear.bin"));
		chunksArr.push_back(c);
	}
	tmpl["chunks"] = chunksArr;

	std::string tmplPath = (fs::path(_outputDir) / "deploy.uros.json").string();
	std::ofstream tf(tmplPath);
	tf << tmpl.dump(2);
	Logger::instance().info("Wrote: " + tmplPath);
	return 0;
}

} // namespace puyasol::splitter
