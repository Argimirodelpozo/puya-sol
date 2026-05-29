#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <boost/multiprecision/cpp_int.hpp>

#include <functional>
#include <string>

namespace puyasol::builder
{

void rewriteARC4Returns(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& m_typeMapper,
	std::vector<SignedReturnInfo> const& signedReturns,
	std::vector<UnsignedMaskInfo> const& unsignedMasks,
	bool funcHasInlineAssembly)
{
	auto const& returnParams = _func.returnParameters();

	// For ARC4 methods returning dynamic arrays, convert the return type
	// to ARC4 encoding and wrap return values in ARC4Encode.
	if (method.arc4MethodConfig.has_value()
		&& method.returnType->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* arc4RetType = m_typeMapper.mapToARC4Type(method.returnType);
		if (arc4RetType != method.returnType)
		{
			// Wrap all return values in ARC4Encode
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrapReturns;
			wrapReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (auto& stmt: stmts)
				{
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (ret->value)
						{
							auto encode = awst::makeARC4Encode(std::move(ret->value), arc4RetType, ret->value->sourceLocation);
							ret->value = std::move(encode);
						}
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
					{
						if (ifElse->ifBranch)
							wrapReturns(ifElse->ifBranch->body);
						if (ifElse->elseBranch)
							wrapReturns(ifElse->elseBranch->body);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
					{
						wrapReturns(block->body);
					}
				}
			};
			wrapReturns(method.body->body);
			method.returnType = arc4RetType;
		}
	}

	// EVM inline assembly is UNCHECKED: every Yul opcode (add/mul/exp/...)
	// wraps mod 2^256, so an assembly-produced value can never exceed its
	// declared width in EVM. AVM biguint does NOT wrap, so an assembly
	// computation can grow past 2^N here. To match EVM semantics we wrap
	// (val % 2^N) before ARC4Encode for assembly-bodied functions — this is
	// the deferred equivalent of EVM's per-opcode wrapping, NOT an overflow
	// being swallowed. For non-assembly (checked) functions we leave the bare
	// ARC4Encode, whose `len <= N/8` assert correctly REVERTS on a genuine
	// overflow (e.g. checked add/mul/exp that a test expects to trap).
	auto pow2Str = [](unsigned bits) -> std::string {
		boost::multiprecision::cpp_int v = 1;
		v <<= bits;
		return v.str();
	};
	auto encodeRet = [&](std::shared_ptr<awst::Expression> val, unsigned bits,
		awst::WType const* arc4Ty, awst::SourceLocation const& loc)
		-> std::shared_ptr<awst::Expression>
	{
		if (funcHasInlineAssembly)
		{
			auto mod = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod,
				awst::makeIntegerConstant(pow2Str(bits), loc, awst::WType::biguintType()), loc);
			return awst::makeARC4Encode(std::move(mod), arc4Ty, loc);
		}
		return awst::makeARC4Encode(std::move(val), arc4Ty, loc);
	};

