/// @file PureHelperExtractor.cpp
/// See header for design overview.

#include "splitter/PureHelperExtractor.h"
#include "splitter/AwstWalker.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::splitter
{

namespace
{

constexpr int TxnTypeAppl = 6;

/// Map a wtype to its canonical ARC4 type name (used inside method
/// signatures fed to MethodConstant — puya turns those into the
/// 4-byte sha512_256 selector at compile time, so the C++ side never
/// needs a hash impl).
std::string arc4TypeName(awst::WType const* _t)
{
	if (!_t || _t == awst::WType::voidType()) return "void";
	if (_t == awst::WType::biguintType()) return "uint256";
	if (_t == awst::WType::uint64Type()) return "uint64";
	if (_t == awst::WType::boolType()) return "bool";
	if (_t == awst::WType::accountType()) return "address";
	if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(_t))
	{
		if (bwt->length().has_value())
			return "byte[" + std::to_string(*bwt->length()) + "]";
		return "byte[]";
	}
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_t))
	{
		std::string s = "(";
		bool first = true;
		for (auto const* ft : tup->types())
		{
			if (!first) s += ",";
			s += arc4TypeName(ft);
			first = false;
		}
		s += ")";
		return s;
	}
	// Fallback: use the wtype's internal name (covers ARC4Struct,
	// ARC4StaticArray, ARC4DynamicArray, etc.).
	if (_t) return _t->name();
	return "?";
}

/// Estimate the static encoded size of a return wtype. Returns 0 if
/// dynamic or unknown. The `LastLog` ABI return channel caps single-
/// log size at AVM MaxLogSize (1024 B) — we surface a warning if a
/// return type doesn't fit.
int staticEncodedSize(awst::WType const* _t)
{
	if (!_t || _t == awst::WType::voidType()) return 0;
	if (_t == awst::WType::biguintType()) return 32;
	if (_t == awst::WType::uint64Type()) return 8;
	if (_t == awst::WType::boolType()) return 1;
	if (_t == awst::WType::accountType()) return 32;
	if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(_t))
		return bwt->length().value_or(0);
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_t))
	{
		int sum = 0;
		for (auto const* ft : tup->types())
		{
			int s = staticEncodedSize(ft);
			if (s == 0) return 0;
			sum += s;
		}
		return sum;
	}
	return 0;
}

/// Build the canonical ABI signature string for a Subroutine.
std::string canonicalSig(awst::Subroutine const& _sub)
{
	std::string sig = _sub.name + "(";
	bool first = true;
	for (auto const& a : _sub.args)
	{
		if (!first) sig += ",";
		sig += arc4TypeName(a.wtype);
		first = false;
	}
	sig += ")" + arc4TypeName(_sub.returnType);
	return sig;
}

/// `txn ApplicationID == 0` (creation guard).
std::shared_ptr<awst::Expression> isCreate(awst::SourceLocation const& _loc)
{
	auto appId = awst::makeIntrinsicCall(
		"txn", awst::WType::uint64Type(), _loc);
	appId->immediates = {std::string("ApplicationID")};
	return awst::makeNumericCompare(
		std::move(appId), awst::NumericComparison::Eq,
		awst::makeIntegerConstant("0", _loc), _loc);
}

/// `txna ApplicationArgs <i>`.
std::shared_ptr<awst::Expression> appArgAt(int _i, awst::SourceLocation const& _loc)
{
	auto call = awst::makeIntrinsicCall("txna", awst::WType::bytesType(), _loc);
	call->immediates = {std::string("ApplicationArgs"), _i};
	return call;
}

/// MethodConstant(sig) — puya resolves to sha512_256(sig)[:4] at
/// compile time. Used both at the helper's selector check and at the
/// caller's ApplicationArgs[0] entry, so they always agree.
std::shared_ptr<awst::Expression> selectorConst(
	std::string const& _sig, awst::SourceLocation const& _loc)
{
	auto mc = std::make_shared<awst::MethodConstant>();
	mc->sourceLocation = _loc;
	mc->wtype = awst::WType::bytesType();
	mc->value = _sig;
	return mc;
}

