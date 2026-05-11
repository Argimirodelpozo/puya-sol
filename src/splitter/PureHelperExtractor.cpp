/// @file PureHelperExtractor.cpp
/// See header for design overview.

#include "splitter/PureHelperExtractor.h"
#include "splitter/AwstWalker.h"
#include "splitter/FunctionSplitter.h"
#include "Logger.h"

#include <algorithm>
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

/// Estimate the static encoded size of a return wtype in ABI-encoded
/// bytes (matches the byte-count puya emits via `log` for the return
/// value). Returns 0 if dynamic or unknown. Used to decide whether the
/// return value fits in a single AVM log call (≤1024 B), needs the
/// chunked-log path (1024 B–4096 B) or has to be skipped entirely
/// (>4096 B can't be reassembled into a single stack value).
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
	// ARC4 fixed-width integer: bits / 8 bytes (e.g. arc4.uint256 = 32).
	if (auto const* ui = dynamic_cast<awst::ARC4UIntN const*>(_t))
		return (ui->n() + 7) / 8;
	// ARC4 static array: elementCount × elementSize. Returns 0 when the
	// element is itself dynamic.
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_t))
	{
		int es = staticEncodedSize(sa->elementType());
		if (es == 0) return 0;
		return static_cast<int>(sa->arraySize()) * es;
	}
	// ARC4 struct: sum of field sizes. Returns 0 if any field is
	// dynamic. (puya packs static-only structs head-to-head with no
	// per-field length prefix.)
	if (auto const* st = dynamic_cast<awst::ARC4Struct const*>(_t))
	{
		int sum = 0;
		for (auto const& [_, ft] : st->fields())
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
	return awst::makeMethodConstant(_sig, awst::WType::bytesType(), _loc);
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
		auto extract = awst::makeExtract3(cat, std::move(offset), awst::makeIntegerConstant("32", _loc), _loc);
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

/// Selector signature for the per-helper "fetch next chunk" entry
/// point used by the chunked-return path. Sidecars whose method's
/// augmented return exceeds the per-program 1024 B log cap also expose
/// this method; the caller bundles N-1 invocations of it after the
/// main method into one inner-txn group, and each helper-txn `gloadss`
/// the chunk stashed by the main method.
constexpr char const* kBigReturnHelperSig = "__big_return_helper()void";

/// Body of `__big_return_helper`: read this txn's GroupIndex; gloadss
/// scratch slot (99 + GroupIndex) of txn 0 (the main method's call),
/// `log` it, return.
void appendBigReturnHelperBranch(
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	awst::SourceLocation const& _loc)
{
	// gloadss takes the txn index (top-of-stack-1) and slot (top-of-
	// stack) and pushes the chunk bytes. Build:
	//   pushint 0           // txn 0 (main method)
	//   txn GroupIndex      // my idx in inner group
	//   pushint 99
	//   +                   // slot = 99 + GroupIndex
	//   gloadss
	//   log
	auto txnIdx = awst::makeIntegerConstant("0", _loc);
	auto myIdx = awst::makeIntrinsicCall(
		"txn", awst::WType::uint64Type(), _loc);
	myIdx->immediates = {std::string("GroupIndex")};
	auto base = awst::makeIntegerConstant("99", _loc);
	auto slot = awst::makeUInt64BinOp(
		std::move(myIdx), awst::UInt64BinaryOperator::Add,
		std::move(base), _loc);
	auto gloadss = awst::makeIntrinsicCall(
		"gloadss", awst::WType::bytesType(), _loc);
	gloadss->stackArgs.push_back(std::move(txnIdx));
	gloadss->stackArgs.push_back(std::move(slot));
	auto logCall = awst::makeIntrinsicCall(
		"log", awst::WType::voidType(), _loc);
	logCall->stackArgs.push_back(std::move(gloadss));
	_out.push_back(awst::makeExpressionStatement(std::move(logCall), _loc));
}

/// Build the helper Contract's hand-written approval body. Routes one
/// selector to the lifted Subroutine. When the lifted method's return
/// exceeds the per-program log cap, also routes a second selector
/// (`__big_return_helper`) for sibling inner txns to pull the stashed
/// chunks out of the main method's scratch.
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

	int retSize = staticEncodedSize(_sub.returnType);
	bool needsBigReturnHelper =
		_sub.returnType
		&& _sub.returnType != awst::WType::voidType()
		&& retSize > 0
		&& (4 + retSize) > 1024;

	auto makeSelectorEq = [&](std::string const& _sigToMatch) {
		return awst::makeBytesComparison(appArgAt(0, _loc),
			awst::EqualityComparison::Eq,
			selectorConst(_sigToMatch, _loc), _loc);
	};

	if (!needsBigReturnHelper)
	{
		// Single-selector dispatch: assert match.
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(makeSelectorEq(_sig), _loc,
				std::string("helper: unknown selector")), _loc));
	}
	else
	{
		// Two-selector dispatch:
		//   if selector == originalSig: fall through to original method body
		//   else if selector == helperSig: emit chunk + return
		//   else: assert false
		// We emit the helper branch first as an early-return, then let
		// the rest of the body be the original method's path.
		auto helperBlock = awst::makeBlock(_loc);
		appendBigReturnHelperBranch(helperBlock->body, _loc);
		helperBlock->body.push_back(awst::makeReturnStatement(
			awst::makeBoolConstant(true, _loc), _loc));
		body->body.push_back(awst::makeIfElse(
			makeSelectorEq(kBigReturnHelperSig),
			std::move(helperBlock), nullptr, _loc));

		// Past the helper-selector branch, only the original selector
		// is valid. Assert that.
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(makeSelectorEq(_sig), _loc,
				std::string("helper: unknown selector")), _loc));
	}

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
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{_sub.id}, _sub.returnType, _loc);
	for (size_t i = 0; i < _sub.args.size(); ++i)
		awst::pushCallArg(call->args, _sub.args[i].name, std::move(callArgs[i]));

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

		// Total payload size is `4 B prefix + retSize`. If it fits in
		// the per-program log cap (1024 B total per program — confirmed
		// empirically in localnet), emit a single `log`. Otherwise
		// stash chunks 1..N-1 into scratch slots 100..100+N-2 and let
		// the caller invoke `__big_return_helper` once per chunk in
		// the same inner-txn group; each helper txn `gloadss` its
		// chunk from txn 0 and emits it as its own log.
		int retSize = staticEncodedSize(_sub.returnType);
		int totalSize = 4 + retSize;
		constexpr int kChunkSize = 1024;
		constexpr int kChunkBaseSlot = 100;
		if (totalSize <= kChunkSize)
		{
			auto logCall = awst::makeIntrinsicCall(
				"log", awst::WType::voidType(), _loc);
			logCall->stackArgs.push_back(std::move(payload));
			body->body.push_back(awst::makeExpressionStatement(
				std::move(logCall), _loc));
		}
		else
		{
			// Stash payload once.
			std::string bufName = "__pure_helper_buf";
			auto bufTarget = awst::makeVarExpression(
				bufName, awst::WType::bytesType(), _loc);
			body->body.push_back(awst::makeAssignmentStatement(
				bufTarget, std::move(payload), _loc));

			int chunks = (totalSize + kChunkSize - 1) / kChunkSize;
			// Emit chunk 0 via log (caller reads via `gitxn 0 LastLog`).
			{
				auto bufRead = awst::makeVarExpression(
					bufName, awst::WType::bytesType(), _loc);
				auto extract = awst::makeIntrinsicCall(
					"extract3", awst::WType::bytesType(), _loc);
				extract->stackArgs.push_back(std::move(bufRead));
				extract->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
				extract->stackArgs.push_back(awst::makeIntegerConstant(
					std::to_string(std::min(kChunkSize, totalSize)), _loc));
				auto logCall = awst::makeIntrinsicCall(
					"log", awst::WType::voidType(), _loc);
				logCall->stackArgs.push_back(std::move(extract));
				body->body.push_back(awst::makeExpressionStatement(
					std::move(logCall), _loc));
			}
			// Stash chunks 1..N-1 to scratch slots kChunkBaseSlot+0..N-2.
			// `__big_return_helper` (sibling inner txn) will gloadss each
			// from this txn's scratch to emit them as its own log.
			for (int i = 1; i < chunks; ++i)
			{
				int off = i * kChunkSize;
				int len = std::min(kChunkSize, totalSize - off);
				auto bufRead = awst::makeVarExpression(
					bufName, awst::WType::bytesType(), _loc);
				auto extract = awst::makeIntrinsicCall(
					"extract3", awst::WType::bytesType(), _loc);
				extract->stackArgs.push_back(std::move(bufRead));
				extract->stackArgs.push_back(awst::makeIntegerConstant(
					std::to_string(off), _loc));
				extract->stackArgs.push_back(awst::makeIntegerConstant(
					std::to_string(len), _loc));
				auto storeOp = awst::makeIntrinsicCall(
					"store", awst::WType::voidType(), _loc);
				storeOp->immediates = {kChunkBaseSlot + (i - 1)};
				storeOp->stackArgs.push_back(std::move(extract));
				body->body.push_back(awst::makeExpressionStatement(
					std::move(storeOp), _loc));
			}
		}
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
	// AWST convention (mirrors UrosSplitter): include the TMPL_
	// prefix in the AWST name. main.cpp's intTemplateVars uses the
	// prefix-stripped form, which puya re-prefixes via
	// template_vars_prefix when building options.template_variables.
	create->fields["ApplicationID"] = awst::makeTemplateVar(
		"TMPL_" + _templateVar, awst::WType::uint64Type(), _loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	int retSize = staticEncodedSize(_retType);
	int totalSize = (_retType && _retType != awst::WType::voidType())
		? 4 + retSize : 0;
	constexpr int kChunkSize = 1024;
	int chunks = totalSize > 0
		? (totalSize + kChunkSize - 1) / kChunkSize : 0;

	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = awst::makeSubmitInnerTransaction(
		&s_applTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	// Chunked-return case: append N-1 `__big_return_helper()` inner
	// txns to the same inner-txn group. Each helper-txn `gloadss`es
	// its assigned chunk from txn 0's scratch (slot 99 + GroupIndex)
	// and emits it as its own LastLog. The caller then concatenates
	// `gitxn 0..N-1 LastLog` to recover the full ABI-encoded return.
	if (chunks > 1)
	{
		// Helper inner-txn args: just the helper selector. No data
		// args — the chunk is fetched by groupIndex from txn 0.
		auto helperArgs = awst::makeTupleExpression(nullptr, _loc);
		helperArgs->items.push_back(selectorConst(kBigReturnHelperSig, _loc));
		std::vector<awst::WType const*> helperArgTypes;
		for (auto const& it : helperArgs->items)
			helperArgTypes.push_back(it->wtype);
		ownedTupleTypes.push_back(std::make_unique<awst::WTuple>(
			std::move(helperArgTypes), std::nullopt));
		helperArgs->wtype = ownedTupleTypes.back().get();

		for (int i = 1; i < chunks; ++i)
		{
			auto helperCreate = std::make_shared<awst::CreateInnerTransaction>();
			helperCreate->sourceLocation = _loc;
			helperCreate->wtype = &s_applFieldsType;
			helperCreate->fields["TypeEnum"] = awst::makeIntegerConstant(
				std::to_string(TxnTypeAppl), _loc);
			helperCreate->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
			helperCreate->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
			helperCreate->fields["ApplicationID"] = awst::makeTemplateVar(
				"TMPL_" + _templateVar, awst::WType::uint64Type(), _loc);
			// Re-clone the args tuple per inner txn (puya
			// disallows shared sub-AST in itxns field map).
			auto haCopy = awst::makeTupleExpression(helperArgs->wtype, _loc);
			haCopy->items.push_back(selectorConst(kBigReturnHelperSig, _loc));
			helperCreate->fields["ApplicationArgs"] = std::move(haCopy);
			submit->itxns.push_back(std::move(helperCreate));
		}
	}

	if (!_retType || _retType == awst::WType::voidType())
		return submit;

	// Read the inner-call's logged ABI return. ≤1024 B payloads come
	// back in one log via `itxn LastLog`. Larger payloads were sliced
	// across `chunks` inner txns (txn 0 logs chunk 0, helper txns 1..N-1
	// log chunks 1..N-1) — stitch them via `gitxn i LastLog`.
	std::shared_ptr<awst::Expression> readLog;
	if (chunks <= 1)
	{
		auto last = awst::makeIntrinsicCall(
			"itxn", awst::WType::bytesType(), _loc);
		last->immediates = {std::string("LastLog")};
		readLog = std::move(last);
	}
	else
	{
		auto pickLog = [&](int idx) -> std::shared_ptr<awst::Expression>
		{
			auto c = awst::makeIntrinsicCall(
				"gitxn", awst::WType::bytesType(), _loc);
			c->immediates = {idx, std::string("LastLog")};
			return c;
		};
		readLog = pickLog(0);
		for (int i = 1; i < chunks; ++i)
			readLog = awst::makeConcat(std::move(readLog), pickLog(i), _loc);
	}

	// Strip 4-byte ABI return prefix, decode.
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

/// Multi-piece variant of `buildInnerCallReplacement`: when a pure
/// helper has been pre-sliced via `--pure-helper-split`, each piece is
/// its own deployed sidecar Contract and the call becomes one inner-txn
/// group with M piece-calls in sequence. State threads between pieces
/// via FunctionSplitter's scratch-slot-100 + gload prologue (the
/// pieces' bodies were emitted with `crossChunk=true,
/// prevCallStride=1`). Chunked-return helpers (when the final piece's
/// return exceeds 1024 B) attach to the same group, hitting the last
/// piece's sidecar.
std::shared_ptr<awst::Expression> buildChainedInnerCallReplacement(
	std::vector<std::tuple<std::string, std::string,
		std::vector<std::shared_ptr<awst::Expression>>,
		std::vector<awst::WType const*>>> const& _pieceCalls,
	std::string const& _lastPieceTemplateVar,
	awst::WType const* _retType,
	awst::SourceLocation const& _loc)
{
	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	static std::vector<std::unique_ptr<awst::WTuple>> ownedTupleTypes;

	auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, _loc);

	for (auto const& [tmpl, sig, args, argTypes] : _pieceCalls)
	{
		auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
		argsTuple->items.push_back(selectorConst(sig, _loc));
		for (size_t i = 0; i < args.size(); ++i)
		{
			auto encoded = encodeValueToBytes(
				args[i], argTypes[i], _loc);
			argsTuple->items.push_back(std::move(encoded));
		}
		std::vector<awst::WType const*> argT;
		for (auto const& it : argsTuple->items)
			argT.push_back(it->wtype);
		ownedTupleTypes.push_back(std::make_unique<awst::WTuple>(
			std::move(argT), std::nullopt));
		argsTuple->wtype = ownedTupleTypes.back().get();

		auto create = std::make_shared<awst::CreateInnerTransaction>();
		create->sourceLocation = _loc;
		create->wtype = &s_applFieldsType;
		create->fields["TypeEnum"] = awst::makeIntegerConstant(
			std::to_string(TxnTypeAppl), _loc);
		create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
		create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
		create->fields["ApplicationID"] = awst::makeTemplateVar(
			"TMPL_" + tmpl, awst::WType::uint64Type(), _loc);
		create->fields["ApplicationArgs"] = std::move(argsTuple);

		submit->itxns.push_back(std::move(create));
	}

	int retSize = staticEncodedSize(_retType);
	int totalSize = (_retType && _retType != awst::WType::voidType())
		? 4 + retSize : 0;
	constexpr int kChunkSize = 1024;
	int returnChunks = totalSize > 0
		? (totalSize + kChunkSize - 1) / kChunkSize : 0;
	int basePieces = static_cast<int>(_pieceCalls.size());

	if (returnChunks > 1)
	{
		// Append (returnChunks - 1) helper inner txns hitting the LAST
		// piece's sidecar, mirroring the single-piece chunked-return
		// path. They `gloadss` chunk i from the last piece's scratch
		// (slot 99 + GroupIndex within this inner group).
		for (int i = 1; i < returnChunks; ++i)
		{
			auto helperArgs = awst::makeTupleExpression(nullptr, _loc);
			helperArgs->items.push_back(selectorConst(kBigReturnHelperSig, _loc));
			std::vector<awst::WType const*> at;
			for (auto const& it : helperArgs->items) at.push_back(it->wtype);
			ownedTupleTypes.push_back(std::make_unique<awst::WTuple>(
				std::move(at), std::nullopt));
			helperArgs->wtype = ownedTupleTypes.back().get();

			auto hc = std::make_shared<awst::CreateInnerTransaction>();
			hc->sourceLocation = _loc;
			hc->wtype = &s_applFieldsType;
			hc->fields["TypeEnum"] = awst::makeIntegerConstant(
				std::to_string(TxnTypeAppl), _loc);
			hc->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
			hc->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
			hc->fields["ApplicationID"] = awst::makeTemplateVar(
				"TMPL_" + _lastPieceTemplateVar, awst::WType::uint64Type(), _loc);
			hc->fields["ApplicationArgs"] = std::move(helperArgs);
			submit->itxns.push_back(std::move(hc));
		}
	}

	if (!_retType || _retType == awst::WType::voidType())
		return submit;

	// Read the LAST piece's logged return — chunk 0 sits at gitxn
	// (basePieces - 1) LastLog; chunks 1..N-1 at gitxn (basePieces
	// + i - 1) LastLog.
	auto pickLog = [&](int idx) -> std::shared_ptr<awst::Expression>
	{
		auto c = awst::makeIntrinsicCall("gitxn", awst::WType::bytesType(), _loc);
		c->immediates = {idx, std::string("LastLog")};
		return c;
	};
	std::shared_ptr<awst::Expression> readLog = pickLog(basePieces - 1);
	for (int i = 1; i < returnChunks; ++i)
		readLog = awst::makeConcat(std::move(readLog),
			pickLog(basePieces - 1 + i), _loc);

	auto strip = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
	strip->immediates = {4, 0};
	strip->stackArgs.push_back(std::move(readLog));
	auto decoded = decodeArgFromBytes(std::move(strip), _retType, _loc);

	auto comma = std::make_shared<awst::CommaExpression>();
	comma->sourceLocation = _loc;
	comma->wtype = _retType;
	comma->expressions.push_back(std::move(submit));
	comma->expressions.push_back(std::move(decoded));
	return comma;
}

/// True if a node represents a side-effect from the caller's POV: any
/// state read/write, log, inner-txn submit, asset-holding query, etc.
/// Sidecars don't share storage with the main contract, so a helper
/// that reads/writes state can't safely be lifted (it'd hit the
/// sidecar's empty state instead of the main app's). Same for logs
/// (caller can't see logs emitted by sidecars except via LastLog),
/// inner txns (extra side effects observable by the caller), and
/// asset-holding/app-params queries (depend on the running app's id).
bool isSideEffectful(awst::Expression const& _e)
{
	using namespace awst;
	if (dynamic_cast<AppStateExpression const*>(&_e)) return true;
	if (dynamic_cast<AppAccountStateExpression const*>(&_e)) return true;
	if (dynamic_cast<BoxValueExpression const*>(&_e)) return true;
	if (dynamic_cast<BoxPrefixedKeyExpression const*>(&_e)) return true;
	if (dynamic_cast<StateGet const*>(&_e)) return true;
	if (dynamic_cast<StateExists const*>(&_e)) return true;
	if (dynamic_cast<StateGetEx const*>(&_e)) return true;
	if (dynamic_cast<StateDelete const*>(&_e)) return true;
	if (dynamic_cast<Emit const*>(&_e)) return true;
	if (dynamic_cast<SubmitInnerTransaction const*>(&_e)) return true;
	if (dynamic_cast<CreateInnerTransaction const*>(&_e)) return true;
	if (auto ic = dynamic_cast<IntrinsicCall const*>(&_e))
	{
		auto const& op = ic->opCode;
		// Storage ops (read or write — both unsafe in a sidecar).
		if (op == "app_global_get" || op == "app_global_get_ex"
			|| op == "app_global_put" || op == "app_global_del") return true;
		if (op == "app_local_get" || op == "app_local_get_ex"
			|| op == "app_local_put" || op == "app_local_del") return true;
		if (op == "box_get" || op == "box_put" || op == "box_create"
			|| op == "box_del" || op == "box_replace" || op == "box_extract"
			|| op == "box_resize" || op == "box_splice"
			|| op == "box_len") return true;
		// Log + inner-txn opcodes.
		if (op == "log") return true;
		if (op == "itxn_begin" || op == "itxn_next" || op == "itxn_field"
			|| op == "itxn_submit") return true;
		// App / asset / account params depend on running-app context.
		if (op == "asset_holding_get" || op == "asset_params_get"
			|| op == "app_params_get" || op == "acct_params_get") return true;
	}
	return false;
}

bool isLiftableByWalk(awst::Block const& _body)
{
	bool unsafe = false;
	auto& blk = const_cast<awst::Block&>(_body);
	walkBlock(blk, [&unsafe](awst::Expression const& e)
		-> std::shared_ptr<awst::Expression>
	{
		if (isSideEffectful(e)) unsafe = true;
		return nullptr;
	});
	return !unsafe;
}

/// Rough TEAL-bytes estimate for one Expression node (the node itself,
/// children are added by the walker that calls this). Tuned to recognize
/// biguint operations as heavy: a single `b*` / `b/` / `b**` op emits
/// 25-50 B of TEAL whereas a uint64 `*` is 1 B.
int estimateExpressionBytes(awst::Expression const& _e)
{
	using namespace awst;
	if (dynamic_cast<IntegerConstant const*>(&_e)) return 2;
	if (dynamic_cast<BoolConstant const*>(&_e)) return 1;
	if (dynamic_cast<VarExpression const*>(&_e)) return 1;
	if (auto bc = dynamic_cast<BytesConstant const*>(&_e))
		return std::max(2, static_cast<int>(bc->value.size()));
	if (dynamic_cast<UInt64BinaryOperation const*>(&_e)) return 2;
	if (dynamic_cast<BigUIntBinaryOperation const*>(&_e)) return 30;
	if (dynamic_cast<NumericComparisonExpression const*>(&_e)) return 2;
	if (dynamic_cast<BytesBinaryOperation const*>(&_e)) return 5;
	if (dynamic_cast<BytesComparisonExpression const*>(&_e)) return 3;
	if (dynamic_cast<BooleanBinaryOperation const*>(&_e)) return 2;
	if (dynamic_cast<Not const*>(&_e)) return 1;
	if (dynamic_cast<AssertExpression const*>(&_e)) return 2;
	if (dynamic_cast<IntrinsicCall const*>(&_e)) return 3;
	if (dynamic_cast<SubroutineCallExpression const*>(&_e)) return 5;
	if (dynamic_cast<ARC4Encode const*>(&_e)) return 10;
	if (dynamic_cast<ARC4Decode const*>(&_e)) return 10;
	if (dynamic_cast<ReinterpretCast const*>(&_e)) return 0;
	if (dynamic_cast<NewArray const*>(&_e)) return 5;
	if (dynamic_cast<NewStruct const*>(&_e)) return 5;
	if (dynamic_cast<TupleItemExpression const*>(&_e)) return 1;
	if (dynamic_cast<FieldExpression const*>(&_e)) return 2;
	if (dynamic_cast<IndexExpression const*>(&_e)) return 4;
	if (dynamic_cast<ConditionalExpression const*>(&_e)) return 4;
	if (dynamic_cast<AssignmentExpression const*>(&_e)) return 2;
	if (dynamic_cast<AppStateExpression const*>(&_e)) return 3;
	if (dynamic_cast<StateGet const*>(&_e)) return 5;
	if (dynamic_cast<StateExists const*>(&_e)) return 4;
	if (dynamic_cast<StateGetEx const*>(&_e)) return 5;
	if (dynamic_cast<ArrayLength const*>(&_e)) return 1;
	return 1; // unknown / cheap default
}

/// Walk a Block, sum estimateExpressionBytes for every reachable
/// Expression node + a small per-statement base. Yields a TEAL-bytes
/// proxy good enough to compare biguint-heavy bodies (which lift
/// usefully) against trivial wrappers (which don't).
int estimateBodyBytes(awst::Block const& _b)
{
	int total = 0;
	auto& blk = const_cast<awst::Block&>(_b);
	walkBlock(blk, [&total](awst::Expression const& e)
		-> std::shared_ptr<awst::Expression>
	{
		total += estimateExpressionBytes(e);
		return nullptr;
	});
	total += static_cast<int>(_b.body.size()) * 5;
	return total;
}

/// Count `SubroutineCallExpression(target=SubroutineID(_subId))` sites
/// reachable from any root's body. One call site = one `callsub` in
/// TEAL today, one `itxn_submit`+decode dance after lifting.
int countCallSites(
	std::vector<std::shared_ptr<awst::RootNode>>& _roots,
	std::string const& _subId)
{
	int n = 0;
	auto fn = [&](awst::Expression const& e)
		-> std::shared_ptr<awst::Expression>
	{
		if (auto sce = dynamic_cast<awst::SubroutineCallExpression const*>(&e))
			if (auto sid = std::get_if<awst::SubroutineID>(&sce->target))
				if (sid->target == _subId)
					++n;
		return nullptr;
	};
	for (auto& r : _roots)
	{
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r))
		{
			if (sub->body) walkBlock(*sub->body, fn);
		}
		else if (auto contract = std::dynamic_pointer_cast<awst::Contract>(r))
		{
			if (contract->approvalProgram.body)
				walkBlock(*contract->approvalProgram.body, fn);
			if (contract->clearProgram.body)
				walkBlock(*contract->clearProgram.body, fn);
			for (auto& m : contract->methods)
				if (m.body) walkBlock(*m.body, fn);
		}
	}
	return n;
}