	// For ARC4 methods returning biguint, wrap return values in ARC4Encode
	// with the correct bit width (e.g., uint256 not uint512).
	// Skip signed returns and functions with modifiers. Inline-assembly bodies
	// are handled too (their returns are wrapped mod 2^N via encodeRet so the
	// ABI exposes arc4.uintN, matching cross-contract callers' uint256 selectors).
	if (method.arc4MethodConfig.has_value() && method.returnType == awst::WType::biguintType()
		&& signedReturns.empty() && _func.modifiers().empty())
	{
		// Get original Solidity bit width for the return type
		unsigned retBits = 256;
		if (returnParams.size() == 1)
		{
			auto const* retSolType = returnParams[0]->type();
			if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
				retSolType = &udvt->underlyingType();
			if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType))
				retBits = intType->numBits();
			else if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retSolType))
				if (auto const* encType = dynamic_cast<solidity::frontend::IntegerType const*>(
					enumType->encodingType()))
					retBits = encType->numBits();
		}
		auto const* arc4RetType = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(retBits));

		std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrapBiguintReturns;
		wrapBiguintReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
		{
			for (auto& stmt: stmts)
			{
				if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
				{
					if (ret->value && ret->value->wtype == awst::WType::biguintType())
					{
						auto loc = ret->value->sourceLocation;
						ret->value = encodeRet(std::move(ret->value), retBits, arc4RetType, loc);
					}
				}
				else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
				{
					if (ifElse->ifBranch) wrapBiguintReturns(ifElse->ifBranch->body);
					if (ifElse->elseBranch) wrapBiguintReturns(ifElse->elseBranch->body);
				}
				else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
					wrapBiguintReturns(block->body);
				else if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
					if (loop->loopBody) wrapBiguintReturns(loop->loopBody->body);
			}
		};
		wrapBiguintReturns(method.body->body);
		method.returnType = arc4RetType;
	}

	// For ARC4 methods returning tuples with biguint elements,
	// wrap each biguint element in ARC4Encode with correct bit width.
	// Inline-assembly bodies handled too (encodeRet wraps mod 2^N per element).
	if (method.arc4MethodConfig.has_value() && method.returnType
		&& method.returnType->kind() == awst::WTypeKind::WTuple
		&& signedReturns.empty() && _func.modifiers().empty())
	{
		auto const* tupleType = static_cast<awst::WTuple const*>(method.returnType);
		// Only wrap when ALL elements are biguint or uint64/bool (simple scalars).
		// Mixed tuples with arrays/structs/strings need different handling.
		bool allScalar = true;
		bool hasBiguintElement = false;
		for (auto const* t : tupleType->types())
		{
			if (t == awst::WType::biguintType())
				hasBiguintElement = true;
			else if (t != awst::WType::uint64Type() && t != awst::WType::boolType())
				allScalar = false;
		}

		if (hasBiguintElement && allScalar)
		{
			// Build ARC4 type for each element
			std::vector<awst::WType const*> arc4Types;
			for (size_t ri = 0; ri < returnParams.size() && ri < tupleType->types().size(); ++ri)
			{
				auto const* elemType = tupleType->types()[ri];
				if (elemType == awst::WType::biguintType())
				{
					auto const* retSolType = returnParams[ri]->type();
					if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
						retSolType = &udvt->underlyingType();
					unsigned bits = 256;
					if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType))
						bits = intType->numBits();
					arc4Types.push_back(m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits)));
				}
				else
					arc4Types.push_back(elemType);
			}

			// Helper: wrap biguint items inside a single TupleExpression with
			// ARC4Encode, and update the tuple's wtype to the ARC4 tuple type.
			auto wrapTupleItems = [&](awst::TupleExpression* tuple)
			{
				if (!tuple) return;
				for (size_t i = 0; i < tuple->items.size() && i < arc4Types.size(); ++i)
				{
					if (tuple->items[i]->wtype == awst::WType::biguintType()
						&& arc4Types[i]->kind() == awst::WTypeKind::ARC4UIntN)
					{
						unsigned bits = static_cast<unsigned>(
							static_cast<awst::ARC4UIntN const*>(arc4Types[i])->n());
						auto loc = tuple->items[i]->sourceLocation;
						tuple->items[i] = encodeRet(std::move(tuple->items[i]), bits, arc4Types[i], loc);
					}
				}
				tuple->wtype = new awst::WTuple(
					std::vector<awst::WType const*>(arc4Types));
			};

			// Walk the body and wrap biguint tuple elements in ARC4Encode.
			// Handles direct tuple returns and conditional expressions whose
			// branches are tuple literals.
			static int retTmpCounter = 0;
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrapTupleReturns;
			wrapTupleReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (size_t si = 0; si < stmts.size(); ++si)
				{
					auto& stmt = stmts[si];
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (!ret->value) continue;
						if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
							wrapTupleItems(tuple);
						else if (auto* cond = dynamic_cast<awst::ConditionalExpression*>(ret->value.get()))
						{
							wrapTupleItems(dynamic_cast<awst::TupleExpression*>(cond->trueExpr.get()));
							wrapTupleItems(dynamic_cast<awst::TupleExpression*>(cond->falseExpr.get()));
							cond->wtype = new awst::WTuple(
								std::vector<awst::WType const*>(arc4Types));
						}
						else if (ret->value->wtype
							&& ret->value->wtype->kind() == awst::WTypeKind::WTuple)
						{
							// Non-literal tuple expression (e.g. `return fu()`):
							// spill into a local, then build a TupleExpression of
							// ARC4-encoded TupleItemExpressions so each biguint
							// element is properly widened to its ARC4UIntN width.
							auto const* subTupleType = static_cast<awst::WTuple const*>(ret->value->wtype);
							bool needsWrap = false;
							for (auto const* t : subTupleType->types())
								if (t == awst::WType::biguintType()) { needsWrap = true; break; }
							if (!needsWrap) continue;

							std::string tmpName = "__ret_tmp_" + std::to_string(retTmpCounter++);
							auto tmpVar = awst::makeVarExpression(tmpName, ret->value->wtype, ret->sourceLocation);

							auto assign = awst::makeAssignmentStatement(tmpVar, std::move(ret->value), ret->sourceLocation);

							auto newTuple = awst::makeTupleExpression(nullptr, assign->sourceLocation);
							for (size_t i = 0; i < arc4Types.size() && i < subTupleType->types().size(); ++i)
							{
								auto item = awst::makeTupleItem(tmpVar, static_cast<int>(i), subTupleType->types()[i], assign->sourceLocation);
								if (subTupleType->types()[i] == awst::WType::biguintType()
									&& arc4Types[i]->kind() == awst::WTypeKind::ARC4UIntN)
								{
									unsigned bits = static_cast<unsigned>(
										static_cast<awst::ARC4UIntN const*>(arc4Types[i])->n());
									newTuple->items.push_back(
										encodeRet(std::move(item), bits, arc4Types[i], assign->sourceLocation));
								}
								else
									newTuple->items.push_back(std::move(item));
							}
							newTuple->wtype = new awst::WTuple(
								std::vector<awst::WType const*>(arc4Types));
							ret->value = std::move(newTuple);

							stmts.insert(stmts.begin() + si, std::move(assign));
							++si; // skip the newly-inserted assign
						}
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
					{
						if (ifElse->ifBranch) wrapTupleReturns(ifElse->ifBranch->body);
						if (ifElse->elseBranch) wrapTupleReturns(ifElse->elseBranch->body);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
						wrapTupleReturns(block->body);
					else if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
						if (loop->loopBody) wrapTupleReturns(loop->loopBody->body);
				}
			};
			wrapTupleReturns(method.body->body);
			method.returnType = new awst::WTuple(std::vector<awst::WType const*>(arc4Types));
		}
	}

	// Sign-extend return values for signed integer types ≤64 bits, and
	// for ≤256-bit signed returns wrap the result in an ARC4Encode of
	// ARC4UIntN(256) so the ABI output is uint256 (32 bytes) rather
	// than puya's default biguint→uint512 (64 bytes).
	if (!signedReturns.empty() && method.arc4MethodConfig.has_value())
	{
		// All signed returns are wrapped to 256 bits by signExtendToUint256,
		// so the ABI element is uint256 in every case.
		auto const* arc4SignedType =
			m_typeMapper.createType<awst::ARC4UIntN>(256);

		auto wrapArc4 = [&](std::shared_ptr<awst::Expression> val,
			awst::SourceLocation const& loc) -> std::shared_ptr<awst::Expression> {
			if (val->wtype != awst::WType::biguintType())
				return val;
			auto encode = awst::makeARC4Encode(std::move(val), arc4SignedType, loc);
			return encode;
		};

		bool wrapSingleReturn = (signedReturns.size() == 1
			&& signedReturns[0].index == 0
			&& returnParams.size() == 1
			&& method.returnType == awst::WType::biguintType()
			&& _func.modifiers().empty()
			&& !funcHasInlineAssembly);

		std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> walk;
		walk = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
		{
			for (auto& stmt: stmts)
			{
				if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
				{
					if (!ret->value) continue;
					auto srcLoc = ret->value->sourceLocation;

					if (signedReturns.size() == 1 && signedReturns[0].index == 0
						&& returnParams.size() == 1)
					{
						// Single return — sign-extend directly
						ret->value = TypeCoercion::signExtendToUint256(
							std::move(ret->value), signedReturns[0].bits, srcLoc);
						if (wrapSingleReturn)
							ret->value = wrapArc4(std::move(ret->value), srcLoc);
					}
					else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
					{
						// Tuple return — sign-extend individual elements
						for (auto const& sr: signedReturns)
						{
							if (sr.index < tuple->items.size())
							{
								tuple->items[sr.index] = TypeCoercion::signExtendToUint256(
									std::move(tuple->items[sr.index]), sr.bits, srcLoc);
							}
						}
						tuple->wtype = method.returnType;
					}
				}
				else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
				{
					if (ifElse->ifBranch) walk(ifElse->ifBranch->body);
					if (ifElse->elseBranch) walk(ifElse->elseBranch->body);
				}
				else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
					walk(block->body);
			}
		};
		walk(method.body->body);

		if (wrapSingleReturn)
			method.returnType = arc4SignedType;
	}

	// Mask unsigned sub-word return values to their declared bit width.
	// EVM implicitly cleans values on ABI encoding; AVM preserves full uint64.
	if (!unsignedMasks.empty() && method.arc4MethodConfig.has_value())
	{
		auto maskValue = [&](std::shared_ptr<awst::Expression> val,
			unsigned bits, awst::SourceLocation const& loc)
			-> std::shared_ptr<awst::Expression>
		{
			uint64_t mask = (uint64_t(1) << bits) - 1;
			auto maskConst = awst::makeIntegerConstant(mask, loc);
			auto bitAnd = awst::makeUInt64BinOp(std::move(val), awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), loc);
			return bitAnd;
		};

		std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> walkMask;
		walkMask = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
		{
			for (auto& stmt: stmts)
			{
				if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
				{
					if (!ret->value) continue;
					auto srcLoc = ret->value->sourceLocation;
					if (unsignedMasks.size() == 1 && unsignedMasks[0].index == 0
						&& returnParams.size() == 1)
					{
						ret->value = maskValue(std::move(ret->value),
							unsignedMasks[0].bits, srcLoc);
					}
					else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
					{
						for (auto const& um: unsignedMasks)
						{
							if (um.index < tuple->items.size())
								tuple->items[um.index] = maskValue(
									std::move(tuple->items[um.index]), um.bits, srcLoc);
						}
					}
				}
				else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
				{
					if (ifElse->ifBranch) walkMask(ifElse->ifBranch->body);
					if (ifElse->elseBranch) walkMask(ifElse->elseBranch->body);
				}
				else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
					walkMask(block->body);
			}
		};
		walkMask(method.body->body);
	}
}

} // namespace puyasol::builder
