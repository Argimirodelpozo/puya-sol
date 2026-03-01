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

struct BoolConstant: Expression
{
	std::string nodeType() const override { return "BoolConstant"; }
	bool value = false;
};

struct BytesConstant: Expression
{
	std::string nodeType() const override { return "BytesConstant"; }
	std::vector<uint8_t> value;
	BytesEncoding encoding = BytesEncoding::Unknown;
};

struct StringConstant: Expression
{
	std::string nodeType() const override { return "StringConstant"; }
	std::string value;
};

struct VoidConstant: Expression
{
	std::string nodeType() const override { return "VoidConstant"; }
};

struct VarExpression: Expression
{
	std::string nodeType() const override { return "VarExpression"; }
	std::string name;
};

struct UInt64BinaryOperation: Expression
{
	std::string nodeType() const override { return "UInt64BinaryOperation"; }
	std::shared_ptr<Expression> left;
	UInt64BinaryOperator op;
	std::shared_ptr<Expression> right;
};

struct BigUIntBinaryOperation: Expression
{
	std::string nodeType() const override { return "BigUIntBinaryOperation"; }
	std::shared_ptr<Expression> left;
	BigUIntBinaryOperator op;
	std::shared_ptr<Expression> right;
};

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

struct BytesComparisonExpression: Expression
{
	std::string nodeType() const override { return "BytesComparisonExpression"; }
	std::shared_ptr<Expression> lhs;
	EqualityComparison op;
	std::shared_ptr<Expression> rhs;
};

struct BooleanBinaryOperation: Expression
{
	std::string nodeType() const override { return "BooleanBinaryOperation"; }
	std::shared_ptr<Expression> left;
	BinaryBooleanOperator op;
	std::shared_ptr<Expression> right;
};

struct Not: Expression
{
	std::string nodeType() const override { return "Not"; }
	std::shared_ptr<Expression> expr;
};

struct AssertExpression: Expression
{
	std::string nodeType() const override { return "AssertExpression"; }
	std::shared_ptr<Expression> condition;
	std::optional<std::string> errorMessage;
};

struct AssignmentExpression: Expression
{
	std::string nodeType() const override { return "AssignmentExpression"; }
	std::shared_ptr<Expression> target;
	std::shared_ptr<Expression> value;
};

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

struct IntrinsicCall: Expression
{
	std::string nodeType() const override { return "IntrinsicCall"; }
	std::string opCode;
	std::vector<std::variant<std::string, int>> immediates;
	std::vector<std::shared_ptr<Expression>> stackArgs;
};

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

struct TupleItemExpression: Expression
{
	std::string nodeType() const override { return "TupleItemExpression"; }
	std::shared_ptr<Expression> base;
	int index = 0;
};

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

struct ARC4Router: Expression
{
	std::string nodeType() const override { return "ARC4Router"; }
};

struct ReinterpretCast: Expression
{
	std::string nodeType() const override { return "ReinterpretCast"; }
	std::shared_ptr<Expression> expr;
};

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

struct ArrayLength: Expression
{
	std::string nodeType() const override { return "ArrayLength"; }
	std::shared_ptr<Expression> array;
};

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

struct NewStruct: Expression
{
	std::string nodeType() const override { return "NewStruct"; }
	std::map<std::string, std::shared_ptr<Expression>> values;
};

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

struct ExpressionStatement: Statement
{
	std::string nodeType() const override { return "ExpressionStatement"; }
	std::shared_ptr<Expression> expr;
};

struct ReturnStatement: Statement
{
	std::string nodeType() const override { return "ReturnStatement"; }
	std::shared_ptr<Expression> value;
};

struct IfElse: Statement
{
	std::string nodeType() const override { return "IfElse"; }
	std::shared_ptr<Expression> condition;
	std::shared_ptr<Block> ifBranch;
	std::shared_ptr<Block> elseBranch; // nullable
};

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
