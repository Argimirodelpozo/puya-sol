/// @file UrosSplitter.cpp

#include "splitter/UrosSplitter.h"

#include "Logger.h"
#include "awst/WType.h"
#include "builder/sol-types/TypeCoercion.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"
#include "splitter/AwstWalker.h"

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
		return awst::makeBiguintConstant("0", _loc);
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
		auto ns = awst::makeNewStruct(_t, _loc);
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
		auto te = awst::makeTupleExpression(_t, _loc);
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
			// Variable-length `bytes`: emit "byte[]" to match the
			// chunk-side ABI router signature (registered with the
			// orch). wtypeToABIName's fallback returns the raw type
			// name "bytes", giving a different selector — the
			// chunk-forward stub would push a selector that orch's
			// csel_<sel> box never sees, → dispatch box_get assert.
			return "byte[]";
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

	auto extract = awst::makeExtract3(std::move(_bytes), awst::makeIntegerConstant(_offset, _loc), awst::makeIntegerConstant(size, _loc), _loc);
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
		static int seCounter = 0;
		auto singleBytes = awst::makeSingleEvaluation(
			std::move(_bytes), awst::WType::bytesType(), ++seCounter, _loc);

		auto tuple = awst::makeTupleExpression(_ret, _loc);

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

// ─── og_sender / og_value globals (Pass 1) ────────────────────────────
//
// Every split-method call carries the user's identity (msg.sender) and
// payable amount (msg.value) into the chunk that runs on __storage.
// Those values are NOT visible from inside the chunk via Txn.Sender /
// gtxns Amount — the chunk runs as an inner-itxn issued by orch, so
// Txn.Sender = orch's app account and the group containing the user's
// pay txn isn't reachable.
//
// The fix has two halves:
//   1. main carries two app-global slots, __og_sender (account) and
//      __og_value (uint64). Main's forwarding stub writes them BEFORE
//      issuing the dispatch itxn and zeros them AFTER the call returns.
//   2. Chunk method bodies get patched (Pass 2/3) to read msg.sender /
//      msg.value from those globals via app_global_get_ex(MAIN, key).
//
// This file is the Pass 1 half: schema + setup/cleanup wrapping.
constexpr char const* kOgSenderKey = "__og_sender";
constexpr char const* kOgValueKey = "__og_value";

// Template-var names baked into chunk and main TEAL by puya. The deploy
// harness substitutes them at deploy time once the real app ids are
// known. See uros_dance.py::_substitute_*.
constexpr char const* kMainAppIdTmpl = "TMPL_UROS_MAIN_APP_ID";
constexpr char const* kStorageAppIdTmpl = "TMPL_UROS_STORAGE_APP_ID";

/// Build an AssignmentStatement that writes `_value` into main's
/// app-global slot named `_keyName`. The target is an AppStateExpression
/// keyed by a UTF-8 BytesConstant of the slot name.
std::shared_ptr<awst::Statement> makeAppGlobalPutStmt(
	std::string const& _keyName,
	awst::WType const* _valueType,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc)
{
	auto target = awst::makeAppStateExpression(
		awst::makeUtf8BytesConstant(_keyName, _loc, awst::WType::stateKeyType()),
		_valueType, _loc);
	return awst::makeAssignmentStatement(
		std::move(target), std::move(_value), _loc);
}

/// The original caller's "address" to store as __og_sender. Two cases:
///
///   1. EOA caller (user wallet): Txn.Sender is the user's 32-byte
///      Algorand address. Store as-is.
///   2. App caller (cross-contract): Txn.Sender is the caller app's
///      Algorand address = sha512_256("appID" + caller_app_id). That
///      hash is one-way, so puya-sol can't extract app_id back for
///      callbacks. Instead, store the puya-sol address convention
///      `b"\x00"*24 + itob(caller_app_id)` so chunks that do
///      `extract_uint64(msg.sender, 24)` to recover the app_id (and
///      cross-call back to it) get the right value.
///
/// Built as: `caller_id == 0 ? Txn.Sender : (bzero(24) ++ itob(caller_id))`
std::shared_ptr<awst::Expression> makeTxnSenderExpr(
	awst::SourceLocation const& _loc)
{
	auto sender = awst::makeTxn("Sender", awst::WType::accountType(), _loc);

	auto callerId = awst::makeGlobal(
		"CallerApplicationID", awst::WType::uint64Type(), _loc);
	auto zero = awst::makeIntegerConstant("0", _loc);
	auto isEOA = awst::makeNumericCompare(
		callerId, awst::NumericComparison::Eq, std::move(zero), _loc);

	// App-caller branch: build b"\x00"*24 ++ itob(caller_id) as account
	auto callerId2 = awst::makeGlobal(
		"CallerApplicationID", awst::WType::uint64Type(), _loc);
	auto idBytes = awst::makeItob(std::move(callerId2), _loc);
	// bzero(24) -- need a 24-byte zero prefix
	auto bzero24 = awst::makeIntrinsicCall(
		"bzero", awst::WType::bytesType(), _loc);
	bzero24->stackArgs.push_back(awst::makeIntegerConstant("24", _loc));
	auto padded = awst::makeConcat(std::move(bzero24), std::move(idBytes), _loc);
	auto appAddr = awst::makeReinterpretCast(
		std::move(padded), awst::WType::accountType(), _loc);

	// Ternary: isEOA ? Txn.Sender : (b"\x00"*24 ++ itob(caller_id))
	return awst::makeConditional(
		std::move(isEOA),
		std::move(sender),
		std::move(appAddr),
		awst::WType::accountType(),
		_loc);
}