/// Decode a single fixed-size scalar slice — helper for tuple decode.
/// `_offset`/`_size` specify the slice within `_bytes` (a
/// SingleEvaluation node so the underlying expression isn't re-run).
std::shared_ptr<awst::Expression> decodeScalarSlice(
	std::shared_ptr<awst::Expression> _bytes,
	int _offset, int _size,
	awst::WType const* _t,
	awst::SourceLocation const& _loc)
{
	auto extract = awst::makeIntrinsicCall(
		"extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(std::move(_bytes));
	extract->stackArgs.push_back(awst::makeIntegerConstant(
		std::to_string(_offset), _loc));
	extract->stackArgs.push_back(awst::makeIntegerConstant(
		std::to_string(_size), _loc));
	if (_t == awst::WType::biguintType())
		return awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
	if (_t == awst::WType::uint64Type())
		return awst::makeBtoi(std::move(extract), _loc);
	if (_t == awst::WType::boolType())
	{
		auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), _loc);
		getbit->stackArgs.push_back(std::move(extract));
		getbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		return awst::makeNumericCompare(
			std::move(getbit), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", _loc), _loc);
	}
	if (_t == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(extract), awst::WType::accountType(), _loc);
	return awst::makeReinterpretCast(std::move(extract), _t, _loc);
}

/// Decode an ABI ApplicationArg value into the native wtype (helper-
/// approval prologue, and call-site return decode). Handles scalars
/// + fixed-size tuples by walking fields. Reinterpret-casts what's
/// left to its declared wtype (works for fixed-bytes / ARC4 aggregates
/// where bytes IS the AVM-internal shape).
std::shared_ptr<awst::Expression> decodeArgFromBytes(
	std::shared_ptr<awst::Expression> _bytes,
	awst::WType const* _t,
	awst::SourceLocation const& _loc)
{
	if (_t == awst::WType::biguintType())
		return awst::makeReinterpretCast(std::move(_bytes), awst::WType::biguintType(), _loc);
	if (_t == awst::WType::uint64Type())
		return awst::makeBtoi(std::move(_bytes), _loc);
	if (_t == awst::WType::boolType())
	{
		auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), _loc);
		getbit->stackArgs.push_back(std::move(_bytes));
		getbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		return awst::makeNumericCompare(
			std::move(getbit), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", _loc), _loc);
	}
	if (_t == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(_bytes), awst::WType::accountType(), _loc);
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_t))
	{
		// Walk fields, decode each at known offset. Wrap the input in
		// SingleEvaluation so multiple field-decodes don't re-evaluate
		// the source expression (which may have side effects, like
		// `extract` of `itxn LastLog`).
		auto se = std::make_shared<awst::SingleEvaluation>();
		se->sourceLocation = _loc;
		se->wtype = awst::WType::bytesType();
		se->source = std::move(_bytes);
		static int seCounter = 0;
		se->id = ++seCounter;

		auto out = awst::makeTupleExpression(_t, _loc);
		int offset = 0;
		for (auto const* ft : tup->types())
		{
			int sz = staticEncodedSize(ft);
			if (sz == 0)
			{
				// Dynamic field: punt — caller should have skipped
				// this sub during pass-1. Best-effort: pass the
				// remaining bytes verbatim and break.
				out->items.push_back(se);
				break;
			}
			out->items.push_back(decodeScalarSlice(se, offset, sz, ft, _loc));
			offset += sz;
		}
		return out;
	}
	return awst::makeReinterpretCast(std::move(_bytes), _t, _loc);
}