/// Static call graph at the AWST level: for every Sub / ContractMethod,
/// collect the set of Subs it directly invokes via SubroutineCallExpression.
/// Method bodies are keyed under a synthetic id "__method__<cref>.<name>".
struct StaticCallGraph
{
	std::map<std::string, std::set<std::string>> directCalls;
	std::vector<std::string> methodIds;

	int countReachingMethods(std::string const& _target) const
	{
		int n = 0;
		for (auto const& mid : methodIds)
		{
			std::set<std::string> visited;
			std::vector<std::string> stack = {mid};
			bool reached = false;
			while (!stack.empty())
			{
				auto cur = std::move(stack.back()); stack.pop_back();
				if (!visited.insert(cur).second) continue;
				if (cur == _target) { reached = true; break; }
				auto it = directCalls.find(cur);
				if (it != directCalls.end())
					for (auto const& c : it->second) stack.push_back(c);
			}
			if (reached) ++n;
		}
		return n;
	}
};

StaticCallGraph buildStaticCallGraph(
	std::vector<std::shared_ptr<awst::RootNode>>& _roots)
{
	StaticCallGraph g;
	auto recordFrom = [&](std::string const& fromId, awst::Block& body)
	{
		walkBlock(body, [&](awst::Expression const& e)
			-> std::shared_ptr<awst::Expression>
		{
			if (auto sce = dynamic_cast<awst::SubroutineCallExpression const*>(&e))
				if (auto sid = std::get_if<awst::SubroutineID>(&sce->target))
					g.directCalls[fromId].insert(sid->target);
			return nullptr;
		});
	};
	for (auto& r : _roots)
	{
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r))
		{
			if (sub->body) recordFrom(sub->id, *sub->body);
		}
		else if (auto contract = std::dynamic_pointer_cast<awst::Contract>(r))
		{
			for (auto& m : contract->methods)
			{
				if (!m.body) continue;
				std::string mid = "__method__" + m.cref + "." + m.memberName;
				recordFrom(mid, *m.body);
				g.methodIds.push_back(mid);
			}
		}
	}
	return g;
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
	std::vector<std::shared_ptr<awst::RootNode>>& _roots,
	std::vector<HelperSplitSpec> const& _splitSpecs)
{
	auto& logger = Logger::instance();
	Result out;
	(void)_splitSpecs;  // overridden below; keep silenced for unused-warning-cleanliness

	// Pass 1: identify pure subroutines that are candidates for
	// extraction. Skip stubs / those whose return type doesn't fit
	// through `LastLog`, then apply a call-count-aware lift gate.
	//
	// Lift accounting:
	//   BEFORE lift: body_bytes inlined into every chunk that reaches
	//                the sub  →  body_bytes × N_chunks
	//   AFTER lift:  body_bytes lives in the sidecar (1 copy + helper
	//                approval skeleton), every call site pays itxn
	//                machinery (begin / 5 field / submit / LastLog /
	//                extract / decode) ≈ 49 B, +helper_overhead ≈ 200 B
	//                fixed
	// Lift wins iff:
	//   (N_chunks − 1) × body_bytes > kHelperOverhead + total_calls × 49
	//
	// We don't know N_chunks (UrosSplitter runs after this pass). We
	// approximate it as N_methods_reaching_sub: the count of distinct
	// ABI methods whose body transitively invokes the sub. This is an
	// upper bound on N_chunks (bin-packing only collapses methods into
	// a chunk, never expands), so it errs on the side of lifting more
	// — but the floor on body_bytes prevents pathological lifts.
	// Body floor: smaller and the chunk-shrink win never beats the
	// single-itxn-call cost (body − 100). 300 B is the rough break-even
	// (above that, removing the body from a chunk and replacing 1
	// callsub with 1 itxn dance still nets bytes).
	constexpr int kMinBodyBytes = 300;
	// Empirically measured itxn-dance per call site: ~100 B for 2-arg
	// uint256 helpers (selector push + 5 itxn_field + per-arg biguint
	// encode + LastLog + extract + decode). Rises with arg count.
	constexpr int kItxnOverhead = 100;
	constexpr int kHelperOverhead = 200;
	auto callGraph = buildStaticCallGraph(_roots);

	// Build sub-by-id index for transitive liftability check.
	std::map<std::string, std::shared_ptr<awst::Subroutine>> subById;
	for (auto const& r : _roots)
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r))
			subById[sub->id] = sub;

	// Transitive liftability: a sub can be lifted iff its body has no
	// side-effect ops AND every transitively-called sub is also
	// liftable. Memoized; cycles default to optimistic-true (the
	// non-cycle work will catch real side-effects).
	std::map<std::string, bool> liftableMemo;
	std::function<bool(std::string const&)> isLiftableTransitive =
		[&](std::string const& subId) -> bool
	{
		auto it = liftableMemo.find(subId);
		if (it != liftableMemo.end()) return it->second;
		liftableMemo[subId] = true;
		auto sit = subById.find(subId);
		if (sit == subById.end())
		{
			// Callee isn't a known Subroutine root — could be a stub,
			// external, or AWST node we don't track. Be conservative:
			// treat as unsafe so we don't lift a sub that calls into
			// unknown territory.
			liftableMemo[subId] = false;
			return false;
		}
		auto const& sub = *sit->second;
		if (!sub.body) { liftableMemo[subId] = false; return false; }
		if (!isLiftableByWalk(*sub.body))
		{ liftableMemo[subId] = false; return false; }
		auto cit = callGraph.directCalls.find(subId);
		if (cit != callGraph.directCalls.end())
		{
			for (auto const& callee : cit->second)
				if (!isLiftableTransitive(callee))
				{ liftableMemo[subId] = false; return false; }
		}
		return true;
	};

	std::vector<std::shared_ptr<awst::Subroutine>> pureSubs;
	for (auto const& r : _roots)
	{
		auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r);
		if (!sub || !sub->body) continue;
		// Accept Solidity-pure subs (fast path) AND any sub that's
		// transitively side-effect-free by AWST walk. The latter
		// covers internal/view helpers that don't actually touch
		// storage, logs, or inner txns even though they're not marked
		// `pure` in source.
		if (!sub->pure && !isLiftableTransitive(sub->id)) continue;
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
			// 4 B ABI return prefix + payload. Caller materialises the
			// concatenated chunks as one stack value, capped at the AVM
			// max bytes-per-stack-element (4096 B).
			int totalSize = 4 + retSize;
			if (totalSize > 4096)
			{
				logger.warning(
					"--deploy-pure-helpers: skipping '" + sub->name +
					"': return size " + std::to_string(retSize) + " B + "
					"4 B prefix > 4096 B AVM stack-element cap. Would "
					"need chunked decode (out of scope for now).");
				continue;
			}
		}

		int bodyBytes = estimateBodyBytes(*sub->body);
		int callSites = countCallSites(_roots, sub->id);
		int reachingMethods = callGraph.countReachingMethods(sub->id);

		// Lift gate — chunk-shrink criterion:
		//   1. Body must be substantial (≥ kMinBodyBytes) so the lift
		//      pays for the helper-Contract deploy overhead.
		//   2. Body bytes must dominate per-call itxn overhead so that
		//      even if all calls localize to one chunk, that chunk
		//      doesn't grow:  body_bytes ≥ kItxnOverhead × call_sites.
		// This is more permissive than a global-savings check (which
		// requires N_chunks ≥ 2 to ever win): we accept some lifts that
		// inflate the total bytecode footprint by ~helper_overhead/sub
		// in exchange for shrinking the largest chunk. That's the
		// tradeoff we want when chunks bump against the 8 KB cap.
		(void)kHelperOverhead;
		(void)reachingMethods;
		if (callSites == 0) continue;
		if (bodyBytes < kMinBodyBytes) continue;
		if (bodyBytes < kItxnOverhead * callSites) continue;
		int savings = bodyBytes;
		int cost = callSites * kItxnOverhead;
		if (false)
		{
			logger.info(
				"--deploy-pure-helpers: skipping '" + sub->name +
				"' (body=" + std::to_string(bodyBytes) +
				"B, calls=" + std::to_string(callSites) +
				", reach=" + std::to_string(reachingMethods) +
				"): savings " + std::to_string(savings) +
				" ≤ cost " + std::to_string(cost));
			continue;
		}
		logger.info(
			"--deploy-pure-helpers: candidate '" + sub->name +
			"' body=" + std::to_string(bodyBytes) +
			"B calls=" + std::to_string(callSites) +
			" reach=" + std::to_string(reachingMethods) +
			" (savings=" + std::to_string(savings) +
			", cost=" + std::to_string(cost) + ")");
		pureSubs.push_back(sub);
	}

	if (pureSubs.empty())
	{
		logger.info("--deploy-pure-helpers: no pure subroutines to extract");
		return out;
	}

	// Pass 1.5: apply user-supplied --pure-helper-split for the lifted
	// Subs that won't fit in one 8 KB sidecar. FunctionSplitter slices
	// each named Sub into N+1 pieces; we then deploy each piece as its
	// own one-method sidecar Contract and chain them at the call site
	// via an inner-txn group with `gload`-based live-vars threading
	// (slot 100).
	std::map<std::string, std::vector<size_t>> splitByName;
	for (auto const& s : _splitSpecs)
		splitByName[s.subroutineName] = s.splitPoints;

	// originalSubId → vector of piece sub pointers. For unsplit Subs,
	// vector has one entry: the original sub itself. For split Subs,
	// vector has N+1 entries: the FunctionSplitter-emitted pieces in
	// order (piece_0..piece_{N}).
	std::map<std::string, std::vector<std::shared_ptr<awst::Subroutine>>> piecesBySubId;
	for (auto const& sub : pureSubs)
	{
		auto sit = splitByName.find(sub->name);
		if (sit == splitByName.end())
		{
			piecesBySubId[sub->id] = {sub};
			continue;
		}
		FunctionSplitter fs;
		FunctionSplitter::PieceSpec ps;
		ps.subroutineName = sub->name;
		ps.splitPoints = sit->second;
		ps.groupId = 0;
		ps.crossChunk = true;     // pieces run as siblings in inner-txn group
		ps.prevCallStride = 1;    // each piece is one inner txn (no orch install)
		auto sr = fs.splitAt(_roots, {ps});
		if (!sr.didSplit)
		{
			logger.warning(
				"--pure-helper-split: failed to split '" + sub->name +
				"' — falling back to single sidecar");
			piecesBySubId[sub->id] = {sub};
			continue;
		}
		// FunctionSplitter pushes pieces onto _roots. Pull the ones
		// belonging to this sub by name-prefix match.
		std::vector<std::shared_ptr<awst::Subroutine>> pieces;
		for (auto const& nr : sr.newSubroutines)
			pieces.push_back(nr);
		// Sort pieces by their name's __piece_N suffix (FunctionSplitter
		// emits them in order, but be defensive).
		std::sort(pieces.begin(), pieces.end(),
			[](auto const& a, auto const& b) { return a->name < b->name; });
		piecesBySubId[sub->id] = std::move(pieces);
		logger.info(
			"--pure-helper-split: '" + sub->name + "' → " +
			std::to_string(piecesBySubId[sub->id].size()) + " pieces");
	}

	// Pass 2: build helper Contracts and call-site replacement table.
	// Per-piece tracking — a split Sub yields N+1 ExtractedHelper
	// entries, one sidecar per piece.
	struct PieceInfo {
		std::string templateVar;
		std::string helperContractId;
		std::string sig;
		awst::WType const* returnType = nullptr;  // void for non-last pieces
		std::shared_ptr<awst::Subroutine> sub;
	};
	std::map<std::string, std::vector<PieceInfo>> bySubId;
	int counter = 0;
	for (auto const& sub : pureSubs)
	{
		auto const& pieces = piecesBySubId[sub->id];
		std::vector<PieceInfo> infos;
		infos.reserve(pieces.size());
		for (auto const& piece : pieces)
		{
			PieceInfo pi;
			pi.sub = piece;
			pi.sig = canonicalSig(*piece);
			pi.returnType = piece->returnType;
			std::string suffix = std::to_string(counter++);
			pi.templateVar =
				"PURE_HELPER_" + sanitizeIdent(piece->name) +
				"_" + suffix + "_APP_ID";
			pi.helperContractId =
				"PureHelper__" + sanitizeIdent(piece->name) + "__" + suffix;

			ExtractedHelper eh;
			eh.subId = piece->id;
			eh.templateVarName = pi.templateVar;
			eh.helperContractId = pi.helperContractId;
			out.extracted.push_back(std::move(eh));

			logger.info(
				"--deploy-pure-helpers: lifting '" + piece->name +
				"' (sig='" + pi.sig + "') to " + pi.helperContractId);
			infos.push_back(std::move(pi));
		}
		bySubId[sub->id] = std::move(infos);
	}

	// Pass 3: build the helper Contracts (one per piece).
	for (auto const& sub : pureSubs)
		for (auto const& pi : bySubId[sub->id])
			out.helperContracts.push_back(
				buildHelperContract(*pi.sub, pi.helperContractId, pi.sig));

	// Pass 4: walk every body in every NON-helper root and rewrite
	// SubroutineCall(SubroutineID(extracted_id), ...) sites. For
	// unsplit Subs (pieces.size()==1) that's a single inner-app-call;
	// for split Subs it's a chained inner-txn group of M piece-calls
	// with the original args on piece 0 and the chunked-return helpers
	// hung off the last piece.
	std::set<std::string> allPieceIds;
	for (auto const& [_, infos] : bySubId)
		for (auto const& pi : infos)
			allPieceIds.insert(pi.sub->id);
	auto rewriteFn = [&bySubId](awst::Expression const& e)
		-> std::shared_ptr<awst::Expression>
	{
		auto const* sce = dynamic_cast<awst::SubroutineCallExpression const*>(&e);
		if (!sce) return nullptr;
		auto const* sid = std::get_if<awst::SubroutineID>(&sce->target);
		if (!sid) return nullptr;
		auto it = bySubId.find(sid->target);
		if (it == bySubId.end()) return nullptr;
		auto const& infos = it->second;
		std::vector<std::shared_ptr<awst::Expression>> args;
		std::vector<awst::WType const*> argTypes;
		for (auto const& a : sce->args)
		{
			args.push_back(a.value);
			argTypes.push_back(a.value ? a.value->wtype : nullptr);
		}
		if (infos.size() == 1)
		{
			auto const& pi = infos[0];
			return buildInnerCallReplacement(
				pi.templateVar, pi.sig, std::move(args), argTypes,
				sce->wtype, sce->sourceLocation);
		}
		// Multi-piece: build chained inner-txn group. Args go to piece 0.
		// Pieces 1..M-1 are invoked with empty args (their bodies start
		// with the gload prologue that pulls live vars from the previous
		// txn's scratch slot 100). The chunked-return helpers attach to
		// the LAST piece's sidecar (since that's where the chunks live).
		std::vector<std::tuple<std::string, std::string,
			std::vector<std::shared_ptr<awst::Expression>>,
			std::vector<awst::WType const*>>> pieceCalls;
		pieceCalls.reserve(infos.size());
		for (size_t i = 0; i < infos.size(); ++i)
		{
			auto const& pi = infos[i];
			if (i == 0)
				pieceCalls.emplace_back(pi.templateVar, pi.sig, args, argTypes);
			else
				pieceCalls.emplace_back(pi.templateVar, pi.sig,
					std::vector<std::shared_ptr<awst::Expression>>{},
					std::vector<awst::WType const*>{});
		}
		return buildChainedInnerCallReplacement(
			pieceCalls,
			infos.back().templateVar,  // chunked-return helpers hit last piece
			sce->wtype,
			sce->sourceLocation);
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