/// `(GroupIndex > 0) ? gtxns(GroupIndex - 1, Amount) : 0` as uint64 —
/// matches `SolIntrinsicAccess::msg.value` lowering. Captures the user's
/// paired pay-txn amount when present, zero otherwise.
std::shared_ptr<awst::Expression> makeMsgValueUint64Expr(
	awst::SourceLocation const& _loc)
{
	auto groupIdx = awst::makeTxn("GroupIndex", awst::WType::uint64Type(), _loc);
	auto zero = awst::makeIntegerConstant("0", _loc);
	auto hasPay = awst::makeNumericCompare(
		groupIdx, awst::NumericComparison::Gt, std::move(zero), _loc);

	auto groupIdx2 = awst::makeTxn("GroupIndex", awst::WType::uint64Type(), _loc);
	auto one = awst::makeIntegerConstant("1", _loc);
	auto payIdx = awst::makeUInt64BinOp(
		std::move(groupIdx2), awst::UInt64BinaryOperator::Sub,
		std::move(one), _loc);

	auto amount = awst::makeGtxns(
		"Amount", std::move(payIdx), awst::WType::uint64Type(), _loc);

	auto zeroVal = awst::makeIntegerConstant("0", _loc);
	return awst::makeConditional(
		std::move(hasPay), std::move(amount), std::move(zeroVal),
		awst::WType::uint64Type(), _loc);
}

/// Build `app_params_get(STORAGE_APP_ID, AppAddress) . value` — runtime
/// expression yielding __storage's hash-form address. Used by the
/// pay-forward shim in main's stub: when the user's call to main is
/// paired with a pay txn, main forwards that value to __storage so
/// __storage holds the funds (chunks running on __storage then have
/// access to them via plain Receiver=address(this) inner pays).
std::shared_ptr<awst::Expression> makeStorageAddressExpr(
	awst::SourceLocation const& _loc)
{
	auto storageTmpl = awst::makeTemplateVar(
		kStorageAppIdTmpl, awst::WType::uint64Type(), _loc);

	auto* tupleType = makeOwnedType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::accountType(), awst::WType::boolType()},
		std::nullopt);

	auto get = awst::makeAppParamsGet(
		"AppAddress", std::move(storageTmpl), tupleType, _loc);

	return awst::makeTupleItem(
		get, 0, awst::WType::accountType(), _loc);
}

/// Memberids for the synthetic helpers on every main contract. All are
/// called via SubroutineCallExpression(InstanceMethodTarget) from each
/// forwarding stub. Without these, inlining the bodies per-stub blows
/// main's bytecode past the 8 KB AVM cap on contracts with many split
/// methods (AccessManagerEnumerable: 40+ stubs).
constexpr char const* kForwardValueMethodName = "__uros_forward_value";
constexpr char const* kOgSetupMethodName = "__uros_og_setup";
constexpr char const* kOgCleanupMethodName = "__uros_og_cleanup";

// Forward declarations — these helpers are defined further down but
// referenced by makeOgSetupBody / makeOgCleanupBody below.
std::vector<std::shared_ptr<awst::Statement>> makeOgSetupStmts(
	awst::SourceLocation const& _loc);
std::vector<std::shared_ptr<awst::Statement>> makeOgCleanupStmts(
	awst::SourceLocation const& _loc);

