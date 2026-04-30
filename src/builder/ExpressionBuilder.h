#pragma once

#include "awst/Node.h"
#include "builder/sol-eb/BuilderContext.h"
#include "builder/sol-eb/BuilderRegistry.h"
#include "builder/sol-eb/BuiltinCallables.h"
#include "builder/sol-eb/TypeConversions.h"
#include "builder/sol-ast/SolExpressionFactory.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-intrinsics/IntrinsicMapper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

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

/// Builds AWST Expression nodes from Solidity AST expressions.
///
/// Owns an `eb::BuilderContext` which holds all per-translation mutable state
/// (scope tables, pending-statement buffers, parameter remaps, etc.). Most of
/// this class is thin forwarders to `m_ctx`; over the course of the migration
/// these forwarders will move to `BuilderContext` directly and this class will
/// be deleted.
class ExpressionBuilder
{
public:
	ExpressionBuilder(
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

	/// Build an AWST expression from a Solidity expression.
	std::shared_ptr<awst::Expression> build(solidity::frontend::Expression const& _expr);

	/// Insert implicit numeric cast if needed (e.g. uint64 → biguint).
	static std::shared_ptr<awst::Expression> implicitNumericCast(
		std::shared_ptr<awst::Expression> _expr,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	/// Get the embedded BuilderContext. Used by external code (e.g. ContractBuilder
	/// dispatcher generation) that needs to pass a BuilderContext to static helpers.
	eb::BuilderContext& builderContext() { return m_ctx; }

	/// Consume any pending statements generated during expression translation.
	std::vector<std::shared_ptr<awst::Statement>> takePendingStatements();

	/// Consume any pre-pending statements (must execute before the expression).
	std::vector<std::shared_ptr<awst::Statement>> takePrePendingStatements();

	void setCurrentContract(solidity::frontend::ContractDefinition const* _c) { m_ctx.currentContract = _c; }

	/// Register a parameter remap: when an Identifier references the AST declaration
	/// with ID _declId, resolve it as a VarExpression with _uniqueName instead.
	/// Used for modifier parameter binding.
	void addParamRemap(int64_t _declId, std::string const& _uniqueName, awst::WType const* _type);
	void removeParamRemap(int64_t _declId);

	/// Register a super call target: when `super.method()` is translated,
	/// the base function with AST ID _funcId is targeted as subroutine _name.
	void addSuperTarget(int64_t _funcId, std::string const& _name);
	void clearSuperTargets();
	std::unordered_map<int64_t, std::string> const& superTargetNames() const { return m_ctx.superTargetNames; }

	/// Register a storage pointer alias.
	void addStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr);
	void removeStorageAlias(int64_t _declId);

	/// Mark a declaration as a mapping-storage-ref parameter whose runtime
	/// value is the box key prefix (bytes).
	void addMappingKeyParam(int64_t _declId, std::string const& _paramName);
	std::string getMappingKeyParam(int64_t _declId) const;

	void setTransientStorage(TransientStorage* _ts) { m_ctx.transientStorage = _ts; }
	TransientStorage* transientStorage() const { return m_ctx.transientStorage; }
	void setInConstructor(bool _inConstructor) { m_ctx.inConstructor = _inConstructor; }
	bool inConstructor() const { return m_ctx.inConstructor; }

	void addSlotStorageRef(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
	{ m_ctx.slotStorageRefs[_declId] = std::move(_expr); }
	std::shared_ptr<awst::Expression> getSlotStorageRef(int64_t _declId) const
	{
		auto it = m_ctx.slotStorageRefs.find(_declId);
		return it != m_ctx.slotStorageRefs.end() ? it->second : nullptr;
	}

	std::string resolveVarName(std::string const& _name, int64_t _declId)
	{
		auto remapIt = m_ctx.paramRemaps.find(_declId);
		if (remapIt != m_ctx.paramRemaps.end())
			return remapIt->second.name;

		auto it = m_ctx.varNameToId.find(_name);
		if (it != m_ctx.varNameToId.end() && it->second != _declId)
		{
			std::string unique = _name + "__" + std::to_string(_declId);
			m_ctx.varNameToId[unique] = _declId;
			return unique;
		}
		m_ctx.varNameToId[_name] = _declId;
		return _name;
	}

	std::string lookupVarName(std::string const& _name, int64_t _declId) const
	{
		std::string unique = _name + "__" + std::to_string(_declId);
		auto it = m_ctx.varNameToId.find(unique);
		if (it != m_ctx.varNameToId.end() && it->second == _declId)
			return unique;
		return _name;
	}

	void setInUncheckedBlock(bool _v) { m_ctx.inUncheckedBlock = _v; }
	bool inUncheckedBlock() const { return m_ctx.inUncheckedBlock; }

	/// Record that a local variable declaration has a known constant value.
	void trackConstantLocal(int64_t _declId, unsigned long long _value);
	unsigned long long getConstantLocal(solidity::frontend::Declaration const* _decl) const;

	/// Record that a function-type local variable points to a specific function.
	void trackFuncPtrTarget(int64_t _declId, solidity::frontend::FunctionDefinition const* _func);
	solidity::frontend::FunctionDefinition const* getFuncPtrTarget(int64_t _declId) const;

	/// RAII scope guard that snapshots mutable context state on construction
	/// and restores it on destruction.
	class ScopeGuard
	{
	public:
		explicit ScopeGuard(ExpressionBuilder& _eb)
			: m_ctx(_eb.m_ctx),
			  m_savedFuncPtrTargets(_eb.m_ctx.funcPtrTargets),
			  m_savedStorageAliases(_eb.m_ctx.storageAliases),
			  m_savedConstantLocals(_eb.m_ctx.constantLocals),
			  m_savedVarNames(_eb.m_ctx.varNameToId)
		{}
		~ScopeGuard()
		{
			m_ctx.funcPtrTargets = std::move(m_savedFuncPtrTargets);
			m_ctx.storageAliases = std::move(m_savedStorageAliases);
			m_ctx.constantLocals = std::move(m_savedConstantLocals);
			m_ctx.varNameToId = std::move(m_savedVarNames);
		}
		ScopeGuard(ScopeGuard const&) = delete;
		ScopeGuard& operator=(ScopeGuard const&) = delete;
	private:
		eb::BuilderContext& m_ctx;
		std::map<int64_t, solidity::frontend::FunctionDefinition const*> m_savedFuncPtrTargets;
		std::map<int64_t, std::shared_ptr<awst::Expression>> m_savedStorageAliases;
		std::unordered_map<int64_t, unsigned long long> m_savedConstantLocals;
		std::map<std::string, int64_t> m_savedVarNames;
	};

	ScopeGuard pushScope() { return ScopeGuard(*this); }

private:
	/// The owned builder context: holds all mutable per-translation state.
	eb::BuilderContext m_ctx;

	eb::BuilderRegistry m_registry;
	eb::BuiltinCallableRegistry m_builtinCallables;
	eb::TypeConversionRegistry m_typeConversions;
	std::unique_ptr<sol_ast::SolExpressionFactory> m_factory;

	LibraryFunctionIdMap const& m_libraryFunctionIds;
	OverloadedNamesSet const& m_overloadedNames;
	FreeFunctionIdMap const& m_freeFunctionById;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc);

	/// Get the disambiguated method name for a function.
	std::string resolveMethodName(solidity::frontend::FunctionDefinition const& _func);

	/// Build a copy-on-write tuple with one field updated.
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
