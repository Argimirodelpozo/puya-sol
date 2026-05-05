/// @file UrosSplitter.cpp

#include "splitter/UrosSplitter.h"

#include "Logger.h"
#include "awst/WType.h"
#include "builder/sol-types/TypeCoercion.h"
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

/// AVM TypeEnum constant for ApplicationCall (`appl`).
constexpr int TXN_TYPE_APPL = 6;

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

/// Stub body for chunks' non-group methods: just return the type's
/// default value. (Chunks are only invoked when their own group's
/// method is requested via the orch's selector→idx routing, so
/// non-group selectors can't be reached on chunks under normal
/// flow. Body is just a typed default to keep the dispatch table
/// well-typed.)
std::shared_ptr<awst::Block> makeStubBody(
	awst::WType const* _ret, awst::SourceLocation const& _loc)
{
	auto block = awst::makeBlock(_loc);
	auto retVal = makeDefaultValue(_ret, _loc);
	auto ret = awst::makeReturnStatement(std::move(retVal), _loc);
	block->body.push_back(std::move(ret));
	return block;
}

/// Owned dynamically-created WTypes (WTuple instances built for stub
/// bodies). The splitter has no TypeMapper to register against, so we
/// keep a process-lifetime cache here. Sufficient since the splitter
/// is invoked once per puya-sol run.
inline std::vector<std::unique_ptr<awst::WType>>& ownedSplitterTypes()
{
	static std::vector<std::unique_ptr<awst::WType>> instance;
	return instance;
}

template <typename T, typename... Args>
awst::WType const* makeOwnedType(Args&&... _args)
{
	auto p = std::make_unique<T>(std::forward<Args>(_args)...);
	auto* raw = p.get();
	ownedSplitterTypes().push_back(std::move(p));
	return raw;
}

/// Build the ARC4 method-selector signature for a contract method by
/// reading its arg + return wtypes. Format: `name(t1,t2,...)retType`.
/// puya's MethodConstant takes this string and emits the 4-byte
/// sha512_256 selector.
std::string buildSelectorSig(awst::ContractMethod const& _m)
{
	auto wtypeName = [](awst::WType const* _t) -> std::string {
		// wtypeToABIName covers most cases; handle a couple of
		// puya-sol-specific names that diverge from the ARC4 ABI
		// surface (selectors must match exactly what the chunk's
		// dispatch table emits, which is determined by the
		// chunk-side wtype).
		if (_t == awst::WType::accountType()) return "address";
		if (_t->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bw = static_cast<awst::BytesWType const*>(_t);
			if (bw->length().has_value())
				return "byte[" + std::to_string(*bw->length()) + "]";
		}
		return builder::TypeCoercion::wtypeToABIName(_t);
	};
	std::string s = _m.memberName + "(";
	bool first = true;
	for (auto const& a : _m.args)
	{
		if (!first) s += ",";
		s += wtypeName(a.wtype);
		first = false;
	}
	s += ")";
	auto const* ret = _m.returnType;
	if (!ret || ret == awst::WType::voidType())
		s += "void";
	else if (auto const* tup = dynamic_cast<awst::WTuple const*>(ret))
	{
		s += "(";
		bool firstR = true;
		for (auto const* t : tup->types())
		{
			if (!firstR) s += ",";
			s += wtypeName(t);
			firstR = false;
		}
		s += ")";
	}
	else
		s += wtypeName(ret);
	return s;
}

/// Encoded size of an ARC4 / native AWST type, in bytes. Returns 0
/// for variable-size or unsupported types — the caller falls back
/// to a single-bytes pass-through in that case.
int fixedEncodedSize(awst::WType const* _t)
{
	if (_t == awst::WType::biguintType()) return 32;
	if (_t == awst::WType::uint64Type()) return 8;
	if (_t == awst::WType::boolType()) return 1;
	if (_t == awst::WType::accountType()) return 32;
	if (auto const* bw = dynamic_cast<awst::BytesWType const*>(_t))
		if (bw->length().has_value())
			return static_cast<int>(bw->length().value());
	if (_t->kind() == awst::WTypeKind::ARC4UIntN)
	{
		auto const* uw = static_cast<awst::ARC4UIntN const*>(_t);
		return uw->n() / 8;
	}
	return 0;
}