/// Body of __uros_forward_value (called via callsub from every stub):
///
///     uint64 amount = msg.value          // ternary on GroupIndex
///     if (amount > 0) {
///         itxn pay
///           Receiver = __storage.app_address
///           Amount   = amount
///           Fee      = 0
///         itxn_submit
///     }
///
/// Sender defaults to main (= currentApp). Result: the user's paired
/// pay txn lands on main, main forwards it to __storage, __storage
/// has the funds. Chunks issuing payments later draw from __storage's
/// own balance (Sender = __storage = currentApp inside the chunk),
/// no rekey needed.
std::shared_ptr<awst::Block> makeForwardValueBody(
	awst::SourceLocation const& _loc)
{
	auto block = awst::makeBlock(_loc);

	// amount := msg.value-uint64
	auto amountVar = awst::makeVarExpression(
		"__amount", awst::WType::uint64Type(), _loc);
	block->body.push_back(awst::makeAssignmentStatement(
		amountVar, makeMsgValueUint64Expr(_loc), _loc));

	// itxn pay: Receiver=__storage.address, Amount=amount, Fee=0
	static awst::WInnerTransactionFields s_payFieldsType(1);
	auto create = awst::makeCreateInnerTransaction(&s_payFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant("1", _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["Receiver"] = makeStorageAddressExpr(_loc);
	create->fields["Amount"] = amountVar;

	static awst::WInnerTransaction s_payTxnType(1);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto submitBlock = awst::makeBlock(_loc);
	submitBlock->body.push_back(awst::makeExpressionStatement(submit, _loc));

	auto cond = awst::makeNumericCompare(
		amountVar, awst::NumericComparison::Gt,
		awst::makeIntegerConstant("0", _loc), _loc);
	block->body.push_back(awst::makeIfElse(
		std::move(cond), std::move(submitBlock), nullptr, _loc));

	block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	return block;
}

/// Helper for building an internal-only ContractMethod with a body
/// supplied by `_bodyBuilder`. No ARC4 method config — the method is
/// callable only via SubroutineCallExpression(InstanceMethodTarget),
/// not through the outer ABI router.
awst::ContractMethod makeInternalHelperMethod(
	std::string const& _cref, std::string const& _name,
	std::shared_ptr<awst::Block> _body, awst::SourceLocation const& _loc)
{
	awst::ContractMethod m;
	m.sourceLocation = _loc;
	m.cref = _cref;
	m.memberName = _name;
	m.returnType = awst::WType::voidType();
	m.body = std::move(_body);
	return m;
}

/// Build the synthetic `__uros_forward_value() void` ContractMethod on
/// main. Called via callsub from every forwarding stub.
awst::ContractMethod makeForwardValueMethod(
	std::string const& _cref, awst::SourceLocation const& _loc)
{
	return makeInternalHelperMethod(
		_cref, kForwardValueMethodName, makeForwardValueBody(_loc), _loc);
}

/// Body of __uros_og_setup(): write Txn.Sender → __og_sender,
/// msg.value-uint64 → __og_value. Inlining these per-stub adds ~12
/// bytes per stub × 40+ stubs on AccessManagerEnumerable, blowing
/// the 8 KB cap.
std::shared_ptr<awst::Block> makeOgSetupBody(
	awst::SourceLocation const& _loc)
{
	auto block = awst::makeBlock(_loc);
	for (auto& s : makeOgSetupStmts(_loc))
		block->body.push_back(std::move(s));
	block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	return block;
}

/// Body of __uros_og_cleanup(): write zero address → __og_sender,
/// 0 → __og_value. Mirror of og_setup; same dedup motivation.
std::shared_ptr<awst::Block> makeOgCleanupBody(
	awst::SourceLocation const& _loc)
{
	auto block = awst::makeBlock(_loc);
	for (auto& s : makeOgCleanupStmts(_loc))
		block->body.push_back(std::move(s));
	block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	return block;
}

awst::ContractMethod makeOgSetupMethod(
	std::string const& _cref, awst::SourceLocation const& _loc)
{
	return makeInternalHelperMethod(
		_cref, kOgSetupMethodName, makeOgSetupBody(_loc), _loc);
}

awst::ContractMethod makeOgCleanupMethod(
	std::string const& _cref, awst::SourceLocation const& _loc)
{
	return makeInternalHelperMethod(
		_cref, kOgCleanupMethodName, makeOgCleanupBody(_loc), _loc);
}

/// One-statement callsub to a named internal helper on main.
std::shared_ptr<awst::Statement> makeInternalCallsubStmt(
	std::string const& _name, awst::SourceLocation const& _loc)
{
	auto call = awst::makeSubroutineCall(
		awst::InstanceMethodTarget{_name}, awst::WType::voidType(), _loc);
	return awst::makeExpressionStatement(std::move(call), _loc);
}

/// Setup statements: write Txn.Sender → __og_sender, msg.value →
/// __og_value. Inserted at the top of every forwarding stub body.
std::vector<std::shared_ptr<awst::Statement>> makeOgSetupStmts(
	awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Statement>> stmts;
	stmts.push_back(makeAppGlobalPutStmt(
		kOgSenderKey, awst::WType::accountType(),
		makeTxnSenderExpr(_loc), _loc));
	stmts.push_back(makeAppGlobalPutStmt(
		kOgValueKey, awst::WType::uint64Type(),
		makeMsgValueUint64Expr(_loc), _loc));
	return stmts;
}

/// Cleanup statements: zero out the og_* slots. Run before the stub
/// returns so the values don't leak across calls. We `put` zero rather
/// than `del` so subsequent reads return a defined value (zero address /
/// zero uint) instead of `exists=false` — defensive against any future
/// chunk code path that reads og_* outside a wrapped call.
std::vector<std::shared_ptr<awst::Statement>> makeOgCleanupStmts(
	awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Statement>> stmts;
	auto zeroAddr = awst::makeReinterpretCast(
		awst::makeBytesConstant(std::vector<uint8_t>(32, 0), _loc),
		awst::WType::accountType(), _loc);
	stmts.push_back(makeAppGlobalPutStmt(
		kOgSenderKey, awst::WType::accountType(),
		std::move(zeroAddr), _loc));
	stmts.push_back(makeAppGlobalPutStmt(
		kOgValueKey, awst::WType::uint64Type(),
		awst::makeIntegerConstant("0", _loc), _loc));
	return stmts;
}

/// Inject __og_sender / __og_value into the contract's appState +
/// stateTotals. Called once on the main contract after stub bodies
/// are installed.
void appendOgGlobalsToContract(
	awst::Contract& _c, awst::SourceLocation const& _loc)
{
	awst::AppStorageDefinition senderDef;
	senderDef.sourceLocation = _loc;
	senderDef.memberName = kOgSenderKey;
	senderDef.storageKind = awst::AppStorageKind::AppGlobal;
	senderDef.storageWType = awst::WType::accountType();
	senderDef.key = awst::makeUtf8BytesConstant(
		kOgSenderKey, _loc, awst::WType::stateKeyType());
	senderDef.description = "uros: original caller of the split method "
		"(written by main's stub, read by chunks via app_global_get_ex)";
	_c.appState.push_back(std::move(senderDef));

	awst::AppStorageDefinition valueDef;
	valueDef.sourceLocation = _loc;
	valueDef.memberName = kOgValueKey;
	valueDef.storageKind = awst::AppStorageKind::AppGlobal;
	valueDef.storageWType = awst::WType::uint64Type();
	valueDef.key = awst::makeUtf8BytesConstant(
		kOgValueKey, _loc, awst::WType::stateKeyType());
	valueDef.description = "uros: original msg.value of the split method "
		"(uint64; chunks read via app_global_get_ex)";
	_c.appState.push_back(std::move(valueDef));

	// stateTotals: bump global byteslice + uint counts. The struct itself
	// is optional (puya allows the contract to leave it unset, in which
	// case puya counts from appState entries directly), so we only adjust
	// it when the user contract explicitly reserved counts.
	if (_c.stateTotals.has_value())
	{
		auto& st = *_c.stateTotals;
		if (st.globalBytes.has_value())
			st.globalBytes = *st.globalBytes + 1;
		if (st.globalUints.has_value())
			st.globalUints = *st.globalUints + 1;
	}
}

// ─── Chunk patches (Pass 2/3/4) ───────────────────────────────────────
//
// Chunk method bodies are the user's REAL Solidity logic — they read
// msg.sender, msg.value, address(this), etc. as if they were running
// in main. But chunks actually run on __storage (orch issues an inner
// txn to __storage with the chunk's bytecode installed), so:
//
//   * Txn.Sender resolves to orch's app account (orch sent the itxn)
//   * msg.value (gtxns Amount of paired pay) sees orch's inner-itxn
//     group context, not the user's outer group
//   * Global.CurrentApplicationAddress / Global.CurrentApplicationID
//     resolve to __storage, not main
//
// Pass 1 already plumbed the user's identity into main's app-globals
// (__og_sender, __og_value). Passes 2 and 3 rewrite chunk method bodies
// to read those globals via app_global_get_ex(TMPL_UROS_MAIN_APP_ID,
// key) instead of the local AVM intrinsics. Pass 4 (separate commit)
// rewrites address(this) reads to main's actual app address via
// TMPL_UROS_MAIN_ADDR so balance() and downstream comparisons see the
// user-facing identity.

/// Build `app_global_get_ex(MAIN_ID_TMPL, key) . value` reinterpreted as
/// the requested wtype. Returns the value half of the (value, exists)
/// tuple — chunks always read mid-call when og_* is populated, so the
/// exists half can be discarded.
std::shared_ptr<awst::Expression> makeMainGlobalRead(
	std::string const& _keyName,
	awst::WType const* _readWType,    // bytes for account, uint64 for value
	awst::WType const* _resultWType,  // accountType / uint64Type
	awst::SourceLocation const& _loc)
{
	auto mainTmpl = awst::makeTemplateVar(
		kMainAppIdTmpl, awst::WType::uint64Type(), _loc);

	auto keyConst = awst::makeUtf8BytesConstant(_keyName, _loc);

	auto* tupleType = makeOwnedType<awst::WTuple>(
		std::vector<awst::WType const*>{_readWType, awst::WType::boolType()},
		std::nullopt);

	auto getEx = awst::makeIntrinsicCall(
		"app_global_get_ex", tupleType, _loc);
	getEx->stackArgs.push_back(std::move(mainTmpl));
	getEx->stackArgs.push_back(std::move(keyConst));

	auto value = awst::makeTupleItem(getEx, 0, _readWType, _loc);
	if (_resultWType == _readWType)
		return value;
	return awst::makeReinterpretCast(std::move(value), _resultWType, _loc);
}

/// Pass 2 rewriter: every literal `IntrinsicCall("txn", "Sender")` in a
/// chunk method body becomes a read of main's __og_sender global.
/// `Txn.Sender` references that DON'T denote msg.sender semantically —
/// e.g. inside an inner-txn field assignment, `itxn ApplicationArgs`
/// pulled from main's outer call — are extremely rare in chunk code
/// after puya-sol's lowering; the original `txn Sender` intrinsic is
/// the canonical msg.sender shape, so a strict AST match is safe.
std::shared_ptr<awst::Expression> patchMsgSenderExpr(
	awst::Expression const& _e)
{
	auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(&_e);
	if (!ic) return nullptr;
	if (ic->opCode != "txn") return nullptr;
	if (ic->immediates.empty()) return nullptr;
	auto const* imm = std::get_if<std::string>(&ic->immediates[0]);
	if (!imm || *imm != "Sender") return nullptr;
	return makeMainGlobalRead(
		kOgSenderKey, awst::WType::bytesType(),
		awst::WType::accountType(), ic->sourceLocation);
}

/// Pass 3 rewriter: detect the canonical `msg.value` AST shape and
/// replace it with a read of main's __og_value global.
///
/// puya-sol lowers `msg.value` to:
///
///     ConditionalExpression<uint64>(
///       cond=NumericCompare(IntrinsicCall("txn", GroupIndex) > 0),
///       trueExpr=IntrinsicCall("gtxns", Amount, GroupIndex - 1),
///       falseExpr=IntegerConstant("0"))
///
/// The whole conditional is then wrapped in an Itob+ReinterpretCast to
/// biguint by the caller; we replace just the inner Conditional and let
/// the outer wrapping convert our uint64 read to biguint as before.
///
/// Strict AST match — any user code that happens to write the same
/// ternary by hand would also be rewritten, but that pattern is the
/// puya-sol lowering of msg.value specifically. It's rare for hand-
/// written Solidity to express msg.value any other way.
std::shared_ptr<awst::Expression> patchMsgValueExpr(
	awst::Expression const& _e)
{
	auto const* cond = dynamic_cast<awst::ConditionalExpression const*>(&_e);
	if (!cond) return nullptr;
	if (cond->wtype != awst::WType::uint64Type()) return nullptr;

	// Condition: txn GroupIndex > 0
	auto const* nc = dynamic_cast<awst::NumericComparisonExpression const*>(
		cond->condition.get());
	if (!nc || nc->op != awst::NumericComparison::Gt) return nullptr;
	{
		auto const* lhs = dynamic_cast<awst::IntrinsicCall const*>(nc->lhs.get());
		if (!lhs || lhs->opCode != "txn" || lhs->immediates.empty())
			return nullptr;
		auto const* imm = std::get_if<std::string>(&lhs->immediates[0]);
		if (!imm || *imm != "GroupIndex") return nullptr;
	}
	{
		auto const* rhs = dynamic_cast<awst::IntegerConstant const*>(nc->rhs.get());
		if (!rhs || rhs->value != "0") return nullptr;
	}

	// trueExpr: gtxns Amount (GroupIndex - 1) — accept any stack arg
	// because we don't want a tight-coupled match on the (GroupIndex - 1)
	// expression's exact shape.
	{
		auto const* gtxns = dynamic_cast<awst::IntrinsicCall const*>(
			cond->trueExpr.get());
		if (!gtxns || gtxns->opCode != "gtxns" || gtxns->immediates.empty())
			return nullptr;
		auto const* imm = std::get_if<std::string>(&gtxns->immediates[0]);
		if (!imm || *imm != "Amount") return nullptr;
	}

	// falseExpr: IntegerConstant("0")
	{
		auto const* fc = dynamic_cast<awst::IntegerConstant const*>(
			cond->falseExpr.get());
		if (!fc || fc->value != "0") return nullptr;
	}

	return makeMainGlobalRead(
		kOgValueKey, awst::WType::uint64Type(),
		awst::WType::uint64Type(), cond->sourceLocation);
}

/// Pass 4 rewriter: every literal `IntrinsicCall("global",
/// "CurrentApplicationAddress")` (the AST shape Solidity lowers
/// `address(this)` to) is rewritten as a runtime read of main's app
/// address via `app_params_get AppAddress` keyed by
/// TMPL_UROS_MAIN_APP_ID.
///
///     app_params_get(TMPL_UROS_MAIN_APP_ID, AppAddress)  →  (account, bool)
///     . item[0]  // the address; ignore exists, main is always deployed
///
/// We reuse the integer TMPL_UROS_MAIN_APP_ID rather than introducing
/// a new bytes template var (TMPL_UROS_MAIN_ADDR) because the runtime
/// hash address can be re-derived from the app id via app_params_get
/// at zero stored-bytes cost. Three extra opcodes per read versus a
/// pushbytes template, but no new options.json plumbing.
///
/// Chunk runtime requirement: main's app id must be in the inner-txn's
/// foreign-apps when orch dispatches into __storage. Otherwise
/// app_params_get reverts on resource availability. The orch update
/// to forward main_id (via global CallerApplicationID at the orch
/// frame) lands separately.
/// Helper that builds the (account, bool) tuple expression returned by
/// `app_params_get(MAIN_ID, AppAddress)`. Used by Pass 4 (address(this))
/// and Pass 5 (inner-pay Sender override). Both extract the value half
/// via TupleItem; the exists half is unused (main is always deployed
/// by the time chunks run).
std::shared_ptr<awst::Expression> makeMainAddressExpr(
	awst::SourceLocation const& _loc)
{
	auto mainTmpl = awst::makeTemplateVar(
		kMainAppIdTmpl, awst::WType::uint64Type(), _loc);

	auto* tupleType = makeOwnedType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::accountType(), awst::WType::boolType()},
		std::nullopt);

	auto get = awst::makeAppParamsGet(
		"AppAddress", std::move(mainTmpl), tupleType, _loc);

	return awst::makeTupleItem(
		get, 0, awst::WType::accountType(), _loc);
}

