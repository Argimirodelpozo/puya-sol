#include "builder/ContractBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <functional>
#include <map>
#include <set>

namespace puyasol::builder
{

namespace {

/// Checks if a Solidity AST subtree references any state variable whose AST ID
/// is in the given set (i.e. box-stored state variables).
class BoxVarRefChecker: public solidity::frontend::ASTConstVisitor
{
public:
	explicit BoxVarRefChecker(std::set<int64_t> const& _boxVarIds): m_boxVarIds(_boxVarIds) {}
	bool found() const { return m_found; }

	bool visit(solidity::frontend::Identifier const& _node) override
	{
		if (m_found)
			return false;
		if (auto const* decl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				_node.annotation().referencedDeclaration))
		{
			if (m_boxVarIds.count(decl->id()))
				m_found = true;
		}
		return !m_found;
	}

private:
	std::set<int64_t> const& m_boxVarIds;
	bool m_found = false;
};

/// Walks every constructor body in `_contract`'s linearised inheritance
/// chain (current contract first, then bases), running `_checker` against
/// each body. Stops as soon as `_done` returns true.
template <class V>
void walkAllConstructors(
	solidity::frontend::ContractDefinition const& _contract,
	V& _checker,
	std::function<bool()> const& _done)
{
	if (auto const* ctor = _contract.constructor())
		if (ctor->isImplemented() && !ctor->body().statements().empty())
		{
			ctor->body().accept(_checker);
			if (_done()) return;
		}

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base == &_contract)
			continue;
		auto const* baseCtor = base->constructor();
		if (baseCtor && baseCtor->isImplemented()
			&& !baseCtor->body().statements().empty())
		{
			baseCtor->body().accept(_checker);
			if (_done()) return;
		}
	}
}

/// Walks every state-variable initializer expression in the contract's
/// linearised chain.
template <class V>
void walkAllStateVarInits(
	solidity::frontend::ContractDefinition const& _contract,
	V& _checker)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* var: base->stateVariables())
			if (var->value())
				var->value()->accept(_checker);
}

/// Box-write detector — true if the constructor (directly or transitively
/// via a function call) touches any box-stored state variable.
bool detectBoxRefsInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	std::set<int64_t> boxVarIds;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* var: base->stateVariables())
		{
			if (var->isConstant())
				continue;
			if (StorageMapper::shouldUseBoxStorage(*var))
				boxVarIds.insert(var->id());
		}
	if (boxVarIds.empty())
		return false;

	BoxVarRefChecker direct(boxVarIds);
	walkAllConstructors(_contract, direct, [&]{ return direct.found(); });
	if (direct.found())
		return true;

	// Indirect: collect AST IDs of non-constructor functions that touch
	// box-stored vars, then re-walk constructors looking for calls to them.
	std::set<int64_t> boxWriteFuncIds;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* func: base->definedFunctions())
		{
			if (func->isConstructor() || !func->isImplemented())
				continue;
			BoxVarRefChecker fnCheck(boxVarIds);
			func->body().accept(fnCheck);
			if (fnCheck.found())
				boxWriteFuncIds.insert(func->id());
		}
	if (boxWriteFuncIds.empty())
		return false;

	struct CtorCallChecker: public solidity::frontend::ASTConstVisitor
	{
		std::set<int64_t> const& targetIds;
		bool found = false;
		explicit CtorCallChecker(std::set<int64_t> const& _ids): targetIds(_ids) {}
		bool visit(solidity::frontend::FunctionCall const& _node) override
		{
			if (found) return false;
			// solc's ASTNode::referencedDeclaration covers both
			// `f(...)` (Identifier) and `Lib.f(...)` (MemberAccess) —
			// the original Identifier-only check missed library /
			// base-qualified calls into known box-touching functions.
			auto const* decl = solidity::frontend::ASTNode::referencedDeclaration(_node.expression());
			if (decl && targetIds.count(decl->id()))
				found = true;
			return !found;
		}
	};
	CtorCallChecker callChecker(boxWriteFuncIds);
	walkAllConstructors(_contract, callChecker, [&]{ return callChecker.found; });
	return callChecker.found;
}

/// `new C(...)` detector — child contracts deployed via inner-create need
/// the parent to already hold balance, which only lands after AppCreate.
bool detectNewExprInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct NewExprChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::NewExpression const&) override
		{ found = true; return false; }
	};
	NewExprChecker checker;
	walkAllStateVarInits(_contract, checker);
	if (auto const* ctor = _contract.constructor())
		if (ctor->isImplemented())
			ctor->body().accept(checker);
	return checker.found;
}

/// `msg.value` / `msg.sender` / `msg.data` detector — these only resolve
/// correctly inside `__postInit` when the parent groups the Payment with
/// the post-init call (not with AppCreate).
bool detectMsgRefInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct MsgRefChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::MemberAccess const& _ma) override
		{
			// Resolve via solc's MagicVariableDeclaration so a user-defined
			// local named `msg` doesn't trigger a false positive — and a
			// member-access path like `(somelookup).value` doesn't accidentally
			// look like `msg.value`. The magic globals msg/block/tx are tagged
			// as MagicVariableDeclaration on the resolved Identifier.
			auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&_ma.expression());
			if (!id) return !found;
			auto const* magic = dynamic_cast<solidity::frontend::MagicVariableDeclaration const*>(
				id->annotation().referencedDeclaration);
			if (magic && magic->name() == "msg"
				&& (_ma.memberName() == "value"
					|| _ma.memberName() == "sender"
					|| _ma.memberName() == "data"))
				found = true;
			return !found;
		}
	};
	MsgRefChecker checker;
	walkAllStateVarInits(_contract, checker);
	if (auto const* ctor = _contract.constructor())
		if (ctor->isImplemented())
			ctor->body().accept(checker);
	return checker.found;
}

/// `AVM.<x>(...)` detector — the stdlib library issues inner txns that
/// require the contract to already hold MBR + ASA reserves, which can
/// only land via a pay txn AFTER AppCreate. Defers to __postInit.
bool detectAvmLibCallInConstructor(solidity::frontend::ContractDefinition const& _contract)
{
	struct AvmLibCallChecker: public solidity::frontend::ASTConstVisitor
	{
		bool found = false;
		bool visit(solidity::frontend::MemberAccess const& _ma) override
		{
			if (found) return false;
			// solc's ASTNode::referencedDeclaration handles both
			// `AVM.foo()` (Identifier base) and module-aliased forms
			// `import "tokens/AVM.sol" as Mod; Mod.AVM.foo()`
			// (MemberAccess base) without a per-shape dynamic_cast.
			if (auto const* contractDef = dynamic_cast<solidity::frontend::ContractDefinition const*>(
					solidity::frontend::ASTNode::referencedDeclaration(_ma.expression())))
				if (contractDef->isLibrary() && contractDef->name() == "AVM")
					found = true;
			return !found;
		}
	};
	AvmLibCallChecker checker;
	walkAllStateVarInits(_contract, checker);
	walkAllConstructors(_contract, checker, [&]{ return checker.found; });
	return checker.found;
}

