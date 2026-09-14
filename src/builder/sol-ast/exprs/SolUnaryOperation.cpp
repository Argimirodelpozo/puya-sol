/// @file SolUnaryOperation.cpp — unary operation translation.

#include "builder/sol-types/SolcConstFold.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-ast/ResolvedLValue.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/itxn/CallResolver.h"

#include <libsolidity/ast/AST.h>
#include <stdexcept>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolUnaryOperation::SolUnaryOperation(
	eb::ContractContext& _ctx, UnaryOperation const& _node)
	: SolExpression(_ctx, _node), m_unaryOp(_node)
{
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleIncDec(
	std::shared_ptr<awst::Expression> _operand, ResolvedLValue::Resolution _resolution)
{
	ResolvedLValue target(m_ctx, m_unaryOp.subExpression(), m_loc, std::move(_resolution), std::move(_operand));
	auto old = m_ctx.emitSequencedOperand({}, target.read(), true, m_loc);
	auto const integer = SolIntType::fromSol(m_unaryOp.subExpression().annotation().type);
	assert(integer);
	auto value = eb::buildIncDec(m_ctx, m_scope.isUnchecked(),
		m_unaryOp.getOperator() == Token::Inc,
		integer->isSigned ? integer->bits : 0,
		integer->isSigned ? 0 : integer->bits, old, m_loc);
	auto assigned = target.write(std::move(value));
	return m_unaryOp.isPrefixOperation() ? assigned : old;
}

bool SolUnaryOperation::clearMultiBoxElement(
	VariableDeclaration const& _var,
	awst::WType const* _arrWtype,
	std::shared_ptr<awst::Expression> const& _index)
{
	using ::puyasol::builder::StorageMapper;

	unsigned const elemSize = StorageMapper::arc4StaticArrayElementSize(_arrWtype);
	unsigned const elemsPerBox = StorageMapper::elementsPerBox(_arrWtype);
	if (elemSize == 0 || elemsPerBox == 0)
		return false;
	auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_arrWtype);
	if (!sa)
		return false;

	auto page = StorageMapper::arrayPageForIndex(
		m_ctx.storageMapper.physicalBindingFor(_var).key,
		_arrWtype, _index, m_ctx.preEffects(), m_loc);

	// The cleared element is its ARC-4 default, which for a fixed-width element
	// is elemSize zero bytes — not necessarily all-zero for shapes carrying head
	// offsets, so take the encoding rather than assuming.
	auto encoded = builder::arc4DefaultEncoding(sa->elementType());
	if (!encoded || encoded->size() != elemSize)
		throw SizeError("multi-box array element has no supported default encoding");

	auto replace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), m_loc);
	replace->stackArgs.push_back(page.key);
	replace->stackArgs.push_back(page.offset);
	replace->stackArgs.push_back(awst::makeBytesConstant(std::move(*encoded), m_loc));
	auto exists = awst::makeTupleItem(StorageMapper::makeBoxLenTuple(
		m_ctx.typeMapper, page.key, m_loc), 1, awst::WType::boolType(), m_loc);
	auto body = awst::makeBlock(m_loc);
	body->body.push_back(awst::makeExpressionStatement(std::move(replace), m_loc));
	m_ctx.postEffects().push_back(awst::makeIfElse(std::move(exists), std::move(body), nullptr, m_loc));
	return true;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleDelete(
	std::shared_ptr<awst::Expression> _operand, ResolvedLValue::Resolution _resolution)
{
	if (!_operand)
	{
		ResolvedLValue(m_ctx, m_unaryOp.subExpression(), m_loc).clear();
		return awst::makeVoidConstant(m_loc);
	}
	// Paged arrays retain their page-lifecycle operation; ordinary targets share clear().
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (varDecl->isStateVariable() && !varDecl->isConstant() && !varDecl->immutable()
				&& !m_ctx.typeMapper.profile().evmStorageLayout)
			{
				auto const* type = m_ctx.typeMapper.map(varDecl->type());
				if (StorageMapper::isMultiBoxArray(type))
				{
					auto count = StorageMapper::numBoxesForArray(type);
					if (count > 4096) throw SizeError("multi-box delete exceeds the 4096-page capacity");
					auto name = m_ctx.storageMapper.physicalBindingFor(*varDecl).key;
					if (count > 4)
					{
						auto index = awst::makeVarExpression("__delete_page_" + std::to_string(
							awst::NameGen::next("SolUnaryOperation.deletePages")), awst::WType::uint64Type(), m_loc);
						auto block = awst::makeBlock(m_loc), body = awst::makeBlock(m_loc);
						block->body.push_back(awst::makeAssignmentStatement(index, awst::makeZero(m_loc), m_loc));
						auto key = awst::makeConcat(awst::makeUtf8BytesConstant(name, m_loc), awst::makeItob(index, m_loc), m_loc);
						key->wtype = awst::WType::boxKeyType();
						body->body.push_back(awst::makeExpressionStatement(awst::makeStateDelete(
							awst::makeBoxValueExpression(std::move(key), awst::WType::bytesType(), m_loc), m_loc), m_loc));
						body->body.push_back(awst::makeAssignmentStatement(index,
							awst::makeUInt64BinOp(index, awst::UInt64BinaryOperator::Add, awst::makeOne(m_loc), m_loc), m_loc));
						block->body.push_back(awst::makeWhileLoop(awst::makeNumericCompare(index,
							awst::NumericComparison::Lt, awst::makeIntegerConstant(count, m_loc), m_loc), std::move(body), m_loc));
						m_ctx.postEffects().push_back(std::move(block));
						return _operand;
					}
					for (unsigned page = 0; page < count; ++page)
					{
						auto key = awst::makeConcat(awst::makeUtf8BytesConstant(name, m_loc),
							awst::makeItob(awst::makeIntegerConstant(page, m_loc), m_loc), m_loc);
						key->wtype = awst::WType::boxKeyType();
						m_ctx.queuePostExpression(awst::makeStateDelete(
							awst::makeBoxValueExpression(std::move(key), awst::WType::bytesType(), m_loc), m_loc), m_loc);
					}
					return _operand;
				}
			}
		}
	}

	// `delete m[i]` on a MULTI-BOX array: the element lives at a page/offset, so
	// there is no single lvalue to assign a default to and puya rejected the
	// plain IndexExpression with "unsupported assignment target". Zero the
	// element's slice in place instead. Reached once struct elements became
	// eligible for multi-box paging; before that such arrays fell back to a
	// single box, whose delete the generic path below already handles.
	//
	// The index comes off the ALREADY-BUILT operand, never from rebuilding the
	// solc node: `delete m[f()]` would otherwise call f() twice.
	if (auto const* index = dynamic_cast<IndexAccess const*>(&m_unaryOp.subExpression()))
		if (auto const* ident = dynamic_cast<Identifier const*>(&index->baseExpression()))
			if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
					ident->annotation().referencedDeclaration);
				varDecl && varDecl->isStateVariable()
				&& !varDecl->isConstant() && !varDecl->immutable())
			{
				auto const* arrWtype = m_ctx.typeMapper.map(varDecl->type());
				auto indexed = _operand;
				if (auto const* decode = dynamic_cast<awst::ARC4Decode const*>(indexed.get()))
					indexed = decode->value;
				auto const* builtIndex = dynamic_cast<awst::IndexExpression const*>(indexed.get());
				if (builder::StorageMapper::isMultiBoxArray(arrWtype) && builtIndex
					&& clearMultiBoxElement(*varDecl, arrWtype, builtIndex->index))
					return _operand;
			}

	ResolvedLValue(m_ctx, m_unaryOp.subExpression(), m_loc, std::move(_resolution), std::move(_operand)).clear();
	return awst::makeVoidConstant(m_loc);
}