/// Encode a value into ABI bytes (caller side: stuff into
/// ApplicationArgs; helper return-side: build the log payload).
/// Handles scalars + fixed-size tuples by walking fields and
/// concat'ing per-field encoded bytes.
std::shared_ptr<awst::Expression> encodeValueToBytes(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _t,
	awst::SourceLocation const& _loc)
{
	if (_t == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_value), awst::WType::bytesType(), _loc);
		auto bz = awst::makeBzero(32, _loc);
		auto cat = awst::makeConcat(std::move(bz), std::move(cast), _loc);
		auto len = awst::makeLen(cat, _loc);
		auto offset = awst::makeUInt64BinOp(
			std::move(len), awst::UInt64BinaryOperator::Sub,
			awst::makeIntegerConstant("32", _loc), _loc);
		auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		extract->stackArgs.push_back(cat);
		extract->stackArgs.push_back(std::move(offset));
		extract->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));
		return extract;
	}
	if (_t == awst::WType::uint64Type())
		return awst::makeItob(std::move(_value), _loc);
	if (_t == awst::WType::boolType())
	{
		auto zeroByte = awst::makeBytesConstant({0x00}, _loc);
		auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
		setbit->stackArgs.push_back(std::move(zeroByte));
		setbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		setbit->stackArgs.push_back(std::move(_value));
		return setbit;
	}
	if (_t == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(_value), awst::WType::bytesType(), _loc);
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_t))
	{
		// Walk fields, concat per-field encoded bytes. SingleEvaluation
		// the source so the per-field TupleItemExpression reads don't
		// re-trigger any side effects.
		auto se = std::make_shared<awst::SingleEvaluation>();
		se->sourceLocation = _loc;
		se->wtype = _t;
		se->source = std::move(_value);
		static int seCounter = 1000;
		se->id = ++seCounter;

		std::shared_ptr<awst::Expression> acc;
		for (size_t i = 0; i < tup->types().size(); ++i)
		{
			auto item = awst::makeTupleItem(se, static_cast<int>(i),
				tup->types()[i], _loc);
			auto enc = encodeValueToBytes(std::move(item),
				tup->types()[i], _loc);
			if (!acc) acc = std::move(enc);
			else acc = awst::makeConcat(std::move(acc), std::move(enc), _loc);
		}
		if (!acc) acc = awst::makeBytesConstant({}, _loc);
		return acc;
	}
	return awst::makeReinterpretCast(std::move(_value), awst::WType::bytesType(), _loc);
}

/// Wrap an encoded return value in the `0x151f7c75 ++ ARC4(value)`
/// ABI return-log shape so the caller's `itxn LastLog` strip-4-and-
/// decode dance recovers it.
std::shared_ptr<awst::Expression> encodeReturnLogPayload(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _t,
	awst::SourceLocation const& _loc)
{
	auto encoded = encodeValueToBytes(std::move(_value), _t, _loc);
	auto prefix = awst::makeBytesConstant(
		{0x15, 0x1f, 0x7c, 0x75}, _loc);
	return awst::makeConcat(std::move(prefix), std::move(encoded), _loc);
}

