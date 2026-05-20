#pragma once

#include "awst/SourceLocation.h"
#include "awst/WType.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace puyasol::awst
{

// ─── Forward declarations ───────────────────────────────────────────────────

struct Expression;
struct Statement;
struct Block;

// ─── Enums ──────────────────────────────────────────────────────────────────

enum class UInt64BinaryOperator
{
	Add,
	Sub,
	Mult,
	FloorDiv,
	Mod,
	Pow,
	LShift,
	RShift,
	BitOr,
	BitXor,
	BitAnd
};

enum class BigUIntBinaryOperator
{
	Add,
	Sub,
	Mult,
	FloorDiv,
	Mod,
	BitOr,
	BitXor,
	BitAnd
};

enum class BytesBinaryOperator
{
	Add,
	BitOr,
	BitXor,
	BitAnd
};

enum class NumericComparison
{
	Eq,
	Ne,
	Lt,
	Lte,
	Gt,
	Gte
};

enum class EqualityComparison
{
	Eq,
	Ne
};

enum class BinaryBooleanOperator
{
	And,
	Or
};

enum class BytesEncoding
{
	Unknown,
	Base16,
	Base32,
	Base64,
	Utf8
};

enum class AppStorageKind
{
	AppGlobal,
	AccountLocal,
	Box
};

enum class OnCompletionAction
{
	NoOp = 0,
	OptIn = 1,
	CloseOut = 2,
	ClearState = 3,
	UpdateApplication = 4,
	DeleteApplication = 5
};

enum class ARC4CreateOption
{
	Allow = 1,
	Require = 2,
	Disallow = 3
};

// ─── Helper structs ─────────────────────────────────────────────────────────

struct MethodDocumentation
{
	std::optional<std::string> description;
	std::map<std::string, std::string> args;
	std::optional<std::string> returns;
};

struct CallArg
{
	std::optional<std::string> name;
	std::shared_ptr<Expression> value;
};

// ─── SubroutineTarget (tagged union) ────────────────────────────────────────

struct SubroutineID
{
	std::string target;
};

struct InstanceMethodTarget
{
	std::string memberName;
};

struct InstanceSuperMethodTarget
{
	std::string memberName;
};

struct ContractMethodTarget
{
	std::string cref;
	std::string memberName;
};

using SubroutineTarget = std::variant<
	SubroutineID,
	InstanceMethodTarget,
	InstanceSuperMethodTarget,
	ContractMethodTarget>;

// ─── ARC4MethodConfig (tagged union) ────────────────────────────────────────

struct ARC4BareMethodConfig
{
	SourceLocation sourceLocation;
	std::vector<int> allowedCompletionTypes;
	int create = 3; // ARC4CreateOption::Disallow
};

struct ARC4ABIMethodConfig
{
	SourceLocation sourceLocation;
	std::vector<int> allowedCompletionTypes;
	int create = 3;
	std::string name;
	bool readonly = false;
	std::map<std::string, std::string> defaultArgs;
};

using ARC4MethodConfig = std::variant<ARC4BareMethodConfig, ARC4ABIMethodConfig>;

// ─── Expressions ────────────────────────────────────────────────────────────

struct Expression
{
	virtual ~Expression() = default;
	virtual std::string nodeType() const = 0;
	SourceLocation sourceLocation;
	WType const* wtype = WType::voidType();
};

struct IntegerConstant: Expression
{
	std::string nodeType() const override { return "IntegerConstant"; }
	std::string value; // use string for biguint support
};

// Construct an IntegerConstant. wtype defaults to uint64Type(); pass biguintType()
// for values > 2^64 or for biguint contexts.
inline std::shared_ptr<IntegerConstant> makeIntegerConstant(
	std::string value,
	SourceLocation loc,
	WType const* wtype = WType::uint64Type())
{
	auto node = std::make_shared<IntegerConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = std::move(value);
	return node;
}

// Numeric overload — accepts a uint64_t and converts internally.
// Drops the std::to_string adapter at >100 call sites.
inline std::shared_ptr<IntegerConstant> makeIntegerConstant(
	uint64_t value,
	SourceLocation loc,
	WType const* wtype = WType::uint64Type())
{
	return makeIntegerConstant(std::to_string(value), std::move(loc), wtype);
}

// Shorthand for `makeIntegerConstant(value, loc, biguintType())` — the
// most common biguint-constant construction (~30 sites across the
// builder layer for "0", "1", and 2^256 wraps).
inline std::shared_ptr<IntegerConstant> makeBiguintConstant(
	std::string value, SourceLocation loc)
{
	return makeIntegerConstant(std::move(value), std::move(loc), WType::biguintType());
}

// Common `0` / `1` shorthands. wtype defaults to uint64Type() to match
// makeIntegerConstant; pass biguintType() for biguint zero/one (~30 sites
// across the builder layer respectively).
inline std::shared_ptr<IntegerConstant> makeZero(
	SourceLocation loc, WType const* wtype = WType::uint64Type())
{
	return makeIntegerConstant("0", std::move(loc), wtype);
}
inline std::shared_ptr<IntegerConstant> makeOne(
	SourceLocation loc, WType const* wtype = WType::uint64Type())
{
	return makeIntegerConstant("1", std::move(loc), wtype);
}

struct BoolConstant: Expression
{
	std::string nodeType() const override { return "BoolConstant"; }
	bool value = false;
};

// Construct a BoolConstant node. The wtype defaults to the canonical bool
// singleton; callers only need to pass a custom type when they are e.g.
// cloning another node or using an ABI-widened return-path type.
inline std::shared_ptr<BoolConstant> makeBoolConstant(
	bool value, SourceLocation loc, WType const* wtype = WType::boolType())
{
	auto node = std::make_shared<BoolConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = value;
	return node;
}

// `true` / `false` shorthands. ~30 sites across the builder layer.
inline std::shared_ptr<BoolConstant> makeTrue(SourceLocation loc)
{
	return makeBoolConstant(true, std::move(loc));
}
inline std::shared_ptr<BoolConstant> makeFalse(SourceLocation loc)
{
	return makeBoolConstant(false, std::move(loc));
}

struct BytesConstant: Expression
{
	std::string nodeType() const override { return "BytesConstant"; }
	std::vector<uint8_t> value;
	BytesEncoding encoding = BytesEncoding::Unknown;
};

// Construct a BytesConstant. wtype defaults to the canonical bytesType()
// singleton and encoding defaults to Base16 (hex literal / raw bytes).
// Pass the utf8 encoding + a boxKeyType()/stateKeyType() wtype when the
// value is a human-readable name used as a box/state-global key.
inline std::shared_ptr<BytesConstant> makeBytesConstant(
	std::vector<uint8_t> value,
	SourceLocation loc,
	BytesEncoding encoding = BytesEncoding::Base16,
	WType const* wtype = WType::bytesType())
{
	auto node = std::make_shared<BytesConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->encoding = encoding;
	node->value = std::move(value);
	return node;
}

// Utf8 variant for names-as-byte-keys (state/box keys, selector sigs, etc).
inline std::shared_ptr<BytesConstant> makeUtf8BytesConstant(
	std::string const& str,
	SourceLocation loc,
	WType const* wtype = WType::bytesType())
{
	return makeBytesConstant(
		std::vector<uint8_t>(str.begin(), str.end()),
		std::move(loc),
		BytesEncoding::Utf8,
		wtype);
}

struct StringConstant: Expression
{
	std::string nodeType() const override { return "StringConstant"; }
	std::string value;
};

inline std::shared_ptr<StringConstant> makeStringConstant(
	std::string value, SourceLocation loc)
{
	auto node = std::make_shared<StringConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::stringType();
	node->value = std::move(value);
	return node;
}

struct VoidConstant: Expression
{
	std::string nodeType() const override { return "VoidConstant"; }
};

// `void` value — the zero of the unit type. Always typed `voidType()`.
inline std::shared_ptr<VoidConstant> makeVoidConstant(SourceLocation loc)
{
	auto node = std::make_shared<VoidConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::voidType();
	return node;
}

struct VarExpression: Expression
{
	std::string nodeType() const override { return "VarExpression"; }
	std::string name;
};

// Construct a VarExpression (variable reference by name).
inline std::shared_ptr<VarExpression> makeVarExpression(
	std::string name,
	WType const* wtype,
	SourceLocation loc)
{
	auto node = std::make_shared<VarExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->name = std::move(name);
	return node;
}

struct UInt64BinaryOperation: Expression
{
	std::string nodeType() const override { return "UInt64BinaryOperation"; }
	std::shared_ptr<Expression> left;
	UInt64BinaryOperator op;
	std::shared_ptr<Expression> right;
};

// Construct a UInt64BinaryOperation. wtype is uint64Type().
inline std::shared_ptr<UInt64BinaryOperation> makeUInt64BinOp(
	std::shared_ptr<Expression> left,
	UInt64BinaryOperator op,
	std::shared_ptr<Expression> right,
	SourceLocation loc)
{
	auto node = std::make_shared<UInt64BinaryOperation>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::uint64Type();
	node->left = std::move(left);
	node->op = op;
	node->right = std::move(right);
	return node;
}

struct BigUIntBinaryOperation: Expression
{
	std::string nodeType() const override { return "BigUIntBinaryOperation"; }
	std::shared_ptr<Expression> left;
	BigUIntBinaryOperator op;
	std::shared_ptr<Expression> right;
};

// Construct a BigUIntBinaryOperation. wtype is biguintType().
inline std::shared_ptr<BigUIntBinaryOperation> makeBigUIntBinOp(
	std::shared_ptr<Expression> left,
	BigUIntBinaryOperator op,
	std::shared_ptr<Expression> right,
	SourceLocation loc)
{
	auto node = std::make_shared<BigUIntBinaryOperation>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::biguintType();
	node->left = std::move(left);
	node->op = op;
	node->right = std::move(right);
	return node;
}

struct BytesBinaryOperation: Expression
{
	std::string nodeType() const override { return "BytesBinaryOperation"; }
	std::shared_ptr<Expression> left;
	BytesBinaryOperator op;
	std::shared_ptr<Expression> right;
};

inline std::shared_ptr<BytesBinaryOperation> makeBytesBinOp(
	std::shared_ptr<Expression> left,
	BytesBinaryOperator op,
	std::shared_ptr<Expression> right,
	SourceLocation loc)
{
	auto node = std::make_shared<BytesBinaryOperation>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::bytesType();
	node->left = std::move(left);
	node->op = op;
	node->right = std::move(right);
	return node;
}

enum class BytesUnaryOperator
{
	BitInvert
};

struct BytesUnaryOperation: Expression
{
	std::string nodeType() const override { return "BytesUnaryOperation"; }
	std::shared_ptr<Expression> expr;
	BytesUnaryOperator op;
};

inline std::shared_ptr<BytesUnaryOperation> makeBitInvert(
	std::shared_ptr<Expression> expr, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<BytesUnaryOperation>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->op = BytesUnaryOperator::BitInvert;
	node->expr = std::move(expr);
	return node;
}

struct NumericComparisonExpression: Expression
{
	std::string nodeType() const override { return "NumericComparisonExpression"; }
	std::shared_ptr<Expression> lhs;
	NumericComparison op;
	std::shared_ptr<Expression> rhs;
};

// Construct a NumericComparisonExpression. wtype is always boolType().
inline std::shared_ptr<NumericComparisonExpression> makeNumericCompare(
	std::shared_ptr<Expression> lhs,
	NumericComparison op,
	std::shared_ptr<Expression> rhs,
	SourceLocation loc)
{
	auto node = std::make_shared<NumericComparisonExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::boolType();
	node->lhs = std::move(lhs);
	node->op = op;
	node->rhs = std::move(rhs);
	return node;
}

struct BytesComparisonExpression: Expression
{
	std::string nodeType() const override { return "BytesComparisonExpression"; }
	std::shared_ptr<Expression> lhs;
	EqualityComparison op;
	std::shared_ptr<Expression> rhs;
};

// Construct a BytesComparisonExpression. wtype is always boolType().
inline std::shared_ptr<BytesComparisonExpression> makeBytesComparison(
	std::shared_ptr<Expression> lhs,
	EqualityComparison op,
	std::shared_ptr<Expression> rhs,
	SourceLocation loc)
{
	auto node = std::make_shared<BytesComparisonExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::boolType();
	node->lhs = std::move(lhs);
	node->op = op;
	node->rhs = std::move(rhs);
	return node;
}

struct BooleanBinaryOperation: Expression
{
	std::string nodeType() const override { return "BooleanBinaryOperation"; }
	std::shared_ptr<Expression> left;
	BinaryBooleanOperator op;
	std::shared_ptr<Expression> right;
};

// `left {AND,OR} right` over bool operands. Result type is always bool.
inline std::shared_ptr<BooleanBinaryOperation> makeBoolBinOp(
	std::shared_ptr<Expression> left, BinaryBooleanOperator op,
	std::shared_ptr<Expression> right, SourceLocation loc)
{
	auto node = std::make_shared<BooleanBinaryOperation>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::boolType();
	node->left = std::move(left);
	node->op = op;
	node->right = std::move(right);
	return node;
}

struct Not: Expression
{
	std::string nodeType() const override { return "Not"; }
	std::shared_ptr<Expression> expr;
};

// Logical-not on a bool expression. Result type is always bool.
inline std::shared_ptr<Not> makeNot(
	std::shared_ptr<Expression> expr, SourceLocation loc)
{
	auto node = std::make_shared<Not>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::boolType();
	node->expr = std::move(expr);
	return node;
}

struct AssertExpression: Expression
{
	std::string nodeType() const override { return "AssertExpression"; }
	std::shared_ptr<Expression> condition;
	std::optional<std::string> errorMessage;
};

// Construct an AssertExpression node. The wtype defaults to voidType()
// (what the vast majority of callers use); the splitter uses boolType()
// for helper-group flags, and a few sites clone an existing node's wtype.
inline std::shared_ptr<AssertExpression> makeAssert(
	std::shared_ptr<Expression> condition,
	SourceLocation loc,
	std::optional<std::string> errorMessage = std::nullopt,
	WType const* wtype = WType::voidType())
{
	auto node = std::make_shared<AssertExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->condition = std::move(condition);
	node->errorMessage = std::move(errorMessage);
	return node;
}

struct AssignmentExpression: Expression
{
	std::string nodeType() const override { return "AssignmentExpression"; }
	std::shared_ptr<Expression> target;
	std::shared_ptr<Expression> value;
};

// Construct an AssignmentExpression. wtype defaults to target->wtype, which
// is correct for ~all sites; pass an explicit wtype only when the assignment
// type differs from the target type (e.g. tuple LHS, library-storage write).
inline std::shared_ptr<AssignmentExpression> makeAssignmentExpression(
	std::shared_ptr<Expression> target,
	std::shared_ptr<Expression> value,
	SourceLocation loc,
	WType const* wtype = nullptr)
{
	if (!wtype && target) wtype = target->wtype;
	auto node = std::make_shared<AssignmentExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->target = std::move(target);
	node->value = std::move(value);
	return node;
}

struct ConditionalExpression: Expression
{
	std::string nodeType() const override { return "ConditionalExpression"; }
	std::shared_ptr<Expression> condition;
	std::shared_ptr<Expression> trueExpr;
	std::shared_ptr<Expression> falseExpr;
};

struct SubroutineCallExpression: Expression
{
	std::string nodeType() const override { return "SubroutineCallExpression"; }
	SubroutineTarget target;
	std::vector<CallArg> args;
};

// Construct a SubroutineCallExpression header (sourceLocation/wtype/target).
// Callers append CallArg entries to `args` afterwards. Reduces the canonical
// 4-line construction (make_shared / sourceLocation / wtype / target) to one.
inline std::shared_ptr<SubroutineCallExpression> makeSubroutineCall(
	SubroutineTarget target,
	WType const* returnType,
	SourceLocation loc)
{
	auto node = std::make_shared<SubroutineCallExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = returnType;
	node->target = std::move(target);
	return node;
}

// Append a named CallArg to a SubroutineCallExpression / PuyaLibCall args list.
// Reduces the 4-line `CallArg ca; ca.name = ...; ca.value = ...; args.push_back(...);`
// boilerplate to a single call.
inline void pushCallArg(
	std::vector<CallArg>& args,
	std::string name,
	std::shared_ptr<Expression> value)
{
	CallArg ca;
	ca.name = std::move(name);
	ca.value = std::move(value);
	args.push_back(std::move(ca));
}

// Overload: append an unnamed (positional) CallArg.
inline void pushCallArg(
	std::vector<CallArg>& args,
	std::shared_ptr<Expression> value)
{
	CallArg ca;
	ca.value = std::move(value);
	args.push_back(std::move(ca));
}

struct IntrinsicCall: Expression
{
	std::string nodeType() const override { return "IntrinsicCall"; }
	std::string opCode;
	std::vector<std::variant<std::string, int>> immediates;
	std::vector<std::shared_ptr<Expression>> stackArgs;
};

// Construct an IntrinsicCall header (sourceLocation/wtype/opCode). Callers
// append to `stackArgs` and `immediates` as needed afterwards.
inline std::shared_ptr<IntrinsicCall> makeIntrinsicCall(
	std::string opCode,
	WType const* wtype,
	SourceLocation loc)
{
	auto node = std::make_shared<IntrinsicCall>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->opCode = std::move(opCode);
	return node;
}

// `itob(uint64Expr)` → 8-byte big-endian bytes. Common enough to deserve
// a convenience helper.
inline std::shared_ptr<IntrinsicCall> makeItob(
	std::shared_ptr<Expression> uint64Expr, SourceLocation loc)
{
	auto node = makeIntrinsicCall("itob", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(uint64Expr));
	return node;
}

// `btoi(bytesExpr)` → uint64. bytesExpr must be ≤ 8 bytes.
inline std::shared_ptr<IntrinsicCall> makeBtoi(
	std::shared_ptr<Expression> bytesExpr, SourceLocation loc,
	WType const* wtype = nullptr)
{
	auto node = makeIntrinsicCall(
		"btoi", wtype ? wtype : WType::uint64Type(), std::move(loc));
	node->stackArgs.push_back(std::move(bytesExpr));
	return node;
}

// `len(bytesExpr)` → uint64.
inline std::shared_ptr<IntrinsicCall> makeLen(
	std::shared_ptr<Expression> bytesExpr, SourceLocation loc)
{
	auto node = makeIntrinsicCall("len", WType::uint64Type(), std::move(loc));
	node->stackArgs.push_back(std::move(bytesExpr));
	return node;
}

// `concat(left, right)` → bytes. Two-arg form; callers wanting N-way concat
// should chain or use a dedicated helper.
inline std::shared_ptr<IntrinsicCall> makeConcat(
	std::shared_ptr<Expression> left,
	std::shared_ptr<Expression> right,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("concat", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(left));
	node->stackArgs.push_back(std::move(right));
	return node;
}

// `global <field>` — read a global field (e.g. "CurrentApplicationID",
// "LatestTimestamp", "OpcodeBudget"). 20+ sites use the pattern of
// creating the intrinsic then setting `immediates = {field}` manually.
inline std::shared_ptr<IntrinsicCall> makeGlobal(
	std::string field, WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("global", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	return node;
}

// `txn <field>` — read a current-transaction field (e.g. "GroupIndex",
// "Sender", "NumAppArgs", "ApplicationID").
inline std::shared_ptr<IntrinsicCall> makeTxn(
	std::string field, WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("txn", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	return node;
}

// `txna ApplicationArgs <i>` — read the i-th application argument as
// bytes. `wtype` defaults to `WType::bytesType()` but callers that need
// a fixed-width view (e.g. `BytesWType(4)` for the 4-byte method
// selector) can override.
inline std::shared_ptr<IntrinsicCall> makeAppArg(
	int i, SourceLocation loc, WType const* wtype = nullptr)
{
	auto node = makeIntrinsicCall(
		"txna", wtype ? wtype : WType::bytesType(), std::move(loc));
	node->immediates = {std::string("ApplicationArgs"), i};
	return node;
}

// `itxn <field>` — read a field of the most recently submitted inner
// transaction (e.g. "LastLog" for the inner app call's return data,
// "CreatedApplicationID" after a Create itxn). Companion to makeTxn /
// makeGlobal; takes wtype because field types vary (LastLog→bytes,
// most others→uint64 or account).
inline std::shared_ptr<IntrinsicCall> makeItxn(
	std::string field, WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("itxn", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	return node;
}

// `block <field> <roundExpr>` — read a past-block field (BlkSeed,
// BlkTimestamp). Unlike makeTxn/makeGlobal/makeItxn, this opcode takes
// the round as a stack argument, not an immediate.
inline std::shared_ptr<IntrinsicCall> makeBlock(
	std::string field, std::shared_ptr<Expression> roundExpr,
	WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("block", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(roundExpr));
	return node;
}

// `app_params_get <field> <appId>` — query a deployed app's params
// (AppAddress, AppApprovalProgram, AppGlobalNumByteSlice, etc.).
// Always returns a (value, exists) tuple; caller passes the matching
// `tupleType`. Stack arg is the appId.
inline std::shared_ptr<IntrinsicCall> makeAppParamsGet(
	std::string field, std::shared_ptr<Expression> appId,
	WType const* tupleType, SourceLocation loc)
{
	auto node = makeIntrinsicCall("app_params_get", tupleType, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(appId));
	return node;
}

// `asset_params_get <field> <assetId>` — query an ASA's params
// (AssetTotal, AssetDecimals, AssetUnitName, AssetName, etc.). Always
// returns a (value, exists) tuple; caller passes the matching
// `tupleType` (uint64 fields → uint64+bool, byte fields → bytes+bool).
inline std::shared_ptr<IntrinsicCall> makeAssetParamsGet(
	std::string field, std::shared_ptr<Expression> assetId,
	WType const* tupleType, SourceLocation loc)
{
	auto node = makeIntrinsicCall("asset_params_get", tupleType, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(assetId));
	return node;
}

// `gtxns <field> <groupIdx>` — read a field of another transaction in
// the current group, indexed by `groupIdx`. Companion to makeTxn
// (current txn) and makeGitxn (inner txn by index).
inline std::shared_ptr<IntrinsicCall> makeGtxns(
	std::string field, std::shared_ptr<Expression> groupIdx,
	WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("gtxns", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(groupIdx));
	return node;
}

// `load <slot>` — read a scratch slot as bytes. Used for the EVM memory
// blob (MEMORY_SLOT_FIRST + n) and the transient-storage blob
// (TRANSIENT_SLOT). Always returns bytes — callers that need a numeric
// view btoi the result.
inline std::shared_ptr<IntrinsicCall> makeLoadSlot(
	int slot, SourceLocation loc)
{
	auto node = makeIntrinsicCall("load", WType::bytesType(), std::move(loc));
	node->immediates = {slot};
	return node;
}

// `store <slot> <value>` — write `value` to a scratch slot. Companion
// to `makeLoadSlot`; `value` typically comes from a `replace3` over a
// fresh slot read.
inline std::shared_ptr<IntrinsicCall> makeStoreSlot(
	int slot, std::shared_ptr<Expression> value, SourceLocation loc)
{
	auto node = makeIntrinsicCall("store", WType::voidType(), std::move(loc));
	node->immediates = {slot};
	node->stackArgs.push_back(std::move(value));
	return node;
}

// `box_put key value` — write `value` to the box stored at `key`. The
// box's size must equal `len(value)` (otherwise it fails); callers that
// resize must `box_del` first.
inline std::shared_ptr<IntrinsicCall> makeBoxPut(
	std::shared_ptr<Expression> key,
	std::shared_ptr<Expression> value,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("box_put", WType::voidType(), std::move(loc));
	node->stackArgs.push_back(std::move(key));
	node->stackArgs.push_back(std::move(value));
	return node;
}

// `box_del key` — delete the box at `key`. Returns a bool indicating
// whether the box existed; most callsites ignore that and discard via a
// statement.
inline std::shared_ptr<IntrinsicCall> makeBoxDel(
	std::shared_ptr<Expression> key, SourceLocation loc)
{
	auto node = makeIntrinsicCall("box_del", WType::boolType(), std::move(loc));
	node->stackArgs.push_back(std::move(key));
	return node;
}

// `app_global_put key value` — write `value` to the global state slot at
// `key`. No size restriction beyond the global-state schema.
inline std::shared_ptr<IntrinsicCall> makeAppGlobalPut(
	std::shared_ptr<Expression> key,
	std::shared_ptr<Expression> value,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("app_global_put", WType::voidType(), std::move(loc));
	node->stackArgs.push_back(std::move(key));
	node->stackArgs.push_back(std::move(value));
	return node;
}

// `extract <offset> <length>; <bytesExpr>` — 2-immediate form for constant
// offset/length; stack arg is the source bytes expression.
inline std::shared_ptr<IntrinsicCall> makeExtract(
	std::shared_ptr<Expression> bytesExpr,
	int offset, int length,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("extract", WType::bytesType(), std::move(loc));
	node->immediates = {offset, length};
	node->stackArgs.push_back(std::move(bytesExpr));
	return node;
}

// Take the LAST n bytes of `bytesExpr`: lowers to
//   extract3(bytesExpr, len(bytesExpr) - n, n)
// 3-arg `extract3` form because the offset is `len - n` at runtime
// (constant only if `len` is known statically — which it usually isn't
// for box reads and padded biguints, so the dynamic form is correct).
// ~8 sites across the builder use this exact pattern to right-align
// a bytes value to a fixed width after a left-pad. Note `bytesExpr` is
// referenced TWICE (once by len, once by extract3); puya's IR builder
// deduplicates by AST identity, so passing the same shared_ptr to both
// produces the right wiring.
inline std::shared_ptr<IntrinsicCall> makeExtractLastN(
	std::shared_ptr<Expression> bytesExpr,
	int n,
	SourceLocation loc)
{
	auto nStr = std::to_string(n);
	auto lenCall = makeLen(bytesExpr, loc);
	auto nConstOffset = makeIntegerConstant(nStr, loc);
	auto offset = std::make_shared<UInt64BinaryOperation>();
	offset->sourceLocation = loc;
	offset->wtype = WType::uint64Type();
	offset->left = std::move(lenCall);
	offset->op = UInt64BinaryOperator::Sub;
	offset->right = std::move(nConstOffset);
	auto nConstWidth = makeIntegerConstant(std::move(nStr), loc);
	auto extract = makeIntrinsicCall("extract3", WType::bytesType(), std::move(loc));
	extract->stackArgs.push_back(bytesExpr);
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(std::move(nConstWidth));
	return extract;
}

// `setbit(bytes, bitIdx, value)` → bytes with the specified bit set
// or cleared. Used heavily to build ARC4-encoded bools (0x80/0x00 byte).
inline std::shared_ptr<IntrinsicCall> makeSetbit(
	std::shared_ptr<Expression> bytes,
	std::shared_ptr<Expression> bitIdx,
	std::shared_ptr<Expression> value,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("setbit", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(bytes));
	node->stackArgs.push_back(std::move(bitIdx));
	node->stackArgs.push_back(std::move(value));
	return node;
}

// `getbit(bytes, bitIdx)` → uint64 (0 or 1). Companion to makeSetbit;
// dominant use is ARC4-bool decode (read bit 0 of a single-byte arg).
inline std::shared_ptr<IntrinsicCall> makeGetbit(
	std::shared_ptr<Expression> bytes,
	std::shared_ptr<Expression> bitIdx,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("getbit", WType::uint64Type(), std::move(loc));
	node->stackArgs.push_back(std::move(bytes));
	node->stackArgs.push_back(std::move(bitIdx));
	return node;
}

// `extract_uint64(bytes, offset)` → 8-byte big-endian uint64 read.
// Result type is `WType::uint64Type()` by default.
inline std::shared_ptr<IntrinsicCall> makeExtractUInt64(
	std::shared_ptr<Expression> bytes,
	std::shared_ptr<Expression> offset,
	SourceLocation loc, WType const* wtype = nullptr)
{
	auto node = makeIntrinsicCall(
		"extract_uint64", wtype ? wtype : WType::uint64Type(), std::move(loc));
	node->stackArgs.push_back(std::move(bytes));
	node->stackArgs.push_back(std::move(offset));
	return node;
}

// `extract_uint16(bytes, offset)` → 2-byte big-endian uint16 read,
// widened to a stack uint64. Used heavily to read ARC4 dynamic-array
// length prefixes.
inline std::shared_ptr<IntrinsicCall> makeExtractUInt16(
	std::shared_ptr<Expression> bytes,
	std::shared_ptr<Expression> offset,
	SourceLocation loc, WType const* wtype = nullptr)
{
	auto node = makeIntrinsicCall(
		"extract_uint16", wtype ? wtype : WType::uint64Type(), std::move(loc));
	node->stackArgs.push_back(std::move(bytes));
	node->stackArgs.push_back(std::move(offset));
	return node;
}

// `extract3(bytes, offset, length)` → bytes slice. ~70 sites across the
// builder layer use this exact 3-stack-arg shape. `wtype` defaults to
// `WType::bytesType()` but callers that need a fixed-width view
// (e.g. `BytesWType(1)` for a single-byte slice) can override.
inline std::shared_ptr<IntrinsicCall> makeExtract3(
	std::shared_ptr<Expression> bytes,
	std::shared_ptr<Expression> offset,
	std::shared_ptr<Expression> length,
	SourceLocation loc,
	WType const* wtype = nullptr)
{
	auto node = makeIntrinsicCall(
		"extract3", wtype ? wtype : WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(bytes));
	node->stackArgs.push_back(std::move(offset));
	node->stackArgs.push_back(std::move(length));
	return node;
}

// `replace3(bytes, offset, replacement)` → bytes with `replacement` overlaid
// starting at `offset`. ~16 sites use this exact 3-stack-arg shape.
inline std::shared_ptr<IntrinsicCall> makeReplace3(
	std::shared_ptr<Expression> bytes,
	std::shared_ptr<Expression> offset,
	std::shared_ptr<Expression> replacement,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("replace3", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(bytes));
	node->stackArgs.push_back(std::move(offset));
	node->stackArgs.push_back(std::move(replacement));
	return node;
}

// `bzero(count)` → `count` zero bytes.
inline std::shared_ptr<IntrinsicCall> makeBzero(int count, SourceLocation loc)
{
	auto countExpr = makeIntegerConstant(static_cast<uint64_t>(count), loc);
	auto node = makeIntrinsicCall("bzero", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(countExpr));
	return node;
}

// `concat(bzero(padBytes), value)` — zero-extend `value` on the left to
// produce a `padBytes + len(value)` byte result.
inline std::shared_ptr<IntrinsicCall> makeLeftPad(
	std::shared_ptr<Expression> value, int padBytes, SourceLocation loc)
{
	auto pad = makeBzero(padBytes, loc);
	return makeConcat(std::move(pad), std::move(value), std::move(loc));
}

// `concat(value, bzero(padBytes))` — zero-extend `value` on the right.
inline std::shared_ptr<IntrinsicCall> makeRightPad(
	std::shared_ptr<Expression> value, int padBytes, SourceLocation loc)
{
	auto pad = makeBzero(padBytes, loc);
	return makeConcat(std::move(value), std::move(pad), std::move(loc));
}

// Left-pad `value` to *exactly* `n` bytes — `extract3(bzero(n) ++ value,
// len - n, n)`. Required for ABI-encoding values whose minimal AVM
// representation is shorter than the target ABI width (biguint, etc.):
// makeLeftPad alone produces `n + len(value)` bytes; this helper trims
// to `n` via dynamic-offset extract3.
inline std::shared_ptr<IntrinsicCall> makeLeftPadToN(
	std::shared_ptr<Expression> value, int n, SourceLocation loc)
{
	auto padded = makeLeftPad(std::move(value), n, loc);
	auto offset = makeUInt64BinOp(makeLen(padded, loc),
		UInt64BinaryOperator::Sub,
		makeIntegerConstant(static_cast<uint64_t>(n), loc), loc);
	auto extract = makeIntrinsicCall("extract3", WType::bytesType(), loc);
	extract->stackArgs.push_back(std::move(padded));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(makeIntegerConstant(static_cast<uint64_t>(n), std::move(loc)));
	return extract;
}

// `keccak256(bytes)` → 32-byte hash.
inline std::shared_ptr<IntrinsicCall> makeKeccak256(
	std::shared_ptr<Expression> input, SourceLocation loc)
{
	auto node = makeIntrinsicCall("keccak256", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(input));
	return node;
}

// `condition ? trueExpr : falseExpr` — assemble in one call instead of
// the std::make_shared + 5 field assignments boilerplate.
inline std::shared_ptr<ConditionalExpression> makeConditional(
	std::shared_ptr<Expression> condition,
	std::shared_ptr<Expression> trueExpr,
	std::shared_ptr<Expression> falseExpr,
	WType const* wtype,
	SourceLocation loc)
{
	auto node = std::make_shared<ConditionalExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->condition = std::move(condition);
	node->trueExpr = std::move(trueExpr);
	node->falseExpr = std::move(falseExpr);
	return node;
}

struct FieldExpression: Expression
{
	std::string nodeType() const override { return "FieldExpression"; }
	std::shared_ptr<Expression> base;
	std::string name;
};

struct IndexExpression: Expression
{
	std::string nodeType() const override { return "IndexExpression"; }
	std::shared_ptr<Expression> base;
	std::shared_ptr<Expression> index;
};

struct TupleExpression: Expression
{
	std::string nodeType() const override { return "TupleExpression"; }
	std::vector<std::shared_ptr<Expression>> items;
};

// Empty TupleExpression with location and wtype set; caller fills items.
inline std::shared_ptr<TupleExpression> makeTupleExpression(
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<TupleExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	return node;
}

struct TupleItemExpression: Expression
{
	std::string nodeType() const override { return "TupleItemExpression"; }
	std::shared_ptr<Expression> base;
	int index = 0;
};

// `base.name` member access on a struct-typed expression.
inline std::shared_ptr<FieldExpression> makeFieldExpression(
	std::shared_ptr<Expression> base, std::string name,
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<FieldExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->base = std::move(base);
	node->name = std::move(name);
	return node;
}

// `base[index]` indexed access — for arrays, mappings, etc.
inline std::shared_ptr<IndexExpression> makeIndexExpression(
	std::shared_ptr<Expression> base, std::shared_ptr<Expression> index,
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<IndexExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->base = std::move(base);
	node->index = std::move(index);
	return node;
}

// `tuple.N` element access on a WTuple-typed expression.
inline std::shared_ptr<TupleItemExpression> makeTupleItem(
	std::shared_ptr<Expression> base, int index,
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<TupleItemExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->base = std::move(base);
	node->index = index;
	return node;
}

struct ARC4Encode: Expression
{
	std::string nodeType() const override { return "ARC4Encode"; }
	std::shared_ptr<Expression> value;
};

struct ARC4Decode: Expression
{
	std::string nodeType() const override { return "ARC4Decode"; }
	std::shared_ptr<Expression> value;
};

// Wrap an expression in an ARC4Encode (native → ARC4-encoded bytes).
inline std::shared_ptr<ARC4Encode> makeARC4Encode(
	std::shared_ptr<Expression> value, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<ARC4Encode>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = std::move(value);
	return node;
}

// Wrap an expression in an ARC4Decode (ARC4-encoded bytes → native).
inline std::shared_ptr<ARC4Decode> makeARC4Decode(
	std::shared_ptr<Expression> value, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<ARC4Decode>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = std::move(value);
	return node;
}

struct ARC4FromBytes: Expression
{
	std::string nodeType() const override { return "ARC4FromBytes"; }
	std::shared_ptr<Expression> value;
	bool validate = false;
};

inline std::shared_ptr<ARC4FromBytes> makeARC4FromBytes(
	std::shared_ptr<Expression> value, WType const* wtype, SourceLocation loc, bool validate = false)
{
	auto node = std::make_shared<ARC4FromBytes>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = std::move(value);
	node->validate = validate;
	return node;
}

struct ARC4Router: Expression
{
	std::string nodeType() const override { return "ARC4Router"; }
};

inline std::shared_ptr<ARC4Router> makeARC4Router(WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<ARC4Router>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	return node;
}

struct ReinterpretCast: Expression
{
	std::string nodeType() const override { return "ReinterpretCast"; }
	std::shared_ptr<Expression> expr;
};

// Construct a ReinterpretCast that bit-reinterprets `expr` as `targetType`.
inline std::shared_ptr<ReinterpretCast> makeReinterpretCast(
	std::shared_ptr<Expression> expr,
	WType const* targetType,
	SourceLocation loc)
{
	auto node = std::make_shared<ReinterpretCast>();
	node->sourceLocation = std::move(loc);
	node->wtype = targetType;
	node->expr = std::move(expr);
	return node;
}

// Encode a typed key value to its canonical byte form for storage-key
// derivation: uint64 → itob (8 B); biguint → left-padded then trimmed
// to exactly 32 B (matching Solidity uint256 ABI width); anything else
// → reinterpret-cast to bytes (already in canonical form for string /
// bytesN / address).
inline std::shared_ptr<Expression> makeKeyBytes(
	std::shared_ptr<Expression> value, WType const* encType, SourceLocation loc)
{
	if (encType == WType::uint64Type())
		return makeItob(std::move(value), std::move(loc));
	if (encType == WType::biguintType())
	{
		auto reinterpret = makeReinterpretCast(std::move(value), WType::bytesType(), loc);
		auto cat = makeLeftPad(std::move(reinterpret), 32, loc);
		return makeExtractLastN(std::move(cat), 32, std::move(loc));
	}
	return makeReinterpretCast(std::move(value), WType::bytesType(), std::move(loc));
}

// One layer of Solidity-style storage-key derivation:
// `sha256(keyBytes(value, encType) ++ prefix)`. Chain repeatedly for
// nested mappings / arrays of compound types.
inline std::shared_ptr<IntrinsicCall> makeMappingKeyLayer(
	std::shared_ptr<Expression> value,
	WType const* encType,
	std::shared_ptr<Expression> prefix,
	SourceLocation loc)
{
	auto keyBytes = makeKeyBytes(std::move(value), encType, loc);
	auto concat = makeConcat(std::move(keyBytes), std::move(prefix), loc);
	auto hash = makeIntrinsicCall("sha256", WType::boxKeyType(), std::move(loc));
	hash->stackArgs.push_back(std::move(concat));
	return hash;
}

/// A placeholder for a value not known at compile time — substituted
/// before deployment. Compiles to `pushbytes TMPL_<name>` in TEAL.
struct TemplateVar: Expression
{
	std::string nodeType() const override { return "TemplateVar"; }
	std::string name;
};

inline std::shared_ptr<TemplateVar> makeTemplateVar(
	std::string name, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<TemplateVar>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->name = std::move(name);
	return node;
}

struct Copy: Expression
{
	std::string nodeType() const override { return "Copy"; }
	std::shared_ptr<Expression> value;
};

struct SingleEvaluation: Expression
{
	std::string nodeType() const override { return "SingleEvaluation"; }
	std::shared_ptr<Expression> source;
	int id = 0;
};

inline std::shared_ptr<SingleEvaluation> makeSingleEvaluation(
	std::shared_ptr<Expression> source, WType const* wtype, int id, SourceLocation loc)
{
	auto node = std::make_shared<SingleEvaluation>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->source = std::move(source);
	node->id = id;
	return node;
}

struct CheckedMaybe: Expression
{
	std::string nodeType() const override { return "CheckedMaybe"; }
	std::shared_ptr<Expression> expr;
	std::string comment;
};

struct Emit: Expression
{
	std::string nodeType() const override { return "Emit"; }
	std::string signature;
	std::shared_ptr<Expression> value;
};

inline std::shared_ptr<Emit> makeEmit(
	std::string signature, std::shared_ptr<Expression> value, SourceLocation loc)
{
	auto node = std::make_shared<Emit>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::voidType();
	node->signature = std::move(signature);
	node->value = std::move(value);
	return node;
}

struct NewArray: Expression
{
	std::string nodeType() const override { return "NewArray"; }
	std::vector<std::shared_ptr<Expression>> values;
};

// Empty NewArray with location and wtype set; caller fills values.
inline std::shared_ptr<NewArray> makeNewArray(
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<NewArray>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	return node;
}

struct ArrayLength: Expression
{
	std::string nodeType() const override { return "ArrayLength"; }
	std::shared_ptr<Expression> array;
};

inline std::shared_ptr<ArrayLength> makeArrayLength(
	std::shared_ptr<Expression> array, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<ArrayLength>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->array = std::move(array);
	return node;
}

struct ArrayPop: Expression
{
	std::string nodeType() const override { return "ArrayPop"; }
	std::shared_ptr<Expression> base;
};

inline std::shared_ptr<ArrayPop> makeArrayPop(
	std::shared_ptr<Expression> base, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<ArrayPop>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->base = std::move(base);
	return node;
}

struct ArrayConcat: Expression
{
	std::string nodeType() const override { return "ArrayConcat"; }
	std::shared_ptr<Expression> left;
	std::shared_ptr<Expression> right;
};

struct ArrayExtend: Expression
{
	std::string nodeType() const override { return "ArrayExtend"; }
	std::shared_ptr<Expression> base;
	std::shared_ptr<Expression> other;
};

inline std::shared_ptr<ArrayExtend> makeArrayExtend(
	std::shared_ptr<Expression> base, std::shared_ptr<Expression> other, SourceLocation loc)
{
	auto node = std::make_shared<ArrayExtend>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::voidType();
	node->base = std::move(base);
	node->other = std::move(other);
	return node;
}

// `base.push(elem)` lowered as a single-element NewArray wrapped in
// ArrayExtend. `arrWType` is the type of the dynamic array (the type of
// `base`); the helper wraps `elem` in a single-element NewArray of that
// type and emits the extend.
inline std::shared_ptr<ArrayExtend> makeArrayPushOne(
	std::shared_ptr<Expression> base, std::shared_ptr<Expression> elem,
	WType const* arrWType, SourceLocation loc)
{
	auto singleArr = makeNewArray(arrWType, loc);
	singleArr->values.push_back(std::move(elem));
	return makeArrayExtend(std::move(base), std::move(singleArr), std::move(loc));
}

// `base.pop()` returning a value: ArrayPop produces the (still ARC4-encoded)
// element, ARC4Decode unwraps it to its native representation.
inline std::shared_ptr<ARC4Decode> makeArrayPopDecode(
	std::shared_ptr<Expression> base, WType const* arc4ElemType,
	WType const* nativeElemType, SourceLocation loc)
{
	auto pop = makeArrayPop(std::move(base), arc4ElemType, loc);
	return makeARC4Decode(std::move(pop), nativeElemType, std::move(loc));
}

struct ConvertArray: Expression
{
	std::string nodeType() const override { return "ConvertArray"; }
	std::shared_ptr<Expression> expr;
};

inline std::shared_ptr<ConvertArray> makeConvertArray(
	std::shared_ptr<Expression> expr, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<ConvertArray>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->expr = std::move(expr);
	return node;
}

struct NewStruct: Expression
{
	std::string nodeType() const override { return "NewStruct"; }
	std::map<std::string, std::shared_ptr<Expression>> values;
};

// Empty NewStruct with location and wtype set; caller fills `values` map.
inline std::shared_ptr<NewStruct> makeNewStruct(
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<NewStruct>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	return node;
}

struct NamedTupleExpression: Expression
{
	std::string nodeType() const override { return "NamedTupleExpression"; }
	std::map<std::string, std::shared_ptr<Expression>> values;
};

inline std::shared_ptr<NamedTupleExpression> makeNamedTupleExpression(
	WType const* wtype, std::map<std::string, std::shared_ptr<Expression>> values,
	SourceLocation loc)
{
	auto node = std::make_shared<NamedTupleExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->values = std::move(values);
	return node;
}

struct StateGet: Expression
{
	std::string nodeType() const override { return "StateGet"; }
	std::shared_ptr<Expression> field;
	std::shared_ptr<Expression> defaultValue;
};

// Read of a state field with a default value when uninitialized.
inline std::shared_ptr<StateGet> makeStateGet(
	std::shared_ptr<Expression> field,
	std::shared_ptr<Expression> defaultValue,
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<StateGet>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->field = std::move(field);
	node->defaultValue = std::move(defaultValue);
	return node;
}

struct StateExists: Expression
{
	std::string nodeType() const override { return "StateExists"; }
	std::shared_ptr<Expression> field;
};

struct StateDelete: Expression
{
	std::string nodeType() const override { return "StateDelete"; }
	std::shared_ptr<Expression> field;
};

inline std::shared_ptr<StateDelete> makeStateDelete(
	std::shared_ptr<Expression> field, SourceLocation loc)
{
	auto node = std::make_shared<StateDelete>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::boolType();
	node->field = std::move(field);
	return node;
}

// Walk a `IndexExpression` / `FieldExpression` chain and rebuild it as a
// writable form: any inner `StateGet` (read-with-default wrapper) becomes
// its `field`, and any inner `ARC4Decode` becomes its `value`. Used to
// turn a read-shaped expression like
// `IndexExpression(FieldExpression(StateGet(BoxValueExpression), "f"), i)`
// into the writable target
// `IndexExpression(FieldExpression(BoxValueExpression, "f"), i)`. Puya
// rejects StateGet / ARC4Decode as lvalues, so any assignment or
// array-mutation codegen that derived its target from a read expression
// must funnel through this normalizer first.
//
// Returns a freshly-rebuilt chain (the input shared_ptrs are not mutated)
// so it is safe to call on expressions that may be aliased elsewhere.
inline std::shared_ptr<Expression> makeWritableTarget(
	std::shared_ptr<Expression> e)
{
	if (auto const* ie = dynamic_cast<IndexExpression const*>(e.get()))
	{
		auto newBase = makeWritableTarget(ie->base);
		if (newBase.get() == ie->base.get())
			return e;
		auto ne = std::make_shared<IndexExpression>();
		ne->sourceLocation = ie->sourceLocation;
		ne->wtype = ie->wtype;
		ne->base = std::move(newBase);
		ne->index = ie->index;
		return ne;
	}
	if (auto const* fe = dynamic_cast<FieldExpression const*>(e.get()))
	{
		auto newBase = makeWritableTarget(fe->base);
		if (newBase.get() == fe->base.get())
			return e;
		auto ne = std::make_shared<FieldExpression>();
		ne->sourceLocation = fe->sourceLocation;
		ne->wtype = fe->wtype;
		ne->base = std::move(newBase);
		ne->name = fe->name;
		return ne;
	}
	if (auto const* sg = dynamic_cast<StateGet const*>(e.get()))
		return makeWritableTarget(sg->field);
	if (auto const* dec = dynamic_cast<ARC4Decode const*>(e.get()))
		return makeWritableTarget(dec->value);
	return e;
}

struct StateGetEx: Expression
{
	std::string nodeType() const override { return "StateGetEx"; }
	std::shared_ptr<Expression> field;
};

// Storage expressions
struct AppStateExpression: Expression
{
	std::string nodeType() const override { return "AppStateExpression"; }
	std::shared_ptr<Expression> key;
	std::optional<std::string> existsAssertionMessage;
};

inline std::shared_ptr<AppStateExpression> makeAppStateExpression(
	std::shared_ptr<Expression> key, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<AppStateExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->key = std::move(key);
	return node;
}

struct AppAccountStateExpression: Expression
{
	std::string nodeType() const override { return "AppAccountStateExpression"; }
	std::shared_ptr<Expression> key;
	std::shared_ptr<Expression> account;
	std::optional<std::string> existsAssertionMessage;
};

struct BoxPrefixedKeyExpression: Expression
{
	std::string nodeType() const override { return "BoxPrefixedKeyExpression"; }
	std::shared_ptr<Expression> prefix;
	std::shared_ptr<Expression> key;
};

inline std::shared_ptr<BoxPrefixedKeyExpression> makeBoxPrefixedKey(
	std::shared_ptr<Expression> prefix, std::shared_ptr<Expression> key, SourceLocation loc)
{
	auto node = std::make_shared<BoxPrefixedKeyExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::boxKeyType();
	node->prefix = std::move(prefix);
	node->key = std::move(key);
	return node;
}

struct BoxValueExpression: Expression
{
	std::string nodeType() const override { return "BoxValueExpression"; }
	std::shared_ptr<Expression> key;
	std::optional<std::string> existsAssertionMessage;
};

inline std::shared_ptr<BoxValueExpression> makeBoxValueExpression(
	std::shared_ptr<Expression> key, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<BoxValueExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->key = std::move(key);
	return node;
}

/// True if `_e` is a raw storage-read expression — a BoxValueExpression
/// (box-backed slot) or an AppStateExpression (app-global slot) — i.e.,
/// a slot reference that has not yet been wrapped in a `StateGet` to
/// supply a default value. Used at sites that need to decide whether
/// to wrap a value in `StateGet` before consuming it as an rvalue.
inline bool isRawStorageRead(Expression const* _e)
{
	return dynamic_cast<BoxValueExpression const*>(_e) != nullptr
		|| dynamic_cast<AppStateExpression const*>(_e) != nullptr;
}

// Inner transactions
struct CreateInnerTransaction: Expression
{
	std::string nodeType() const override { return "CreateInnerTransaction"; }
	std::map<std::string, std::shared_ptr<Expression>> fields;
};

// Empty CreateInnerTransaction with location and wtype set; caller fills fields.
inline std::shared_ptr<CreateInnerTransaction> makeCreateInnerTransaction(
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<CreateInnerTransaction>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	return node;
}

struct SubmitInnerTransaction: Expression
{
	std::string nodeType() const override { return "SubmitInnerTransaction"; }
	std::vector<std::shared_ptr<Expression>> itxns;
};

// Empty SubmitInnerTransaction with location and wtype set; caller fills itxns.
inline std::shared_ptr<SubmitInnerTransaction> makeSubmitInnerTransaction(
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<SubmitInnerTransaction>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	return node;
}

struct InnerTransactionField: Expression
{
	std::string nodeType() const override { return "InnerTransactionField"; }
	std::shared_ptr<Expression> itxn;
	std::string field;
	std::shared_ptr<Expression> arrayIndex;
};

struct CommaExpression: Expression
{
	std::string nodeType() const override { return "CommaExpression"; }
	std::vector<std::shared_ptr<Expression>> expressions;
};

// Empty CommaExpression with location and wtype set; caller fills expressions.
inline std::shared_ptr<CommaExpression> makeCommaExpression(
	WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<CommaExpression>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	return node;
}

struct MethodConstant: Expression
{
	std::string nodeType() const override { return "MethodConstant"; }
	std::string value;
};

inline std::shared_ptr<MethodConstant> makeMethodConstant(
	std::string value, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<MethodConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = std::move(value);
	return node;
}

struct AddressConstant: Expression
{
	std::string nodeType() const override { return "AddressConstant"; }
	std::string value;
};

// `address(<value>)` AVM account literal — defaults to the zero address
// (32 zero bytes, base32-encoded with 4-byte SHA-512/256 checksum).
inline std::shared_ptr<AddressConstant> makeAddressConstant(
	std::string value, SourceLocation loc)
{
	auto node = std::make_shared<AddressConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = WType::accountType();
	node->value = std::move(value);
	return node;
}

struct PuyaLibCall: Expression
{
	std::string nodeType() const override { return "PuyaLibCall"; }
	std::string func; // enum name, e.g. "ensure_budget"
	std::vector<CallArg> args;
};

inline std::shared_ptr<PuyaLibCall> makePuyaLibCall(
	std::string func, std::vector<CallArg> args, WType const* wtype, SourceLocation loc)
{
	auto node = std::make_shared<PuyaLibCall>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->func = std::move(func);
	node->args = std::move(args);
	return node;
}

// ─── Statements ─────────────────────────────────────────────────────────────

struct Statement
{
	virtual ~Statement() = default;
	virtual std::string nodeType() const = 0;
	SourceLocation sourceLocation;
};

struct Block: Statement
{
	std::string nodeType() const override { return "Block"; }
	std::vector<std::shared_ptr<Statement>> body;
	std::optional<std::string> label;
	std::optional<std::string> comment;
};

// Empty Block at `loc`. Caller appends to `body`.
inline std::shared_ptr<Block> makeBlock(SourceLocation loc)
{
	auto node = std::make_shared<Block>();
	node->sourceLocation = std::move(loc);
	return node;
}

struct ExpressionStatement: Statement
{
	std::string nodeType() const override { return "ExpressionStatement"; }
	std::shared_ptr<Expression> expr;
};

// Construct an ExpressionStatement wrapping `expr` (side-effectful call, etc).
inline std::shared_ptr<ExpressionStatement> makeExpressionStatement(
	std::shared_ptr<Expression> expr,
	SourceLocation loc)
{
	auto node = std::make_shared<ExpressionStatement>();
	node->sourceLocation = std::move(loc);
	node->expr = std::move(expr);
	return node;
}

struct ReturnStatement: Statement
{
	std::string nodeType() const override { return "ReturnStatement"; }
	std::shared_ptr<Expression> value;
};

// Construct a ReturnStatement. `value` is nullable (bare `return;`).
inline std::shared_ptr<ReturnStatement> makeReturnStatement(
	std::shared_ptr<Expression> value,
	SourceLocation loc)
{
	auto node = std::make_shared<ReturnStatement>();
	node->sourceLocation = std::move(loc);
	node->value = std::move(value);
	return node;
}

struct IfElse: Statement
{
	std::string nodeType() const override { return "IfElse"; }
	std::shared_ptr<Expression> condition;
	std::shared_ptr<Block> ifBranch;
	std::shared_ptr<Block> elseBranch; // nullable
};

// `if (condition) ifBranch else elseBranch` — IfElse is a Statement,
// elseBranch may be null.
inline std::shared_ptr<IfElse> makeIfElse(
	std::shared_ptr<Expression> condition,
	std::shared_ptr<Block> ifBranch,
	std::shared_ptr<Block> elseBranch,
	SourceLocation loc)
{
	auto node = std::make_shared<IfElse>();
	node->sourceLocation = std::move(loc);
	node->condition = std::move(condition);
	node->ifBranch = std::move(ifBranch);
	node->elseBranch = std::move(elseBranch);
	return node;
}

struct WhileLoop: Statement
{
	std::string nodeType() const override { return "WhileLoop"; }
	std::shared_ptr<Expression> condition;
	std::shared_ptr<Block> loopBody;
};

// `while (condition) loopBody` — 14 callers across builder/.
inline std::shared_ptr<WhileLoop> makeWhileLoop(
	std::shared_ptr<Expression> condition,
	std::shared_ptr<Block> loopBody,
	SourceLocation loc)
{
	auto node = std::make_shared<WhileLoop>();
	node->sourceLocation = std::move(loc);
	node->condition = std::move(condition);
	node->loopBody = std::move(loopBody);
	return node;
}

struct LoopExit: Statement
{
	std::string nodeType() const override { return "LoopExit"; }
};

// `break;` statement — 6 callers across builder/.
inline std::shared_ptr<LoopExit> makeLoopExit(SourceLocation loc)
{
	auto node = std::make_shared<LoopExit>();
	node->sourceLocation = std::move(loc);
	return node;
}

struct LoopContinue: Statement
{
	std::string nodeType() const override { return "LoopContinue"; }
};

// `continue;` statement — 4 callers across builder/.
inline std::shared_ptr<LoopContinue> makeLoopContinue(SourceLocation loc)
{
	auto node = std::make_shared<LoopContinue>();
	node->sourceLocation = std::move(loc);
	return node;
}

struct AssignmentStatement: Statement
{
	std::string nodeType() const override { return "AssignmentStatement"; }
	std::shared_ptr<Expression> target;
	std::shared_ptr<Expression> value;
};

// Construct an AssignmentStatement. Standard 3-field shape.
inline std::shared_ptr<AssignmentStatement> makeAssignmentStatement(
	std::shared_ptr<Expression> target,
	std::shared_ptr<Expression> value,
	SourceLocation loc)
{
	auto node = std::make_shared<AssignmentStatement>();
	node->sourceLocation = std::move(loc);
	node->target = std::move(target);
	node->value = std::move(value);
	return node;
}

struct Goto: Statement
{
	std::string nodeType() const override { return "Goto"; }
	std::string target;
};

struct Switch: Statement
{
	std::string nodeType() const override { return "Switch"; }
	std::shared_ptr<Expression> value;
	std::vector<std::pair<std::shared_ptr<Expression>, std::shared_ptr<Block>>> cases;
	std::shared_ptr<Block> defaultCase;
};

struct ForInLoop: Statement
{
	std::string nodeType() const override { return "ForInLoop"; }
	std::shared_ptr<Expression> sequence;
	std::shared_ptr<Expression> items;
	std::shared_ptr<Block> loopBody;
};

struct UInt64AugmentedAssignment: Statement
{
	std::string nodeType() const override { return "UInt64AugmentedAssignment"; }
	std::shared_ptr<Expression> target;
	UInt64BinaryOperator op;
	std::shared_ptr<Expression> value;
};

struct BigUIntAugmentedAssignment: Statement
{
	std::string nodeType() const override { return "BigUIntAugmentedAssignment"; }
	std::shared_ptr<Expression> target;
	BigUIntBinaryOperator op;
	std::shared_ptr<Expression> value;
};

// ─── Contract member nodes ──────────────────────────────────────────────────

struct SubroutineArgument
{
	std::string name;
	SourceLocation sourceLocation;
	WType const* wtype = WType::voidType();
};

struct ContractMethod
{
	SourceLocation sourceLocation;
	std::vector<SubroutineArgument> args;
	WType const* returnType = WType::voidType();
	std::shared_ptr<Block> body;
	MethodDocumentation documentation;
	std::optional<bool> inlineOpt;
	bool pure = false;
	std::string cref; // contract reference (fully qualified name)
	std::string memberName;
	std::optional<ARC4MethodConfig> arc4MethodConfig;
};

struct AppStorageDefinition
{
	SourceLocation sourceLocation;
	std::string memberName;
	AppStorageKind storageKind = AppStorageKind::AppGlobal;
	WType const* storageWType = WType::bytesType();
	std::shared_ptr<Expression> key;
	bool isMap = false; // true for mapping types (key_wtype != null in AWST JSON)
	std::optional<std::string> description;
};

struct StateTotals
{
	std::optional<int> globalUints;
	std::optional<int> localUints;
	std::optional<int> globalBytes;
	std::optional<int> localBytes;
};

// ─── Root nodes ─────────────────────────────────────────────────────────────

struct RootNode
{
	virtual ~RootNode() = default;
	virtual std::string nodeType() const = 0;
	SourceLocation sourceLocation;
};

struct Contract: RootNode
{
	std::string nodeType() const override { return "Contract"; }
	std::string id;
	std::string name;
	std::optional<std::string> description;
	std::vector<std::string> methodResolutionOrder;
	ContractMethod approvalProgram;
	ContractMethod clearProgram;
	std::vector<ContractMethod> methods;
	std::vector<AppStorageDefinition> appState;
	std::optional<StateTotals> stateTotals;
	std::vector<int> reservedScratchSpace;
	std::optional<int> avmVersion;
};

struct Subroutine: RootNode
{
	std::string nodeType() const override { return "Subroutine"; }
	std::string id;
	std::string name;
	std::vector<SubroutineArgument> args;
	WType const* returnType = WType::voidType();
	std::shared_ptr<Block> body;
	MethodDocumentation documentation;
	std::optional<bool> inlineOpt;
	bool pure = false;
};

inline std::shared_ptr<Subroutine> makeSubroutine(
	std::string id, std::string name, std::vector<SubroutineArgument> args,
	WType const* returnType, std::shared_ptr<Block> body, bool pure, SourceLocation loc)
{
	auto node = std::make_shared<Subroutine>();
	node->sourceLocation = std::move(loc);
	node->id = std::move(id);
	node->name = std::move(name);
	node->args = std::move(args);
	node->returnType = returnType;
	node->body = std::move(body);
	node->pure = pure;
	return node;
}

} // namespace puyasol::awst