std::shared_ptr<awst::Expression> SolUnaryOperation::toAwst()
{
	if (auto const* function = *m_unaryOp.annotation().userDefinedFunction)
		return eb::CallResolver::buildOperatorCall(m_ctx, *function,
			{&m_unaryOp.subExpression()}, m_loc);

	// The canonical constant path (fable-review item 1): solc folded the WHOLE
	// expression (non-fractional rational annotation, e.g. `-2`, `~5`) → emit
	// its value directly; never fold built AWST downstream. Runtime-typed
	// expressions (incl. AWST-constant operands like a lowered type(intN).min)
	// take the full checked paths below.
	if (auto folded = builder::SolcConstFold::foldAnnotated(m_unaryOp, m_ctx.typeMapper, m_loc))
		return folded;
	// intN-typed constant expression (e.g. `-M` over a constant variable) —
	// foldTyped's in-range guard keeps `-intN.min` on the checked path.
	if (auto folded = builder::SolcConstFold::foldTyped(m_unaryOp, m_loc))
		return folded;

	auto const op = m_unaryOp.getOperator();
	bool const modifies = op == Token::Inc || op == Token::Dec || op == Token::Delete;
	// Addressed destinations are resolved by the shared lvalue, not first read
	// as expressions and then resolved again for the write.
	auto resolution = modifies ? ResolvedLValue::classify(m_ctx, m_unaryOp.subExpression()) : ResolvedLValue::Resolution{};
	auto operand = resolution.isAddressed()
		? nullptr : buildExpr(m_unaryOp.subExpression());
	// Try sol-eb builder dispatch for Not/Sub/BitNot
	{
		eb::BuilderUnaryOp builderOp;
		bool hasUnaryOp = true;
		switch (m_unaryOp.getOperator())
		{
		case Token::Not: builderOp = eb::BuilderUnaryOp::LogicalNot; break;
		case Token::Sub: builderOp = eb::BuilderUnaryOp::Negative; break;
		case Token::BitNot: builderOp = eb::BuilderUnaryOp::BitInvert; break;
		default: hasUnaryOp = false; break;
		}
		if (hasUnaryOp)
		{
			// Checked negate references the operand in the overflow assert AND
			// the negation (verified: -g() ran g 3×); biguint ~ references it
			// in len + extract. Pin once — the same wrapped node also feeds the
			// fixed-bytes complement fallback below. Inc/Dec/
			// Delete never enter this block (their operand must stay an
			// lvalue-shaped tree).
			operand = awst::makeEvalOnce(std::move(operand), m_loc);
			auto* solType = m_unaryOp.subExpression().annotation().type;
			auto builder = m_ctx.builderForInstance(solType, operand);
			if (builder)
			{
				auto result = builder->unary_op(builderOp, m_loc);
				if (result)
					return result->resolve();
			}
		}
	}

	switch (m_unaryOp.getOperator())
	{
	case Token::BitNot:
		// Integer/bool operations are owned by their registered sol-eb builders.
		// Fixed bytes have a bytewise complement, without integer width conversion.
		assert(dynamic_cast<FixedBytesType const*>(
			m_unaryOp.subExpression().annotation().type));
		return awst::makeBitInvert(operand, operand->wtype, m_loc);
	case Token::Not:
	case Token::Sub:
		throw std::logic_error("Missing sol-eb unary operation for solc-checked type");
	case Token::Inc:
	case Token::Dec:    return handleIncDec(std::move(operand), std::move(resolution));
	case Token::Delete: return handleDelete(std::move(operand), std::move(resolution));
	default:            return operand;
	}
}

} // namespace puyasol::builder::sol_ast