std::shared_ptr<awst::Expression> patchAddressThisExpr(
	awst::Expression const& _e)
{
	auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(&_e);
	if (!ic) return nullptr;
	if (ic->opCode != "global") return nullptr;
	if (ic->immediates.empty()) return nullptr;
	auto const* imm = std::get_if<std::string>(&ic->immediates[0]);
	if (!imm || *imm != "CurrentApplicationAddress") return nullptr;
	return makeMainAddressExpr(ic->sourceLocation);
}

/// Pass 5 rewriter: every `CreateInnerTransaction` inside a chunk method
/// body (regardless of TypeEnum) has its `Sender` field set to main's
/// address. Combined with the bidirectional rekey
/// (main->__storage and __storage->main) installed at deploy time:
///   * main->__storage rekey: __storage signs for main, so chunk-emitted
///     itxns with Sender=main_addr are authorised by AVM (the issuing app
///     for these is __storage, since chunks run on __storage; AVM walks
///     the rekey chain and admits because main.AuthAddr == __storage_addr).
///   * __storage->main rekey: main signs for __storage, so main-stub-emitted
///     itxns with Sender=__storage_addr are authorised symmetrically.
///
/// External contracts called by chunks therefore observe Sender=main_addr,
/// preserving the "main is the in-address and the out-address" property
/// the user wants regardless of inner-txn type (pay, appl, axfer, etc.).
///
/// We do NOT override Sender if the user code already set it explicitly.
std::shared_ptr<awst::Expression> patchInnerTxnSenderExpr(
	awst::Expression const& _e)
{
	auto const* cit = dynamic_cast<awst::CreateInnerTransaction const*>(&_e);
	if (!cit) return nullptr;

	// Don't overwrite a user-supplied Sender.
	if (cit->fields.count("Sender")) return nullptr;

	// Clone (preserve all existing fields), inject Sender override.
	auto cloned = awst::makeCreateInnerTransaction(cit->wtype, cit->sourceLocation);
	cloned->fields = cit->fields;
	cloned->fields["Sender"] = makeMainAddressExpr(cit->sourceLocation);
	return cloned;
}