/// Combines the four post-init triggers; logs which one fired (via debug).
bool computeNeedsPostInit(solidity::frontend::ContractDefinition const& _contract)
{
	if (detectBoxRefsInConstructor(_contract))
		return true;
	if (detectNewExprInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init deploys child contracts via new C()");
		return true;
	}
	if (detectMsgRefInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init references msg.*");
		return true;
	}
	if (detectAvmLibCallInConstructor(_contract))
	{
		Logger::instance().debug("Forcing __postInit: constructor/state-init calls into AVM stdlib");
		return true;
	}
	return false;
}

} // namespace

awst::ContractMethod ContractBuilder::buildApprovalProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "approval_program";

	auto body = awst::makeBlock(method.sourceLocation);

	// Detect if the constructor needs auto-split into __postInit. Triggered
	// by box state-var writes (direct or transitive), `new C()` deployments,
	// `msg.*` references, or AVM stdlib calls — see helpers above.
	bool needsPostInit = computeNeedsPostInit(_contract);

	// Create-time check: if (Txn.ApplicationID == 0) { base_ctors; ctor_body; return true; }
	{
		auto appIdCheck = awst::makeTxn(std::string("ApplicationID"), awst::WType::uint64Type(), method.sourceLocation);

		auto zero = awst::makeZero(method.sourceLocation);

		auto isCreate = awst::makeNumericCompare(appIdCheck, awst::NumericComparison::Eq, zero, method.sourceLocation);

		auto createBlock = awst::makeBlock(method.sourceLocation);

		// Helper: emit state variable initialization statements for one contract's state vars.
		// Initializes global state variables with explicit initializers or zero/default values.
		// Tracks already-initialized variable names via the 'initialized' set to handle overrides.
		std::set<std::string> stateVarInitialized;
		auto emitStateVarInit = [&](solidity::frontend::ContractDefinition const& base,
			std::vector<std::shared_ptr<awst::Statement>>& targetBody)
		{
			for (auto const* var: base.stateVariables())
			{
				if (var->isConstant())
					continue;
				if (stateVarInitialized.count(var->name()))
					continue;
				stateVarInitialized.insert(var->name());

				auto kind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				auto* wtype = m_typeMapper.map(var->type());

				// Box-stored ARC4 struct with explicit initializer: encode
				// the initializer and box_put it. Box arrays/bytes/dyn
				// arrays are handled by the dedicated m_boxArrayVarNames
				// loop above, so skip those kinds here.
				if (kind == awst::AppStorageKind::Box)
				{
					if (!var->value())
						continue;
					bool isStructBox = wtype
						&& wtype->kind() == awst::WTypeKind::ARC4Struct;
					if (!isStructBox)
						continue;
					auto initVal = m_exprBuilder->build(*var->value());
					if (!initVal)
						continue;
					initVal = TypeCoercion::coerceForAssignment(
						std::move(initVal), wtype, method.sourceLocation);
					for (auto& preStmt: m_exprBuilder->takePrePending())
						targetBody.push_back(std::move(preStmt));
					for (auto& postStmt: m_exprBuilder->takePending())
						targetBody.push_back(std::move(postStmt));
					auto boxKey = awst::makeUtf8BytesConstant(
						var->name(), method.sourceLocation);
					auto put = awst::makeIntrinsicCall(
						"box_put", awst::WType::voidType(), method.sourceLocation);
					put->stackArgs.push_back(std::move(boxKey));
					put->stackArgs.push_back(std::move(initVal));
					targetBody.push_back(awst::makeExpressionStatement(
						std::move(put), method.sourceLocation));
					continue;
				}

				// Only zero-initialize global state (not box storage)
				if (kind != awst::AppStorageKind::AppGlobal)
					continue;

				// Build key
				auto key = awst::makeUtf8BytesConstant(var->name(), method.sourceLocation);

				// Build initial value: use explicit initializer if present,
				// otherwise default to zero/empty.
				std::shared_ptr<awst::Expression> defaultVal;
				if (var->value())
				{
					// Translate the initializer expression (e.g. `= 'Wrapped Ether'`)
					defaultVal = m_exprBuilder->build(*var->value());
					if (defaultVal)
						defaultVal = TypeCoercion::coerceForAssignment(
							std::move(defaultVal), wtype, method.sourceLocation);
					// Flush any prePending statements (e.g. `new C()` emits an
					// inner-txn create + fund before referencing __new_app_id_N)
					// into the target body so the referenced vars are bound
					// before the state-var assignment.
					for (auto& preStmt: m_exprBuilder->takePrePending())
						targetBody.push_back(std::move(preStmt));
					for (auto& postStmt: m_exprBuilder->takePending())
						targetBody.push_back(std::move(postStmt));
				}
				if (!defaultVal)
				{
				if (wtype == awst::WType::accountType())
					defaultVal = awst::makeAddressConstant(
						"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ",
						method.sourceLocation);
				else if (wtype == awst::WType::biguintType())
				{
					auto val = awst::makeZero(method.sourceLocation, awst::WType::biguintType());
					defaultVal = val;
				}
				else if (wtype == awst::WType::boolType()
					|| wtype == awst::WType::uint64Type())
				{
					auto val = awst::makeZero(method.sourceLocation);
					defaultVal = val;
				}
				else if (wtype->kind() == awst::WTypeKind::ReferenceArray
					|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray)
				{
					defaultVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
				}
				else if (wtype->kind() == awst::WTypeKind::ARC4Struct
					|| wtype->kind() == awst::WTypeKind::WTuple)
				{
					// Struct → use StorageMapper's default
					defaultVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
				}
				else
				{
					// Fixed-size bytes (bytes1..bytes32) → N zero bytes so the
					// auto-getter ABI emits the declared width. Dynamic bytes /
					// string keep the empty default.
					int bytesLen = 0;
					if (auto const* bw = dynamic_cast<awst::BytesWType const*>(wtype))
						if (bw->length().has_value() && *bw->length() > 0)
							bytesLen = static_cast<int>(*bw->length());
					defaultVal = awst::makeBytesConstant(
						std::vector<uint8_t>(static_cast<size_t>(bytesLen), 0),
						method.sourceLocation,
						awst::BytesEncoding::Base16,
						wtype && wtype->kind() == awst::WTypeKind::Bytes
							? wtype : awst::WType::bytesType());
				}
				} // end if (!defaultVal)

				// app_global_put(key, defaultVal)
				auto put = awst::makeAppGlobalPut(key, defaultVal, method.sourceLocation);

				auto stmt = awst::makeExpressionStatement(put, method.sourceLocation);
				targetBody.push_back(stmt);
			}
		};

		// Initialize length counters for dynamic array state variables stored in boxes
		{
			auto const& linearized = _contract.annotation().linearizedBaseContracts;
			std::set<std::string> lengthInitialized;
			for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			{
				for (auto const* var: (*it)->stateVariables())
				{
					if (var->isConstant())
						continue;
					if (lengthInitialized.count(var->name()))
						continue;

					auto kind = StorageMapper::shouldUseBoxStorage(*var)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;

					// Only for box-stored arrays (dynamic arrays)
					if (kind != awst::AppStorageKind::Box)
						continue;

					auto* wtype = m_typeMapper.map(var->type());
					if (!wtype)
						continue;
					// Collect dynamic arrays AND dynamic bytes for box creation,
					// PLUS fixed-size ARC4 static arrays (uint[N]) which are
					// stored in a single box of fixed length and need box_create
					// at deploy time so the contract can write to slots without
					// hitting "no such box" at runtime.
					bool isBoxType = wtype->kind() == awst::WTypeKind::ReferenceArray
						|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
						|| wtype == awst::WType::bytesType()
						|| (wtype->kind() == awst::WTypeKind::Bytes
							&& !dynamic_cast<awst::BytesWType const*>(wtype)->length().has_value());
					if (!isBoxType)
						continue;

					// ARC4StaticArray sizing — accept oversized declared sizes by
					// switching to the multi-box layout. AVM caps a single box's
					// value at 32768 bytes; arrays larger than that get split
					// across N boxes keyed `<name>` ++ `itob(page)`. Element
					// reads/writes route at runtime via
					// `page = idx / elemsPerBox`. Pathological declarations
					// (`uint[2 ether]`) still get rejected (effectively infinite
					// pages) so we don't allocate billions of boxes.
					if (wtype->kind() == awst::WTypeKind::ARC4StaticArray)
					{
						auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(wtype);
						if (sa && sa->arraySize() > 0)
						{
							uint64_t totalBytes = StorageMapper::arc4StaticArrayTotalBytes(wtype);
							// Cap pre-allocation at 4 boxes (128 KB). Beyond
							// that, __postInit's box_create burst exceeds
							// reasonable txn-group budget (one app call ≈ 8
							// box_create calls before write-budget exhaustion).
							// Skipping is safe for tests that only access
							// `.length` (compile-time constant); element
							// reads/writes on un-pre-allocated multi-box arrays
							// are a known limitation tracked in
							// multi-box-storage.md. totalBytes == 0
							// (struct/dynamic-element) falls through to the
							// legacy single-box path.
							constexpr uint64_t MAX_PREALLOC_BYTES = 4ULL * 32768ULL;
							if (totalBytes > MAX_PREALLOC_BYTES)
							{
								Logger::instance().warning(
									"state array '" + var->name() + "' has declared size "
									+ std::to_string(sa->arraySize())
									+ " which exceeds 4-box (128 KB) pre-allocation cap — skipping box_create. "
									"Element writes will fail at runtime but .length reads "
									"still return the declared size.",
									method.sourceLocation);
								continue;
							}
						}
					}

					lengthInitialized.insert(var->name());
					// Dynamic array boxes are created in __postInit (after funding)
					// Length is derived from box_len / element_size (no separate counter)
					m_boxArrayVarNames.push_back(var->name());
				}
			}
		}

		// Force __postInit if we have box array vars that need box_create
		if (!m_boxArrayVarNames.empty())
			needsPostInit = true;

		// Collect explicit base constructor calls from the constructor's modifiers
		auto const* constructor = _contract.constructor();
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
			explicitBaseArgs;

		if (constructor)
		{
			// Read constructor parameters from ApplicationArgs during create.
			// Each param is ARC4-encoded in ApplicationArgs[i].
			// For contracts with no constructor params, this loop is skipped.
			int argIndex = 0;
			for (auto const& param: constructor->parameters())
			{
				auto* paramType = m_typeMapper.map(param->type());

				// txna ApplicationArgs i → raw ARC4 bytes
				auto readArg = awst::makeAppArg(argIndex, method.sourceLocation);

				std::shared_ptr<awst::Expression> paramVal;

				if (paramType == awst::WType::accountType())
				{
					// bytes → account via ReinterpretCast
					auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::accountType(), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::biguintType())
				{
					// bytes → biguint via ReinterpretCast (big-endian, no-op on AVM)
					auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::biguintType(), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::uint64Type()
					|| paramType == awst::WType::boolType())
				{
					// Constructor args come as 32-byte big-endian (EVM ABI encoding).
					// Extract last 8 bytes, then btoi to native uint64/bool.
					auto len = awst::makeLen(readArg, method.sourceLocation);

					auto eight = awst::makeIntegerConstant("8", method.sourceLocation);

					auto offset = awst::makeUInt64BinOp(std::move(len), awst::UInt64BinaryOperator::Sub, eight, method.sourceLocation);

					auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), method.sourceLocation);
					extract->stackArgs.push_back(std::move(readArg));
					extract->stackArgs.push_back(std::move(offset));
					auto eight2 = awst::makeIntegerConstant("8", method.sourceLocation);
					extract->stackArgs.push_back(std::move(eight2));

					auto btoi = awst::makeIntrinsicCall("btoi", paramType, method.sourceLocation);
					btoi->stackArgs.push_back(std::move(extract));
					paramVal = std::move(btoi);
				}
				else if (paramType == awst::WType::stringType())
				{
					// bytes → string via ReinterpretCast
					auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::stringType(), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType->kind() == awst::WTypeKind::ReferenceArray)
				{
					// Array params: ReinterpretCast to ARC4 type, then ARC4Decode
					auto const* arc4Type = m_typeMapper.mapToARC4Type(paramType);
					auto cast = awst::makeReinterpretCast(std::move(readArg), arc4Type, method.sourceLocation);

					auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(paramType);
					if (refArr && !refArr->arraySize().has_value())
					{
						paramVal = awst::makeConvertArray(std::move(cast), paramType, method.sourceLocation);
					}
					else
					{
						auto decode = awst::makeARC4Decode(std::move(cast), paramType, method.sourceLocation);
						paramVal = std::move(decode);
					}
				}
				else if (paramType->kind() == awst::WTypeKind::ARC4StaticArray
					|| paramType->kind() == awst::WTypeKind::ARC4DynamicArray)
				{
					// ARC4 array params: just ReinterpretCast raw bytes to ARC4 type
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType->kind() == awst::WTypeKind::Bytes
					&& dynamic_cast<awst::BytesWType const*>(paramType)
					&& dynamic_cast<awst::BytesWType const*>(paramType)->length().has_value())
				{
					// bytes[N] params: ReinterpretCast from raw bytes
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (dynamic_cast<awst::ARC4Struct const*>(paramType))
				{
					// Struct params: ReinterpretCast raw bytes to ARC4 struct type
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else
				{
					// bytes, etc. → use raw bytes directly
					paramVal = std::move(readArg);
				}

				auto target = awst::makeVarExpression(param->name(), paramType, method.sourceLocation);

				auto assignment = awst::makeAssignmentStatement(target, std::move(paramVal), method.sourceLocation);
				createBlock->body.push_back(std::move(assignment));

				++argIndex;
			}

		}

		// solc's ContractLevelChecker has already walked the contract's
		// `is Base(args)` specifiers + constructor modifier invocations
		// (transitively across the inheritance chain) and populated
		// `_contract.annotation().baseConstructorArguments` mapping each
		// base constructor's FunctionDefinition* to the ASTNode (either
		// InheritanceSpecifier or ModifierInvocation) that provides its
		// args. Replaces ~70 LOC of manual InheritanceSpecifier walks +
		// transitive lookups across linearizedBaseContracts.
		for (auto const& [baseCtor, argNode] : _contract.annotation().baseConstructorArguments)
		{
			auto const* baseContract = baseCtor->annotation().contract;
			if (!baseContract) continue;
			std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const* args = nullptr;
			if (auto const* mod = dynamic_cast<solidity::frontend::ModifierInvocation const*>(argNode))
				args = mod->arguments();
			else if (auto const* spec = dynamic_cast<solidity::frontend::InheritanceSpecifier const*>(argNode))
				args = spec->arguments();
			if (args && !args->empty())
				explicitBaseArgs[baseContract] = args;
		}

		if (needsPostInit)
		{
			// All init code deferred to __postInit (state var defaults + constructor body).
			// Create call only sets the pending flag.
			// Set __ctor_pending = 1 in create block.
			auto pendingKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto one = awst::makeOne(method.sourceLocation);

			auto setPending = awst::makeAppGlobalPut(pendingKey, one, method.sourceLocation);

			auto setPendingStmt = awst::makeExpressionStatement(setPending, method.sourceLocation);
			createBlock->body.push_back(std::move(setPendingStmt));

			// Build __postInit method with deferred constructor body
			awst::ContractMethod postInit;
			postInit.sourceLocation = method.sourceLocation;
			postInit.returnType = awst::WType::voidType();
			postInit.cref = m_sourceFile + "." + _contractName;
			postInit.memberName = "__postInit";

			// Add constructor parameters as __postInit method arguments.
			// This allows the caller to pass the same values when calling __postInit
			// that were originally passed to the constructor.
			if (constructor)
			{
				int paramIdx = 0;
				for (auto const& param: constructor->parameters())
				{
					awst::SubroutineArgument arg;
					arg.name = param->name().empty()
						? "_param" + std::to_string(paramIdx)
						: param->name();
					arg.sourceLocation = method.sourceLocation;
					arg.wtype = m_typeMapper.map(param->type());
					postInit.args.push_back(std::move(arg));
					++paramIdx;
				}
			}

			awst::ARC4ABIMethodConfig postInitConfig;
			postInitConfig.name = "__postInit";
			postInitConfig.sourceLocation = method.sourceLocation;
			postInitConfig.allowedCompletionTypes = {0}; // NoOp
			postInitConfig.create = 3; // Disallow
			postInitConfig.readonly = false;
			postInit.arc4MethodConfig = postInitConfig;

			// Remap aggregate types (arrays, tuples) to ARC4 encoding for __postInit args,
			// plus biguint uintN to ARC4UIntN so the ABI signature and the stored value
			// both use Solidity's declared bit width (matches regular method-param remap).
			// Biguint remap tracks (orig name, arc4 name) so we can emit ARC4Decode
			// statements at the top of __postInit body below.
			struct PostInitDecode { std::string origName; std::string arc4Name; awst::WType const* arc4Type; awst::WType const* origType; };
			std::vector<PostInitDecode> postInitDecodes;
			for (size_t pi = 0; pi < postInit.args.size(); ++pi)
			{
				auto& arg = postInit.args[pi];

				if (arg.wtype == awst::WType::biguintType() && constructor
					&& pi < constructor->parameters().size())
				{
					auto const* solType = constructor->parameters()[pi]->annotation().type;
					auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
					if (!intType && solType)
						if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
							intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
					// Only unsigned — signed uses two's-complement in biguint which ARC4UIntN would reject.
					if (intType && !intType->isSigned())
					{
						unsigned bits = intType->numBits();
						auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
						std::string origName = arg.name;
						std::string arc4Name = "__arc4_" + origName;
						postInitDecodes.push_back({origName, arc4Name, arc4Type, arg.wtype});
						arg.name = arc4Name;
						arg.wtype = arc4Type;
						continue;
					}
				}

				bool isAggregate = arg.wtype
					&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
						|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
						|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						|| arg.wtype->kind() == awst::WTypeKind::WTuple);
				if (isAggregate)
				{
					awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
					if (arc4Type != arg.wtype)
						arg.wtype = arc4Type;
				}
			}

			// Set function context so constructor body can reference params by name.
			// For biguint args remapped to ARC4UIntN, use the ORIGINAL name + biguint
			// type so the body looks them up via the decoded local (emitted below).
			{
				std::vector<std::pair<std::string, awst::WType const*>> paramContext;
				std::set<std::string> arc4Names;
				for (auto const& d: postInitDecodes)
					arc4Names.insert(d.arc4Name);
				for (auto const& arg: postInit.args)
				{
					if (arc4Names.count(arg.name)) continue; // skip the __arc4_ shim
					paramContext.emplace_back(arg.name, arg.wtype);
				}
				for (auto const& d: postInitDecodes)
					paramContext.emplace_back(d.origName, d.origType);
				setFunctionContext(paramContext, postInit.returnType);
			}

			auto postInitBody = awst::makeBlock(method.sourceLocation);

			// Guard: assert(__ctor_pending == 1)
			auto readPending = awst::makeIntrinsicCall("app_global_get", awst::WType::uint64Type(), method.sourceLocation);
			readPending->stackArgs.push_back(
				awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation));

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(
				readPending, method.sourceLocation, "__postInit already called"), method.sourceLocation);
			postInitBody->body.push_back(std::move(assertStmt));

			// Clear flag: __ctor_pending = 0
			auto clearKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto zeroVal = awst::makeZero(method.sourceLocation);

			auto clearPending = awst::makeAppGlobalPut(clearKey, zeroVal, method.sourceLocation);

			auto clearStmt = awst::makeExpressionStatement(clearPending, method.sourceLocation);
			postInitBody->body.push_back(std::move(clearStmt));

			// Emit ARC4Decode statements for biguint uintN args remapped to ARC4UIntN.
			// `<origName> = ARC4Decode(<__arc4_origName>)` — constructor body then
			// references the original name as biguint, matching pre-remap semantics.
			for (auto const& decode: postInitDecodes)
			{
				auto arc4Var = awst::makeVarExpression(decode.arc4Name, decode.arc4Type, method.sourceLocation);

				auto decodeExpr = awst::makeARC4Decode(std::move(arc4Var), decode.origType, method.sourceLocation);

				auto target = awst::makeVarExpression(decode.origName, decode.origType, method.sourceLocation);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decodeExpr), method.sourceLocation);
				postInitBody->body.push_back(std::move(assign));
			}

			// Create boxes for dynamic array state variables
			for (auto const& varName: m_boxArrayVarNames)
			{
				auto boxKey = awst::makeUtf8BytesConstant(varName, method.sourceLocation);

				// Uninitialised dynamic `bytes` state vars: skip the
				// box_create so the reader's `box_get → select` fallback
				// returns zero-length bytes. The raw box content is the
				// bytes value stripped of the ARC4 length header, so
				// pre-creating with 2 zero bytes would mean the reader
				// sees 2 bytes of raw data and wraps them with a fresh
				// length header — producing `0x0002 0x0000` instead of
				// an empty `0x0000`. Deferring box creation to the first
				// write fixes the empty case without breaking writes.
				bool isDynamicBytesWithoutInit = false;
				{
					auto const& lin = _contract.annotation().linearizedBaseContracts;
					for (auto const* base: lin)
					{
						for (auto const* var: base->stateVariables())
						{
							if (var->name() != varName || var->isConstant())
								continue;
							auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
							if (arrType && arrType->isByteArrayOrString() && !var->value())
								isDynamicBytesWithoutInit = true;
						}
					}
				}
				if (isDynamicBytesWithoutInit)
					continue;

				// Compute box size: 2 bytes for ARC4 length header (empty dynamic array),
				// or string literal size for bytes/string initializers, or
				// elementSize*N for fixed-size ARC4 static arrays (e.g. uint[20]).
				unsigned boxSizeVal = 2; // ARC4 dynamic array length header
				std::shared_ptr<awst::Expression> boxInitVal;
				// For ARC4StaticArray<dynamic T> a zero-filled buffer is not a
				// valid ARC4 encoding (head offsets must point past the head),
				// so we synthesise the default encoding here and emit a
				// `box_put` instead of `box_create`. dynArc4Default holds the
				// computed bytes when applicable.
				std::optional<std::vector<uint8_t>> dynArc4Default;
				{
					auto const& lin = _contract.annotation().linearizedBaseContracts;
					for (auto const* base: lin)
						for (auto const* var: base->stateVariables())
						{
							if (var->name() != varName || var->isConstant())
								continue;
							// ARC4StaticArray (uint[N], int[N], etc.): allocate
							// elementSize * arraySize bytes so the contract can
							// write to slot indices without "no such box".
							auto* varWtype = m_typeMapper.map(var->type());
							if (varWtype && varWtype->kind() == awst::WTypeKind::ARC4StaticArray)
							{
								auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(varWtype);
								if (sa && sa->arraySize() > 0)
								{
									if (TypeCoercion::arc4IsDynamic(sa))
									{
										if (auto enc = TypeCoercion::arc4DefaultEncoding(sa))
											if (enc->size() > 0 && enc->size() <= 32768)
												dynArc4Default = std::move(*enc);
									}
									uint64_t elemSize = 32; // default for uint256
									auto const* elemT = sa->elementType();
									if (elemT)
									{
										if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(elemT))
											elemSize = std::max<uint64_t>(1u, static_cast<uint64_t>(uintN->n() / 8));
										else if (elemT->kind() == awst::WTypeKind::Bytes)
										{
											auto const* bw = dynamic_cast<awst::BytesWType const*>(elemT);
											if (bw && bw->length().has_value())
												elemSize = *bw->length();
										}
									}
									// AVM box max value is 32768 bytes. For arrays whose
									// encoded size exceeds that, we'll emit N
									// box_create calls (multi-box layout) below;
									// the size we record here is the per-box size.
									uint64_t size = elemSize * static_cast<uint64_t>(sa->arraySize());
									if (size > 32768)
										size = 32768;
									boxSizeVal = static_cast<unsigned>(size);
								}
							}
							if (!var->value())
								continue;
							auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
							if (arrType && arrType->isByteArrayOrString())
							{
								if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(var->value().get()))
									boxSizeVal = static_cast<unsigned>(lit->value().size());
								if (boxSizeVal > 0)
								{
									boxInitVal = m_exprBuilder->build(*var->value());
									if (boxInitVal && boxInitVal->wtype == awst::WType::stringType())
									{
										auto cast = awst::makeReinterpretCast(std::move(boxInitVal), awst::WType::bytesType(), method.sourceLocation);
										boxInitVal = std::move(cast);
									}
								}
							}
							// Non-bytes dynamic arrays with explicit initialiser:
							// `int16[] public x = [-1, -2]`. Build the initialiser
							// via the expression builder, run it through
							// TypeCoercion::coerceForAssignment so any narrower→wider
							// element widening lands, and box_put the result. The
							// dedicated box_array loop below will see boxInitVal set
							// and emit `box_put(key, value)` instead of the empty-
							// header `box_create(key, 2)`.
							else if (arrType && arrType->isDynamicallySized()
								&& !arrType->isByteArrayOrString())
							{
								auto initVal = m_exprBuilder->build(*var->value());
								if (initVal)
								{
									auto* tgtWtype = m_typeMapper.map(arrType);
									initVal = TypeCoercion::coerceForAssignment(
										std::move(initVal), tgtWtype, method.sourceLocation);
									// Materialise as bytes for box_put.
									if (initVal->wtype != awst::WType::bytesType())
										initVal = awst::makeReinterpretCast(
											std::move(initVal), awst::WType::bytesType(),
											method.sourceLocation);
									boxInitVal = std::move(initVal);
								}
							}
						}
				}

				// Multi-box detection: if the var's ARC4StaticArray total size
				// exceeds a single box's capacity, emit N box_create calls
				// keyed `<name>` ++ `itob(page)` instead of one. Element
				// reads/writes route at runtime via the same key suffix
				// scheme (see SolIndexAccessHandlers.cpp).
				unsigned multiBoxN = 0;
				unsigned multiBoxElemSize = 0;
				uint64_t multiBoxTotalBytes = 0;
				uint64_t multiBoxPerPageBytes = 0;
				{
					auto const& lin = _contract.annotation().linearizedBaseContracts;
					for (auto const* base: lin)
						for (auto const* var: base->stateVariables())
						{
							if (var->name() != varName || var->isConstant())
								continue;
							auto* varWtype = m_typeMapper.map(var->type());
							if (StorageMapper::isMultiBoxArray(varWtype))
							{
								multiBoxN = StorageMapper::numBoxesForArray(varWtype);
								multiBoxElemSize = StorageMapper::arc4StaticArrayElementSize(varWtype);
								multiBoxTotalBytes = StorageMapper::arc4StaticArrayTotalBytes(varWtype);
								multiBoxPerPageBytes = static_cast<uint64_t>(
									StorageMapper::elementsPerBox(varWtype)) * multiBoxElemSize;
							}
						}
				}

				if (multiBoxN > 1 && multiBoxElemSize > 0 && !dynArc4Default && !boxInitVal)
				{
					// Multi-box layout: emit N box_create calls.

					for (unsigned page = 0; page < multiBoxN; ++page)
					{
						// Per-page key = name_bytes ++ itob(page)
						auto nameBytes = awst::makeUtf8BytesConstant(varName, method.sourceLocation);
						auto pageInt = awst::makeIntegerConstant(page, method.sourceLocation);
						auto pageItob = awst::makeItob(std::move(pageInt), method.sourceLocation);
						auto pageKey = awst::makeConcat(std::move(nameBytes), std::move(pageItob), method.sourceLocation);

						uint64_t pageSize = (page == multiBoxN - 1)
							? (multiBoxTotalBytes - static_cast<uint64_t>(page) * multiBoxPerPageBytes)
							: multiBoxPerPageBytes;
						auto pageSizeExpr = awst::makeIntegerConstant(pageSize, method.sourceLocation);

						auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), method.sourceLocation);
						boxCreate->stackArgs.push_back(std::move(pageKey));
						boxCreate->stackArgs.push_back(std::move(pageSizeExpr));

						auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), method.sourceLocation);
						postInitBody->body.push_back(std::move(boxStmt));
					}
				}
				else if (dynArc4Default)
				{
					// box_put(key, default_encoding) — creates the box and
					// initialises with a valid ARC4 head/tail layout in one op.
					auto put = awst::makeBoxPut(std::move(boxKey), awst::makeBytesConstant(
						std::move(*dynArc4Default), method.sourceLocation), method.sourceLocation);
					auto putStmt = awst::makeExpressionStatement(std::move(put), method.sourceLocation);
					postInitBody->body.push_back(std::move(putStmt));
				}
				else
				{
					// For dynamic-array initializers (non-bytes case, e.g.
					// `int16[] x = [-1, -2]`) the encoded value's length doesn't
					// match the empty-header `boxSizeVal=2`. box_put can't grow
					// a pre-created box, so skip the box_create entirely and
					// let box_put create the box at the right size in one op.
					bool isNonBytesDynArrInit = false;
					{
						auto const& lin = _contract.annotation().linearizedBaseContracts;
						for (auto const* base: lin)
							for (auto const* var: base->stateVariables())
							{
								if (var->name() != varName || var->isConstant() || !var->value())
									continue;
								auto const* arrType =
									dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
								if (arrType && arrType->isDynamicallySized()
									&& !arrType->isByteArrayOrString())
									isNonBytesDynArrInit = true;
							}
					}

					if (!isNonBytesDynArrInit)
					{
						auto boxSize = awst::makeIntegerConstant(boxSizeVal, method.sourceLocation);

						auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), method.sourceLocation);
						boxCreate->stackArgs.push_back(std::move(boxKey));
						boxCreate->stackArgs.push_back(std::move(boxSize));

						auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), method.sourceLocation);
						postInitBody->body.push_back(std::move(boxStmt));
					}

					// Write initial value for bytes vars with initializers (and
					// for the non-bytes dyn-array case, this is the sole op).
					if (boxInitVal)
					{
						auto putKey = awst::makeUtf8BytesConstant(varName, method.sourceLocation);
						auto put = awst::makeBoxPut(std::move(putKey), std::move(boxInitVal), method.sourceLocation);
						auto putStmt = awst::makeExpressionStatement(std::move(put), method.sourceLocation);
						postInitBody->body.push_back(std::move(putStmt));
					}
				}
			}

			// Initialize all state variable defaults in __postInit
			// (after boxes are created, before constructor bodies run)
			{
				auto const& lin = _contract.annotation().linearizedBaseContracts;
				for (auto it2 = lin.rbegin(); it2 != lin.rend(); ++it2)
					emitStateVarInit(**it2, postInitBody->body);
			}

			// Inline base constructor bodies into __postInit
			auto const& linearized = _contract.annotation().linearizedBaseContracts;
			for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			{
				auto const* base = *it;
				if (base == &_contract)
					continue;

				auto const* baseCtor = base->constructor();
				if (!baseCtor || !baseCtor->isImplemented())
					continue;
				if (baseCtor->body().statements().empty())
					continue;

				// Base constructor parameter assignments
				auto argIt = explicitBaseArgs.find(base);
				if (argIt != explicitBaseArgs.end() && argIt->second && !argIt->second->empty())
				{
					auto const& args = *(argIt->second);
					auto const& params = baseCtor->parameters();
					for (size_t i = 0; i < args.size() && i < params.size(); ++i)
					{
						auto argExpr = m_exprBuilder->build(*args[i]);
						if (!argExpr)
							continue;

						// Storage-pointer params (`T storage p`, `mapping… storage m`,
						// `mapping…[] storage`): register a storage alias instead of
						// materialising a local copy, mirroring the modifier inliner
						// pattern at ModifierInliner.cpp:200-228. Without this, writes
						// inside the base ctor body land in the local var (a noop) and
						// the underlying state never sees them — e.g. `A(m[1])` with
						// A's body doing `m.push(); m[0][1] = 2` silently drops the
						// [1] from the inheritance arg's index chain.
						if (params[i]->referenceLocation()
							== solidity::frontend::VariableDeclaration::Location::Storage)
						{
							sol_ast::StorageAlias alias = [&]() -> sol_ast::StorageAlias {
								if (dynamic_cast<awst::BytesConstant const*>(argExpr.get()))
									return sol_ast::StorageAlias::mappingHolder(std::move(argExpr));
								if (dynamic_cast<awst::IndexExpression const*>(argExpr.get()))
									return sol_ast::StorageAlias::indexedPath(std::move(argExpr));
								if (dynamic_cast<awst::FieldExpression const*>(argExpr.get()))
									return sol_ast::StorageAlias::fieldPath(std::move(argExpr));
								if (dynamic_cast<awst::TupleItemExpression const*>(argExpr.get()))
									return sol_ast::StorageAlias::tupleSlice(std::move(argExpr));
								return sol_ast::StorageAlias::stateRead(std::move(argExpr));
							}();
							m_tr->setStorageAlias(params[i]->id(), std::move(alias));
							continue;
						}

						auto target = awst::makeVarExpression(params[i]->name(), m_typeMapper.map(params[i]->type()), makeLoc(args[i]->location()));

						argExpr = TypeCoercion::implicitNumericCast(
							std::move(argExpr), target->wtype, target->sourceLocation
						);

						auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
						postInitBody->body.push_back(std::move(assignment));
					}
				}

				auto baseBody = buildBlock(baseCtor->body());
				inlineModifiers(*baseCtor, baseBody);
				for (auto& stmt: baseBody->body)
					postInitBody->body.push_back(std::move(stmt));
			}

			// Main constructor body
			if (constructor && constructor->body().statements().size() > 0)
			{
				m_tr->setInConstructor(true);
				auto ctorBody = buildBlock(constructor->body());
				inlineModifiers(*constructor, ctorBody);
				m_tr->setInConstructor(false);
				for (auto& stmt: ctorBody->body)
					postInitBody->body.push_back(std::move(stmt));
			}

			postInit.body = postInitBody;
			m_postInitMethod = std::move(postInit);
		}
		else
		{
		// Constructor body is inlined into the bool-returning approval program.
		// Assembly `return(offset, size)` inside the ctor needs to emit a bool
		// return (handled by AssemblyBuilder::handleReturn when m_returnType is
		// bool). Set stmtCtx.returnType accordingly; restore at the end of the
		// else branch.
		auto const* savedReturnType = m_currentReturnType;
		m_currentReturnType = awst::WType::boolType();

		// LEGACY MODE: state var inits emitted BEFORE constructor arg eval.
		// In Solidity legacy (compileViaYul: false) semantics, base state vars
		// initialize before any constructor work — including before the args
		// to base constructors are evaluated by the derived contract. Tests
		// like inheritance/constructor_inheritance_init_order_3_legacy rely
		// on this: A's `uint x = 2` runs first, THEN B's `A(f())` evaluates
		// f() (which sets x=4), THEN A's body, THEN B's body — final x=4.
		// The interleaved init+body loop further down still works in legacy
		// mode because emitStateVarInit dedups via `stateVarInitialized` set.
		// In viaIR mode (m_viaIR == true) we keep the interleaved behavior:
		// derived state var inits can observe state set by base constructors.
		if (!m_viaIR)
		{
			auto const& linEarly = _contract.annotation().linearizedBaseContracts;
			for (auto itEarly = linEarly.rbegin(); itEarly != linEarly.rend(); ++itEarly)
				emitStateVarInit(**itEarly, createBlock->body);
		}

		// Pre-evaluate constructor arguments in dependency order.
		// In viaIR, all ctor args are evaluated before any state var init or ctor body.
		// For transitive args (D→C→A), C's params must be assigned first so that
		// A's args (from C's modifier) see C's param values, not D's raw values.
		//
		// Phase 1: Assign direct ctor params (from D's modifiers/specifiers) into createBlock
		// Phase 2: Build pre-evaluated expressions for ALL base args
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<std::shared_ptr<awst::Expression>>> preEvaluatedArgs;
		{
			// Identify which args come directly from the main contract vs transitive.
			// Direct base ctor invocations land in two AST shapes: as a
			// ModifierInvocation on the derived ctor, or as an
			// InheritanceSpecifier on the contract itself. Both have a
			// `.name()` whose `referencedDeclaration` is the base
			// ContractDefinition.
			std::set<solidity::frontend::ContractDefinition const*> directBases;
			auto recordBase = [&](solidity::frontend::Declaration const* _ref) {
				if (auto const* bc = dynamic_cast<solidity::frontend::ContractDefinition const*>(_ref))
					directBases.insert(bc);
			};
			if (constructor)
				for (auto const& mod: constructor->modifiers())
					recordBase(mod->name().annotation().referencedDeclaration);
			for (auto const& baseSpec: _contract.baseContracts())
				recordBase(baseSpec->name().annotation().referencedDeclaration);

			// Phase 1: Assign direct base ctor params into createBlock
			// (so transitive args can reference them)
			for (auto const* directBase: directBases)
			{
				auto argIt = explicitBaseArgs.find(directBase);
				if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
					continue;
				auto const* baseCtor = directBase->constructor();
				if (!baseCtor)
					continue;

				auto const& args = *(argIt->second);
				auto const& params = baseCtor->parameters();
				for (size_t i = 0; i < args.size() && i < params.size(); ++i)
				{
					auto argExpr = m_exprBuilder->build(*args[i]);
					if (!argExpr)
						continue;
					auto* targetType = m_typeMapper.map(params[i]->type());
					argExpr = TypeCoercion::implicitNumericCast(
						std::move(argExpr), targetType, makeLoc(args[i]->location()));

					auto target = awst::makeVarExpression(params[i]->name(), targetType, makeLoc(args[i]->location()));

					auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
					createBlock->body.push_back(std::move(assignment));
				}
			}

			// Phase 2: Pre-evaluate transitive base ctor args in reverse-MRO order
			// (most-derived first), so intermediate params are assigned before
			// deeper transitive args reference them.
			// E.g., Final→Derived→Base1→Base: assign Base1.k first (from Derived.i),
			// then evaluate Base.j (from Base1.k).
			auto const& lin = _contract.annotation().linearizedBaseContracts;
			for (auto it = lin.begin(); it != lin.end(); ++it)
			{
				auto const* base = *it;
				if (base == &_contract)
					continue;
				if (directBases.count(base))
					continue;

				auto argIt = explicitBaseArgs.find(base);
				if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
					continue;
				auto const* baseCtor = base->constructor();
				if (!baseCtor)
					continue;

				auto const& args = *(argIt->second);
				auto const& params = baseCtor->parameters();

				// Assign these params into createBlock NOW (so deeper transitives can see them)
				for (size_t i = 0; i < args.size() && i < params.size(); ++i)
				{
					auto argExpr = m_exprBuilder->build(*args[i]);
					if (!argExpr)
						continue;
					auto* targetType = m_typeMapper.map(params[i]->type());
					argExpr = TypeCoercion::implicitNumericCast(
						std::move(argExpr), targetType, makeLoc(args[i]->location()));

					auto target = awst::makeVarExpression(params[i]->name(), targetType, makeLoc(args[i]->location()));

					auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
					createBlock->body.push_back(std::move(assignment));
				}

				// Mark these params as pre-evaluated (empty vector = already assigned)
				preEvaluatedArgs[base] = {};
			}
		}

		// Interleave state variable initialization with constructor bodies.
		// For each base class in C3 linearization order (most-base first):
		//   1. Initialize that base's state variables (explicit initializers or zero)
		//   2. Inline that base's constructor body (with argument assignments)
		// This matches Solidity's viaIR semantics: a derived class's state variable
		// initializer (e.g. `uint y = f()`) can see state set by base constructors.
		auto const& linearized = _contract.annotation().linearizedBaseContracts;
		for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
		{
			auto const* base = *it;

			// 1. Initialize this base's state variables
			emitStateVarInit(*base, createBlock->body);

			if (base == &_contract)
				continue; // Main contract ctor handled separately below

			// 2. Inline this base's constructor body
			auto const* baseCtor = base->constructor();
			if (!baseCtor || !baseCtor->isImplemented())
				continue;
			if (baseCtor->body().statements().empty())
				continue;

			// Generate parameter assignments from pre-evaluated constructor arguments.
			// Direct base params were already assigned in Phase 1.
			// Transitive base params use pre-evaluated expressions.
			auto preIt = preEvaluatedArgs.find(base);
			if (preIt != preEvaluatedArgs.end())
			{
				auto const& evaledArgs = preIt->second;
				auto const& params = baseCtor->parameters();
				for (size_t i = 0; i < evaledArgs.size() && i < params.size(); ++i)
				{
					if (!evaledArgs[i])
						continue;

					auto target = awst::makeVarExpression(params[i]->name(), m_typeMapper.map(params[i]->type()), method.sourceLocation);

					auto assignment = awst::makeAssignmentStatement(target, evaledArgs[i], method.sourceLocation);
					createBlock->body.push_back(std::move(assignment));
				}
			}

			// Translate the base constructor body and inline its modifiers
			auto baseBody = buildBlock(baseCtor->body());
			inlineModifiers(*baseCtor, baseBody);
			for (auto& stmt: baseBody->body)
				createBlock->body.push_back(std::move(stmt));
		}

		// Include main contract constructor body if present
		if (constructor && constructor->body().statements().size() > 0)
		{
			// Restore super targets for constructor body (needed for super.f() calls).
			for (auto const& [id, name]: m_allSuperTargetNames)
				m_tr->setSuperTarget(id, name);
			// Also set up MRO overrides for the constructor specifically
			if (constructor)
			{
				auto pfit = m_perFuncSuperOverrides.find(constructor->id());
				if (pfit != m_perFuncSuperOverrides.end())
					for (auto const& [targetId, superName]: pfit->second)
						m_tr->setSuperTarget(targetId, superName);
			}
			m_tr->setInConstructor(true);
			auto ctorBody = buildBlock(constructor->body());
			inlineModifiers(*constructor, ctorBody);
			m_tr->setInConstructor(false);
			m_tr->clearSuperTargets();
			for (auto& stmt: ctorBody->body)
				createBlock->body.push_back(std::move(stmt));
		}
		m_currentReturnType = savedReturnType;
		} // end else (no postInit needed)

		// Return true to complete the create transaction
		auto createReturn = awst::makeReturnStatement(awst::makeTrue(method.sourceLocation), method.sourceLocation);
		createBlock->body.push_back(createReturn);

		// Initialize the transient-storage blob in scratch slot TRANSIENT_SLOT
		// before the create/dispatch split, so the constructor body can also
		// use tload/tstore (the create branch returns before reaching the
		// post-dispatch preamble below). Scratch slots are per-txn on AVM, so
		// a fresh bzero per app call matches EIP-1153 per-tx transient
		// semantics; writes persist across callsub within an app call because
		// scratch slots do. Size covers all declared transient vars (packed)
		// plus at least one slot to back asm tload/tstore when no named vars
		// are declared.
		{
			unsigned blobBytes = m_transientStorage.blobSize();
			if (blobBytes < AssemblyBuilder::SLOT_SIZE)
				blobBytes = AssemblyBuilder::SLOT_SIZE;

			auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
			storeOp->immediates = {AssemblyBuilder::TRANSIENT_SLOT};
			storeOp->stackArgs.push_back(awst::makeBzero(blobBytes, method.sourceLocation));

			auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation);
			body->body.push_back(std::move(exprStmt));
		}

		// Initialize EVM memory blob in scratch slot 0 BEFORE the create/dispatch
		// split so the constructor body (which can declare `T memory t;` locals
		// that emit FMP bumps reading slot 0) sees a properly initialized blob.
		// Each app call gets fresh scratch space, so we must initialize on every call.
		// store 0, bzero(4096) — pre-allocate a 4KB memory blob
		{
			auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
			storeOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
			storeOp->stackArgs.push_back(awst::makeBzero(AssemblyBuilder::SLOT_SIZE, method.sourceLocation));

			auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation);
			body->body.push_back(std::move(exprStmt));

			// Write the free memory pointer (FMP) at offset 0x40 = 0x80.
			auto loadBlob = awst::makeIntrinsicCall("load", awst::WType::bytesType(), method.sourceLocation);
			loadBlob->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};

			auto fmpOffset = awst::makeIntegerConstant("64", method.sourceLocation); // 0x40

			std::vector<uint8_t> fmpBytesVal(31, 0);
			fmpBytesVal.push_back(0x80);
			auto fmpBytes = awst::makeBytesConstant(
				std::move(fmpBytesVal), method.sourceLocation, awst::BytesEncoding::Unknown);

			auto replaceOp = awst::makeReplace3(std::move(loadBlob), std::move(fmpOffset), std::move(fmpBytes), method.sourceLocation);
			auto storeFmpOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
			storeFmpOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
			storeFmpOp->stackArgs.push_back(std::move(replaceOp));

			auto fmpStmt = awst::makeExpressionStatement(std::move(storeFmpOp), method.sourceLocation);
			body->body.push_back(std::move(fmpStmt));
		}

		body->body.push_back(awst::makeIfElse(
			isCreate, createBlock, nullptr, method.sourceLocation));
	}

	// Transient state vars live in scratch slot TRANSIENT_SLOT (packed blob,
	// shared with asm tload/tstore). Scratch is per-txn on AVM, so the
	// scratch bzero in the preamble above already satisfies EIP-1153 per-tx
	// reset — no per-call app_global reset needed.

	// Solc's `ContractDefinition::fallbackFunction()` and
	// `receiveFunction()` walk the linearized MRO themselves and return
	// the first match — same semantics as the hand-rolled double loop.
	auto const* fallbackFunc = _contract.fallbackFunction();
	auto const* receiveFunc = _contract.receiveFunction();
	if (fallbackFunc && !fallbackFunc->isImplemented())
		fallbackFunc = nullptr;
	if (receiveFunc && !receiveFunc->isImplemented())
		receiveFunc = nullptr;

	if (!fallbackFunc && !receiveFunc)
	{
		// No fallback/receive: use the normal pattern `return ARC4Router()`
		// which triggers puya's can_exit_early=True (rejects on no selector match).
		auto routerExpr = awst::makeARC4Router(awst::WType::boolType(), method.sourceLocation);

		auto routerReturn = awst::makeReturnStatement(routerExpr, method.sourceLocation);
		body->body.push_back(routerReturn);
	}
	else
	{
		// Custom dispatch for fallback/receive.
		// Pattern:
		//   if (NumAppArgs == 0) {
		//     if (receive) call receive; else call fallback;
		//     return true;
		//   }
		//   __did_match = ARC4Router();
		//   if (!__did_match) {
		//     call fallback;  // or reject if no fallback
		//     __did_match = true;
		//   }
		//   return __did_match;
		//
		// Using ARC4Router as an assignment value forces puya's
		// can_exit_early=False mode, so the router returns false on no-match
		// instead of calling err.

		// isBareCall=true → pass empty bytes as the fallback argument
		// isBareCall=false → pass ApplicationArgs[0] (the unmatched data)
		auto makeCall = [&](std::string const& _name,
			solidity::frontend::FunctionDefinition const* _func,
			bool _isBareCall)
			-> std::shared_ptr<awst::Statement>
		{
			auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{_name}, awst::WType::voidType(), method.sourceLocation);
			// If the function takes a bytes parameter, pass the calldata.
			// Fallback may take `bytes calldata _input`.
			if (_func && _func->parameters().size() == 1)
			{
				std::shared_ptr<awst::Expression> argExpr;
				if (_isBareCall)
				{
					// No calldata in bare calls — pass empty bytes
					argExpr = awst::makeBytesConstant({}, method.sourceLocation);
				}
				else
				{
					argExpr = awst::makeAppArg(0, method.sourceLocation);
				}

				awst::pushCallArg(call->args, std::move(argExpr));
			}

			auto stmt = awst::makeExpressionStatement(call, method.sourceLocation);
			return stmt;
		};

		auto makeTrueLit = [&]() {
			return awst::makeTrue(method.sourceLocation);
		};

		auto makeReturnTrue = [&]() -> std::shared_ptr<awst::Statement> {
			auto r = awst::makeReturnStatement(makeTrueLit(), method.sourceLocation);
			return r;
		};

		// Step 1: Bare call check (NumAppArgs == 0).
		// Call receive/fallback and return true — no selector to match.
		{
			auto numAppArgs = awst::makeTxn(std::string("NumAppArgs"), awst::WType::uint64Type(), method.sourceLocation);

			auto zero = awst::makeZero(method.sourceLocation);

			auto isBareCall = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Eq, std::move(zero), method.sourceLocation);

			auto bareBlock = awst::makeBlock(method.sourceLocation);
			if (receiveFunc)
				bareBlock->body.push_back(makeCall("__receive", receiveFunc, true));
			else if (fallbackFunc)
				bareBlock->body.push_back(makeCall("__fallback", fallbackFunc, true));
			bareBlock->body.push_back(makeReturnTrue());

			body->body.push_back(awst::makeIfElse(
				std::move(isBareCall), std::move(bareBlock), nullptr,
				method.sourceLocation));
		}

		// Step 2: Non-bare call — run the ARC4 router.
		// Assign result to var (triggers can_exit_early=False in puya).
		std::string matchVarName = "__did_match_routing";
		{
			auto matchVar = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto routerExpr = awst::makeARC4Router(awst::WType::boolType(), method.sourceLocation);

			auto assignMatch = awst::makeAssignmentStatement(std::move(matchVar), std::move(routerExpr), method.sourceLocation);
			body->body.push_back(std::move(assignMatch));
		}

		// Step 3: If no match AND fallback exists, call fallback.
		if (fallbackFunc)
		{
			auto matchVarRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto notMatch = awst::makeNot(std::move(matchVarRead), method.sourceLocation);

			auto dispatchBlock = awst::makeBlock(method.sourceLocation);
			dispatchBlock->body.push_back(makeCall("__fallback", fallbackFunc, false));

			// Set __did_match = true so the approval returns true.
			auto matchVarWrite = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto assignTrue = awst::makeAssignmentStatement(std::move(matchVarWrite), makeTrueLit(), method.sourceLocation);
			dispatchBlock->body.push_back(std::move(assignTrue));

			body->body.push_back(awst::makeIfElse(
				std::move(notMatch), std::move(dispatchBlock), nullptr,
				method.sourceLocation));
		}

		// Step 4: return __did_match_routing
		auto finalRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

		auto retStmt = awst::makeReturnStatement(std::move(finalRead), method.sourceLocation);
		body->body.push_back(std::move(retStmt));
	}

	method.body = body;

	return method;
}

} // namespace puyasol::builder
