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
	std::shared_ptr<Expression> bytesExpr, SourceLocation loc)
{
	auto node = makeIntrinsicCall("btoi", WType::uint64Type(), std::move(loc));
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

// `bzero(count)` → `count` zero bytes.
inline std::shared_ptr<IntrinsicCall> makeBzero(int count, SourceLocation loc)
{
	auto countExpr = makeIntegerConstant(std::to_string(count), loc);
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
		makeIntegerConstant(std::to_string(n), loc), loc);
	auto extract = makeIntrinsicCall("extract3", WType::bytesType(), loc);
	extract->stackArgs.push_back(std::move(padded));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(makeIntegerConstant(std::to_string(n), std::move(loc)));
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

struct ARC4Router: Expression
{
	std::string nodeType() const override { return "ARC4Router"; }
};

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

struct ConvertArray: Expression
{
	std::string nodeType() const override { return "ConvertArray"; }
	std::shared_ptr<Expression> expr;
};

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

// Inner transactions
struct CreateInnerTransaction: Expression
{
	std::string nodeType() const override { return "CreateInnerTransaction"; }
	std::map<std::string, std::shared_ptr<Expression>> fields;
};

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

struct PuyaLibCall: Expression
{
	std::string nodeType() const override { return "PuyaLibCall"; }
	std::string func; // enum name, e.g. "ensure_budget"
	std::vector<CallArg> args;
};

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

struct LoopExit: Statement
{
	std::string nodeType() const override { return "LoopExit"; }
};

struct LoopContinue: Statement
{
	std::string nodeType() const override { return "LoopContinue"; }
};

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

} // namespace puyasol::awst