/// Build the helper Contract's hand-written approval body — routes
/// the one selector to the lifted Subroutine. Anything else asserts.
std::shared_ptr<awst::Block> buildHelperApprovalBody(
	awst::Subroutine const& _sub,
	std::string const& _sig,
	awst::SourceLocation const& _loc)
{
	auto body = awst::makeBlock(_loc);

	// Creation: txn ApplicationID == 0 → return true.
	auto thenBlock = awst::makeBlock(_loc);
	thenBlock->body.push_back(awst::makeReturnStatement(
		awst::makeBoolConstant(true, _loc), _loc));
	body->body.push_back(awst::makeIfElse(
		isCreate(_loc), std::move(thenBlock), nullptr, _loc));

	// Selector check (BytesComparisonExpression — selector and
	// ApplicationArgs[0] are both 4-byte byte strings, not numeric
	// types).
	auto selectorMatches = std::make_shared<awst::BytesComparisonExpression>();
	selectorMatches->sourceLocation = _loc;
	selectorMatches->wtype = awst::WType::boolType();
	selectorMatches->lhs = appArgAt(0, _loc);
	selectorMatches->op = awst::EqualityComparison::Eq;
	selectorMatches->rhs = selectorConst(_sig, _loc);
	body->body.push_back(awst::makeExpressionStatement(
		awst::makeAssert(std::move(selectorMatches), _loc,
			std::string("helper: unknown selector")), _loc));

	// Decode each ABI arg and bind to a local.
	std::vector<std::shared_ptr<awst::Expression>> callArgs;
	int argIdx = 1;
	for (auto const& arg : _sub.args)
	{
		auto raw = appArgAt(argIdx++, _loc);
		auto decoded = decodeArgFromBytes(std::move(raw), arg.wtype, _loc);
		auto target = awst::makeVarExpression(arg.name, arg.wtype, _loc);
		body->body.push_back(awst::makeAssignmentStatement(
			target, std::move(decoded), _loc));
		callArgs.push_back(awst::makeVarExpression(arg.name, arg.wtype, _loc));
	}

	// SubroutineCall to the lifted body. The Sub stays in roots; this
	// approval is its sole caller post-rewrite, so per-Contract DCE
	// keeps it for the helper and drops it from contexts that no
	// longer reach it.
	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = _loc;
	call->wtype = _sub.returnType;
	call->target = awst::SubroutineID{_sub.id};
	for (size_t i = 0; i < _sub.args.size(); ++i)
	{
		awst::CallArg ca;
		ca.name = _sub.args[i].name;
		ca.value = std::move(callArgs[i]);
		call->args.push_back(std::move(ca));
	}

	if (_sub.returnType && _sub.returnType != awst::WType::voidType())
	{
		auto resultName = std::string("__pure_helper_ret");
		auto resultTarget = awst::makeVarExpression(
			resultName, _sub.returnType, _loc);
		body->body.push_back(awst::makeAssignmentStatement(
			resultTarget, std::move(call), _loc));

		auto resultRead = awst::makeVarExpression(
			resultName, _sub.returnType, _loc);
		auto payload = encodeReturnLogPayload(
			std::move(resultRead), _sub.returnType, _loc);
		auto logCall = awst::makeIntrinsicCall(
			"log", awst::WType::voidType(), _loc);
		logCall->stackArgs.push_back(std::move(payload));
		body->body.push_back(awst::makeExpressionStatement(
			std::move(logCall), _loc));
	}
	else
	{
		body->body.push_back(awst::makeExpressionStatement(
			std::move(call), _loc));
	}

	body->body.push_back(awst::makeReturnStatement(
		awst::makeBoolConstant(true, _loc), _loc));
	return body;
}

/// Trivial clear-state: `return true`.
std::shared_ptr<awst::Block> buildTrivialClearBody(
	awst::SourceLocation const& _loc)
{
	auto body = awst::makeBlock(_loc);
	body->body.push_back(awst::makeReturnStatement(
		awst::makeBoolConstant(true, _loc), _loc));
	return body;
}

/// Build the helper Contract that wraps a single pure Subroutine.
std::shared_ptr<awst::Contract> buildHelperContract(
	awst::Subroutine const& _sub,
	std::string const& _helperId,
	std::string const& _sig)
{
	auto loc = _sub.sourceLocation;
	auto contract = std::make_shared<awst::Contract>();
	contract->sourceLocation = loc;
	contract->id = _helperId;
	auto dot = _helperId.find_last_of('.');
	contract->name = (dot == std::string::npos)
		? _helperId : _helperId.substr(dot + 1);
	contract->methodResolutionOrder = {_helperId};

	awst::ContractMethod approval;
	approval.sourceLocation = loc;
	approval.cref = _helperId;
	approval.memberName = "approval_program";
	approval.returnType = awst::WType::boolType();
	approval.body = buildHelperApprovalBody(_sub, _sig, loc);
	contract->approvalProgram = std::move(approval);

	awst::ContractMethod clear;
	clear.sourceLocation = loc;
	clear.cref = _helperId;
	clear.memberName = "clear_state_program";
	clear.returnType = awst::WType::boolType();
	clear.body = buildTrivialClearBody(loc);
	contract->clearProgram = std::move(clear);

	return contract;
}

