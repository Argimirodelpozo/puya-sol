/// @file MemberAccessBuilder.cpp
/// Handles member access expressions (struct fields, enum values, intrinsics).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/expressions/ExpressionUtils.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::MemberAccess const& _node)
{
	auto loc = makeLoc(_node.location());
	std::string memberName = _node.memberName();

	auto const& baseExpr = _node.expression();

	// block.prevrandao → block BlkSeed (global Round)
	// block.difficulty → 0 (Algorand has no PoW difficulty)
	if (auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
	{
		if (baseId->name() == "block" && memberName == "difficulty")
		{
			Logger::instance().warning(
				"block.difficulty returns 0 on AVM — Algorand has no proof-of-work. "
				"Post-Paris EVM also deprecated this (returns prevrandao instead). "
				"Use block.prevrandao for randomness.", loc);
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = loc;
			zero->wtype = awst::WType::biguintType();
			zero->value = "0";
			push(std::move(zero));
			return false;
		}
		if (baseId->name() == "block" && memberName == "prevrandao")
		{
			Logger::instance().warning(
				"block.prevrandao mapped to AVM block seed (BlkSeed) of previous round. "
				"AVM block seed is the VRF output — analogous but not equivalent to "
				"EVM prevrandao. Both provide pseudo-randomness from the block proposer.", loc);

			// global Round - 2 (the `block` opcode can only access rounds that
			// are fully confirmed; Round-1 is the latest confirmed but the opcode
			// window on localnet only includes Round-2 and earlier)
			auto round = std::make_shared<awst::IntrinsicCall>();
			round->sourceLocation = loc;
			round->wtype = awst::WType::uint64Type();
			round->opCode = "global";
			round->immediates = {std::string("Round")};

			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = loc;
			one->wtype = awst::WType::uint64Type();
			one->value = "2";

			auto prevRound = std::make_shared<awst::UInt64BinaryOperation>();
			prevRound->sourceLocation = loc;
			prevRound->wtype = awst::WType::uint64Type();
			prevRound->left = std::move(round);
			prevRound->op = awst::UInt64BinaryOperator::Sub;
			prevRound->right = std::move(one);

			// block BlkSeed (prevRound) → bytes
			auto blockSeed = std::make_shared<awst::IntrinsicCall>();
			blockSeed->sourceLocation = loc;
			blockSeed->wtype = awst::WType::bytesType();
			blockSeed->opCode = "block";
			blockSeed->immediates = {std::string("BlkSeed")};
			blockSeed->stackArgs.push_back(std::move(prevRound));

			// Cast bytes → biguint (Solidity returns uint256)
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(blockSeed);
			push(std::move(cast));
			return false;
		}
	}

	// Try intrinsic mapping first (msg.sender, block.timestamp, etc.)
	if (auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
	{
		auto intrinsic = IntrinsicMapper::tryMapMemberAccess(baseId->name(), memberName, loc);
		if (intrinsic)
		{
			// If the intrinsic returns bytes but the Solidity type expects uint/biguint,
			// wrap in ReinterpretCast (e.g. GenesisHash bytes → uint256 for block.chainid)
			auto* solType = m_typeMapper.map(_node.annotation().type);
			if (intrinsic->wtype == awst::WType::bytesType()
				&& solType == awst::WType::biguintType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::biguintType();
				cast->expr = std::move(intrinsic);
				push(std::move(cast));
			}
			else
			{
				push(intrinsic);
			}
			return false;
		}
	}

	// Enum member access: MyEnum.Value → integer constant
	if (auto const* enumVal = dynamic_cast<solidity::frontend::EnumValue const*>(
		_node.annotation().referencedDeclaration))
	{
		// Find the enum definition to determine the member's ordinal index
		auto const* enumDef = dynamic_cast<solidity::frontend::EnumDefinition const*>(
			enumVal->scope()
		);
		if (enumDef)
		{
			int index = 0;
			for (auto const& member: enumDef->members())
			{
				if (member.get() == enumVal)
					break;
				++index;
			}
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::uint64Type();
			e->value = std::to_string(index);
			push(e);
			return false;
		}
	}

	// Selector: E.selector or f.selector → keccak256("Name(type1,type2,...)")[:4]
	if (memberName == "selector")
	{
		auto const* baseType = baseExpr.annotation().type;
		std::string sig;

		// Resolve the declaration (event or function)
		solidity::frontend::FunctionType const* funcType = nullptr;
		if (auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(baseType))
			funcType = ft;
		else if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
			funcType = dynamic_cast<solidity::frontend::FunctionType const*>(typeType->actualType());

		if (funcType)
		{
			if (funcType->kind() == solidity::frontend::FunctionType::Kind::Event)
			{
				// Event selector: keccak256("EventName(type1,...)")
				auto const* eventDef = dynamic_cast<solidity::frontend::EventDefinition const*>(
					&funcType->declaration());
				if (eventDef)
				{
					sig = eventDef->name() + "(";
					bool first = true;
					for (auto const& param: eventDef->parameters())
					{
						if (!first) sig += ",";
						sig += param->type()->canonicalName();
						first = false;
					}
					sig += ")";
				}
			}
			else
			{
				// Function selector: keccak256("funcName(type1,...)")[:4]
				// Use the FunctionType's externalSignature() which gives the canonical form.
				// This can throw for function types without a bound declaration
				// (e.g. ternary result: (c ? this.f : this.g).selector).
				try
				{
					sig = funcType->externalSignature();
				}
				catch (...)
				{
					// Fallback: check if base is a ternary (c ? f : g).selector
					// → distribute: c ? f.selector : g.selector
					if (auto const* cond = dynamic_cast<solidity::frontend::Conditional const*>(&baseExpr))
					{
						// Helper: resolve selector from a sub-expression
						auto resolveSelector = [&](solidity::frontend::Expression const& _expr)
							-> std::string
						{
							auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(
								_expr.annotation().type);
							if (ft)
							{
								try { return ft->externalSignature(); }
								catch (...) {}
							}
							// Try identifier resolution
							if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&_expr))
							{
								if (auto const* fd = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
										id->annotation().referencedDeclaration))
								{
									std::string s = fd->name() + "(";
									bool first = true;
									for (auto const& p: fd->parameters())
									{
										if (!first) s += ",";
										s += p->type()->canonicalName();
										first = false;
									}
									return s + ")";
								}
							}
							// Try MemberAccess (this.f)
							if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&_expr))
							{
								auto const* mft = dynamic_cast<solidity::frontend::FunctionType const*>(
									ma->annotation().type);
								if (mft)
								{
									try { return mft->externalSignature(); }
									catch (...) {}
								}
							}
							return {};
						};

						std::string trueSig = resolveSelector(cond->trueExpression());
						std::string falseSig = resolveSelector(cond->falseExpression());

						if (!trueSig.empty() && !falseSig.empty())
						{
							// Build: cond ? keccak256(trueSig)[:4] : keccak256(falseSig)[:4]
							auto condition = build(cond->condition());

							auto makeSelectorExpr = [&](std::string const& s)
								-> std::shared_ptr<awst::Expression>
							{
								auto sigConst = std::make_shared<awst::BytesConstant>();
								sigConst->sourceLocation = loc;
								sigConst->wtype = awst::WType::bytesType();
								sigConst->encoding = awst::BytesEncoding::Utf8;
								sigConst->value = std::vector<uint8_t>(s.begin(), s.end());

								auto keccak = std::make_shared<awst::IntrinsicCall>();
								keccak->sourceLocation = loc;
								keccak->wtype = awst::WType::bytesType();
								keccak->opCode = "keccak256";
								keccak->stackArgs.push_back(std::move(sigConst));

								auto zero = std::make_shared<awst::IntegerConstant>();
								zero->sourceLocation = loc;
								zero->wtype = awst::WType::uint64Type();
								zero->value = "0";
								auto four = std::make_shared<awst::IntegerConstant>();
								four->sourceLocation = loc;
								four->wtype = awst::WType::uint64Type();
								four->value = "4";

								auto extract = std::make_shared<awst::IntrinsicCall>();
								extract->sourceLocation = loc;
								extract->wtype = awst::WType::bytesType();
								extract->opCode = "extract3";
								extract->stackArgs.push_back(std::move(keccak));
								extract->stackArgs.push_back(std::move(zero));
								extract->stackArgs.push_back(std::move(four));
								return extract;
							};

							auto ternary = std::make_shared<awst::ConditionalExpression>();
							ternary->sourceLocation = loc;
							ternary->wtype = awst::WType::bytesType();
							ternary->condition = std::move(condition);
							ternary->trueExpr = makeSelectorExpr(trueSig);
							ternary->falseExpr = makeSelectorExpr(falseSig);

							// Cast to biguint for ARC4 return encoding
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = awst::WType::biguintType();
							cast->expr = std::move(ternary);
							push(cast);
							return false;
						}
					}
					// Final fallback: try identifier
					if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
					{
						if (auto const* funcDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
								ident->annotation().referencedDeclaration))
						{
							sig = funcDef->name() + "(";
							bool first = true;
							for (auto const& param: funcDef->parameters())
							{
								if (!first) sig += ",";
								sig += param->type()->canonicalName();
								first = false;
							}
							sig += ")";
						}
					}
					if (sig.empty())
						Logger::instance().warning("could not resolve function selector (no declaration)", loc);
				}
			}
		}

		if (!sig.empty())
		{
			Logger::instance().debug("selector: " + sig, loc);

			// Compute keccak256 of the signature
			auto keccak = std::make_shared<awst::IntrinsicCall>();
			keccak->sourceLocation = loc;
			keccak->wtype = awst::WType::bytesType();
			keccak->opCode = "keccak256";

			auto sigBytes = std::make_shared<awst::BytesConstant>();
			sigBytes->sourceLocation = loc;
			sigBytes->wtype = awst::WType::bytesType();
			sigBytes->encoding = awst::BytesEncoding::Utf8;
			sigBytes->value = std::vector<uint8_t>(sig.begin(), sig.end());
			keccak->stackArgs.push_back(std::move(sigBytes));

			// For bytes4 selector: extract first 4 bytes
			auto* targetType = m_typeMapper.map(_node.annotation().type);
			auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(targetType);
			if (bytesWType && bytesWType->length().has_value() && *bytesWType->length() == 4)
			{
				// extract first 4 bytes: IntrinsicCall("extract", [0, 4], [keccak_result])
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract";
				extract->immediates = {0, 4};
				extract->stackArgs.push_back(std::move(keccak));

				// Right-pad to 4 bytes and cast to bytes[4]
				auto padded = std::make_shared<awst::ReinterpretCast>();
				padded->sourceLocation = loc;
				padded->wtype = targetType;
				padded->expr = std::move(extract);
				push(std::move(padded));
			}
			else if (targetType != awst::WType::bytesType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = targetType;
				cast->expr = std::move(keccak);
				push(std::move(cast));
			}
			else
				push(std::move(keccak));
			return false;
		}
	}

	// Library/interface member access to events: L.E → resolve event type
	// When the base is a library/interface/contract type and the member is an event,
	// push a placeholder expression for later .selector access.
	if (auto const* refDecl = _node.annotation().referencedDeclaration)
	{
		if (dynamic_cast<solidity::frontend::EventDefinition const*>(refDecl))
		{
			// This is `L.E` — accessing event E from library L.
			// The result type is FunctionType(Event). Push a void placeholder —
			// it will only be used as a base for `.selector` which handles it above.
			auto vc = std::make_shared<awst::VoidConstant>();
			vc->sourceLocation = loc;
			vc->wtype = awst::WType::voidType();
			push(std::move(vc));
			return false;
		}
	}

	// type(uintN).max / type(uintN).min → IntegerConstant
	if (memberName == "max" || memberName == "min")
	{
		solidity::frontend::IntegerType const* intType = nullptr;
		auto const* baseType = baseExpr.annotation().type;

		if (baseType)
		{
			// type(X) produces a MagicType with Kind::MetaType
			if (auto const* magicType = dynamic_cast<solidity::frontend::MagicType const*>(baseType))
			{
				if (auto const* arg = magicType->typeArgument())
					intType = dynamic_cast<solidity::frontend::IntegerType const*>(arg);
			}
			// Also handle TypeType (for completeness)
			else if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
			{
				intType = dynamic_cast<solidity::frontend::IntegerType const*>(typeType->actualType());
			}
		}

		if (intType)
		{
			unsigned bits = intType->numBits();
			auto* wtype = (bits <= 64)
				? awst::WType::uint64Type()
				: awst::WType::biguintType();

			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = wtype;

			if (memberName == "max")
				e->value = computeMaxUint(bits);
			else
				e->value = "0"; // min for unsigned integers

			push(e);
			return false;
		}

		// type(EnumType).max / type(EnumType).min
		solidity::frontend::EnumType const* enumType = nullptr;
		if (baseType)
		{
			if (auto const* magicType = dynamic_cast<solidity::frontend::MagicType const*>(baseType))
			{
				if (auto const* arg = magicType->typeArgument())
					enumType = dynamic_cast<solidity::frontend::EnumType const*>(arg);
			}
		}
		if (enumType)
		{
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::uint64Type();

			if (memberName == "max")
				e->value = std::to_string(enumType->numberOfMembers() - 1);
			else
				e->value = "0";

			push(e);
			return false;
		}

	}

	// type(C).name → contract/interface name as string constant
	if (memberName == "name")
	{
		auto const* baseType = baseExpr.annotation().type;
		solidity::frontend::Type const* typeArg = nullptr;
		if (auto const* magicType = dynamic_cast<solidity::frontend::MagicType const*>(baseType))
			typeArg = magicType->typeArgument();
		else if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
			typeArg = typeType->actualType();

		if (typeArg)
		{
			std::string typeName;
			if (auto const* ct = dynamic_cast<solidity::frontend::ContractType const*>(typeArg))
				typeName = ct->contractDefinition().name();
			else
				typeName = typeArg->toString(true);

			auto strConst = std::make_shared<awst::BytesConstant>();
			strConst->sourceLocation = loc;
			strConst->wtype = awst::WType::stringType();
			strConst->encoding = awst::BytesEncoding::Utf8;
			strConst->value = std::vector<uint8_t>(typeName.begin(), typeName.end());
			push(strConst);
			return false;
		}
	}

	// .length on arrays or bytes
	if (memberName == "length")
	{
		// Check if this is a box-stored dynamic array state variable
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
		{
			if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			{
				if (varDecl->isStateVariable() && StorageMapper::shouldUseBoxStorage(*varDecl)
					&& dynamic_cast<solidity::frontend::ArrayType const*>(varDecl->type()))
				{
					// length = box_len(key) / element_size
					auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(varDecl->type());
					auto* rawElemType = m_typeMapper.map(arrType->baseType());
					auto* arc4ElemType = m_typeMapper.mapToARC4Type(rawElemType);
					unsigned elemSize = StorageMapper::computeEncodedElementSize(arc4ElemType);

					std::string varName = ident->name();
					auto boxKey = std::make_shared<awst::BytesConstant>();
					boxKey->sourceLocation = loc;
					boxKey->wtype = awst::WType::bytesType();
					boxKey->encoding = awst::BytesEncoding::Utf8;
					boxKey->value = std::vector<uint8_t>(varName.begin(), varName.end());

					// box_len returns (uint64, bool)
					auto* tupleType = m_typeMapper.createType<awst::WTuple>(
						std::vector<awst::WType const*>{awst::WType::uint64Type(), awst::WType::boolType()}
					);
					auto boxLen = std::make_shared<awst::IntrinsicCall>();
					boxLen->sourceLocation = loc;
					boxLen->wtype = tupleType;
					boxLen->opCode = "box_len";
					boxLen->stackArgs.push_back(std::move(boxKey));

					// Extract the length (index 0)
					auto lenVal = std::make_shared<awst::TupleItemExpression>();
					lenVal->sourceLocation = loc;
					lenVal->wtype = awst::WType::uint64Type();
					lenVal->base = std::move(boxLen);
					lenVal->index = 0;

					// Divide by element size
					auto elemSizeConst = std::make_shared<awst::IntegerConstant>();
					elemSizeConst->sourceLocation = loc;
					elemSizeConst->wtype = awst::WType::uint64Type();
					elemSizeConst->value = std::to_string(elemSize);

					auto divExpr = std::make_shared<awst::UInt64BinaryOperation>();
					divExpr->sourceLocation = loc;
					divExpr->wtype = awst::WType::uint64Type();
					divExpr->left = std::move(lenVal);
					divExpr->op = awst::UInt64BinaryOperator::FloorDiv;
					divExpr->right = std::move(elemSizeConst);

					push(std::move(divExpr));
					return false;
				}
			}
		}

		auto base = build(baseExpr);
		// For bytes-typed expressions, use the AVM `len` intrinsic instead of ArrayLength
		if (base->wtype == awst::WType::bytesType())
		{
			auto e = std::make_shared<awst::IntrinsicCall>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::uint64Type();
			e->opCode = "len";
			e->stackArgs.push_back(std::move(base));
			push(e);
			return false;
		}
		auto e = std::make_shared<awst::ArrayLength>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::uint64Type();
		e->array = std::move(base);
		push(e);
		return false;
	}

	// Library/contract constant access: inline the constant value
	auto const* refDecl = _node.annotation().referencedDeclaration;
	if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(refDecl))
	{
		if (varDecl->isConstant() && varDecl->value())
		{
			push(build(*varDecl->value()));
			return false;
		}
	}

	// Contract member access (e.g. token.transfer used as function selector in abi.encodeCall)
	auto const* baseType = baseExpr.annotation().type;
	if (baseType && baseType->category() == solidity::frontend::Type::Category::Contract)
	{
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Utf8;
		std::string sel = memberName;
		e->value = std::vector<uint8_t>(sel.begin(), sel.end());
		push(e);
		return false;
	}

	// Address property (.code, .balance, etc.)
	if (baseType && baseType->category() == solidity::frontend::Type::Category::Address)
	{
		if (memberName == "code")
		{
			// address.code → extract app_id from Solidity address, then use
			// app_params_get AppApprovalProgram to fetch the approval program bytes.
			// Solidity addresses are encoded as \x00*24 + app_id.to_bytes(8, "big"),
			// so extract_uint64(addr, 24) recovers the app_id.
			baseExpr.accept(*this);
			auto addrExpr = pop();

			// If the address is in account type, cast to bytes first
			std::shared_ptr<awst::Expression> bytesExpr = std::move(addrExpr);
			if (bytesExpr->wtype == awst::WType::accountType())
			{
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(bytesExpr);
				bytesExpr = std::move(toBytes);
			}

			// extract 24 8 → 8-byte app_id bytes
			auto extract = std::make_shared<awst::IntrinsicCall>();
			extract->sourceLocation = loc;
			extract->wtype = awst::WType::bytesType();
			extract->opCode = "extract";
			extract->immediates = {24, 8};
			extract->stackArgs.push_back(std::move(bytesExpr));

			// btoi → uint64 app_id
			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = loc;
			btoi->wtype = awst::WType::uint64Type();
			btoi->opCode = "btoi";
			btoi->stackArgs.push_back(std::move(extract));

			// ReinterpretCast to application type (app_params_get expects application)
			auto appId = std::make_shared<awst::ReinterpretCast>();
			appId->sourceLocation = loc;
			appId->wtype = awst::WType::applicationType();
			appId->expr = std::move(btoi);

			// app_params_get AppApprovalProgram → (bytes, bool)
			auto* tupleType = m_typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::boolType()}
			);
			auto appParamsGet = std::make_shared<awst::IntrinsicCall>();
			appParamsGet->sourceLocation = loc;
			appParamsGet->wtype = tupleType;
			appParamsGet->opCode = "app_params_get";
			appParamsGet->immediates = {std::string("AppApprovalProgram")};
			appParamsGet->stackArgs.push_back(std::move(appId));

			// TupleItemExpression index 0 → the approval program bytes
			auto item = std::make_shared<awst::TupleItemExpression>();
			item->sourceLocation = loc;
			item->wtype = awst::WType::bytesType();
			item->base = std::move(appParamsGet);
			item->index = 0;

			push(std::move(item));
			return false;
		}
		Logger::instance().warning("address property '." + memberName + "' has no Algorand equivalent", loc);
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {};
		push(e);
		return false;
	}

	// type(Interface).interfaceId → bytes4 constant with actual ERC165 interface ID
	if (memberName == "interfaceId" && baseType)
	{
		uint32_t interfaceIdValue = 0;
		// type(X) produces a MagicType with Kind::MetaType
		if (auto const* magicType = dynamic_cast<solidity::frontend::MagicType const*>(baseType))
		{
			if (auto const* arg = magicType->typeArgument())
				if (auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(arg))
					interfaceIdValue = contractType->contractDefinition().interfaceId();
		}
		// Also handle TypeType (for completeness)
		else if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
		{
			auto const* actualType = typeType->actualType();
			if (auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(actualType))
				interfaceIdValue = contractType->contractDefinition().interfaceId();
		}
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = m_typeMapper.map(_node.annotation().type);
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {
			static_cast<uint8_t>((interfaceIdValue >> 24) & 0xFF),
			static_cast<uint8_t>((interfaceIdValue >> 16) & 0xFF),
			static_cast<uint8_t>((interfaceIdValue >> 8) & 0xFF),
			static_cast<uint8_t>(interfaceIdValue & 0xFF)
		};
		push(e);
		return false;
	}

	// General field access — valid for struct/tuple base types (WTuple or ARC4Struct)
	auto base = build(baseExpr);
	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(base->wtype);
		// Look up the ARC4 field type from the struct definition
		awst::WType const* arc4FieldType = nullptr;
		for (auto const& [fname, ftype]: structType->fields())
			if (fname == memberName)
			{
				arc4FieldType = ftype;
				break;
			}
		auto field = std::make_shared<awst::FieldExpression>();
		field->sourceLocation = loc;
		field->base = std::move(base);
		field->name = memberName;
		field->wtype = arc4FieldType ? arc4FieldType : m_typeMapper.map(_node.annotation().type);
		// Wrap in ARC4Decode to convert back to native type for use in expressions
		auto* nativeType = m_typeMapper.map(_node.annotation().type);
		if (arc4FieldType && arc4FieldType != nativeType)
		{
			{
				auto decode = std::make_shared<awst::ARC4Decode>();
				decode->sourceLocation = loc;
				decode->wtype = nativeType;
				decode->value = std::move(field);
				push(decode);
			}
		}
		else
			push(field);
		return false;
	}
	if (base->wtype && base->wtype->kind() == awst::WTypeKind::WTuple)
	{
		auto e = std::make_shared<awst::FieldExpression>();
		e->sourceLocation = loc;
		e->base = std::move(base);
		e->name = memberName;
		e->wtype = m_typeMapper.map(_node.annotation().type);
		push(e);
		return false;
	}

	// Fallback: non-struct field access → emit mapped type default
	Logger::instance().warning("unsupported member access '." + memberName + "', returning default", loc);
	auto* wtype = m_typeMapper.map(_node.annotation().type);
	if (wtype == awst::WType::uint64Type() || wtype == awst::WType::biguintType())
	{
		auto e = std::make_shared<awst::IntegerConstant>();
		e->sourceLocation = loc;
		e->wtype = wtype;
		e->value = "0";
		push(e);
	}
	else if (wtype == awst::WType::boolType())
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = loc;
		e->wtype = wtype;
		e->value = false;
		push(e);
	}
	else
	{
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {};
		push(e);
	}
	return false;
}


} // namespace puyasol::builder
