/// @file VariableDeclarationBuilder.cpp
/// Handles variable declaration statements.

#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder
{

bool StatementBuilder::visit(solidity::frontend::VariableDeclarationStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	auto const& declarations = _node.declarations();
	auto const* initialValue = _node.initialValue();

	if (declarations.size() == 1 && declarations[0])
	{
		auto const& decl = *declarations[0];
		auto* type = m_typeMapper.map(decl.type());

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = makeLoc(decl.location());
		target->wtype = type;
		target->name = decl.name();

		std::shared_ptr<awst::Expression> value;
		if (initialValue)
		{
			// Track function pointer assignments: `function() ptr = g;`
			// If the variable has a FunctionType and the initializer is an Identifier
			// referencing a FunctionDefinition, record it for later call resolution.
			if (dynamic_cast<solidity::frontend::FunctionType const*>(decl.type()))
			{
				if (auto const* initId = dynamic_cast<solidity::frontend::Identifier const*>(initialValue))
				{
					auto const* refDecl = initId->annotation().referencedDeclaration;
					if (auto const* funcDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(refDecl))
						m_exprBuilder.trackFuncPtrTarget(decl.id(), funcDef);
				}
			}

			value = m_exprBuilder.build(*initialValue);

			// Track compile-time constant values for `new T[](N)` resolution
			if (auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(
				initialValue->annotation().type))
			{
				auto val = ratType->literalValue(nullptr);
				if (val > 0)
					m_exprBuilder.trackConstantLocal(decl.id(), static_cast<unsigned long long>(val));
			}

			// When `new T[](N)` resolved N values, upgrade both variable type
			// and value type to fixed-size so puya allocates correct slot size.
			// Solidity memory arrays are always fixed after creation.
			if (auto* newArr = dynamic_cast<awst::NewArray*>(value.get()))
			{
				if (!newArr->values.empty()
					&& type && type->kind() == awst::WTypeKind::ReferenceArray)
				{
					auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(type);
					if (refArr && !refArr->arraySize())
					{
						int n = static_cast<int>(newArr->values.size());
						type = m_typeMapper.createType<awst::ReferenceArray>(
							refArr->elementType(), true, n);
						newArr->wtype = type;
						target->wtype = type;
					}
				}
			}

			// Insert implicit numeric cast if value type differs from target type
			value = ExpressionBuilder::implicitNumericCast(std::move(value), type, loc);
			// Coerce between bytes-compatible types (string → bytes, bytes → bytes[N], etc.)
			if (value->wtype != type && type && type->kind() == awst::WTypeKind::Bytes)
			{
				auto const* bytesType = dynamic_cast<awst::BytesWType const*>(type);
				auto const* strConst = dynamic_cast<awst::StringConstant const*>(value.get());

				// String literal → bytes[N]: create right-padded BytesConstant
				if (bytesType && bytesType->length().has_value() && *bytesType->length() > 0 && strConst)
				{
					if (auto padded = TypeCoercion::stringToBytesN(
							value.get(), type, *bytesType->length(), loc))
						value = std::move(padded);
				}
				else
				{
					bool valueIsCompatible = value->wtype == awst::WType::stringType()
						|| (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes);
					if (valueIsCompatible)
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = type;
						cast->expr = std::move(value);
						value = std::move(cast);
					}
				}
			}
		}
		else
		{
			// Default value
			if (type == awst::WType::boolType())
			{
				auto def = std::make_shared<awst::BoolConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = false;
				value = def;
			}
			else if (type == awst::WType::uint64Type())
			{
				auto def = std::make_shared<awst::IntegerConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "0";
				value = def;
			}
			else if (type == awst::WType::biguintType())
			{
				// Use BytesConstant for biguint defaults to prevent the puya
				// backend from folding biguint(0) into a uint64 constant pool
				// entry, which breaks concat/b| when biguint is used as bytes.
				auto def = std::make_shared<awst::BytesConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->encoding = awst::BytesEncoding::Base16;
				def->value = {}; // empty bytes = biguint(0)
				value = def;
			}
			else if (type && type->kind() == awst::WTypeKind::Bytes)
			{
				auto const* bytesType = dynamic_cast<awst::BytesWType const*>(type);
				if (bytesType && bytesType->length())
				{
					// Fixed-length bytes (e.g. bytes32) → zero-filled
					auto def = std::make_shared<awst::BytesConstant>();
					def->sourceLocation = loc;
					def->wtype = type;
					def->encoding = awst::BytesEncoding::Base16;
					def->value = std::vector<uint8_t>(*bytesType->length(), 0);
					value = def;
				}
				else
				{
					// Dynamic bytes → empty
					auto def = std::make_shared<awst::BytesConstant>();
					def->sourceLocation = loc;
					def->wtype = type;
					def->encoding = awst::BytesEncoding::Base16;
					def->value = {};
					value = def;
				}
			}
			else if (type == awst::WType::stringType())
			{
				auto def = std::make_shared<awst::StringConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "";
				value = def;
			}
			else if (type == awst::WType::accountType())
			{
				auto def = std::make_shared<awst::AddressConstant>();
				def->sourceLocation = loc;
				def->wtype = type;
				def->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
				value = def;
			}
			else if (type && type->kind() == awst::WTypeKind::ReferenceArray)
			{
				auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(type);
				if (refArr && refArr->arraySize())
				{
					auto def = std::make_shared<awst::NewArray>();
					def->sourceLocation = loc;
					def->wtype = type;
					for (int i = 0; i < *refArr->arraySize(); ++i)
						def->values.push_back(
							StorageMapper::makeDefaultValue(refArr->elementType(), loc));
					value = def;
				}
				else
				{
					// Dynamic array (no fixed size)
					auto def = std::make_shared<awst::NewArray>();
					def->sourceLocation = loc;
					def->wtype = type;
					value = def;
				}
			}
			else
			{
				value = StorageMapper::makeDefaultValue(type, loc);
			}
		}

		// Storage pointer alias: Type storage p = _mapping[key] or p = stateVar
		// Don't create a local variable — register as alias to the storage expression.
		// Later references to `p` will resolve to the storage read, so field writes persist.
		if (decl.referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& initialValue)
		{
			if (dynamic_cast<awst::StateGet const*>(value.get())
				|| dynamic_cast<awst::BoxValueExpression const*>(value.get())
				|| dynamic_cast<awst::AppStateExpression const*>(value.get()))
			{
				// Ensure it's wrapped in StateGet for proper read semantics
				auto aliasExpr = value;
				if (auto const* boxVal = dynamic_cast<awst::BoxValueExpression const*>(value.get()))
				{
					auto stateGet = std::make_shared<awst::StateGet>();
					stateGet->sourceLocation = loc;
					stateGet->wtype = boxVal->wtype;
					stateGet->field = value;
					stateGet->defaultValue = StorageMapper::makeDefaultValue(boxVal->wtype, loc);
					aliasExpr = stateGet;
				}
				else if (auto const* appState = dynamic_cast<awst::AppStateExpression const*>(value.get()))
				{
					// AppGlobal state: wrap in StateGet for read semantics
					auto stateGet = std::make_shared<awst::StateGet>();
					stateGet->sourceLocation = loc;
					stateGet->wtype = appState->wtype;
					stateGet->field = value;
					stateGet->defaultValue = StorageMapper::makeDefaultValue(appState->wtype, loc);
					aliasExpr = stateGet;
				}
				m_exprBuilder.addStorageAlias(decl.id(), aliasExpr);
				// Emit pre-pending + pending statements but skip the local variable assignment
				for (auto& pending: m_exprBuilder.takePrePendingStatements())
					push(std::move(pending));
				for (auto& pending: m_exprBuilder.takePendingStatements())
					push(std::move(pending));
				return false;
			}
		}

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = std::move(target);
		assign->value = std::move(value);

		// Flush pre-pending statements (e.g., biguint exponentiation loops)
		for (auto& pending: m_exprBuilder.takePrePendingStatements())
			push(std::move(pending));

		// Pick up any pending statements from the expression translator
		for (auto& pending: m_exprBuilder.takePendingStatements())
			push(std::move(pending));

		push(assign);

		// Flush any pending ArrayExtend statements from large array chunking
		for (auto& pending: m_pendingExtends)
			push(std::move(pending));
		m_pendingExtends.clear();
	}
	else if (declarations.size() > 1 && initialValue)
	{
		// Tuple destructuring: (type1 var1, type2 var2) = expr
		auto rhsExpr = m_exprBuilder.build(*initialValue);

		// Flush pre-pending statements
		for (auto& pending: m_exprBuilder.takePrePendingStatements())
			push(std::move(pending));

		// Pick up any pending statements (e.g., inner transaction submits)
		for (auto& pending: m_exprBuilder.takePendingStatements())
			push(std::move(pending));

		// Wrap in SingleEvaluation to ensure the RHS is evaluated only once.
		// Without this, each TupleItemExpression would independently evaluate
		// the base expression, causing side effects (like inner txn submits) to repeat.
		auto singleRhs = std::make_shared<awst::SingleEvaluation>();
		singleRhs->sourceLocation = loc;
		singleRhs->wtype = rhsExpr->wtype;
		singleRhs->source = std::move(rhsExpr);
		singleRhs->id = static_cast<int>(_node.id());
		rhsExpr = std::move(singleRhs);

		// Assign each declared variable from the tuple
		for (size_t i = 0; i < declarations.size(); ++i)
		{
			if (!declarations[i])
				continue; // Skip anonymous placeholders (e.g., (bool x, ) = ...)

			auto const& decl = *declarations[i];
			auto* type = m_typeMapper.map(decl.type());

			auto target = std::make_shared<awst::VarExpression>();
			target->sourceLocation = makeLoc(decl.location());
			target->wtype = type;
			target->name = decl.name();

			auto itemExpr = std::make_shared<awst::TupleItemExpression>();
			itemExpr->sourceLocation = loc;
			itemExpr->wtype = type;
			itemExpr->base = rhsExpr;
			itemExpr->index = static_cast<int>(i);

			auto assign = std::make_shared<awst::AssignmentStatement>();
			assign->sourceLocation = loc;
			assign->target = std::move(target);
			assign->value = std::move(itemExpr);
			push(assign);
		}
	}

	return false;
}


} // namespace puyasol::builder
