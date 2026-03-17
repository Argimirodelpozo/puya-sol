/// @file ReturnBuilder.cpp
/// Handles return statement translation.

#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

bool StatementBuilder::visit(solidity::frontend::Return const& _node)
{
	auto loc = makeLoc(_node.location());
	auto stmt = std::make_shared<awst::ReturnStatement>();
	stmt->sourceLocation = loc;

	if (_node.expression())
	{
		stmt->value = m_exprBuilder.build(*_node.expression());

		// Insert implicit numeric cast to match function return type
		auto const& retAnnotation = dynamic_cast<solidity::frontend::ReturnAnnotation const&>(
			_node.annotation()
		);
		if (retAnnotation.functionReturnParameters)
		{
			auto const& retParams = retAnnotation.functionReturnParameters->parameters();
			if (retParams.size() == 1)
			{
				auto* expectedType = m_typeMapper.map(retParams[0]->type());
				stmt->value = ExpressionBuilder::implicitNumericCast(
					std::move(stmt->value), expectedType, loc
				);

				// Coerce IntegerConstant → BytesConstant for bytes[N] return types
				if (expectedType && expectedType->kind() == awst::WTypeKind::Bytes
					&& stmt->value->wtype != expectedType)
				{
					auto const* bytesType = dynamic_cast<awst::BytesWType const*>(expectedType);
					auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(stmt->value.get());
					if (bytesType && intConst && bytesType->length().value_or(0) > 0)
					{
						// Convert integer to fixed-size bytes
						int numBytes = bytesType->length().value();
						uint64_t val = std::stoull(intConst->value);
						std::vector<unsigned char> bytes(numBytes, 0);
						for (int i = numBytes - 1; i >= 0 && val > 0; --i)
						{
							bytes[i] = static_cast<unsigned char>(val & 0xFF);
							val >>= 8;
						}
						auto bc = std::make_shared<awst::BytesConstant>();
						bc->sourceLocation = loc;
						bc->wtype = expectedType;
						bc->encoding = awst::BytesEncoding::Base16;
						bc->value = bytes;
						stmt->value = std::move(bc);
					}
					else if (stmt->value->wtype && stmt->value->wtype->kind() == awst::WTypeKind::Bytes)
					{
						// bytes → bytes[N] reinterpret cast
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = expectedType;
						cast->expr = std::move(stmt->value);
						stmt->value = std::move(cast);
					}
				}
			}
			else if (retParams.size() > 1)
			{
				// Multi-value return: coerce each element of the tuple
				auto* tupleExpr = dynamic_cast<awst::TupleExpression*>(stmt->value.get());
				if (tupleExpr && tupleExpr->items.size() == retParams.size())
				{
					std::vector<awst::WType const*> expectedTypes;
					for (size_t i = 0; i < retParams.size(); ++i)
					{
						auto* expectedElemType = m_typeMapper.map(retParams[i]->type());
						tupleExpr->items[i] = ExpressionBuilder::implicitNumericCast(
							std::move(tupleExpr->items[i]), expectedElemType, loc
						);
						expectedTypes.push_back(tupleExpr->items[i]->wtype);
					}
					// Update the tuple's WType to match coerced element types
					tupleExpr->wtype = new awst::WTuple(expectedTypes);
				}
			}
		}
	}

	// Flush pre-pending statements (e.g., biguint exponentiation loops)
	for (auto& pending: m_exprBuilder.takePrePendingStatements())
		push(std::move(pending));

	// Pick up any pending statements from the expression translator
	// (e.g., inner transaction submits that must execute before return)
	for (auto& pending: m_exprBuilder.takePendingStatements())
		push(std::move(pending));

	push(stmt);
	return false;
}


} // namespace puyasol::builder