/// Decode a single ABI-encoded scalar at fixed offset from the
/// stripped return bytes. Used by `decodeFromBytes` for tuple field
/// decoding. Returns nullptr if the field type is not fixed-encoding.
std::shared_ptr<awst::Expression> decodeScalarAt(
	std::shared_ptr<awst::Expression> _bytes,
	awst::WType const* _t,
	int _offset,
	awst::SourceLocation const& _loc)
{
	int size = fixedEncodedSize(_t);
	if (size == 0)
		return nullptr;

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(std::move(_bytes));
	extract->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_offset), _loc));
	extract->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(size), _loc));

	if (_t == awst::WType::biguintType())
		return awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
	if (_t == awst::WType::uint64Type())
		return awst::makeBtoi(std::move(extract), _loc);
	if (_t == awst::WType::boolType())
	{
		auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), _loc);
		getbit->stackArgs.push_back(std::move(extract));
		getbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		auto cmp = awst::makeNumericCompare(
			std::move(getbit), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", _loc), _loc);
		return cmp;
	}
	if (_t == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(extract), awst::WType::accountType(), _loc);
	// ARC4 native types (incl. ARC4UIntN, ARC4StaticArray, ...): the
	// extracted slice IS the ARC4 encoding for fixed-width types.
	// Reinterpret to the declared wtype.
	if (_t->kind() == awst::WTypeKind::ARC4UIntN
		|| _t->kind() == awst::WTypeKind::ARC4StaticArray
		|| _t->kind() == awst::WTypeKind::ARC4Struct)
		return awst::makeReinterpretCast(std::move(extract), _t, _loc);
	// Fixed-bytes (BytesWType with length): keep as raw bytes (extracted slice).
	return std::move(extract);
}

/// Decode the ABI-encoded return bytes (post 4-byte-prefix strip)
/// into the method's declared return type. Mirrors the decode logic
/// used by `SolExternalCall::submitAndReturn` for cross-contract
/// calls (same need: bytes → typed value).
std::shared_ptr<awst::Expression> decodeFromBytes(
	std::shared_ptr<awst::Expression> _bytes,
	awst::WType const* _ret,
	awst::SourceLocation const& _loc)
{
	if (!_ret || _ret == awst::WType::voidType())
		return nullptr;
	if (_ret == awst::WType::biguintType())
		return awst::makeReinterpretCast(std::move(_bytes), awst::WType::biguintType(), _loc);
	if (_ret == awst::WType::uint64Type())
		return awst::makeBtoi(std::move(_bytes), _loc);
	if (_ret == awst::WType::boolType())
	{
		auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), _loc);
		getbit->stackArgs.push_back(std::move(_bytes));
		getbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		auto cmp = awst::makeNumericCompare(
			std::move(getbit), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", _loc), _loc);
		return cmp;
	}
	if (_ret == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(_bytes), awst::WType::accountType(), _loc);

	// WTuple (puya-sol synthesizes these for multi-return Solidity
	// methods, e.g. `getAccessReturn`): per-field decode via
	// SingleEvaluation + extract3 + per-type cast. Reinterpret cast
	// of bytes→WTuple is rejected by puya — we have to walk the
	// fields manually.
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_ret))
	{
		auto singleBytes = std::make_shared<awst::SingleEvaluation>();
		singleBytes->sourceLocation = _loc;
		singleBytes->wtype = awst::WType::bytesType();
		singleBytes->source = std::move(_bytes);
		static int seCounter = 0;
		singleBytes->id = ++seCounter;

		auto tuple = std::make_shared<awst::TupleExpression>();
		tuple->sourceLocation = _loc;
		tuple->wtype = _ret;

		int offset = 0;
		for (auto const* fieldType : tup->types())
		{
			auto decoded = decodeScalarAt(singleBytes, fieldType, offset, _loc);
			if (!decoded)
			{
				// Unknown / variable-encoding field — fall back to
				// putting the whole remaining bytes here. Best-effort.
				tuple->items.push_back(singleBytes);
				break;
			}
			tuple->items.push_back(std::move(decoded));
			offset += fixedEncodedSize(fieldType);
		}
		return tuple;
	}

	// ARC4 native types and aggregates: the stripped bytes are already
	// the ARC4 encoding, so a reinterpret yields the right typed value
	// for everything algopy/puya treats structurally (ARC4UIntN,
	// ARC4StaticArray, ARC4Struct, ARC4Tuple, etc.). Bytes / strings
	// are already raw, also reinterpret.
	return awst::makeReinterpretCast(std::move(_bytes), _ret, _loc);
}

