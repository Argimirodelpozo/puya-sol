/// @file PureHelperExtractor.cpp
/// See header for design overview.

#include "builder/sol-types/TypeCoercion.h"
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

/// Canonical ARC4 type name for method signatures fed to MethodConstant
/// (puya hashes to the 4-byte selector at compile time).
std::string arc4TypeName(awst::WType const* _t)
{
	// Delegates to THE canonical WType namer. Uint256 = the ABI-selector collapse
	// this extractor has always used for bare biguint. NOTE the deliberate
	// difference from SimpleSplitter's chunk sigs (Uint512, puya's bare-biguint
	// router name) — now an explicit parameter instead of two silently-divergent
	// per-file copies. Aggregate handling improves: ARC4 structs/arrays render as
	// proper "(...)"/elem-suffix forms instead of the wtype's internal name().
	return builder::TypeCoercion::wtypeToABIName(
		_t, builder::TypeCoercion::BareBiguintName::Uint256);
}

/// Static ABI-encoded byte size of a return wtype (0 = dynamic/unknown).
/// Drives the single-log (≤1024 B) vs chunked-log (≤4096 B) vs skip path.
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
	// arc4.uintN: n/8 bytes (e.g. arc4.uint256 = 32).
	if (auto const* ui = dynamic_cast<awst::ARC4UIntN const*>(_t))
		return (ui->n() + 7) / 8;
	// ARC4 static array: count × elementSize; 0 if element is dynamic.
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_t))
	{
		int es = staticEncodedSize(sa->elementType());
		if (es == 0) return 0;
		return static_cast<int>(sa->arraySize()) * es;
	}
	// ARC4 struct: sum of field sizes; 0 if any field is dynamic.
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
	auto appId = awst::makeTxn("ApplicationID", awst::WType::uint64Type(), _loc);
	return awst::makeNumericCompare(
		std::move(appId), awst::NumericComparison::Eq,
		awst::makeIntegerConstant("0", _loc), _loc);
}

/// `txna ApplicationArgs <i>`.
std::shared_ptr<awst::Expression> appArgAt(int _i, awst::SourceLocation const& _loc)
{
	return awst::makeAppArg(_i, _loc);
}

/// MethodConstant(sig) — puya resolves to sha512_256(sig)[:4]; shared
/// by helper router and caller's ApplicationArgs[0] so they always agree.
std::shared_ptr<awst::Expression> selectorConst(
	std::string const& _sig, awst::SourceLocation const& _loc)
{
	return awst::makeMethodConstant(_sig, awst::WType::bytesType(), _loc);
}

/// Decode one fixed-size scalar from `_bytes` at `_offset`/`_size`.
/// `_bytes` should be a SingleEvaluation so the source isn't re-evaluated.
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
		auto getbit = awst::makeGetbit(
			std::move(extract), awst::makeIntegerConstant("0", _loc), _loc);
		return awst::makeNumericCompare(
			std::move(getbit), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", _loc), _loc);
	}
	if (_t == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(extract), awst::WType::accountType(), _loc);
	return awst::makeReinterpretCast(std::move(extract), _t, _loc);
}

