/// @file SolArrayMethodHandlers.cpp
/// Box-backed storage array mutations, including length-only mapping arrays.

#include "builder/ast/calls/SolArrayMethod.h"
#include "awst/NameGen.h"
#include "builder/storage/StorageMapper.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;


std::shared_ptr<awst::Expression> SolArrayMethod::handleBoxArray(
	std::string const& _memberName,
	VariableDeclaration const& _varDecl,
	std::shared_ptr<awst::Expression> _runtimeKey)
{
	auto const* solArrType = dynamic_cast<ArrayType const*>(_varDecl.type());
	auto* arrWType = m_ctx.typeMapper.map(solArrType);

	// Physical binding, not the raw source name — matches every other
	// box-key derivation for this declaration (colliding names diverge).
	std::string arrayVarName = _runtimeKey ? std::string{}
		: m_ctx.storageMapper.physicalBindingFor(_varDecl).key;

	// `mapping(K=>V)[] a`: no element bytes inline; array box is just a
	// 2-byte length header. `a[i][k]` boxes are derived from `a`+`i`+sha256(k)
	// (SolIndexAccess). Gives EVM's "delete leaves data at hash" for free.
	bool elemIsMapping = dynamic_cast<MappingType const*>(solArrType->baseType()) != nullptr;
	if (elemIsMapping && (_memberName == "push" || _memberName == "pop"))
		return handleMappingElementArrayLengthOp(
			_memberName, _varDecl, arrayVarName, std::move(_runtimeKey));

	// Build BoxValueExpression
	auto boxExpr = _runtimeKey
		? awst::makeBoxValueExpression(std::move(_runtimeKey), arrWType, m_loc)
		: builder::StorageMapper::makeTopLevelBoxExpr(arrayVarName, arrWType, m_loc);

	return emitArrayPushPop(_memberName, std::move(boxExpr), *solArrType);
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleMappingElementArrayLengthOp(
	std::string const& _memberName,
	solidity::frontend::VariableDeclaration const& /*_varDecl*/,
	std::string const& _arrayVarName,
	std::shared_ptr<awst::Expression> _runtimeKey)
{
	// Box: 2-byte big-endian length, no element data.
	// Read the shared ARC4 header; missing boxes have length zero.
	// Write: itob(new_len) → extract last 2 bytes → box_put.

	auto boxKey = _runtimeKey ? std::move(_runtimeKey)
		: awst::makeUtf8BytesConstant(
			_arrayVarName, m_loc, awst::WType::boxKeyType());

	auto cur = StorageMapper::makeBoxArrayLength(m_ctx.typeMapper, boxKey, m_loc);

	// new_len = cur ± 1
	auto delta = awst::makeOne(m_loc);
	auto newLen = awst::makeUInt64BinOp(
		std::move(cur),
		_memberName == "push" ? awst::UInt64BinaryOperator::Add
							  : awst::UInt64BinaryOperator::Sub,
		std::move(delta), m_loc);

	auto extract = awst::makeUInt16Bytes(std::move(newLen), m_loc);
	// box_put(arrayVarName, len_bytes)
	m_ctx.queuePostExpression(awst::makeBoxPut(boxKey, std::move(extract), m_loc), m_loc);
	return awst::makeVoidConstant(m_loc);
}

} // namespace puyasol::builder::sol_ast