/// Forwarding stub body for main: inner-calls the orch with the
/// user's (selector + args) tagged onto the dispatch selector. The
/// orch then runs the install→call→restore dance against __storage
/// and returns __storage's last_log, which we decode to the method's
/// declared return type.
///
/// ApplicationArgs layout on the inner call:
///   [0] = orch's `dispatch()` selector  (ABI router match)
///   [1] = THIS method's selector  (user's intent)
///   [2..N+1] = forwarded from `Txn.ApplicationArgs[1..N]`
std::shared_ptr<awst::Block> makeForwardingStubBody(
	awst::ContractMethod const& _m,
	awst::SourceLocation const& _loc)
{
	auto block = awst::makeBlock(_loc);

	// Build the inner-txn ApplicationArgs tuple.
	auto argsTuple = std::make_shared<awst::TupleExpression>();
	argsTuple->sourceLocation = _loc;

	// [0]: orch.dispatch() ARC4 selector. MethodConstant lets puya
	// compute the 4-byte selector from the canonical signature.
	auto dispatchSel = std::make_shared<awst::MethodConstant>();
	dispatchSel->sourceLocation = _loc;
	dispatchSel->wtype = awst::WType::bytesType();
	dispatchSel->value = "dispatch()byte[]";
	argsTuple->items.push_back(std::move(dispatchSel));

	// [1]: this method's ARC4 selector. The orch reads this from its
	// own ApplicationArgs[1], looks up the matching chunk, and
	// forwards the call.
	auto userSel = std::make_shared<awst::MethodConstant>();
	userSel->sourceLocation = _loc;
	userSel->wtype = awst::WType::bytesType();
	userSel->value = buildSelectorSig(_m);
	argsTuple->items.push_back(std::move(userSel));

	// [2..N+1]: forward Txn.ApplicationArgs[1..N] verbatim. Reading
	// raw bytes via `txna ApplicationArgs i` is cheaper than
	// re-encoding the typed args puya already decoded for our
	// signature — and the chunk's ABI router will decode them again
	// from the inner call's ApplicationArgs anyway.
	//
	// KNOWN LIMITATION: `Txn.Sender` inside a split method body
	// resolves to the ORCH's app account, not the user's address,
	// because the chunk runs as an inner txn from orch. Auth-checking
	// flows (msg.sender comparisons, AccessManaged.canConsume, etc.)
	// will see orch as the caller. Sender-forwarding is left as a
	// follow-up; the trailing-arg approach was rejected, packed-into-
	// selector needs router rewrites in puya backend.
	for (size_t i = 0; i < _m.args.size(); ++i)
	{
		auto txna = awst::makeIntrinsicCall("txna", awst::WType::bytesType(), _loc);
		txna->immediates = {std::string("ApplicationArgs"), int(i + 1)};
		argsTuple->items.push_back(std::move(txna));
	}

	// WTuple type for the ApplicationArgs slot.
	std::vector<awst::WType const*> argTypes(
		argsTuple->items.size(), awst::WType::bytesType());
	argsTuple->wtype = makeOwnedType<awst::WTuple>(std::move(argTypes), std::nullopt);

	// Build CreateInnerTransaction(appl, app_id=orch, args=tuple, fee=0).
	static awst::WInnerTransactionFields s_applFieldsType(
		static_cast<int>(TXN_TYPE_APPL));
	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_applFieldsType;
	create->fields["TypeEnum"] = awst::makeIntegerConstant(
		std::to_string(TXN_TYPE_APPL), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
	auto orchTmpl = std::make_shared<awst::TemplateVar>();
	orchTmpl->sourceLocation = _loc;
	orchTmpl->wtype = awst::WType::uint64Type();
	orchTmpl->name = "TMPL_UROS_ORCH_APP_ID";
	create->fields["ApplicationID"] = std::move(orchTmpl);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	// Wrap in SubmitInnerTransaction.
	static awst::WInnerTransaction s_applTxnType(
		static_cast<int>(TXN_TYPE_APPL));
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_applTxnType;
	submit->itxns.push_back(std::move(create));

	// Void return: just submit and return.
	if (!_m.returnType || _m.returnType == awst::WType::voidType())
	{
		block->body.push_back(awst::makeExpressionStatement(std::move(submit), _loc));
		block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
		return block;
	}

	// Non-void: submit, then read inner.LastLog, strip 4-byte ABI
	// prefix, decode to the declared return type.
	block->body.push_back(awst::makeExpressionStatement(std::move(submit), _loc));

	auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), _loc);
	readLog->immediates = {std::string("LastLog")};

	auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
	stripPrefix->sourceLocation = _loc;
	stripPrefix->opCode = "extract";
	stripPrefix->immediates = {4, 0};
	stripPrefix->wtype = awst::WType::bytesType();
	stripPrefix->stackArgs.push_back(std::move(readLog));

	auto decoded = decodeFromBytes(std::move(stripPrefix), _m.returnType, _loc);
	block->body.push_back(awst::makeReturnStatement(std::move(decoded), _loc));
	return block;
}

