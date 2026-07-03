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
	// uros splitter: named chunk this method belongs to; empty = not split.
	// Set from the `@custom:uros-chunk` NatSpec tag. Maps to puya's
	// ARC4ABIMethodConfig.chunk (optional, defaults to None in the backend).
	std::string chunk;
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

// wtype defaults to uint64Type(); pass biguintType() for values >2^64.
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

// uint64_t overload — converts to string internally.
inline std::shared_ptr<IntegerConstant> makeIntegerConstant(
	uint64_t value,
	SourceLocation loc,
	WType const* wtype = WType::uint64Type())
{
	return makeIntegerConstant(std::to_string(value), std::move(loc), wtype);
}

// Shorthand for makeIntegerConstant(value, loc, biguintType()).
inline std::shared_ptr<IntegerConstant> makeBiguintConstant(
	std::string value, SourceLocation loc)
{
	return makeIntegerConstant(std::move(value), std::move(loc), WType::biguintType());
}

// Common 0/1 shorthands. Pass biguintType() for biguint zero/one.
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

// wtype defaults to boolType(); pass a custom type only for ABI-widened paths.
inline std::shared_ptr<BoolConstant> makeBoolConstant(
	bool value, SourceLocation loc, WType const* wtype = WType::boolType())
{
	auto node = std::make_shared<BoolConstant>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = value;
	return node;
}

// true/false shorthands.
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

// encoding defaults to Base16; pass Utf8 + boxKeyType/stateKeyType for name-as-key.
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

// void value (unit type). Always typed voidType().
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

// Variable reference by name.
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

// wtype is uint64Type().
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

// wtype is biguintType().
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

// wtype is boolType().
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

// wtype is boolType().
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

// bool AND/OR. Result type is bool.
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

// Logical-not. Result type is bool.
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
	// Maps to puya's AssertExpression.explicit (default true). Set false on
	// asserts synthesized as the failure half of a revert-payload lowering
	// (the log carries the user-visible contract): puya's TEAL optimizer may
	// soundly strip such an assert when it is unreachable (e.g. downstream
	// of a call to a never-returning assembly-halt function), and its
	// explicit-check accounting would otherwise hard-error on the removal.
	bool isExplicit = true;
};

// wtype defaults to voidType(); splitter uses boolType() for helper-group flags.
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

// wtype defaults to target->wtype; pass explicit wtype for tuple LHS or library writes.
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

// Construct a SubroutineCallExpression (no args; caller appends to `args`).
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

// Append a named CallArg to args.
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

// Append an unnamed (positional) CallArg.
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

// Construct an IntrinsicCall (no args; caller appends stackArgs/immediates).
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

// `itob(uint64Expr)` → 8-byte big-endian bytes.
inline std::shared_ptr<IntrinsicCall> makeItob(
	std::shared_ptr<Expression> uint64Expr, SourceLocation loc)
{
	auto node = makeIntrinsicCall("itob", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(uint64Expr));
	return node;
}

// `btoi(bytesExpr)` → uint64 (bytesExpr must be ≤8 bytes).
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

