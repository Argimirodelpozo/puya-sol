#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <boost/multiprecision/cpp_int.hpp>

#include <functional>
#include <string>

namespace puyasol::builder
{

void forEachReturnStatement(
	std::vector<std::shared_ptr<awst::Statement>>& _stmts,
	std::function<void(awst::ReturnStatement&)> const& _fn)
{
	for (auto& stmt: _stmts)
	{
		if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
			_fn(*ret);
		else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
		{
			if (ifElse->ifBranch) forEachReturnStatement(ifElse->ifBranch->body, _fn);
			if (ifElse->elseBranch) forEachReturnStatement(ifElse->elseBranch->body, _fn);
		}
		else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
			forEachReturnStatement(block->body, _fn);
		else if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
		{
			if (loop->loopBody) forEachReturnStatement(loop->loopBody->body, _fn);
		}
	}
}

void rewriteARC4Returns(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& m_typeMapper,
	std::vector<SignedReturnInfo> const& signedReturns,
	std::vector<UnsignedMaskInfo> const& unsignedMasks,
	bool funcHasInlineAssembly)
{
	auto const& returnParams = _func.returnParameters();

	// Pass 1: dynamic-array returns → ARC4 type + ARC4Encode wrap.
	if (method.arc4MethodConfig.has_value()
		&& method.returnType->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* arc4RetType = m_typeMapper.mapToARC4Type(method.returnType);
		if (arc4RetType != method.returnType)
		{
			// Wrap all return values in ARC4Encode
			forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
				if (ret.value)
				{
					auto loc = ret.value->sourceLocation;
					auto encode = awst::makeARC4Encode(std::move(ret.value), arc4RetType, loc);
					ret.value = std::move(encode);
				}
			});
			method.returnType = arc4RetType;
		}
	}

	// Assembly bodies are UNCHECKED (EVM Yul wraps mod 2^256); AVM biguint
	// does NOT wrap. Wrap (val % 2^N) before ARC4Encode for asm functions
	// to match EVM semantics. Non-asm leaves bare ARC4Encode so overflow REVERTS.
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

	// Pass 2: biguint returns → ARC4Encode(ARC4UIntN(N)); skipped for signed + modifier fns.
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

		forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
			if (ret.value && ret.value->wtype == awst::WType::biguintType())
			{
				auto loc = ret.value->sourceLocation;
				ret.value = encodeRet(std::move(ret.value), retBits, arc4RetType, loc);
			}
		});
		method.returnType = arc4RetType;
	}

	// Pass 3: tuple returns with biguint elements → per-element ARC4Encode.
	if (method.arc4MethodConfig.has_value() && method.returnType
		&& method.returnType->kind() == awst::WTypeKind::WTuple
		&& signedReturns.empty() && _func.modifiers().empty())
	{
		auto const* tupleType = static_cast<awst::WTuple const*>(method.returnType);
		// Only wrap all-scalar tuples; mixed (arrays/structs/strings) need different handling.
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
							// Non-literal tuple (e.g. `return fu()`): spill to local,
							// then rebuild as TupleExpression of ARC4-encoded items.
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

	// Pass 4: signed returns → signExtendToUint256; wrap in ARC4UIntN(256)
	// so ABI output is uint256 (32 bytes) not puya's default biguint→uint512.
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

		forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
			if (!ret.value) return;
			auto srcLoc = ret.value->sourceLocation;

			if (signedReturns.size() == 1 && signedReturns[0].index == 0
				&& returnParams.size() == 1)
			{
				ret.value = TypeCoercion::signExtendToUint256(
					std::move(ret.value), signedReturns[0].bits, srcLoc);
				if (wrapSingleReturn)
					ret.value = wrapArc4(std::move(ret.value), srcLoc);
			}
			else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
			{
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
		});

		if (wrapSingleReturn)
			method.returnType = arc4SignedType;
	}

	// Pass 5: unsigned sub-word returns → mask to declared width (AVM preserves full uint64).
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

		forEachReturnStatement(method.body->body, [&](awst::ReturnStatement& ret) {
			if (!ret.value) return;
			auto srcLoc = ret.value->sourceLocation;
			if (unsignedMasks.size() == 1 && unsignedMasks[0].index == 0
				&& returnParams.size() == 1)
			{
				ret.value = maskValue(std::move(ret.value),
					unsignedMasks[0].bits, srcLoc);
			}
			else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret.value.get()))
			{
				for (auto const& um: unsignedMasks)
				{
					if (um.index < tuple->items.size())
						tuple->items[um.index] = maskValue(
							std::move(tuple->items[um.index]), um.bits, srcLoc);
				}
			}
		});
	}

	// Pass 6 (safety): coerce native-int return values to match method.returnType.
	// Some paths leave uint64↔biguint mismatches that puya rejects; e.g. a
	// signed sub-word sign-extended to biguint while a modifier moves it into a
	// uint64 named-return var (V4 PoolManager.initialize int24 tick phi).
	if (method.returnType
		&& (method.returnType == awst::WType::uint64Type()
			|| method.returnType == awst::WType::biguintType()))
	{
		auto isNativeInt = [](awst::WType const* t) {
			return t == awst::WType::uint64Type() || t == awst::WType::biguintType();
		};
		// Name of the single named-return var, if any. A modifier moves the
		// return value into this var (so the body ends with `<name> = <value>`
		// instead of `return <value>`); we must coerce that assignment too.
		// Also coerce assignments into the single named-return var (modifier path).
		std::string const namedRet =
			returnParams.size() == 1 ? returnParams[0]->name() : std::string{};
		std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> walkCoerce;
		walkCoerce = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
		{
			for (auto& stmt: stmts)
			{
				if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
				{
					if (ret->value && ret->value->wtype && isNativeInt(ret->value->wtype)
						&& ret->value->wtype != method.returnType)
					{
						auto loc = ret->value->sourceLocation;
						ret->value = TypeCoercion::implicitNumericCast(
							std::move(ret->value), method.returnType, loc);
					}
				}
				else if (auto* as = dynamic_cast<awst::AssignmentStatement*>(stmt.get()))
				{
					// Coerce assignment into named-return var; retype target for avm_type consistency.
					if (auto* tv = dynamic_cast<awst::VarExpression*>(as->target.get()))
						if (!namedRet.empty() && tv->name == namedRet && as->value
							&& as->value->wtype && isNativeInt(as->value->wtype)
							&& as->value->wtype != method.returnType)
						{
							auto loc = as->value->sourceLocation;
							as->value = TypeCoercion::implicitNumericCast(
								std::move(as->value), method.returnType, loc);
							tv->wtype = method.returnType;
						}
				}
				else if (auto* ie = dynamic_cast<awst::IfElse*>(stmt.get()))
				{
					if (ie->ifBranch) walkCoerce(ie->ifBranch->body);
					if (ie->elseBranch) walkCoerce(ie->elseBranch->body);
				}
				else if (auto* b = dynamic_cast<awst::Block*>(stmt.get()))
					walkCoerce(b->body);
				else if (auto* wl = dynamic_cast<awst::WhileLoop*>(stmt.get()))
					if (wl->loopBody) walkCoerce(wl->loopBody->body);
			}
		};
		walkCoerce(method.body->body);
	}
}

} // namespace puyasol::builder