/// Deep-clone a ContractMethod's args + signature. `_forwarding`
/// chooses the body shape: forwarding (main's stubs, inner-call
/// orch) vs default-value (chunks' non-group stubs).
awst::ContractMethod cloneStubbed(awst::ContractMethod const& _m, bool _forwarding = false)
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
	stub.body = _forwarding
		? makeForwardingStubBody(_m, _m.sourceLocation)
		: makeStubBody(_m.returnType, _m.sourceLocation);
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

	auto block = awst::makeBlock(_loc);
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
	// stubbed with a forwarding body that inner-calls orch.dispatch
	// with the user's selector + raw ApplicationArgs. Non-split
	// methods keep their real bodies (callable directly on main —
	// useful for getters that don't need the dance).
	//
	// __delegate_update IS kept on main: at deploy time the harness
	// uses main's bytecode for __storage's initial program (so
	// AppCreate runs the user contract's state-var inits on
	// __storage), then UpdateApplications __storage to the thin
	// admit-update default. That UpdateApplication call lands on
	// main's __delegate_update entry point.
	auto mainContract = shallowCloneContract(*primary);
	for (auto& m : mainContract->methods)
	{
		if (appliedAll.count(m.memberName))
			m = cloneStubbed(m, /*forwarding=*/true);
	}
	mainContract->methods.push_back(
		makeDelegateUpdateMethod(primary->id, primary->sourceLocation));

	// Build N chunk contracts. Each chunk_i:
	//  - same surface as primary (same selectors, same state schema)
	//  - REAL bodies for its group's methods
	//  - STUB bodies (default-value return) for everything else;
	//    those selectors are routed to OTHER chunks by orch's
	//    csel→idx map, so they're never invoked when this chunk is
	//    installed on __storage
	//  - synthetic __delegate_update to admit orch's UpdateApplication
	//    inner txns when restoring __storage between calls
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
			// Chunks stub EVERY method that isn't in their own group:
			// orch routes each selector to a single chunk, so chunks
			// only ever execute their own group's methods. Stubbing
			// non-group methods (incl. non-split ones) keeps each
			// chunk small.
			if (!myMethods.count(m.memberName))
				m = cloneStubbed(m, /*forwarding=*/false);
		}
		chunkContract->methods.push_back(
			makeDelegateUpdateMethod(primary->id, primary->sourceLocation));

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