// `global <field>` — read a global field (e.g. "CurrentApplicationID", "LatestTimestamp").
inline std::shared_ptr<IntrinsicCall> makeGlobal(
	std::string field, WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("global", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	return node;
}

// `txn <field>` — read current-transaction field.
inline std::shared_ptr<IntrinsicCall> makeTxn(
	std::string field, WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("txn", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	return node;
}

// `txna ApplicationArgs <i>` — read app arg as bytes; override wtype for fixed-width view.
inline std::shared_ptr<IntrinsicCall> makeAppArg(
	int i, SourceLocation loc, WType const* wtype = nullptr)
{
	auto node = makeIntrinsicCall(
		"txna", wtype ? wtype : WType::bytesType(), std::move(loc));
	node->immediates = {std::string("ApplicationArgs"), i};
	return node;
}

// `itxn <field>` — read a field of the most recent inner txn (e.g. "LastLog").
inline std::shared_ptr<IntrinsicCall> makeItxn(
	std::string field, WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("itxn", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	return node;
}

// `block <field> <roundExpr>` — read a past-block field (BlkSeed, BlkTimestamp);
// round is a stack arg, not an immediate.
inline std::shared_ptr<IntrinsicCall> makeBlock(
	std::string field, std::shared_ptr<Expression> roundExpr,
	WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("block", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(roundExpr));
	return node;
}

// `app_params_get <field> <appId>` → (value, exists) tuple.
inline std::shared_ptr<IntrinsicCall> makeAppParamsGet(
	std::string field, std::shared_ptr<Expression> appId,
	WType const* tupleType, SourceLocation loc)
{
	auto node = makeIntrinsicCall("app_params_get", tupleType, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(appId));
	return node;
}

// `asset_params_get <field> <assetId>` → (value, exists) tuple.
inline std::shared_ptr<IntrinsicCall> makeAssetParamsGet(
	std::string field, std::shared_ptr<Expression> assetId,
	WType const* tupleType, SourceLocation loc)
{
	auto node = makeIntrinsicCall("asset_params_get", tupleType, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(assetId));
	return node;
}

// `gtxns <field> <groupIdx>` — read a group txn field by index.
inline std::shared_ptr<IntrinsicCall> makeGtxns(
	std::string field, std::shared_ptr<Expression> groupIdx,
	WType const* wtype, SourceLocation loc)
{
	auto node = makeIntrinsicCall("gtxns", wtype, std::move(loc));
	node->immediates = {std::move(field)};
	node->stackArgs.push_back(std::move(groupIdx));
	return node;
}

// `load <slot>` → bytes. Used for EVM memory / transient-storage blobs.
inline std::shared_ptr<IntrinsicCall> makeLoadSlot(
	int slot, SourceLocation loc)
{
	auto node = makeIntrinsicCall("load", WType::bytesType(), std::move(loc));
	node->immediates = {slot};
	return node;
}

// `store <slot> <value>` — write to a scratch slot.
inline std::shared_ptr<IntrinsicCall> makeStoreSlot(
	int slot, std::shared_ptr<Expression> value, SourceLocation loc)
{
	auto node = makeIntrinsicCall("store", WType::voidType(), std::move(loc));
	node->immediates = {slot};
	node->stackArgs.push_back(std::move(value));
	return node;
}

// `box_put key value` — box size must equal len(value); resize with box_del first.
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

// `box_create key size` → bool (true if new, false if existed).
inline std::shared_ptr<IntrinsicCall> makeBoxCreate(
	std::shared_ptr<Expression> key,
	std::shared_ptr<Expression> size,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("box_create", WType::boolType(), std::move(loc));
	node->stackArgs.push_back(std::move(key));
	node->stackArgs.push_back(std::move(size));
	return node;
}

// `box_len key` → (length: uint64, exists: bool) tuple.
inline std::shared_ptr<IntrinsicCall> makeBoxLen(
	std::shared_ptr<Expression> key,
	WType const* tupleType,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("box_len", tupleType, std::move(loc));
	node->stackArgs.push_back(std::move(key));
	return node;
}

// `box_extract key offset length` → bytes slice (faults if absent or overflow).
inline std::shared_ptr<IntrinsicCall> makeBoxExtract(
	std::shared_ptr<Expression> key,
	std::shared_ptr<Expression> offset,
	std::shared_ptr<Expression> length,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("box_extract", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(key));
	node->stackArgs.push_back(std::move(offset));
	node->stackArgs.push_back(std::move(length));
	return node;
}

// `box_replace key offset value` → void. Overwrites len(value) bytes at `offset`
// (box must exist and be large enough). The offset-addressed write counterpart of
// box_extract — the storage-handle leaf write for the reference model.
inline std::shared_ptr<IntrinsicCall> makeBoxReplace(
	std::shared_ptr<Expression> key,
	std::shared_ptr<Expression> offset,
	std::shared_ptr<Expression> value,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("box_replace", WType::voidType(), std::move(loc));
	node->stackArgs.push_back(std::move(key));
	node->stackArgs.push_back(std::move(offset));
	node->stackArgs.push_back(std::move(value));
	return node;
}

// `box_del key` → bool (existed). Most callers discard the result.
inline std::shared_ptr<IntrinsicCall> makeBoxDel(
	std::shared_ptr<Expression> key, SourceLocation loc)
{
	auto node = makeIntrinsicCall("box_del", WType::boolType(), std::move(loc));
	node->stackArgs.push_back(std::move(key));
	return node;
}

// `app_global_put key value` — write to a global state slot.
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
//
// AVM gotcha: length==0 means "to end of source", NOT zero bytes.
// makeExtract(x, N, 0) strips the first N bytes. Use makeExtract3 for
// non-constant length.
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

// Wrap expr in a SingleEvaluation (unique id). Pure leaves pass through.
// Declared here (defined below) so byte-shuffling helpers can use it.
inline std::shared_ptr<Expression> makeEvalOnce(
	std::shared_ptr<Expression> expr, SourceLocation loc);

// Last n bytes of bytesExpr: extract3(bytesExpr, len-n, n).
// bytesExpr is referenced twice — wrap in makeEvalOnce if side-effecting
// (puya has no general AST-identity dedup; SingleEvaluation is the only mechanism).
inline std::shared_ptr<IntrinsicCall> makeExtractLastN(
	std::shared_ptr<Expression> bytesExpr,
	int n,
	SourceLocation loc)
{
	bytesExpr = makeEvalOnce(std::move(bytesExpr), loc);
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

// `setbit(bytes, bitIdx, value)` → bytes with bit set/cleared.
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

// `getbit(bytes, bitIdx)` → uint64 (0 or 1).
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

// `extract_uint64(bytes, offset)` → uint64.
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

// `extract_uint16(bytes, offset)` → uint64 (ARC4 length prefix reads).
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

// `extract3(bytes, offset, length)` → bytes slice.
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

// 2-byte big-endian (ARC4 uint16) encoding: extract3(itob(value), 6, 2).
inline std::shared_ptr<Expression> makeUInt16Bytes(
	std::shared_ptr<Expression> value, SourceLocation loc)
{
	auto itob = makeItob(std::move(value), loc);
	return makeExtract3(
		std::move(itob),
		makeIntegerConstant("6", loc),
		makeIntegerConstant("2", loc),
		loc);
}


// `replace3(bytes, offset, replacement)` → bytes with replacement overlaid at offset.
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

// `bzero(count)` → `count` zero bytes, with a runtime-evaluated count.
inline std::shared_ptr<IntrinsicCall> makeBzero(
	std::shared_ptr<Expression> count, SourceLocation loc)
{
	auto node = makeIntrinsicCall("bzero", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(count));
	return node;
}

// `bzero(count)` → `count` zero bytes, with a compile-time-constant count.
inline std::shared_ptr<IntrinsicCall> makeBzero(int count, SourceLocation loc)
{
	auto countExpr = makeIntegerConstant(static_cast<uint64_t>(count), loc);
	return makeBzero(std::move(countExpr), std::move(loc));
}

// Right-pad a bytes value with zeros to the next 32-byte multiple — the
// EVM-ABI word-alignment write. Exact padding via bzero((32 - len%32) % 32);
// the canonical bridge for ARC4→EVM tail framing (the read-side partners
// are uint64FromAbiWord in builder/abi and makeExtractUInt16 below).
//
// Caution: references `bytes` twice (len + concat) — makeEvalOnce if side-effecting.
inline std::shared_ptr<Expression> makeRightPadTo32Multiple(
	std::shared_ptr<Expression> bytes, SourceLocation loc)
{
	auto lenCall = makeLen(bytes, loc);
	auto modPart = makeUInt64BinOp(
		std::move(lenCall), UInt64BinaryOperator::Mod,
		makeIntegerConstant("32", loc), loc);
	auto sub = makeUInt64BinOp(
		makeIntegerConstant("32", loc), UInt64BinaryOperator::Sub,
		std::move(modPart), loc);
	auto pad = makeUInt64BinOp(
		std::move(sub), UInt64BinaryOperator::Mod,
		makeIntegerConstant("32", loc), loc);
	auto bz = makeBzero(std::move(pad), loc);
	return makeConcat(std::move(bytes), std::move(bz), loc);
}


// `b|(lhs, rhs)` — bitwise-OR of two byte strings (commutative).
inline std::shared_ptr<IntrinsicCall> makeBytesOr(
	std::shared_ptr<Expression> lhs, std::shared_ptr<Expression> rhs,
	SourceLocation loc)
{
	auto node = makeIntrinsicCall("b|", WType::bytesType(), std::move(loc));
	node->stackArgs.push_back(std::move(lhs));
	node->stackArgs.push_back(std::move(rhs));
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

// `b|(bzero(n), value)` — zero-extend to at least n bytes.
// Unlike makeLeftPad (+n always) and makeLeftPadToN (trim to exactly n),
// this is a no-op for values already ≥n bytes.
inline std::shared_ptr<IntrinsicCall> makeZeroExtendToN(
	std::shared_ptr<Expression> value, int n, SourceLocation loc)
{
	auto pad = makeBzero(n, loc);
	return makeBytesOr(std::move(pad), std::move(value), std::move(loc));
}

// Left-pad to exactly n bytes: extract3(bzero(n)++value, len-n, n).
// makeLeftPad alone produces n+len bytes; this trims via dynamic-offset extract3.
inline std::shared_ptr<IntrinsicCall> makeLeftPadToN(
	std::shared_ptr<Expression> value, int n, SourceLocation loc)
{
	// Wrap in makeEvalOnce: value is referenced twice (len + extract3 source).
	auto padded = makeEvalOnce(makeLeftPad(std::move(value), n, loc), loc);
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

// `assert(value < numMembers)` — the EVM Panic(0x21) enum-range check.
// `value` must already be uint64-typed; callers wrap/queue the returned
// Assert expression themselves (statement vs queuePreStmt differs per site).
inline std::shared_ptr<Expression> makeEnumRangeAssert(
	std::shared_ptr<Expression> value,
	unsigned numMembers,
	SourceLocation const& loc,
	std::string message = "enum out of range")
{
	auto cmp = makeNumericCompare(
		std::move(value), NumericComparison::Lt,
		makeIntegerConstant(numMembers, loc), loc);
	return makeAssert(std::move(cmp), loc, std::move(message));
}

// `condition ? trueExpr : falseExpr`.
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

// Wrap an expression in an ARC4Encode. Defined after the reinterpret-cast helpers
// it needs for the signed sub-word int24 path (see makeARC4Encode below).

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

// Reinterpret-cast shorthands (aliases for makeReinterpretCast(value, <type>, loc)).
inline std::shared_ptr<ReinterpretCast> makeAsBytes(
	std::shared_ptr<Expression> expr, SourceLocation loc)
{
	return makeReinterpretCast(std::move(expr), WType::bytesType(), std::move(loc));
}
inline std::shared_ptr<ReinterpretCast> makeAsBiguint(
	std::shared_ptr<Expression> expr, SourceLocation loc)
{
	return makeReinterpretCast(std::move(expr), WType::biguintType(), std::move(loc));
}
inline std::shared_ptr<ReinterpretCast> makeAsAccount(
	std::shared_ptr<Expression> expr, SourceLocation loc)
{
	return makeReinterpretCast(std::move(expr), WType::accountType(), std::move(loc));
}
inline std::shared_ptr<ReinterpretCast> makeAsApplication(
	std::shared_ptr<Expression> expr, SourceLocation loc)
{
	return makeReinterpretCast(std::move(expr), WType::applicationType(), std::move(loc));
}
inline std::shared_ptr<ReinterpretCast> makeAsUInt64(
	std::shared_ptr<Expression> expr, SourceLocation loc)
{
	return makeReinterpretCast(std::move(expr), WType::uint64Type(), std::move(loc));
}

// ARC4Encode wrapper. Signed sub-word int (arc4.intN) from uint64: puya rejects
// uint64→arc4.intN directly; convert to biguint first (itob→reinterpret).
inline std::shared_ptr<ARC4Encode> makeARC4Encode(
	std::shared_ptr<Expression> value, WType const* wtype, SourceLocation loc)
{
	if (value && value->wtype == WType::uint64Type())
		if (auto const* uintN = dynamic_cast<ARC4UIntN const*>(wtype))
		{
			std::string const& alias = uintN->arc4Alias();
			bool const isSigned = alias.rfind("int", 0) == 0; // "int24" yes, "uint24" no
			if (isSigned)
			{
				int const n = uintN->n();
				if (n < 64 && n % 8 == 0)
				{
					// Signed sub-word: extract low n/8 bytes of itob(value).
					// `b&` mask does NOT shrink byte width (AVM keeps the wider
					// operand's length), so the len<=n/8 check would wrongly revert.
					auto itob = makeItob(std::move(value), loc);
					auto low = makeExtract(std::move(itob), 8 - n / 8, n / 8, loc);
					value = makeAsBiguint(std::move(low), loc);
				}
				else
				{
					value = makeAsBiguint(makeItob(std::move(value), loc), loc);
				}
			}
		}

	// biguint → arc4.uintN (N<256): `b&` masks leave leading zero bytes (AVM keeps
	// wider operand width), breaking puya's `len<=n/8` check. Trim to low n/8 bytes.
	if (value && value->wtype == WType::biguintType())
		if (auto const* uintN = dynamic_cast<ARC4UIntN const*>(wtype))
		{
			int const n = uintN->n();
			if (n < 256 && n % 8 == 0)
			{
				auto low = makeExtractLastN(
					makeLeftPad(makeAsBytes(std::move(value), loc), n / 8, loc), n / 8, loc);
				value = makeAsBiguint(std::move(low), loc);
			}
		}

	auto node = std::make_shared<ARC4Encode>();
	node->sourceLocation = std::move(loc);
	node->wtype = wtype;
	node->value = std::move(value);
	return node;
}

// Canonical byte encoding for storage-key derivation:
// uint64 → itob (8B); biguint → padded+trimmed to 32B; else → reinterpret as bytes.
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

// Narrow biguint to uint64 (low 8 bytes): extract_uint64(bzero(8)++value, len-8).
// bzero(8) prefix keeps slice in-range for values shorter than 8 bytes.
inline std::shared_ptr<IntrinsicCall> makeBiguintToUInt64(
	std::shared_ptr<Expression> value, SourceLocation loc)
{
	auto cast = makeReinterpretCast(std::move(value), WType::bytesType(), loc);
	auto cat = makeLeftPad(std::move(cast), 8, loc);
	auto start = makeIntrinsicCall("-", WType::uint64Type(), loc);
	start->stackArgs.push_back(makeLen(cat, loc));
	start->stackArgs.push_back(makeIntegerConstant("8", loc));
	return makeExtractUInt64(cat, std::move(start), std::move(loc));
}

// Fixed 32-byte ABI word → uint64: btoi(extract(word, 24, 8)).
// Use makeBiguintToUInt64 for variable-width inputs.
inline std::shared_ptr<IntrinsicCall> makeWord32ToUInt64(
	std::shared_ptr<Expression> word32, SourceLocation loc)
{
	auto last8 = makeExtract(std::move(word32), 24, 8, loc);
	return makeBtoi(std::move(last8), std::move(loc));
}

// One Solidity storage-key layer: sha256(keyBytes(value, encType) ++ prefix).
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

// Globally unique id for SingleEvaluation. puya's cache is keyed by (source, _id)
// per function — two independent nodes wrapping equal sources with equal ids would
// wrongly merge. Share an evaluation by referencing the same node, not reusing an id.
inline int nextSingleEvalId()
{
	static int s_nextSingleEvalId = 1 << 20;
	return ++s_nextSingleEvalId;
}


// OperandPlan's MATERIALIZE-ONCE primitive (fable-review item 7): given an
// expression referenced MORE THAN ONCE, ensure it evaluates exactly once.
// A trivially-duplicable LEAF (var / constant / already-SingleEvaluation) is
// returned as-is — duplicating it is unobservable and cheaper than a temp;
// everything else is wrapped in SingleEvaluation. Prefer this over a raw
// makeSingleEvaluation for the "reference N times" intent: it centralizes the
// skip-trivial-leaf decision and skips a pointless SE on a constant/var operand.
// (Licenses DUPLICATION only — it never makes evaluation conditional; scope a
// CONDITIONAL operand with ContractContext::buildScopedOperand instead. And it
// is NOT for IDENTITY-FORCING: a site that wraps to stop two attrs-equal exprs
// from merging (itxn call caching) needs an unconditional raw
// makeSingleEvaluation — the skip-leaf shortcut would defeat it.)
inline std::shared_ptr<Expression> makeEvalOnce(
	std::shared_ptr<Expression> expr, SourceLocation loc)
{
	if (!expr
		|| dynamic_cast<SingleEvaluation const*>(expr.get())
		|| dynamic_cast<VarExpression const*>(expr.get())
		|| dynamic_cast<IntegerConstant const*>(expr.get())
		|| dynamic_cast<BoolConstant const*>(expr.get())
		|| dynamic_cast<BytesConstant const*>(expr.get())
		|| dynamic_cast<StringConstant const*>(expr.get()))
		return expr;
	auto const* wt = expr->wtype;
	return makeSingleEvaluation(std::move(expr), wt, nextSingleEvalId(), std::move(loc));
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

// `base.push(elem)` → wrap elem in a single-element NewArray, then ArrayExtend.
inline std::shared_ptr<ArrayExtend> makeArrayPushOne(
	std::shared_ptr<Expression> base, std::shared_ptr<Expression> elem,
	WType const* arrWType, SourceLocation loc)
{
	auto singleArr = makeNewArray(arrWType, loc);
	singleArr->values.push_back(std::move(elem));
	return makeArrayExtend(std::move(base), std::move(singleArr), std::move(loc));
}

// `base.pop()` → ARC4Decode(ArrayPop(base)) to native type.
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

// Rebuild an ARC4Struct with one field replaced (copy-on-write; struct is immutable bytes).
inline std::shared_ptr<NewStruct> makeStructWithReplacedField(
	ARC4Struct const* structType,
	std::shared_ptr<Expression> const& readBase,
	std::string const& fieldName,
	std::shared_ptr<Expression> newValue,
	SourceLocation const& loc)
{
	auto newStruct = makeNewStruct(structType, loc);
	for (auto const& [fname, ftype]: structType->fields())
	{
		if (fname == fieldName)
			newStruct->values[fname] = std::move(newValue);
		else
			newStruct->values[fname] = makeFieldExpression(readBase, fname, ftype, loc);
	}
	return newStruct;
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

// State field read with a default for uninitialized slots.
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

// Strip one StateGet layer: StateGet(x) → x. No-op for non-StateGet.
// Use makeWritableTarget to peel full IndexExpression/FieldExpression chains.
inline std::shared_ptr<Expression> unwrapStateGet(std::shared_ptr<Expression> e)
{
	if (auto const* sg = dynamic_cast<StateGet const*>(e.get()))
		return sg->field;
	return e;
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

// Rebuild an IndexExpression/FieldExpression chain as a writable target:
// strip StateGet and ARC4Decode wrappers (puya rejects them as lvalues).
// Returns a fresh chain; input shared_ptrs are not mutated.
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

/// True if _e is a raw (unwrapped) storage-read (BoxValueExpression or AppStateExpression).
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

// AVM account address literal.
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

// Construct an ExpressionStatement.
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

// Construct a ReturnStatement (value is nullable).
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

// IfElse statement; elseBranch may be null.
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

// `while (condition) loopBody`.
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

// `break;` statement.
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

// `continue;` statement.
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

// Construct an AssignmentStatement.
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
	// uros splitter selector (e.g. "uros"); empty = no in-contract splitter.
	// Set from the `@custom:splitter` NatSpec tag. Maps to puya's
	// Contract.splitter (optional, defaults to None in the backend).
	std::string splitter;
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

// Stateless lsig (mirrors puya's LogicSignature). Emitted for contracts `is LogicSig`
// (AVM.sol). Entry function body becomes `program`; no app state/inner-txns.
struct LogicSignature: RootNode
{
	std::string nodeType() const override { return "LogicSignature"; }
	std::string id;
	std::string shortName;
	std::shared_ptr<Subroutine> program;
	std::optional<std::string> docstring;
	std::vector<int> reservedScratchSpace;
	std::optional<int> avmVersion;
	std::optional<bool> validateEncoding;
};

} // namespace puyasol::awst