/// Decode an ABI ApplicationArg bytes value to native wtype. Handles
/// scalars and fixed-size tuples field-by-field; reinterpret-casts the rest
/// (works for fixed-bytes / ARC4 aggregates whose bytes IS the AVM shape).
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
		auto getbit = awst::makeGetbit(
			std::move(_bytes), awst::makeIntegerConstant("0", _loc), _loc);
		return awst::makeNumericCompare(
			std::move(getbit), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", _loc), _loc);
	}
	if (_t == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(_bytes), awst::WType::accountType(), _loc);
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_t))
	{
		// Wrap in SingleEvaluation so multi-field decode doesn't re-evaluate
		// the source (e.g. `extract` of `itxn LastLog` must fire once).
		static int seCounter = 0;
		auto se = awst::makeSingleEvaluation(
			std::move(_bytes), awst::WType::bytesType(), ++seCounter, _loc);

		auto out = awst::makeTupleExpression(_t, _loc);
		int offset = 0;
		for (auto const* ft : tup->types())
		{
			int sz = staticEncodedSize(ft);
			if (sz == 0)
			{
				// Dynamic field: caller should have skipped this sub in pass-1.
				// Best-effort: pass remaining bytes verbatim and break.
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

/// Encode a value to ABI bytes (caller: ApplicationArgs; helper: log payload).
/// Handles scalars and fixed-size tuples by concat'ing per-field bytes.
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
		return awst::makeSetbit(
			awst::makeBytesConstant({0x00}, _loc),
			awst::makeIntegerConstant("0", _loc),
			std::move(_value), _loc);
	}
	if (_t == awst::WType::accountType())
		return awst::makeReinterpretCast(std::move(_value), awst::WType::bytesType(), _loc);
	if (auto const* tup = dynamic_cast<awst::WTuple const*>(_t))
	{
		// Concat per-field encoded bytes. SingleEvaluation guards
		// per-field TupleItemExpression reads from re-triggering side effects.
		static int seCounter = 1000;
		auto se = awst::makeSingleEvaluation(
			std::move(_value), _t, ++seCounter, _loc);

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

/// Wrap encoded return in `0x151f7c75 ++ ARC4(value)` so the caller's
/// `itxn LastLog` strip-4-and-decode recovers it.
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

/// Selector for the "fetch next chunk" entry point (chunked-return path).
/// Sidecars exceeding the 1024 B log cap also expose this; caller bundles
/// N-1 invocations after the main call; each helper-txn `gloadss` its chunk
/// from the main method's scratch.
constexpr char const* kBigReturnHelperSig = "__big_return_helper()void";

/// Emit `__big_return_helper` body: gloadss(0, 99+GroupIndex) and log it.
void appendBigReturnHelperBranch(
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	awst::SourceLocation const& _loc)
{
	// gloadss(0, 99+GroupIndex): txn 0 = main method call, slot = 99 + my GroupIndex.
	auto txnIdx = awst::makeIntegerConstant("0", _loc);
	auto myIdx = awst::makeTxn("GroupIndex", awst::WType::uint64Type(), _loc);
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

/// Build the helper Contract's approval body: routes one selector to the
/// lifted Subroutine. Also routes `__big_return_helper` when the return
/// exceeds the 1024 B log cap.
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
		// Two-selector dispatch: helperSig → emit chunk + return (early-exit);
		// then fall through to originalSig path; else assert false.
		auto helperBlock = awst::makeBlock(_loc);
		appendBigReturnHelperBranch(helperBlock->body, _loc);
		helperBlock->body.push_back(awst::makeReturnStatement(
			awst::makeBoolConstant(true, _loc), _loc));
		body->body.push_back(awst::makeIfElse(
			makeSelectorEq(kBigReturnHelperSig),
			std::move(helperBlock), nullptr, _loc));

		// Assert original selector after helper early-exit.
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

	// Sub stays in roots; this approval is its sole caller post-rewrite,
	// so per-Contract DCE keeps it here and drops it elsewhere.
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

		// Payload = 4 B prefix + retSize. ≤1024 B (localnet-confirmed log cap):
		// single `log`. Otherwise stash chunks 1..N-1 to scratch slots 100..100+N-2;
		// caller invokes `__big_return_helper` per chunk (gloadss txn 0 + log).
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
			// Stash chunks 1..N-1 to scratch slots kChunkBaseSlot+0..N-2;
			// each `__big_return_helper` sibling inner-txn gloadss and logs it.
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

/// Build the helper Contract wrapping a single pure Subroutine.
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

/// Build the inner-txn ApplicationCall expression replacing a
/// SubroutineCallExpression. Returns an Expression with matching wtype.
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
	auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(
		std::to_string(TxnTypeAppl), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
	// Include TMPL_ prefix per UrosSplitter convention; main.cpp strips it
	// for intTemplateVars, puya re-adds via template_vars_prefix.
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

	// Chunked-return: append N-1 `__big_return_helper()` inner txns.
	// Each gloadss its chunk from txn 0's scratch (slot 99+GroupIndex)
	// and emits it as LastLog; caller concats gitxn 0..N-1 LastLog.
	if (chunks > 1)
	{
		// Helper args: just the selector; chunk fetched via groupIndex from txn 0.
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
			auto helperCreate = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
			helperCreate->fields["TypeEnum"] = awst::makeIntegerConstant(
				std::to_string(TxnTypeAppl), _loc);
			helperCreate->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
			helperCreate->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
			helperCreate->fields["ApplicationID"] = awst::makeTemplateVar(
				"TMPL_" + _templateVar, awst::WType::uint64Type(), _loc);
			// Clone per inner txn — puya disallows shared sub-AST in itxns.
			auto haCopy = awst::makeTupleExpression(helperArgs->wtype, _loc);
			haCopy->items.push_back(selectorConst(kBigReturnHelperSig, _loc));
			helperCreate->fields["ApplicationArgs"] = std::move(haCopy);
			submit->itxns.push_back(std::move(helperCreate));
		}
	}

	if (!_retType || _retType == awst::WType::voidType())
		return submit;

	// Read the inner-call's logged ABI return.
	std::shared_ptr<awst::Expression> decoded;
	if (chunks <= 1)
	{
		// ≤1024 B payload: single LastLog, strip 4-byte prefix, decode.
		auto last = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);
		auto strip = awst::makeExtract(std::move(last), 4, 0, _loc);
		decoded = decodeArgFromBytes(std::move(strip), _retType, _loc);
	}
	else
	{
		// 1024 B+ payload: stitch chunks via gitxn i LastLog concats.
		// ≤4096 B decodes as one stack value. >4096 B (e.g. Honk Proof at 14080 B)
		// exceeds AVM max bytes-per-stack-element and fails at runtime —
		// fix: write each chunk to EVM memory blob and route reads through mload
		// (see [[rust-honk-status]]). Naive concat compiled for now; loadProof
		// runtime unverified pending the redesign.
		auto pickLog = [&](int idx) -> std::shared_ptr<awst::Expression>
		{
			auto c = awst::makeIntrinsicCall(
				"gitxn", awst::WType::bytesType(), _loc);
			c->immediates = {idx, std::string("LastLog")};
			return c;
		};
		std::shared_ptr<awst::Expression> readLog = pickLog(0);
		for (int i = 1; i < chunks; ++i)
			readLog = awst::makeConcat(std::move(readLog), pickLog(i), _loc);
		auto strip = awst::makeExtract(std::move(readLog), 4, 0, _loc);
		decoded = decodeArgFromBytes(std::move(strip), _retType, _loc);
	}

	// CommaExpression sequences (submit, decode) as a single expression slot,
	// drop-in replacing the original SubroutineCall site.
	auto comma = awst::makeCommaExpression(_retType, _loc);
	comma->expressions.push_back(std::move(submit));
	comma->expressions.push_back(std::move(decoded));
	return comma;
}

/// Multi-piece variant of `buildInnerCallReplacement` for `--pure-helper-split`.
/// Each piece is its own sidecar; call becomes an inner-txn group of M pieces.
/// State threads via scratch-slot-100 + gload (crossChunk=true, prevCallStride=1).
/// Chunked-return helpers attach to the same group at the last piece's sidecar.
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

		auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
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
		// Append returnChunks-1 helper txns at the last piece's sidecar
		// (mirrors single-piece path); each gloadss chunk i (slot 99+GroupIndex).
		for (int i = 1; i < returnChunks; ++i)
		{
			auto helperArgs = awst::makeTupleExpression(nullptr, _loc);
			helperArgs->items.push_back(selectorConst(kBigReturnHelperSig, _loc));
			std::vector<awst::WType const*> at;
			for (auto const& it : helperArgs->items) at.push_back(it->wtype);
			ownedTupleTypes.push_back(std::make_unique<awst::WTuple>(
				std::move(at), std::nullopt));
			helperArgs->wtype = ownedTupleTypes.back().get();

			auto hc = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
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

	// Last piece's return: chunk 0 at gitxn(basePieces-1) LastLog,
	// chunks 1..N-1 at gitxn(basePieces+i-1) LastLog.
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

	auto strip = awst::makeExtract(std::move(readLog), 4, 0, _loc);
	auto decoded = decodeArgFromBytes(std::move(strip), _retType, _loc);

	auto comma = awst::makeCommaExpression(_retType, _loc);
	comma->expressions.push_back(std::move(submit));
	comma->expressions.push_back(std::move(decoded));
	return comma;
}

/// True if `_e` has a side effect that makes it unsafe to lift into a sidecar:
/// state access (sidecar has empty storage, not the main app's), logs (only
/// LastLog visible to caller), inner txns, or asset/app-params queries
/// (depend on running app id).
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

	// Pass 1: identify pure sub candidates. Skip stubs / non-LastLog-fitting
	// returns, then apply a lift gate.
	//
	// Lift wins iff (N_chunks−1)×body_bytes > kHelperOverhead + calls×49.
	// N_chunks is approximated as N_methods_reaching_sub (upper bound;
	// bin-packing only collapses, never expands) — errs toward lifting more,
	// but kMinBodyBytes (300 B break-even) prevents pathological lifts.
	// kItxnOverhead (~100 B) measured empirically for 2-arg uint256 helpers.
	constexpr int kMinBodyBytes = 300;
	constexpr int kItxnOverhead = 100;
	constexpr int kHelperOverhead = 200;
	auto callGraph = buildStaticCallGraph(_roots);

	// Build sub-by-id index for transitive liftability check.
	std::map<std::string, std::shared_ptr<awst::Subroutine>> subById;
	for (auto const& r : _roots)
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r))
			subById[sub->id] = sub;

	// Transitive liftability: no side-effect ops in body AND all callees
	// also liftable. Memoized; cycles default optimistic-true.
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
			// Unknown callee (stub / external / untracked) — treat as unsafe.
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
		// Accept Solidity-pure (fast path) or transitively side-effect-free
		// by AWST walk (covers view/internal helpers not marked pure).
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
			// 4 B prefix + payload; caller concats chunks as one stack value
			// (AVM max bytes-per-element = 4096 B).
			int totalSize = 4 + retSize;
			// EXPERIMENT: 4096 B gate lifted to test if puya materialises the
			// concat into a memory blob. If "stack element too large", will need
			// chunked-decode + per-chunk mstore.
			(void)totalSize;
		}

		int bodyBytes = estimateBodyBytes(*sub->body);
		int callSites = countCallSites(_roots, sub->id);
		int reachingMethods = callGraph.countReachingMethods(sub->id);

		// Lift gate: body ≥ kMinBodyBytes AND body ≥ kItxnOverhead×calls.
		// More permissive than a global-savings check; accepts lifts that
		// inflate total footprint by ~helper_overhead to shrink the largest
		// chunk (the tradeoff we want near the 8 KB cap).
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

	// Pass 1.5: apply --pure-helper-split for lifted Subs too large for one
	// 8 KB sidecar. FunctionSplitter slices into N+1 pieces; each is its own
	// sidecar, chained via inner-txn group with gload live-vars (slot 100).
	std::map<std::string, std::vector<size_t>> splitByName;
	for (auto const& s : _splitSpecs)
		splitByName[s.subroutineName] = s.splitPoints;

	// originalSubId → piece sub pointers. Unsplit: one entry (original).
	// Split: N+1 entries (FunctionSplitter-emitted pieces, in order).
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
		// Pull pieces from _roots by name-prefix; sort by __piece_N suffix
		// (emitted in order, but sort defensively).
		std::vector<std::shared_ptr<awst::Subroutine>> pieces;
		for (auto const& nr : sr.newSubroutines)
			pieces.push_back(nr);
		std::sort(pieces.begin(), pieces.end(),
			[](auto const& a, auto const& b) { return a->name < b->name; });
		piecesBySubId[sub->id] = std::move(pieces);
		logger.info(
			"--pure-helper-split: '" + sub->name + "' → " +
			std::to_string(piecesBySubId[sub->id].size()) + " pieces");
	}

	// Pass 2: build helper Contracts and call-site replacement table.
	// A split Sub yields N+1 ExtractedHelper entries, one sidecar per piece.
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

	// Pass 4: rewrite SubroutineCall(SubroutineID(extracted_id),...) in all
	// non-helper roots. Unsplit: single inner-app-call. Split: chained
	// inner-txn group (args on piece 0, chunked-return helpers at last piece).
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
		// Multi-piece: args to piece 0; pieces 1..M-1 empty args (gload
		// prologue reads live vars from prev txn's scratch slot 100);
		// chunked-return helpers attach to last piece's sidecar.
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
			// Skip extracted subs — their body remains as the helper's callable.
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

	// Pass 5: prepend helpers to roots. UrosSplitter picks the LAST Contract
	// as primary; prepending keeps helpers from displacing it.
	// Subs stay in roots — per-Contract DCE drops them from non-helper contexts.
	_roots.insert(_roots.begin(), out.helperContracts.begin(), out.helperContracts.end());

	out.didExtract = true;
	logger.info("--deploy-pure-helpers: extracted " +
		std::to_string(out.helperContracts.size()) + " helper(s)");
	return out;
}

} // namespace puyasol::splitter