/// Build the inner-txn ApplicationCall expression that REPLACES a
/// SubroutineCallExpression at the call site. Returns an Expression
/// whose wtype matches the helper's return type — drop-in.
std::shared_ptr<awst::Expression> buildInnerCallReplacement(
	std::string const& _templateVar,
	std::string const& _sig,
	std::vector<std::shared_ptr<awst::Expression>> _args,
	std::vector<awst::WType const*> const& _argWTypes,
	awst::WType const* _retType,
	awst::SourceLocation const& _loc)
{
	// args tuple: [selector, encoded_arg1, encoded_arg2, ...]
	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
	argsTuple->items.push_back(selectorConst(_sig, _loc));
	for (size_t i = 0; i < _args.size(); ++i)
	{
		auto encoded = encodeValueToBytes(
			std::move(_args[i]), _argWTypes[i], _loc);
		argsTuple->items.push_back(std::move(encoded));
	}
	std::vector<awst::WType const*> argTypes;
	for (auto const& it : argsTuple->items)
		argTypes.push_back(it->wtype);
	static std::vector<std::unique_ptr<awst::WTuple>> ownedTupleTypes;
	ownedTupleTypes.push_back(std::make_unique<awst::WTuple>(
		std::move(argTypes), std::nullopt));
	argsTuple->wtype = ownedTupleTypes.back().get();

	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_applFieldsType;
	create->fields["TypeEnum"] = awst::makeIntegerConstant(
		std::to_string(TxnTypeAppl), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
	auto tmplVar = std::make_shared<awst::TemplateVar>();
	tmplVar->sourceLocation = _loc;
	tmplVar->wtype = awst::WType::uint64Type();
	// AWST convention (mirrors UrosSplitter): include the TMPL_
	// prefix in the AWST name. main.cpp's intTemplateVars uses the
	// prefix-stripped form, which puya re-prefixes via
	// template_vars_prefix when building options.template_variables.
	tmplVar->name = "TMPL_" + _templateVar;
	create->fields["ApplicationID"] = std::move(tmplVar);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = awst::makeSubmitInnerTransaction(
		&s_applTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	if (!_retType || _retType == awst::WType::voidType())
		return submit;

	// Read itxn LastLog, strip 4-byte ABI return prefix, decode.
	auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), _loc);
	readLog->immediates = {std::string("LastLog")};
	auto strip = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
	strip->immediates = {4, 0};
	strip->stackArgs.push_back(std::move(readLog));
	auto decoded = decodeArgFromBytes(std::move(strip), _retType, _loc);

	// (submit, decode) sequencing via CommaExpression so the whole
	// thing is one expression slot the original SubroutineCall site
	// can be drop-in replaced with.
	auto comma = std::make_shared<awst::CommaExpression>();
	comma->sourceLocation = _loc;
	comma->wtype = _retType;
	comma->expressions.push_back(std::move(submit));
	comma->expressions.push_back(std::move(decoded));
	return comma;
}

/// Sanitize an arbitrary identifier-ish string into something safe for
/// a TEAL template-var name: keep [A-Za-z0-9_], replace everything
/// else with '_'.
std::string sanitizeIdent(std::string const& _s)
{
	std::string out;
	out.reserve(_s.size());
	for (char c : _s)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9') || c == '_')
			out += c;
		else
			out += '_';
	}
	return out;
}

} // namespace

