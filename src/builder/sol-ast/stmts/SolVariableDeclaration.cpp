/// @file SolVariableDeclaration.cpp
/// Migrated from VariableDeclarationBuilder.cpp.

#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "builder/AWSTBuilder.h" // containsMappingType
#include "builder/sol-eb/ContractContext.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/assembly/AssemblyBuilder.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;


SolVariableDeclaration::SolVariableDeclaration(
	BlockContext& _blk,
	VariableDeclarationStatement const& _node,
	awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolVariableDeclaration::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	auto const& declarations = m_node.declarations();
	auto const* initialValue = m_node.initialValue();

	if (declarations.size() == 1 && declarations[0])
	{
		auto const& decl = *declarations[0];
		auto* type = m_blk.typeMapper().map(decl.type());

		auto target = awst::makeVarExpression(m_blk.resolveVarName(decl.name(), decl.id()), type, m_blk.makeLoc(decl.location()));

		std::shared_ptr<awst::Expression> value;
		if (initialValue)
		{
			// Track function pointer assignments. solc's
			// ASTNode::referencedDeclaration handles both
			// `function() x = f;` (Identifier) and `function() x = Lib.f;` /
			// `function() x = super.f;` (MemberAccess). The MemberAccess
			// case is safe even for super.f now that SolInternalCall's
			// Identifier-form fn-ptr dispatch consults findSuperTarget
			// before settling on a plain InstanceMethodTarget — see the
			// matching block in SolInternalCall::processFromIdent.
			if (dynamic_cast<FunctionType const*>(decl.type()))
			{
				if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(
						ASTNode::referencedDeclaration(*initialValue)))
					m_blk.setFuncPtrTarget(decl.id(), funcDef);
			}

			value = m_blk.builderCtx().build(*initialValue);

			// Track constant locals (only if value fits in unsigned long long)
			if (auto const* ratType = dynamic_cast<RationalNumberType const*>(
					initialValue->annotation().type))
			{
				auto val = ratType->literalValue(nullptr);
				if (val > 0 && val <= std::numeric_limits<unsigned long long>::max())
					m_blk.setConstantLocal(decl.id(), static_cast<unsigned long long>(val));
			}

			// Upgrade dynamic array to fixed-size when N is known
			if (auto* newArr = dynamic_cast<awst::NewArray*>(value.get()))
			{
				if (!newArr->values.empty())
				{
					if (type && type->kind() == awst::WTypeKind::ReferenceArray)
					{
						auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(type);
						if (refArr && !refArr->arraySize())
						{
							int n = static_cast<int>(newArr->values.size());
							type = m_blk.typeMapper().createType<awst::ReferenceArray>(
								refArr->elementType(), true, n);
							newArr->wtype = type;
							target->wtype = type;
						}
					}
					// Note: don't upgrade ARC4DynamicArray→ARC4StaticArray here.
					// Subsequent references to the variable use TypeMapper which
					// returns ARC4DynamicArray, causing type mismatches.
				}
			}

			value = builder::TypeCoercion::coerceForAssignment(std::move(value), type, m_loc);
		}
		else
			value = StorageMapper::makeDefaultValue(type, m_loc);

		// Storage pointer alias
		if (decl.referenceLocation() == VariableDeclaration::Location::Storage && initialValue)
		{
			// Mapping state-var Identifier resolves to BytesConstant (the
			// holder name) — register as storage alias so SolIndexAccess
			// resolves `m[k]` to the underlying state-var prefix at compile
			// time. This is the `mapping storage m = m1;` pattern.
			if (dynamic_cast<awst::BytesConstant const*>(value.get())
				&& decl.type()
				&& decl.type()->category() == solidity::frontend::Type::Category::Mapping)
			{
				m_blk.setStorageAlias(decl.id(), StorageAlias::mappingHolder(value));
				m_blk.builderCtx().appendPendingTo(result);
				return result;
			}

			if (dynamic_cast<awst::StateGet const*>(value.get())
				|| awst::isRawStorageRead(value.get()))
			{
				// Raw box/app-state reads must be wrapped in StateGet with
				// a default so the alias evaluates the same as a direct read.
				// Already-wrapped StateGet passes through unchanged.
				auto aliasExpr = awst::isRawStorageRead(value.get())
					? StorageMapper::makeStateGetWithDefault(value, value->wtype, m_loc)
					: value;
				m_blk.setStorageAlias(decl.id(), StorageAlias::stateRead(std::move(aliasExpr)));
				m_blk.builderCtx().appendPendingTo(result);
				return result;
			}

			// Indexed / field path into a state container:
			//   `T storage b = a[i];` or `T storage b = a.field;`
			// Register the IndexExpression / FieldExpression as the alias so
			// subsequent operations through `b` (push, pop, indexed write)
			// route through the underlying state container's read-modify-write
			// codegen.
			if (dynamic_cast<awst::IndexExpression const*>(value.get()))
			{
				m_blk.setStorageAlias(decl.id(), StorageAlias::indexedPath(value));
				m_blk.builderCtx().appendPendingTo(result);
				return result;
			}
			if (dynamic_cast<awst::FieldExpression const*>(value.get()))
			{
				m_blk.setStorageAlias(decl.id(), StorageAlias::fieldPath(value));
				m_blk.builderCtx().appendPendingTo(result);
				return result;
			}

			// Slot-based storage reference: initialized from internal function call
			// that returns a storage reference (typically has .slot := in assembly).
			// Register as slot ref so IndexAccess translates to sload/sstore.
			if (dynamic_cast<awst::SubroutineCallExpression const*>(value.get()))
			{
				// Distinguish two patterns:
				// 1. `mapping(K=>V) storage m = libOrInternal()` — value is bytes
				//    (mapping holder name). Register as mappingKeyParam so
				//    SolIndexAccess builds box keys with `m` as runtime prefix.
				// 2. `T storage m = ...` with a slot-int (biguint) return —
				//    register as slotStorageRef for __storage_read/write paths.
				// A `mapping(K=>V) storage` ref, OR a struct-storage ref whose
				// struct carries nested mappings (e.g. `Pool.State storage pool =
				// _getPool(id)`). Both travel as a bytes box-key prefix; bind the
				// local as a mappingKeyParam so `pool.field` / `pool.map[k]`
				// resolve against that prefix (see SolIdentifier struct-ref read).
				bool isMappingPtr = decl.type()
					&& (decl.type()->category() == solidity::frontend::Type::Category::Mapping
						|| builder::containsMappingType(decl.type()));
				if (isMappingPtr && value->wtype == awst::WType::bytesType())
				{
					m_blk.setMappingKeyParam(decl.id(), decl.name());
					// Emit `m = f()` as a plain bytes assignment so `m` holds the
					// mapping holder name at runtime; subsequent reassignments
					// (`m = otherMapping`) update which mapping `m` points to.
					auto var = awst::makeVarExpression(decl.name(), awst::WType::bytesType(), m_loc);
					auto assign = awst::makeAssignmentStatement(std::move(var), std::move(value), m_loc);
					result.push_back(std::move(assign));

					m_blk.builderCtx().appendPendingTo(result);
					return result;
				}

				m_blk.setSlotStorageRef(decl.id(), value);
				// Also emit the call as an assignment so the slot value is available.
				// The slot var's wtype must match the function's return wtype to
				// keep AssignmentStatement happy.
				auto* slotWType = value->wtype ? value->wtype : awst::WType::biguintType();
				auto slotVar = awst::makeVarExpression(decl.name(), slotWType, m_loc);

				auto assign = awst::makeAssignmentStatement(std::move(slotVar), std::move(value), m_loc);
				result.push_back(std::move(assign));

				m_blk.builderCtx().appendPendingTo(result);
				return result;
			}
		}

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), m_loc);

		m_blk.builderCtx().appendPendingTo(result);

		// EVM free-memory-pointer simulation: `T memory t;` (no initializer)
		// allocates fresh memory and bumps mload(0x40) by sizeof(T). We mirror
		// this so contracts that read mload(0x40) see the expected advance.
		// Memory locals with initializers are pointer copies in EVM (no alloc).
		if (!initialValue
			&& decl.referenceLocation() == VariableDeclaration::Location::Memory)
		{
			int sz = builder::computeEncodedElementSize(type);
			if (sz > 0)
				for (auto& s: builder::AssemblyBuilder::emitFreeMemoryBump(
						sz, m_loc, static_cast<int>(decl.id())))
					result.push_back(std::move(s));
		}

		result.push_back(assign);
	}
	else if (declarations.size() > 1 && initialValue)
	{
		// Tuple destructuring `(a, b) = expr;` — bind each component to a
		// separate local. The RHS must be evaluated EXACTLY ONCE, otherwise
		// a tuple-returning function call gets re-emitted per destructured
		// component (manifesting as duplicate `callsub`s in the generated
		// TEAL with all the call's side-effects repeated). `SingleEvaluation`
		// is the in-memory hint for that, but the AWST JSON serialization
		// inlines its `source` per consumer and the puya backend then
		// re-emits the call for each `TupleItemExpression`. We spell out
		// the temp explicitly: assign the RHS to a synthetic variable, then
		// extract each tuple item from that variable by name.
		// (Port of polymarket-experiment commit `271d85851`.)
		auto rhsExpr = m_blk.builderCtx().build(*initialValue);
		m_blk.builderCtx().appendPendingTo(result);

		auto const* tupleType = rhsExpr->wtype;
		std::string tempName = "__tuple_destruct_" + std::to_string(m_node.id());
		auto tempTarget = awst::makeVarExpression(tempName, tupleType, m_loc);
		auto tempAssign = awst::makeAssignmentStatement(
			std::move(tempTarget), std::move(rhsExpr), m_loc);
		result.push_back(std::move(tempAssign));

		for (size_t i = 0; i < declarations.size(); ++i)
		{
			if (!declarations[i]) continue;
			auto const& decl = *declarations[i];
			auto* type = m_blk.typeMapper().map(decl.type());

			auto target = awst::makeVarExpression(decl.name(), type, m_blk.makeLoc(decl.location()));

			auto baseRef = awst::makeVarExpression(tempName, tupleType, m_loc);
			auto itemExpr = awst::makeTupleItem(std::move(baseRef), static_cast<int>(i), type, m_loc);

			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(itemExpr), m_loc);
			result.push_back(assign);
		}
	}

	return result;
}

} // namespace puyasol::builder::sol_ast
