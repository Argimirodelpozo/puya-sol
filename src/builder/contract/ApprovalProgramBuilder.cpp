#include "builder/ContractBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <map>
#include <set>

namespace puyasol::builder
{

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

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = method.sourceLocation;

	// Detect if constructor needs auto-split (box writes in constructor)
	// Only generate __postInit if the constructor body actually references
	// box-stored state variables. Having box storage + constructor code is
	// not sufficient — if the constructor only writes global state, it can
	// all happen during the create transaction.
	bool needsPostInit = false;
	{
		// Collect AST IDs of all box-stored state variables
		std::set<int64_t> boxVarIds;
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
		{
			for (auto const* var: base->stateVariables())
			{
				if (var->isConstant())
					continue;
				if (StorageMapper::shouldUseBoxStorage(*var))
					boxVarIds.insert(var->id());
			}
		}

		if (!boxVarIds.empty())
		{
			// Walk constructor bodies to check if they reference any box-stored variable.
			// Also check functions called from constructors (transitively).
			BoxVarRefChecker checker(boxVarIds);

			// First, scan ALL non-constructor functions to find which ones
			// reference box-stored state variables
			std::set<int64_t> boxWriteFuncIds;
			for (auto const* base: _contract.annotation().linearizedBaseContracts)
			{
				for (auto const* func: base->definedFunctions())
				{
					if (func->isConstructor() || !func->isImplemented())
						continue;
					BoxVarRefChecker funcChecker(boxVarIds);
					func->body().accept(funcChecker);
					if (funcChecker.found())
						boxWriteFuncIds.insert(func->id());
				}
			}

			// Now walk constructor bodies checking for:
			// 1. Direct references to box-stored state variables
			// 2. Calls to functions that reference box-stored state variables
			auto const* ctor = _contract.constructor();
			if (ctor && !ctor->body().statements().empty())
				ctor->body().accept(checker);

			if (!checker.found())
			{
				for (auto const* base: _contract.annotation().linearizedBaseContracts)
				{
					if (base == &_contract)
						continue;
					auto const* baseCtor = base->constructor();
					if (baseCtor && baseCtor->isImplemented()
						&& !baseCtor->body().statements().empty())
					{
						baseCtor->body().accept(checker);
						if (checker.found())
							break;
					}
				}
			}

			// If direct references weren't found, check if constructors
			// call any function that writes to boxes
			if (!checker.found() && !boxWriteFuncIds.empty())
			{
				// Scan constructor bodies for FunctionCall nodes whose
				// referenced declaration is in boxWriteFuncIds
				struct CtorCallChecker: public solidity::frontend::ASTConstVisitor
				{
					std::set<int64_t> const& targetIds;
					bool found = false;
					explicit CtorCallChecker(std::set<int64_t> const& _ids): targetIds(_ids) {}
					bool visit(solidity::frontend::FunctionCall const& _node) override
					{
						if (found) return false;
						auto const* expr = &_node.expression();
						// Unwrap MemberAccess for calls like _grantRole(...)
						if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(expr))
						{
							auto const* decl = id->annotation().referencedDeclaration;
							if (decl && targetIds.count(decl->id()))
								found = true;
						}
						return !found;
					}
				};
				CtorCallChecker callChecker(boxWriteFuncIds);
				if (ctor && !ctor->body().statements().empty())
					ctor->body().accept(callChecker);
				if (!callChecker.found)
				{
					for (auto const* base: _contract.annotation().linearizedBaseContracts)
					{
						if (base == &_contract)
							continue;
						auto const* baseCtor = base->constructor();
						if (baseCtor && baseCtor->isImplemented()
							&& !baseCtor->body().statements().empty())
						{
							baseCtor->body().accept(callChecker);
							if (callChecker.found)
								break;
						}
					}
				}
				needsPostInit = callChecker.found;
			}
			else
			{
				needsPostInit = checker.found();
			}
		}
	}

	// Force __postInit if the constructor (or state var initializers)
	// contains `new C()` — the inner create/fund txns need the parent
	// to already have balance, which only happens after deployment.
	if (!needsPostInit)
	{
		struct NewExprChecker: public solidity::frontend::ASTConstVisitor
		{
			bool found = false;
			bool visit(solidity::frontend::NewExpression const&) override
			{ found = true; return false; }
		};
		NewExprChecker newChecker;
		// Check state variable initializers
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
			for (auto const* var: base->stateVariables())
				if (var->value())
					var->value()->accept(newChecker);
		// Check constructor body
		if (auto const* ctor = _contract.constructor())
			if (ctor->isImplemented())
				ctor->body().accept(newChecker);
		if (newChecker.found)
		{
			needsPostInit = true;
			Logger::instance().debug("Forcing __postInit: constructor/state-init deploys child contracts via new C()");
		}
	}

	// Force __postInit when any state-var initializer or constructor body
	// references msg.value / msg.sender / msg.data. At AppCreate time these
	// read from the caller's group context (e.g. msg.value sees Amount of the
	// preceding group txn), which is correct when the contract is deployed
	// by a PaymentTxn-preceded ApplicationCreateTxn. But when this contract
	// is deployed as a CHILD via `new C{value: N}()`, the parent's
	// SolNewExpression groups the Payment+__postInit call (not the Payment+
	// AppCreate), so msg.value is only visible inside __postInit. Deferring
	// the initializer is the simplest way to make `new C{value:N}(...)`
	// with msg.value semantics work.
	if (!needsPostInit)
	{
		struct MsgRefChecker: public solidity::frontend::ASTConstVisitor
		{
			bool found = false;
			bool visit(solidity::frontend::MemberAccess const& _ma) override
			{
				if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&_ma.expression()))
				{
					if (id->name() == "msg"
						&& (_ma.memberName() == "value"
							|| _ma.memberName() == "sender"
							|| _ma.memberName() == "data"))
						found = true;
				}
				return !found;
			}
		};
		MsgRefChecker msgChecker;
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
			for (auto const* var: base->stateVariables())
				if (var->value())
					var->value()->accept(msgChecker);
		if (auto const* ctor = _contract.constructor())
			if (ctor->isImplemented())
				ctor->body().accept(msgChecker);
		if (msgChecker.found)
		{
			needsPostInit = true;
			Logger::instance().debug("Forcing __postInit: constructor/state-init references msg.*");
		}
	}

	// Create-time check: if (Txn.ApplicationID == 0) { base_ctors; ctor_body; return true; }
	{
		auto appIdCheck = std::make_shared<awst::IntrinsicCall>();
		appIdCheck->sourceLocation = method.sourceLocation;
		appIdCheck->opCode = "txn";
		appIdCheck->immediates = {std::string("ApplicationID")};
		appIdCheck->wtype = awst::WType::uint64Type();

		auto zero = awst::makeIntegerConstant("0", method.sourceLocation);

		auto isCreate = awst::makeNumericCompare(appIdCheck, awst::NumericComparison::Eq, zero, method.sourceLocation);

		auto createBlock = std::make_shared<awst::Block>();
		createBlock->sourceLocation = method.sourceLocation;

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
				{
					auto addr = std::make_shared<awst::AddressConstant>();
					addr->sourceLocation = method.sourceLocation;
					addr->wtype = awst::WType::accountType();
					addr->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
					defaultVal = addr;
				}
				else if (wtype == awst::WType::biguintType())
				{
					auto val = awst::makeIntegerConstant("0", method.sourceLocation, awst::WType::biguintType());
					defaultVal = val;
				}
				else if (wtype == awst::WType::boolType()
					|| wtype == awst::WType::uint64Type())
				{
					auto val = awst::makeIntegerConstant("0", method.sourceLocation);
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
				auto put = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), method.sourceLocation);
				put->stackArgs.push_back(key);
				put->stackArgs.push_back(defaultVal);

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

					// Skip box_create for oversized static arrays (e.g.
					// `uint[2 ether]` which would need 2^63 bytes). The
					// array's `.length` is a compile-time literal so reads
					// keep working; element writes would fail at runtime,
					// but such arrays are almost always declared and never
					// written in tests.
					if (wtype->kind() == awst::WTypeKind::ARC4StaticArray)
					{
						auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(wtype);
						if (sa && sa->arraySize() > 0)
						{
							uint64_t elemSize = 32;
							if (auto const* elemT = sa->elementType())
							{
								if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(elemT))
									elemSize = std::max<uint64_t>(1u, static_cast<uint64_t>(uintN->n() / 8));
								else if (elemT->kind() == awst::WTypeKind::Bytes)
									if (auto const* bw = dynamic_cast<awst::BytesWType const*>(elemT))
										if (bw->length().has_value())
											elemSize = *bw->length();
							}
							uint64_t sz = elemSize * static_cast<uint64_t>(sa->arraySize());
							if (sz > 32768)
							{
								Logger::instance().warning(
									"state array '" + var->name() + "' has declared size "
									+ std::to_string(sa->arraySize())
									+ " which exceeds the 32KB box limit — skipping box_create. "
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
				auto readArg = std::make_shared<awst::IntrinsicCall>();
				readArg->sourceLocation = method.sourceLocation;
				readArg->opCode = "txna";
				readArg->immediates = {std::string("ApplicationArgs"), argIndex};
				readArg->wtype = awst::WType::bytesType();

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
					auto len = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), method.sourceLocation);
					len->stackArgs.push_back(readArg);

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
						auto convert = std::make_shared<awst::ConvertArray>();
						convert->sourceLocation = method.sourceLocation;
						convert->wtype = paramType;
						convert->expr = std::move(cast);
						paramVal = std::move(convert);
					}
					else
					{
						auto decode = std::make_shared<awst::ARC4Decode>();
						decode->sourceLocation = method.sourceLocation;
						decode->wtype = paramType;
						decode->value = std::move(cast);
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

			for (auto const& mod: constructor->modifiers())
			{
				auto const* refDecl = mod->name().annotation().referencedDeclaration;
				if (auto const* baseContract =
						dynamic_cast<solidity::frontend::ContractDefinition const*>(refDecl))
				{
					explicitBaseArgs[baseContract] = mod->arguments();
				}
			}
		}

		// Also collect arguments from inheritance specifiers (e.g. `is Base(arg1, arg2)`)
		for (auto const& baseSpec: _contract.baseContracts())
		{
			auto const* refDecl = baseSpec->name().annotation().referencedDeclaration;
			auto const* baseContract =
				dynamic_cast<solidity::frontend::ContractDefinition const*>(refDecl);
			if (baseContract && baseSpec->arguments()
				&& !baseSpec->arguments()->empty()
				&& explicitBaseArgs.find(baseContract) == explicitBaseArgs.end())
			{
				explicitBaseArgs[baseContract] = baseSpec->arguments();
			}
		}

		// Collect transitive base constructor args from intermediate base contracts.
		// e.g. if ConfigPositionManager → PositionManagerBase(x) → Ownable(x),
		// we need explicitBaseArgs[Ownable] from PositionManagerBase's constructor.
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
		{
			if (base == &_contract)
				continue;
			// Check base's constructor modifiers
			if (auto const* baseCtor = base->constructor())
			{
				for (auto const& mod: baseCtor->modifiers())
				{
					auto const* ref = mod->name().annotation().referencedDeclaration;
					if (auto const* grandBase =
							dynamic_cast<solidity::frontend::ContractDefinition const*>(ref))
					{
						if (explicitBaseArgs.find(grandBase) == explicitBaseArgs.end()
							&& mod->arguments() && !mod->arguments()->empty())
						{
							explicitBaseArgs[grandBase] = mod->arguments();
						}
					}
				}
			}
			// Check base's inheritance specifiers
			for (auto const& baseSpec: base->baseContracts())
			{
				auto const* ref = baseSpec->name().annotation().referencedDeclaration;
				auto const* grandBase =
					dynamic_cast<solidity::frontend::ContractDefinition const*>(ref);
				if (grandBase && baseSpec->arguments()
					&& !baseSpec->arguments()->empty()
					&& explicitBaseArgs.find(grandBase) == explicitBaseArgs.end())
				{
					explicitBaseArgs[grandBase] = baseSpec->arguments();
				}
			}
		}

		if (needsPostInit)
		{
			// All init code deferred to __postInit (state var defaults + constructor body).
			// Create call only sets the pending flag.
			// Set __ctor_pending = 1 in create block.
			auto pendingKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto one = awst::makeIntegerConstant("1", method.sourceLocation);

			auto setPending = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), method.sourceLocation);
			setPending->stackArgs.push_back(pendingKey);
			setPending->stackArgs.push_back(one);

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

			auto postInitBody = std::make_shared<awst::Block>();
			postInitBody->sourceLocation = method.sourceLocation;

			// Guard: assert(__ctor_pending == 1)
			auto readPending = awst::makeIntrinsicCall("app_global_get", awst::WType::uint64Type(), method.sourceLocation);
			readPending->stackArgs.push_back(
				awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation));

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(
				readPending, method.sourceLocation, "__postInit already called"), method.sourceLocation);
			postInitBody->body.push_back(std::move(assertStmt));

			// Clear flag: __ctor_pending = 0
			auto clearKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto zeroVal = awst::makeIntegerConstant("0", method.sourceLocation);

			auto clearPending = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), method.sourceLocation);
			clearPending->stackArgs.push_back(clearKey);
			clearPending->stackArgs.push_back(zeroVal);

			auto clearStmt = awst::makeExpressionStatement(clearPending, method.sourceLocation);
			postInitBody->body.push_back(std::move(clearStmt));

			// Emit ARC4Decode statements for biguint uintN args remapped to ARC4UIntN.
			// `<origName> = ARC4Decode(<__arc4_origName>)` — constructor body then
			// references the original name as biguint, matching pre-remap semantics.
			for (auto const& decode: postInitDecodes)
			{
				auto arc4Var = awst::makeVarExpression(decode.arc4Name, decode.arc4Type, method.sourceLocation);

				auto decodeExpr = std::make_shared<awst::ARC4Decode>();
				decodeExpr->sourceLocation = method.sourceLocation;
				decodeExpr->wtype = decode.origType;
				decodeExpr->value = std::move(arc4Var);

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
									// AVM box max is 32768 bytes. Cap huge declared
									// sizes (e.g. `uint[2 ether]`) at the max so the
									// contract still deploys; element access beyond
									// the cap will fail at runtime, but `.length`
									// (a compile-time constant) keeps working.
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
						}
				}
				auto boxSize = awst::makeIntegerConstant(std::to_string(boxSizeVal), method.sourceLocation);

				auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), method.sourceLocation);
				boxCreate->stackArgs.push_back(std::move(boxKey));
				boxCreate->stackArgs.push_back(std::move(boxSize));

				auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), method.sourceLocation);
				postInitBody->body.push_back(std::move(boxStmt));

				// Write initial value for bytes vars with initializers
				if (boxInitVal)
				{
					auto putKey = awst::makeUtf8BytesConstant(varName, method.sourceLocation);
					auto put = awst::makeIntrinsicCall("box_put", awst::WType::voidType(), method.sourceLocation);
					put->stackArgs.push_back(std::move(putKey));
					put->stackArgs.push_back(std::move(boxInitVal));
					auto putStmt = awst::makeExpressionStatement(std::move(put), method.sourceLocation);
					postInitBody->body.push_back(std::move(putStmt));
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
				m_exprBuilder->inConstructor = true;
				auto ctorBody = buildBlock(constructor->body());
				inlineModifiers(*constructor, ctorBody);
				m_exprBuilder->inConstructor = false;
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
			// Identify which args come directly from the main contract vs transitive
			std::set<solidity::frontend::ContractDefinition const*> directBases;
			if (constructor)
			{
				for (auto const& mod: constructor->modifiers())
				{
					auto const* ref = mod->name().annotation().referencedDeclaration;
					if (auto const* bc = dynamic_cast<solidity::frontend::ContractDefinition const*>(ref))
						directBases.insert(bc);
				}
			}
			for (auto const& baseSpec: _contract.baseContracts())
			{
				auto const* ref = baseSpec->name().annotation().referencedDeclaration;
				if (auto const* bc = dynamic_cast<solidity::frontend::ContractDefinition const*>(ref))
					directBases.insert(bc);
			}

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
				m_exprBuilder->superTargetNames[id] = name;
			// Also set up MRO overrides for the constructor specifically
			if (constructor)
			{
				auto pfit = m_perFuncSuperOverrides.find(constructor->id());
				if (pfit != m_perFuncSuperOverrides.end())
					for (auto const& [targetId, superName]: pfit->second)
						m_exprBuilder->superTargetNames[targetId] = superName;
			}
			m_exprBuilder->inConstructor = true;
			auto ctorBody = buildBlock(constructor->body());
			inlineModifiers(*constructor, ctorBody);
			m_exprBuilder->inConstructor = false;
			m_exprBuilder->superTargetNames.clear();
			for (auto& stmt: ctorBody->body)
				createBlock->body.push_back(std::move(stmt));
		}
		m_currentReturnType = savedReturnType;
		} // end else (no postInit needed)

		// Return true to complete the create transaction
		auto createReturn = awst::makeReturnStatement(awst::makeBoolConstant(true, method.sourceLocation), method.sourceLocation);
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
			auto blobSize = awst::makeIntegerConstant(std::to_string(blobBytes), method.sourceLocation);

			auto bzeroCall = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), method.sourceLocation);
			bzeroCall->stackArgs.push_back(std::move(blobSize));

			auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
			storeOp->immediates = {AssemblyBuilder::TRANSIENT_SLOT};
			storeOp->stackArgs.push_back(std::move(bzeroCall));

			auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation);
			body->body.push_back(std::move(exprStmt));
		}

		// Initialize EVM memory blob in scratch slot 0 BEFORE the create/dispatch
		// split so the constructor body (which can declare `T memory t;` locals
		// that emit FMP bumps reading slot 0) sees a properly initialized blob.
		// Each app call gets fresh scratch space, so we must initialize on every call.
		// store 0, bzero(4096) — pre-allocate a 4KB memory blob
		{
			auto blobSize = awst::makeIntegerConstant(std::to_string(AssemblyBuilder::SLOT_SIZE), method.sourceLocation);

			auto bzeroCall = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), method.sourceLocation);
			bzeroCall->stackArgs.push_back(std::move(blobSize));

			auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
			storeOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
			storeOp->stackArgs.push_back(std::move(bzeroCall));

			auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation);
			body->body.push_back(std::move(exprStmt));

			// Write the free memory pointer (FMP) at offset 0x40 = 0x80.
			auto loadBlob = awst::makeIntrinsicCall("load", awst::WType::bytesType(), method.sourceLocation);
			loadBlob->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};

			auto fmpOffset = std::make_shared<awst::IntegerConstant>();
			fmpOffset->sourceLocation = method.sourceLocation;
			fmpOffset->wtype = awst::WType::uint64Type();
			fmpOffset->value = "64"; // 0x40

			std::vector<uint8_t> fmpBytesVal(31, 0);
			fmpBytesVal.push_back(0x80);
			auto fmpBytes = awst::makeBytesConstant(
				std::move(fmpBytesVal), method.sourceLocation, awst::BytesEncoding::Unknown);

			auto replaceOp = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), method.sourceLocation);
			replaceOp->stackArgs.push_back(std::move(loadBlob));
			replaceOp->stackArgs.push_back(std::move(fmpOffset));
			replaceOp->stackArgs.push_back(std::move(fmpBytes));

			auto storeFmpOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
			storeFmpOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
			storeFmpOp->stackArgs.push_back(std::move(replaceOp));

			auto fmpStmt = awst::makeExpressionStatement(std::move(storeFmpOp), method.sourceLocation);
			body->body.push_back(std::move(fmpStmt));
		}

		auto ifCreate = std::make_shared<awst::IfElse>();
		ifCreate->sourceLocation = method.sourceLocation;
		ifCreate->condition = isCreate;
		ifCreate->ifBranch = createBlock;

		body->body.push_back(ifCreate);
	}

	// Transient state vars live in scratch slot TRANSIENT_SLOT (packed blob,
	// shared with asm tload/tstore). Scratch is per-txn on AVM, so the
	// scratch bzero in the preamble above already satisfies EIP-1153 per-tx
	// reset — no per-call app_global reset needed.

	// Detect fallback and receive functions across the MRO.
	// Solidity allows only one of each in the linearized hierarchy.
	solidity::frontend::FunctionDefinition const* fallbackFunc = nullptr;
	solidity::frontend::FunctionDefinition const* receiveFunc = nullptr;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* func: base->definedFunctions())
		{
			if (!func->isImplemented())
				continue;
			if (func->isFallback() && !fallbackFunc)
				fallbackFunc = func;
			else if (func->isReceive() && !receiveFunc)
				receiveFunc = func;
		}
		if (fallbackFunc && receiveFunc)
			break;
	}

	if (!fallbackFunc && !receiveFunc)
	{
		// No fallback/receive: use the normal pattern `return ARC4Router()`
		// which triggers puya's can_exit_early=True (rejects on no selector match).
		auto routerExpr = std::make_shared<awst::ARC4Router>();
		routerExpr->sourceLocation = method.sourceLocation;
		routerExpr->wtype = awst::WType::boolType();

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
			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = method.sourceLocation;
			call->wtype = awst::WType::voidType();
			call->target = awst::InstanceMethodTarget{_name};
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
					auto argBytes = awst::makeIntrinsicCall("txna", awst::WType::bytesType(), method.sourceLocation);
					argBytes->immediates = {std::string("ApplicationArgs"), 0};
					argExpr = std::move(argBytes);
				}

				awst::CallArg ca;
				ca.name = std::nullopt;
				ca.value = std::move(argExpr);
				call->args.push_back(std::move(ca));
			}

			auto stmt = awst::makeExpressionStatement(call, method.sourceLocation);
			return stmt;
		};

		auto makeTrueLit = [&]() {
			return awst::makeBoolConstant(true, method.sourceLocation);
		};

		auto makeReturnTrue = [&]() -> std::shared_ptr<awst::Statement> {
			auto r = awst::makeReturnStatement(makeTrueLit(), method.sourceLocation);
			return r;
		};

		// Step 1: Bare call check (NumAppArgs == 0).
		// Call receive/fallback and return true — no selector to match.
		{
			auto numAppArgs = awst::makeIntrinsicCall("txn", awst::WType::uint64Type(), method.sourceLocation);
			numAppArgs->immediates = {std::string("NumAppArgs")};

			auto zero = awst::makeIntegerConstant("0", method.sourceLocation);

			auto isBareCall = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Eq, std::move(zero), method.sourceLocation);

			auto bareBlock = std::make_shared<awst::Block>();
			bareBlock->sourceLocation = method.sourceLocation;
			if (receiveFunc)
				bareBlock->body.push_back(makeCall("__receive", receiveFunc, true));
			else if (fallbackFunc)
				bareBlock->body.push_back(makeCall("__fallback", fallbackFunc, true));
			bareBlock->body.push_back(makeReturnTrue());

			auto ifBare = std::make_shared<awst::IfElse>();
			ifBare->sourceLocation = method.sourceLocation;
			ifBare->condition = std::move(isBareCall);
			ifBare->ifBranch = std::move(bareBlock);
			body->body.push_back(std::move(ifBare));
		}

		// Step 2: Non-bare call — run the ARC4 router.
		// Assign result to var (triggers can_exit_early=False in puya).
		std::string matchVarName = "__did_match_routing";
		{
			auto matchVar = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto routerExpr = std::make_shared<awst::ARC4Router>();
			routerExpr->sourceLocation = method.sourceLocation;
			routerExpr->wtype = awst::WType::boolType();

			auto assignMatch = awst::makeAssignmentStatement(std::move(matchVar), std::move(routerExpr), method.sourceLocation);
			body->body.push_back(std::move(assignMatch));
		}

		// Step 3: If no match AND fallback exists, call fallback.
		if (fallbackFunc)
		{
			auto matchVarRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto notMatch = std::make_shared<awst::Not>();
			notMatch->sourceLocation = method.sourceLocation;
			notMatch->wtype = awst::WType::boolType();
			notMatch->expr = std::move(matchVarRead);

			auto dispatchBlock = std::make_shared<awst::Block>();
			dispatchBlock->sourceLocation = method.sourceLocation;
			dispatchBlock->body.push_back(makeCall("__fallback", fallbackFunc, false));

			// Set __did_match = true so the approval returns true.
			auto matchVarWrite = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto assignTrue = awst::makeAssignmentStatement(std::move(matchVarWrite), makeTrueLit(), method.sourceLocation);
			dispatchBlock->body.push_back(std::move(assignTrue));

			auto ifNoMatch = std::make_shared<awst::IfElse>();
			ifNoMatch->sourceLocation = method.sourceLocation;
			ifNoMatch->condition = std::move(notMatch);
			ifNoMatch->ifBranch = std::move(dispatchBlock);
			body->body.push_back(std::move(ifNoMatch));
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