/// Apply all chunk-side patches to a chunk method body. Single walker
/// invocation per method — combined predicate ensures each Expression
/// slot only gets visited once.
///
/// Pass 5 (patchInnerTxnSenderExpr) overrides Sender on every
/// chunk-emitted inner txn to main_addr. This requires the bidirectional
/// rekey to be installed at deploy time: main->__storage gives __storage
/// authority over main, so chunks running on __storage can sign for
/// main_addr; symmetrically __storage->main lets main's stubs sign for
/// __storage_addr (see makeForwardingStubBody / makeForwardValueBody).
void patchChunkMethodBody(awst::ContractMethod& _m)
{
	if (!_m.body) return;
	// Pass A: rewrite leaf expressions (Txn.Sender, msg.value,
	// address(this)). These run first so they apply EVEN INSIDE
	// CreateInnerTransaction.fields slots — the walker's visitSlot
	// stops descending after a replacement, so wrapping passes (B)
	// must come second.
	auto leafFn = [](awst::Expression const& e) -> std::shared_ptr<awst::Expression> {
		if (auto r = patchMsgSenderExpr(e)) return r;
		if (auto r = patchMsgValueExpr(e)) return r;
		if (auto r = patchAddressThisExpr(e)) return r;
		return nullptr;
	};
	walkBlock(*_m.body, leafFn);
	// Pass B: wrap inner txns with Sender = main_addr injection. The
	// walker stops descending after this replacement, but Pass A
	// already handled the inner `txn Sender` reads inside the
	// CreateInnerTransaction.fields above.
	auto wrapFn = [](awst::Expression const& e) -> std::shared_ptr<awst::Expression> {
		if (auto r = patchInnerTxnSenderExpr(e)) return r;
		return nullptr;
	};
	walkBlock(*_m.body, wrapFn);
}