PureHelperExtractor::Result PureHelperExtractor::extract(
	std::vector<std::shared_ptr<awst::RootNode>>& _roots)
{
	auto& logger = Logger::instance();
	Result out;

	// Pass 1: identify pure subroutines that are candidates for
	// extraction. Skip stubs / those whose return type doesn't fit
	// through `LastLog`. Skip tiny ones — the inner-txn overhead at
	// each call site (~50 B vs ~5 B for a callsub) outweighs the
	// savings from removing a tiny body. Heuristic threshold:
	// kMinBodyStatements = 10. Anything smaller stays inline.
	constexpr size_t kMinBodyStatements = 10;
	std::vector<std::shared_ptr<awst::Subroutine>> pureSubs;
	for (auto const& r : _roots)
	{
		auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r);
		if (!sub || !sub->pure || !sub->body) continue;
		if (sub->body->body.size() < kMinBodyStatements) continue;
		if (sub->returnType && sub->returnType != awst::WType::voidType())
		{
			int retSize = staticEncodedSize(sub->returnType);
			if (retSize == 0)
			{
				logger.warning(
					"--deploy-pure-helpers: skipping '" + sub->name +
					"': return type is dynamic / unknown size — can't "
					"static-bound LastLog fit. Let me know if you want "
					"chunked-log support.");
				continue;
			}
			if (retSize > 1024)
			{
				logger.warning(
					"--deploy-pure-helpers: skipping '" + sub->name +
					"': return size " + std::to_string(retSize) +
					" B exceeds AVM MaxLogSize=1024 B. Need chunked-log "
					"workaround.");
				continue;
			}
		}
		pureSubs.push_back(sub);
	}

	if (pureSubs.empty())
	{
		logger.info("--deploy-pure-helpers: no pure subroutines to extract");
		return out;
	}

	// Pass 2: build helper Contracts and call-site replacement table.
	// Counter-based unique suffixes avoid collisions between
	// same-named subs in different libraries (e.g. _validate in
	// LiquidationLogic vs _validate in some other library).
	std::map<std::string, std::tuple<std::string, std::string, std::string>> bySubId;
	int counter = 0;
	for (auto const& sub : pureSubs)
	{
		std::string sig = canonicalSig(*sub);
		std::string suffix = std::to_string(counter++);
		std::string templateVarName =
			"PURE_HELPER_" + sanitizeIdent(sub->name) +
			"_" + suffix + "_APP_ID";
		std::string helperContractId =
			"PureHelper__" + sanitizeIdent(sub->name) + "__" + suffix;
		bySubId[sub->id] = {templateVarName, helperContractId, sig};

		ExtractedHelper eh;
		eh.subId = sub->id;
		eh.templateVarName = templateVarName;
		eh.helperContractId = helperContractId;
		out.extracted.push_back(std::move(eh));

		logger.info(
			"--deploy-pure-helpers: lifting '" + sub->name +
			"' (sig='" + sig + "') to " + helperContractId);
	}

	// Pass 3: build the helper Contracts.
	for (auto const& sub : pureSubs)
	{
		auto const& [_tv, hid, sig] = bySubId[sub->id];
		out.helperContracts.push_back(
			buildHelperContract(*sub, hid, sig));
	}

	// Pass 4: walk every body in every NON-helper root and rewrite
	// SubroutineCall(SubroutineID(extracted_id), ...) sites to
	// inner-txn ApplicationCall on the helper.
	auto rewriteFn = [&bySubId](awst::Expression const& e)
		-> std::shared_ptr<awst::Expression>
	{
		auto const* sce = dynamic_cast<awst::SubroutineCallExpression const*>(&e);
		if (!sce) return nullptr;
		auto const* sid = std::get_if<awst::SubroutineID>(&sce->target);
		if (!sid) return nullptr;
		auto it = bySubId.find(sid->target);
		if (it == bySubId.end()) return nullptr;
		auto const& [tv, _hid, sig] = it->second;
		std::vector<std::shared_ptr<awst::Expression>> args;
		std::vector<awst::WType const*> argTypes;
		for (auto const& a : sce->args)
		{
			args.push_back(a.value);
			argTypes.push_back(a.value ? a.value->wtype : nullptr);
		}
		return buildInnerCallReplacement(
			tv, sig, std::move(args), argTypes,
			sce->wtype, sce->sourceLocation);
	};

	for (auto& root : _roots)
	{
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(root))
		{
			// Don't rewrite inside an extracted Sub's own body — that
			// body still lives on as the helper Contract's callable.
			if (bySubId.count(sub->id)) continue;
			if (sub->body) walkBlock(*sub->body, rewriteFn);
		}
		else if (auto contract = std::dynamic_pointer_cast<awst::Contract>(root))
		{
			if (contract->approvalProgram.body)
				walkBlock(*contract->approvalProgram.body, rewriteFn);
			if (contract->clearProgram.body)
				walkBlock(*contract->clearProgram.body, rewriteFn);
			for (auto& m : contract->methods)
				if (m.body) walkBlock(*m.body, rewriteFn);
		}
	}

	// Pass 5: PREPEND helper Contracts to roots. UrosSplitter picks
	// the LAST Contract as the primary (the user's main contract); we
	// don't want a helper to displace it. Pure Subroutines stay in
	// roots — per-Contract DCE handles which contexts retain them.
	_roots.insert(_roots.begin(), out.helperContracts.begin(), out.helperContracts.end());

	out.didExtract = true;
	logger.info("--deploy-pure-helpers: extracted " +
		std::to_string(out.helperContracts.size()) + " helper(s)");
	return out;
}

} // namespace puyasol::splitter
