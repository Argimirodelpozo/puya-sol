#pragma once

#include "awst/Node.h"
#include "builder/IntrinsicMapper.h"
#include "builder/StorageMapper.h"
#include "builder/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace puyasol::builder
{

/// Maps "LibraryName.functionName" → subroutine ID string.
using LibraryFunctionIdMap = std::unordered_map<std::string, std::string>;

/// Maps AST node ID → subroutine ID for free functions (used by operator overloading).
using FreeFunctionIdMap = std::unordered_map<int64_t, std::string>;

/// Set of function names that have overloads (multiple definitions with same name).
using OverloadedNamesSet = std::unordered_set<std::string>;

/// Translates Solidity expressions to AWST Expression nodes.
/// Uses a stack to return results from visitor methods.
class ExpressionTranslator: public solidity::frontend::ASTConstVisitor
{
public:
	ExpressionTranslator(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		std::string const& _contractName,
		LibraryFunctionIdMap const& _libraryFunctionIds,
		OverloadedNamesSet const& _overloadedNames = {},
		FreeFunctionIdMap const& _freeFunctionById = {}
	);

	static LibraryFunctionIdMap const s_emptyLibraryFunctionIds;
	static FreeFunctionIdMap const s_emptyFreeFunctionIds;

	/// Translate a Solidity expression to AWST.
	std::shared_ptr<awst::Expression> translate(solidity::frontend::Expression const& _expr);

	/// Insert implicit numeric cast if needed (e.g. uint64 → biguint).
	/// Returns the expression unchanged if no cast is needed.
	static std::shared_ptr<awst::Expression> implicitNumericCast(
		std::shared_ptr<awst::Expression> _expr,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	// ASTConstVisitor overrides
	bool visit(solidity::frontend::Literal const& _node) override;
	bool visit(solidity::frontend::Identifier const& _node) override;
	bool visit(solidity::frontend::BinaryOperation const& _node) override;
	bool visit(solidity::frontend::UnaryOperation const& _node) override;
	bool visit(solidity::frontend::FunctionCall const& _node) override;
	bool visit(solidity::frontend::MemberAccess const& _node) override;
	bool visit(solidity::frontend::IndexAccess const& _node) override;
	bool visit(solidity::frontend::Conditional const& _node) override;
	bool visit(solidity::frontend::Assignment const& _node) override;
	bool visit(solidity::frontend::TupleExpression const& _node) override;
	bool visit(solidity::frontend::FunctionCallOptions const& _node) override;
	bool visit(solidity::frontend::ElementaryTypeNameExpression const& _node) override;
	bool visit(solidity::frontend::IndexRangeAccess const& _node) override;

private:
	TypeMapper& m_typeMapper;
	StorageMapper& m_storageMapper;
	std::string m_sourceFile;
	std::string m_contractName;
	LibraryFunctionIdMap const& m_libraryFunctionIds;
	OverloadedNamesSet const& m_overloadedNames;
	FreeFunctionIdMap const& m_freeFunctionById;

	/// Get the disambiguated method name for a function.
	/// Returns "name(paramCount)" if the function name is overloaded, else just "name".
	std::string resolveMethodName(solidity::frontend::FunctionDefinition const& _func);

	/// Expression result stack.
	std::vector<std::shared_ptr<awst::Expression>> m_stack;

	/// Extra statements to emit after the current expression (e.g., array length update).
	std::vector<std::shared_ptr<awst::Statement>> m_pendingStatements;

	/// Statements that must execute before the current expression (e.g., biguint exp loop).
	std::vector<std::shared_ptr<awst::Statement>> m_prePendingStatements;

	void push(std::shared_ptr<awst::Expression> _expr);
	std::shared_ptr<awst::Expression> pop();

public:
	/// Consume any pending statements generated during expression translation.
	std::vector<std::shared_ptr<awst::Statement>> takePendingStatements();

	/// Consume any pre-pending statements (must execute before the expression).
	std::vector<std::shared_ptr<awst::Statement>> takePrePendingStatements();

	/// Register a parameter remap: when an Identifier references the AST declaration
	/// with ID _declId, resolve it as a VarExpression with _uniqueName instead.
	/// Used for modifier parameter binding.
	void addParamRemap(int64_t _declId, std::string const& _uniqueName, awst::WType const* _type);
	/// Remove a previously registered parameter remap.
	void removeParamRemap(int64_t _declId);

	/// Register a super call target: when `super.method()` is translated,
	/// the base function with AST ID _funcId is targeted as subroutine _name.
	void addSuperTarget(int64_t _funcId, std::string const& _name);

	/// Register a storage pointer alias: when `Type storage p = _mapping[key]`,
	/// later references to `p` resolve to the stored box read expression.
	void addStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr);
	/// Remove a previously registered storage pointer alias.
	void removeStorageAlias(int64_t _declId);


private:

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);

	/// Modifier parameter remaps: AST declaration ID → (unique name, type).
	struct ParamRemap { std::string name; awst::WType const* type; };
	std::map<int64_t, ParamRemap> m_paramRemaps;

	/// Super call target names: base function AST ID → subroutine name.
	/// Populated by ContractTranslator for functions called via `super.method()`.
	std::unordered_map<int64_t, std::string> m_superTargetNames;

	/// Storage pointer aliases: maps AST declaration ID to the box read expression.
	/// For `Type storage p = _mapping[key]`, stores the StateGet(BoxValueExpression)
	/// so that later references to `p` resolve to the box, not a local variable.
	std::map<int64_t, std::shared_ptr<awst::Expression>> m_storageAliases;

	/// Whether currently translating expressions inside a Solidity `unchecked` block.
	/// When true, arithmetic operations wrap mod 2^256 (e.g., multiplication results truncated).
	bool m_inUncheckedBlock = false;
public:
	void setInUncheckedBlock(bool _v) { m_inUncheckedBlock = _v; }
	bool inUncheckedBlock() const { return m_inUncheckedBlock; }
private:

	/// Map Solidity binary operator token to AWST equivalent.
	/// Determines whether to use UInt64, BigUInt, or Bytes operation.
	std::shared_ptr<awst::Expression> buildBinaryOp(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		awst::WType const* _resultType,
		awst::SourceLocation const& _loc
	);

	/// Check if a type is biguint.
	static bool isBigUInt(awst::WType const* _type);

	/// Translate a require() or assert() call.
	std::shared_ptr<awst::Expression> translateRequire(
		solidity::frontend::FunctionCall const& _call,
		awst::SourceLocation const& _loc
	);

	/// Build a copy-on-write tuple with one field updated.
	/// Used to transform field assignments on immutable WTuple structs.
	std::shared_ptr<awst::Expression> buildTupleWithUpdatedField(
		std::shared_ptr<awst::Expression> _base,
		std::string const& _fieldName,
		std::shared_ptr<awst::Expression> _newValue,
		awst::SourceLocation const& _loc
	);

	/// State variable info for identifier resolution.
	struct StateVarInfo
	{
		awst::WType const* type;
		awst::AppStorageKind kind;
	};

	/// Try to resolve an identifier as a state variable.
	std::optional<StateVarInfo> resolveStateVar(std::string const& _name);

	/// Transaction type constants for inner transactions.
	static constexpr int TxnTypePay = 1;
	static constexpr int TxnTypeAxfer = 4;
	static constexpr int TxnTypeAppl = 6;

	/// Build a CreateInnerTransaction expression with the given fields.
	std::shared_ptr<awst::Expression> buildCreateInnerTransaction(
		int _txnType,
		std::map<std::string, std::shared_ptr<awst::Expression>> _fields,
		awst::SourceLocation const& _loc
	);

	/// Build a SubmitInnerTransaction and handle the return value.
	/// For bool returns: emit submit as pending statement, return true.
	/// For void returns: return submit directly.
	std::shared_ptr<awst::Expression> buildSubmitAndReturn(
		std::shared_ptr<awst::Expression> _createExpr,
		awst::WType const* _solidityReturnType,
		awst::SourceLocation const& _loc
	);

	/// Create a uint64 IntegerConstant.
	static std::shared_ptr<awst::IntegerConstant> makeUint64(
		std::string _value, awst::SourceLocation const& _loc
	);
};

} // namespace puyasol::builder