/// Forwarding stub body for main: inner-calls the orch with the
/// user's (selector + args) tagged onto the dispatch selector. The
/// orch then runs the install→call→restore dance against __storage
/// and returns __storage's last_log, which we decode to the method's
/// declared return type.
///
/// Body shape (Pass 1: og_* setup/cleanup wrapping the existing dance):
///   __og_sender = Txn.Sender
///   __og_value  = msg.value (ternary on GroupIndex)
///   itxn appl → orch.dispatch(selector + args)
///   [if non-void] tmp = decode(itxn LastLog)
///   __og_sender = zero address
///   __og_value  = 0
///   return [tmp]
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

	// og_setup is factored to an internal callsub'd helper on main
	// (kOgSetupMethodName). Inlining its body per-stub blew main's
	// bytecode past the 8 KB cap on contracts with many split methods
	// (AccessManagerEnumerable has 40+).
	//
	// The pay-forward shim (kForwardValueMethodName) is no longer
	// invoked: under the bidirectional rekey design, chunk-emitted
	// itxns set Sender=main_addr (Pass 5), so they draw from main's
	// balance directly. There's no need to move the user's paired
	// pay-txn value over to __storage. The method body is left in
	// place as dead code in case future variants want to re-enable
	// the forward path.
	block->body.push_back(makeInternalCallsubStmt(
		kOgSetupMethodName, _loc));

	// Build the inner-txn ApplicationArgs tuple.
	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);

	// [0]: orch.dispatch() ARC4 selector. MethodConstant lets puya
	// compute the 4-byte selector from the canonical signature.
	argsTuple->items.push_back(awst::makeMethodConstant(
		"dispatch()byte[]", awst::WType::bytesType(), _loc));

	// [1]: this method's ARC4 selector. The orch reads this from its
	// own ApplicationArgs[1], looks up the matching chunk, and
	// forwards the call.
	argsTuple->items.push_back(awst::makeMethodConstant(
		buildSelectorSig(_m), awst::WType::bytesType(), _loc));

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
		argsTuple->items.push_back(awst::makeAppArg(int(i + 1), _loc));

	// WTuple type for the ApplicationArgs slot.
	std::vector<awst::WType const*> argTypes(
		argsTuple->items.size(), awst::WType::bytesType());
	argsTuple->wtype = makeOwnedType<awst::WTuple>(std::move(argTypes), std::nullopt);

	// Build CreateInnerTransaction(appl, app_id=orch, args=tuple, fee=0).
	static awst::WInnerTransactionFields s_applFieldsType(
		static_cast<int>(TXN_TYPE_APPL));
	auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(
		std::to_string(TXN_TYPE_APPL), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
	create->fields["ApplicationID"] = awst::makeTemplateVar(
		"TMPL_UROS_ORCH_APP_ID", awst::WType::uint64Type(), _loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);
	// Bidirectional rekey: main->__storage at deploy time strips main of
	// authority over its own account, so the inner txn can't use the
	// default Sender=main_addr. __storage->main rekey grants main signing
	// authority over __storage's address, so we set Sender=storage_addr
	// here. AVM walks the rekey chain (storage.AuthAddr == main_addr) and
	// admits.
	create->fields["Sender"] = makeStorageAddressExpr(_loc);

	// Wrap in SubmitInnerTransaction.
	static awst::WInnerTransaction s_applTxnType(
		static_cast<int>(TXN_TYPE_APPL));
	auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	// Void return: submit, cleanup og_*, return.
	if (!_m.returnType || _m.returnType == awst::WType::voidType())
	{
		block->body.push_back(awst::makeExpressionStatement(std::move(submit), _loc));
		for (auto& s : makeOgCleanupStmts(_loc))
			block->body.push_back(std::move(s));
		block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
		return block;
	}

	// Non-void: submit, then unwrap inner.LastLog and decode to the
	// declared return type.
	//
	// The wrapping is deeper than a single ABI prefix because orch.dispatch
	// returns Bytes (raw chunk log), not the original method's return type.
	// The actual log emitted by orch on its outer txn is:
	//
	//   0x151f7c75       (orch's ABI return prefix)
	//   <uint16 len>     (ARC4 length prefix of the Bytes return)
	//   <chunk log>      (= 0x151f7c75 + ARC4(<actual return value>))
	//
	// So to recover the actual return value we strip 4 + 2 + 4 = 10 bytes
	// from the front of itxn LastLog.
	//
	// CAREFUL: the og_* cleanup must run AFTER the decoded value is
	// captured in a temp var. Otherwise the decode (which still reads
	// `itxn LastLog`) could end up after `app_global_put` statements
	// that reset whatever `LastLog` would resolve to (puya's last-itxn
	// reference is brittle). Materialising the decoded value into a
	// local first puts it on solid ground, and the cleanup writes
	// fall AFTER all uses of itxn-derived state.
	block->body.push_back(awst::makeExpressionStatement(std::move(submit), _loc));

	auto readLog = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);

	auto stripPrefix = awst::makeExtract(std::move(readLog), 10, 0, _loc);

	auto decoded = decodeFromBytes(std::move(stripPrefix), _m.returnType, _loc);

	// Bind the decoded value to a temp local, run cleanup, then return
	// the local. Each forwarding stub gets its own counter-suffixed name
	// so multiple stubs in the same contract don't collide on var ids.
	static int s_retVarCounter = 0;
	std::string retVarName =
		"__uros_ret_" + std::to_string(s_retVarCounter++);
	auto retVar = awst::makeVarExpression(retVarName, _m.returnType, _loc);
	block->body.push_back(awst::makeAssignmentStatement(
		retVar, std::move(decoded), _loc));

	block->body.push_back(makeInternalCallsubStmt(
		kOgCleanupMethodName, _loc));

	block->body.push_back(awst::makeReturnStatement(std::move(retVar), _loc));
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

/// Build the synthetic `__rekey_to_storage(address)void` method on
/// main that rekeys main's app account to the supplied address (the
/// __storage app's account). Called once at deploy time after both
/// main and __storage are created.
///
/// Body:
///     itxn pay
///       Receiver = address(this)
///       Amount   = 0
///       Fee      = 0
///       RekeyTo  = storage_addr  (method arg)
///     itxn_submit
///
/// AVM honors the rekey because the inner txn comes from main itself
/// (Sender defaults to CurrentApplicationAddress); after submission,
/// main's account has AuthAddr = storage_addr. Subsequent inner txns
/// issued by __storage code that set Sender = main's address (Pass 5)
/// are then admitted by AVM via the rekey relationship.
///
/// Security note: this method has no caller-auth check. An adversary
/// could call it again to re-rekey main to an attacker-controlled
/// address. Mitigation for follow-up work: add a `rekeyed` global
/// flag and assert false on subsequent calls. For now the deploy
/// harness calls it exactly once at setup, before any user-facing
/// txns can land.
awst::ContractMethod makeRekeyToStorageMethod(
	std::string const& _cref, awst::SourceLocation const& _loc)
{
	awst::ContractMethod m;
	m.sourceLocation = _loc;
	m.cref = _cref;
	m.memberName = "__rekey_to_storage";
	m.returnType = awst::WType::voidType();

	awst::SubroutineArgument arg;
	arg.name = "storage_addr";
	arg.sourceLocation = _loc;
	arg.wtype = awst::WType::accountType();
	m.args.push_back(std::move(arg));

	auto block = awst::makeBlock(_loc);

	static awst::WInnerTransactionFields s_payFieldsType(1);
	auto create = awst::makeCreateInnerTransaction(&s_payFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant("1", _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["Amount"] = awst::makeIntegerConstant("0", _loc);
	// Receiver = address(this). Use the raw global intrinsic — main's
	// stub bodies (this is one) aren't subject to the chunk-side Pass 4
	// patch; they always run on main, so Global.CurrentApplicationAddress
	// is correct here.
	auto recv = awst::makeGlobal(
		"CurrentApplicationAddress", awst::WType::accountType(), _loc);
	create->fields["Receiver"] = std::move(recv);
	create->fields["RekeyTo"] = awst::makeVarExpression(
		"storage_addr", awst::WType::accountType(), _loc);

	static awst::WInnerTransaction s_payTxnType(1);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	block->body.push_back(awst::makeExpressionStatement(submit, _loc));
	block->body.push_back(awst::makeReturnStatement(nullptr, _loc));
	m.body = block;

	awst::ARC4ABIMethodConfig cfg;
	cfg.sourceLocation = _loc;
	cfg.allowedCompletionTypes = {0}; // NoOp
	cfg.create = 3;  // Disallow — main is created via the explicit
	                  // ApplicationCreateTxn in the deploy harness, never
	                  // through this method.
	cfg.name = "__rekey_to_storage";
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
	mainContract->methods.push_back(
		makeForwardValueMethod(primary->id, primary->sourceLocation));
	mainContract->methods.push_back(
		makeOgSetupMethod(primary->id, primary->sourceLocation));
	mainContract->methods.push_back(
		makeOgCleanupMethod(primary->id, primary->sourceLocation));
	// __rekey_to_storage(address)void: bootstrap method called once at
	// deploy time. After it runs, main.AuthAddr = __storage_addr and
	// main can no longer sign for itself — every subsequent itxn from
	// main uses Sender=__storage_addr (see makeForwardingStubBody) and
	// the bidirectional rekey makes that authorisation succeed.
	mainContract->methods.push_back(
		makeRekeyToStorageMethod(primary->id, primary->sourceLocation));

	// Pass 1 of the og_sender / og_value plumbing: declare the two
	// app-global slots on main so puya emits the right state schema.
	// Stubs (above) already write to them via AppStateExpression with
	// these names. Chunks (Pass 2/3, future commits) will read them via
	// app_global_get_ex(MAIN_ID, ...).
	appendOgGlobalsToContract(*mainContract, primary->sourceLocation);

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

		// Pre-compute which internal methods are reachable from this
		// chunk's group (transitively, through InstanceMethodTarget
		// calls). Internal methods (no arc4MethodConfig) must keep
		// their real bodies if reachable from a live ABI method —
		// otherwise the chunked method would hit a stub returning a
		// default value and silently drop side effects (e.g.
		// `_updateReservePriceSource` issuing the inner-call to
		// `oracle.setReserveSource`).
		std::set<std::string> reachableInternalNames;
		{
			std::set<std::string> seenAbi;
			std::vector<std::string> worklist;
			std::map<std::string, awst::ContractMethod*> byName;
			for (auto& m : chunkContract->methods)
				byName[m.memberName] = &m;
			for (auto const& nm : myMethods)
			{
				if (auto it = byName.find(nm); it != byName.end())
				{
					seenAbi.insert(nm);
					worklist.push_back(nm);
				}
			}
			auto rwFn = [&](awst::Expression const& e)
				-> std::shared_ptr<awst::Expression>
			{
				auto const* sce =
					dynamic_cast<awst::SubroutineCallExpression const*>(&e);
				if (!sce) return nullptr;
				if (auto const* imt =
					std::get_if<awst::InstanceMethodTarget>(&sce->target))
				{
					if (!seenAbi.count(imt->memberName)
						&& !reachableInternalNames.count(imt->memberName))
					{
						auto it = byName.find(imt->memberName);
						if (it != byName.end()
							&& !it->second->arc4MethodConfig.has_value())
						{
							reachableInternalNames.insert(imt->memberName);
							worklist.push_back(imt->memberName);
						}
					}
				}
				return nullptr;
			};
			while (!worklist.empty())
			{
				std::string cur = std::move(worklist.back());
				worklist.pop_back();
				auto it = byName.find(cur);
				if (it == byName.end() || !it->second->body) continue;
				walkBlock(*it->second->body, rwFn);
			}
		}

		// Walk methods; produce a new vector that drops unreachable
		// internals entirely (puya retains ContractMethod entries
		// regardless of reachability — leaving them in bloats the
		// chunk; stubbing them defeats the purpose for callers in
		// the same chunk).
		std::vector<awst::ContractMethod> filteredMethods;
		filteredMethods.reserve(chunkContract->methods.size());
		for (auto& m : chunkContract->methods)
		{
			bool isAbi = m.arc4MethodConfig.has_value();
			bool isLiveAbi = isAbi && myMethods.count(m.memberName);
			bool isReachableInternal = !isAbi
				&& reachableInternalNames.count(m.memberName);
			if (isLiveAbi)
			{
				// Real (non-stubbed) ABI method body — apply chunk-
				// side patches so msg.sender / msg.value /
				// address(this) / inner-pay Sender resolve to main's
				// identity rather than __storage's.
				patchChunkMethodBody(m);
				filteredMethods.push_back(std::move(m));
			}
			else if (isReachableInternal)
			{
				// Internal helper called from this chunk's live ABI
				// method(s). We DO patch chunk-side msg.sender /
				// msg.value / address(this) / inner-txn-Sender in
				// these helpers — otherwise calls like
				// `_isSenderAuthorized()` read raw `txn Sender`,
				// which on the chunk's storage-app context resolves
				// to the orch's app account, not the user.
				//
				// Mutating the helper body affects mainContract's
				// copy too (shared via shallowCloneContract). That's
				// SAFE because:
				//   - In all-methods-split mode, mainContract only
				//     has forwarding stubs for ABI methods; the
				//     stubs never callsub the internal helpers, so
				//     puya DCE drops them from main's bytecode.
				//   - In partial-split mode, mainContract's
				//     non-stubbed ABI methods might still call the
				//     helper. The patched body reads main's
				//     __og_sender (set by og_setup at the start of
				//     every forwarding stub). Non-stubbed methods
				//     don't go through og_setup, so __og_sender is
				//     stale, but they also DON'T go through the
				//     chunk-dance — they execute on main directly,
				//     where `Txn.Sender` IS the user. The patched
				//     code reads __og_sender instead, which holds
				//     the *previous* user's address (or zero if
				//     uninitialized). MITIGATION: og_setup writes
				//     Txn.Sender on every forwarding stub, so the
				//     last forwarding call sets it; but a fresh
				//     contract with only a non-stubbed entry point
				//     gets zero. This is a narrow regression risk
				//     for partial-split mixed contracts.
				patchChunkMethodBody(m);
				filteredMethods.push_back(std::move(m));
			}
			else if (isAbi)
			{
				// ABI methods routed to other chunks: stubbed (orch
				// handles dispatch). Keep the entry so the chunk's
				// approval router still has a slot for the selector,
				// even though the body is a no-op default-return.
				filteredMethods.push_back(
					cloneStubbed(m, /*forwarding=*/false));
			}
			// else: unreachable internal — drop entirely.
		}
		chunkContract->methods = std::move(filteredMethods);
		chunkContract->methods.push_back(
			makeDelegateUpdateMethod(primary->id, primary->sourceLocation));

		// Build this chunk's full root set: substitute primary for
		// chunkContract, pass every other root (Subroutines, library
		// contracts, etc.) through unchanged. We don't pre-filter
		// Subroutines for reachability — puya's own DCE drops the
		// unused ones during compile. Bytecode size is the same either
		// way (verified empirically on SpokeInstance: chunk sizes
		// 3239/3271/3939/9318 with and without an upstream filter at
		// O2).
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
	int64_t _orchAppId,
	std::map<std::string, int64_t> const& _extraTemplateVars)
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
		// Chunks reference three template vars:
		//   * TMPL_UROS_ORCH_APP_ID — orc-guards on stubbed methods (kept
		//     for compat with old splits, even now that we use a 3-app
		//     architecture).
		//   * TMPL_UROS_MAIN_APP_ID — chunk-side reads of main's
		//     __og_sender / __og_value globals (Pass 2/3) and main's
		//     address (Pass 4).
		//   * TMPL_UROS_STORAGE_APP_ID — declared for symmetry with main's
		//     pay-forward shim. Chunks don't currently reference it but
		//     the declaration prevents puya from rejecting any future
		//     chunk emit that does.
		// All declared as integer template vars; the deploy harness
		// substitutes with real app ids at deploy time.
		std::map<std::string, int64_t> chunkTemplateVars;
		chunkTemplateVars["UROS_ORCH_APP_ID"] = _orchAppId;
		chunkTemplateVars["UROS_MAIN_APP_ID"] = 0;
		chunkTemplateVars["UROS_STORAGE_APP_ID"] = 0;
		// Extra template vars — typically PURE_HELPER_*_APP_ID
		// declarations forwarded from PureHelperExtractor so chunk
		// bodies that inner-call lifted helpers compile.
		for (auto const& [k, v] : _extraTemplateVars)
			chunkTemplateVars.emplace(k, v);
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
