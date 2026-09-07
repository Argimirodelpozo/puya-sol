#include "json/AWSTSerializer.h"
#include "json/Base85.hpp"

#include <string_view>

namespace puyasol::json
{

using njson = nlohmann::ordered_json;

namespace
{

std::string bytesEncodingToString(awst::BytesEncoding _enc)
{
	switch (_enc)
	{
	case awst::BytesEncoding::Unknown: return "unknown";
	case awst::BytesEncoding::Base16: return "base16";
	case awst::BytesEncoding::Base32: return "base32";
	case awst::BytesEncoding::Base64: return "base64";
	case awst::BytesEncoding::Utf8: return "utf8";
	}
	return "unknown";
}

std::string uint64BinOpToString(awst::UInt64BinaryOperator _op)
{
	switch (_op)
	{
	case awst::UInt64BinaryOperator::Add: return "+";
	case awst::UInt64BinaryOperator::Sub: return "-";
	case awst::UInt64BinaryOperator::Mult: return "*";
	case awst::UInt64BinaryOperator::FloorDiv: return "//";
	case awst::UInt64BinaryOperator::Mod: return "%";
	case awst::UInt64BinaryOperator::Pow: return "**";
	case awst::UInt64BinaryOperator::LShift: return "<<";
	case awst::UInt64BinaryOperator::RShift: return ">>";
	case awst::UInt64BinaryOperator::BitOr: return "|";
	case awst::UInt64BinaryOperator::BitXor: return "^";
	case awst::UInt64BinaryOperator::BitAnd: return "&";
	}
	return "+";
}

std::string bigUIntBinOpToString(awst::BigUIntBinaryOperator _op)
{
	switch (_op)
	{
	case awst::BigUIntBinaryOperator::Add: return "+";
	case awst::BigUIntBinaryOperator::Sub: return "-";
	case awst::BigUIntBinaryOperator::Mult: return "*";
	case awst::BigUIntBinaryOperator::FloorDiv: return "//";
	case awst::BigUIntBinaryOperator::Mod: return "%";
	case awst::BigUIntBinaryOperator::BitOr: return "|";
	case awst::BigUIntBinaryOperator::BitXor: return "^";
	case awst::BigUIntBinaryOperator::BitAnd: return "&";
	}
	return "+";
}

std::string numericCompToString(awst::NumericComparison _op)
{
	switch (_op)
	{
	case awst::NumericComparison::Eq: return "==";
	case awst::NumericComparison::Ne: return "!=";
	case awst::NumericComparison::Lt: return "<";
	case awst::NumericComparison::Lte: return "<=";
	case awst::NumericComparison::Gt: return ">";
	case awst::NumericComparison::Gte: return ">=";
	}
	return "==";
}

std::string equalityCompToString(awst::EqualityComparison _op)
{
	switch (_op)
	{
	case awst::EqualityComparison::Eq: return "==";
	case awst::EqualityComparison::Ne: return "!=";
	}
	return "==";
}

std::string boolBinOpToString(awst::BinaryBooleanOperator _op)
{
	switch (_op)
	{
	case awst::BinaryBooleanOperator::And: return "and";
	case awst::BinaryBooleanOperator::Or: return "or";
	}
	return "and";
}

std::string bytesBinOpToString(awst::BytesBinaryOperator _op)
{
	switch (_op)
	{
	case awst::BytesBinaryOperator::Add: return "+";
	case awst::BytesBinaryOperator::BitOr: return "|";
	case awst::BytesBinaryOperator::BitXor: return "^";
	case awst::BytesBinaryOperator::BitAnd: return "&";
	}
	return "+";
}

} // namespace

njson AWSTSerializer::serialize(std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	njson arr = njson::array();
	for (auto const& root: _roots)
		arr.push_back(serializeRootNode(*root));
	return arr;
}

njson AWSTSerializer::serializeRootNode(awst::RootNode const& _node)
{
	if (auto const* contract = dynamic_cast<awst::Contract const*>(&_node))
		return serializeContract(*contract);
	if (auto const* lsig = dynamic_cast<awst::LogicSignature const*>(&_node))
		return serializeLogicSignature(*lsig);
	if (auto const* sub = dynamic_cast<awst::Subroutine const*>(&_node))
		return serializeSubroutine(*sub);
	return njson::object();
}

njson AWSTSerializer::serializeLogicSignature(awst::LogicSignature const& _lsig)
{
	njson j;
	j["_type"] = "LogicSignature";
	j["source_location"] = serializeSourceLocation(_lsig.sourceLocation);
	j["id"] = _lsig.id;
	j["short_name"] = _lsig.shortName;
	j["program"] = _lsig.program ? serializeSubroutine(*_lsig.program) : njson(nullptr);
	j["docstring"] = _lsig.docstring.has_value() ? njson(_lsig.docstring.value()) : njson(nullptr);
	j["reserved_scratch_space"] = njson(_lsig.reservedScratchSpace);
	j["avm_version"] = _lsig.avmVersion.has_value() ? njson(_lsig.avmVersion.value()) : njson(nullptr);
	j["validate_encoding"] = _lsig.validateEncoding.has_value()
		? njson(_lsig.validateEncoding.value())
		: njson(nullptr);
	return j;
}

njson AWSTSerializer::serializeContract(awst::Contract const& _contract)
{
	njson j;
	j["_type"] = "Contract";
	j["source_location"] = serializeSourceLocation(_contract.sourceLocation);
	j["id"] = _contract.id;
	j["name"] = _contract.name;
	j["description"] = _contract.description.has_value()
		? njson(_contract.description.value())
		: njson(nullptr);
	j["method_resolution_order"] = njson(_contract.methodResolutionOrder);
	j["approval_program"] = serializeContractMethod(_contract.approvalProgram);
	j["clear_program"] = serializeContractMethod(_contract.clearProgram);

	njson methods = njson::array();
	for (auto const& m: _contract.methods)
		methods.push_back(serializeContractMethod(m));
	j["methods"] = methods;

	njson appState = njson::array();
	for (auto const& s: _contract.appState)
		appState.push_back(serializeAppStorageDefinition(s));
	j["app_state"] = appState;

	if (_contract.stateTotals.has_value())
	{
		njson st;
		auto const& totals = _contract.stateTotals.value();
		st["global_uints"] = totals.globalUints.has_value()
			? njson(totals.globalUints.value())
			: njson(nullptr);
		st["local_uints"] = totals.localUints.has_value()
			? njson(totals.localUints.value())
			: njson(nullptr);
		st["global_bytes"] = totals.globalBytes.has_value()
			? njson(totals.globalBytes.value())
			: njson(nullptr);
		st["local_bytes"] = totals.localBytes.has_value()
			? njson(totals.localBytes.value())
			: njson(nullptr);
		j["state_totals"] = st;
	}
	else
		j["state_totals"] = nullptr;

	j["reserved_scratch_space"] = njson(_contract.reservedScratchSpace);
	j["avm_version"] = _contract.avmVersion.has_value()
		? njson(_contract.avmVersion.value())
		: njson(nullptr);

	return j;
}

njson AWSTSerializer::serializeSubroutine(awst::Subroutine const& _sub)
{
	njson j;
	j["_type"] = "Subroutine";
	j["source_location"] = serializeSourceLocation(_sub.sourceLocation);
	j["id"] = _sub.id;
	j["name"] = _sub.name;

	njson args = njson::array();
	for (auto const& a: _sub.args)
		args.push_back(serializeSubroutineArgument(a));
	j["args"] = args;

	j["return_type"] = serializeWType(_sub.returnType);
	j["body"] = _sub.body ? serializeBlock(*_sub.body) : njson(nullptr);
	j["documentation"] = serializeMethodDocumentation(_sub.documentation);
	j["inline"] = _sub.inlineOpt.has_value() ? njson(_sub.inlineOpt.value()) : njson(nullptr);
	j["pure"] = _sub.pure;

	return j;
}

njson AWSTSerializer::serializeContractMethod(awst::ContractMethod const& _method)
{
	njson j;
	j["_type"] = "ContractMethod";
	j["source_location"] = serializeSourceLocation(_method.sourceLocation);

	njson args = njson::array();
	for (auto const& a: _method.args)
		args.push_back(serializeSubroutineArgument(a));
	j["args"] = args;

	j["return_type"] = serializeWType(_method.returnType);
	j["body"] = _method.body ? serializeBlock(*_method.body) : njson(nullptr);
	j["documentation"] = serializeMethodDocumentation(_method.documentation);
	j["inline"] = _method.inlineOpt.has_value() ? njson(_method.inlineOpt.value()) : njson(nullptr);
	j["pure"] = _method.pure;
	j["cref"] = _method.cref;
	j["member_name"] = _method.memberName;
	j["arc4_method_config"] = _method.arc4MethodConfig.has_value()
		? serializeARC4MethodConfig(_method.arc4MethodConfig.value())
		: njson(nullptr);

	return j;
}

njson AWSTSerializer::serializeExpression(awst::Expression const& _expr)
{
	njson j;
	j["_type"] = _expr.nodeType();
	j["source_location"] = serializeSourceLocation(_expr.sourceLocation);
	j["wtype"] = serializeWType(_expr.wtype);

#define PUYASOL_AWST_SERIALIZE_ARM(Node) \
	if (auto const* node = dynamic_cast<awst::Node const*>(&_expr)) \
	{ \
		serializeFields(*node, j); \
		return j; \
	}
	PUYASOL_AWST_EXPRESSION_NODES(PUYASOL_AWST_SERIALIZE_ARM)
#undef PUYASOL_AWST_SERIALIZE_ARM

	return j;
}

void AWSTSerializer::emitStateField(awst::Expression const& _field, njson& _json)
{
	_json.erase("wtype");
	_json["field"] = serializeExpression(_field);
}

void AWSTSerializer::serializeFields(awst::IntegerConstant const& _node, njson& _json)
{
	// Stored as string for biguint; detect hex prefix so stoll handles 0x literals.
	std::string const& s = _node.value;
	bool neg = !s.empty() && s[0] == '-';
	size_t off = neg ? 1 : 0;
	bool isHex = s.size() > off + 2 && s[off] == '0' && (s[off + 1] == 'x' || s[off + 1] == 'X');
	try
	{
		if (isHex)
		{
			// Parse as hex; stoll with base 16 needs the prefix stripped.
			long long val = std::stoll(s.substr(off + 2), nullptr, 16);
			_json["value"] = neg ? -val : val;
		}
		else
			_json["value"] = std::stoll(s);
	}
	catch (...)
	{
		if (isHex)
		{
			// Hex too large for int64: convert to decimal string.
			std::string hex = s.substr(off + 2);
			// Big-integer decimal from hex via repeated base-10 division.
			std::vector<unsigned> digits; // big-endian hex digits
			digits.reserve(hex.size());
			for (char c : hex)
			{
				unsigned d = 0;
				if (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
				else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
				else { digits.clear(); break; }
				digits.push_back(d);
			}
			std::string dec;
			if (digits.empty())
				dec = "0";
			else
			{
				while (!digits.empty())
				{
					unsigned rem = 0;
					std::vector<unsigned> next;
					next.reserve(digits.size());
					for (unsigned d : digits)
					{
						unsigned cur = rem * 16 + d;
						unsigned q = cur / 10;
						rem = cur % 10;
						if (!next.empty() || q) next.push_back(q);
					}
					dec.push_back(char('0' + rem));
					digits = std::move(next);
				}
				std::reverse(dec.begin(), dec.end());
			}
			if (neg) dec.insert(dec.begin(), '-');
			_json["value"] = dec;
		}
		else
			_json["value"] = s;
	}
	_json["teal_alias"] = nullptr;
}

void AWSTSerializer::serializeFields(awst::BoolConstant const& _node, njson& _json)
{
	_json["value"] = _node.value;
}

void AWSTSerializer::serializeFields(awst::BytesConstant const& _node, njson& _json)
{
	_json["value"] = base85Encode(_node.value);
	_json["encoding"] = bytesEncodingToString(_node.encoding);
}

void AWSTSerializer::serializeFields(awst::StringConstant const& _node, njson& _json)
{
	_json["value"] = _node.value;
}

void AWSTSerializer::serializeFields(awst::VoidConstant const&, njson&)
{
	// no extra fields
}

void AWSTSerializer::serializeFields(awst::VarExpression const& _node, njson& _json)
{
	_json["name"] = _node.name;
}

void AWSTSerializer::serializeFields(awst::UInt64BinaryOperation const& _node, njson& _json)
{
	_json["left"] = serializeExpression(*_node.left);
	_json["op"] = uint64BinOpToString(_node.op);
	_json["right"] = serializeExpression(*_node.right);
}

void AWSTSerializer::serializeFields(awst::BigUIntBinaryOperation const& _node, njson& _json)
{
	_json["left"] = serializeExpression(*_node.left);
	_json["op"] = bigUIntBinOpToString(_node.op);
	_json["right"] = serializeExpression(*_node.right);
}

void AWSTSerializer::serializeFields(awst::BytesBinaryOperation const& _node, njson& _json)
{
	_json["left"] = serializeExpression(*_node.left);
	_json["op"] = bytesBinOpToString(_node.op);
	_json["right"] = serializeExpression(*_node.right);
}

void AWSTSerializer::serializeFields(awst::BytesUnaryOperation const& _node, njson& _json)
{
	_json["op"] = "~";
	_json["expr"] = serializeExpression(*_node.expr);
}

void AWSTSerializer::serializeFields(awst::NumericComparisonExpression const& _node, njson& _json)
{
	_json["lhs"] = serializeExpression(*_node.lhs);
	_json["operator"] = numericCompToString(_node.op);
	_json["rhs"] = serializeExpression(*_node.rhs);
}

void AWSTSerializer::serializeFields(awst::BytesComparisonExpression const& _node, njson& _json)
{
	_json["lhs"] = serializeExpression(*_node.lhs);
	_json["operator"] = equalityCompToString(_node.op);
	_json["rhs"] = serializeExpression(*_node.rhs);
}

void AWSTSerializer::serializeFields(awst::BooleanBinaryOperation const& _node, njson& _json)
{
	_json["left"] = serializeExpression(*_node.left);
	_json["op"] = boolBinOpToString(_node.op);
	_json["right"] = serializeExpression(*_node.right);
}

void AWSTSerializer::serializeFields(awst::Not const& _node, njson& _json)
{
	_json["expr"] = serializeExpression(*_node.expr);
}

void AWSTSerializer::serializeFields(awst::AssertExpression const& _node, njson& _json)
{
	_json["condition"] = _node.condition ? serializeExpression(*_node.condition) : njson(nullptr);
	_json["error_message"] = _node.errorMessage.has_value()
		? njson(_node.errorMessage.value())
		: njson(nullptr);
	_json["comment"] = _node.errorMessage.has_value()
		? njson(_node.errorMessage.value())
		: njson(nullptr);
	_json["explicit"] = _node.isExplicit;
}

void AWSTSerializer::serializeFields(awst::AssignmentExpression const& _node, njson& _json)
{
	_json["target"] = serializeExpression(*_node.target);
	_json["value"] = serializeExpression(*_node.value);
}

void AWSTSerializer::serializeFields(awst::ConditionalExpression const& _node, njson& _json)
{
	_json["condition"] = serializeExpression(*_node.condition);
	_json["true_expr"] = serializeExpression(*_node.trueExpr);
	_json["false_expr"] = serializeExpression(*_node.falseExpr);
}

void AWSTSerializer::serializeFields(awst::SubroutineCallExpression const& _node, njson& _json)
{
	_json["target"] = serializeSubroutineTarget(_node.target);
	njson args = njson::array();
	for (auto const& arg: _node.args)
		args.push_back(serializeCallArg(arg));
	_json["args"] = args;
}

void AWSTSerializer::serializeFields(awst::IntrinsicCall const& _node, njson& _json)
{
	_json["op_code"] = _node.opCode;
	njson imms = njson::array();
	for (auto const& imm: _node.immediates)
	{
		if (auto const* s = std::get_if<std::string>(&imm))
			imms.push_back(*s);
		else if (auto const* i = std::get_if<int>(&imm))
			imms.push_back(*i);
	}
	_json["immediates"] = imms;
	njson sargs = njson::array();
	for (auto const& sa: _node.stackArgs)
		sargs.push_back(serializeExpression(*sa));
	_json["stack_args"] = sargs;
}

void AWSTSerializer::serializeFields(awst::FieldExpression const& _node, njson& _json)
{
	_json["base"] = serializeExpression(*_node.base);
	_json["name"] = _node.name;
}

void AWSTSerializer::serializeFields(awst::IndexExpression const& _node, njson& _json)
{
	_json["base"] = serializeExpression(*_node.base);
	_json["index"] = serializeExpression(*_node.index);
}

void AWSTSerializer::serializeFields(awst::TupleExpression const& _node, njson& _json)
{
	njson items = njson::array();
	for (auto const& item: _node.items)
		items.push_back(serializeExpression(*item));
	_json["items"] = items;
}

void AWSTSerializer::serializeFields(awst::TupleItemExpression const& _node, njson& _json)
{
	_json["base"] = serializeExpression(*_node.base);
	_json["index"] = _node.index;
}

void AWSTSerializer::serializeFields(awst::ARC4Encode const& _node, njson& _json)
{
	_json["value"] = serializeExpression(*_node.value);
	_json["error_message"] = nullptr;
}

void AWSTSerializer::serializeFields(awst::ARC4Decode const& _node, njson& _json)
{
	_json["value"] = serializeExpression(*_node.value);
	_json["error_message"] = nullptr;
}

void AWSTSerializer::serializeFields(awst::ARC4FromBytes const& _node, njson& _json)
{
	_json["value"] = serializeExpression(*_node.value);
	_json["validate"] = _node.validate;
}

void AWSTSerializer::serializeFields(awst::ARC4Router const&, njson&)
{
	// no extra fields
}

void AWSTSerializer::serializeFields(awst::ReinterpretCast const& _node, njson& _json)
{
	_json["expr"] = serializeExpression(*_node.expr);
}

void AWSTSerializer::serializeFields(awst::TemplateVar const& _node, njson& _json)
{
	_json["name"] = _node.name;
}

void AWSTSerializer::serializeFields(awst::Copy const& _node, njson& _json)
{
	_json["value"] = serializeExpression(*_node.value);
}

void AWSTSerializer::serializeFields(awst::SingleEvaluation const& _node, njson& _json)
{
	_json["source"] = serializeExpression(*_node.source);
	// cattrs keys on `_id`, not `id` — emit `_id` so the single-eval cache
	// actually deduplicates. Emitting `id` left `_id` at id(self) each time.
	_json["_id"] = _node.id;
}

void AWSTSerializer::serializeFields(awst::CheckedMaybe const& _node, njson& _json)
{
	_json["expr"] = serializeExpression(*_node.expr);
	_json["comment"] = _node.comment;
}

void AWSTSerializer::serializeFields(awst::Emit const& _node, njson& _json)
{
	_json["signature"] = _node.signature;
	_json["value"] = serializeExpression(*_node.value);
}

void AWSTSerializer::serializeFields(awst::AppStateExpression const& _node, njson& _json)
{
	_json["key"] = serializeExpression(*_node.key);
	_json["exists_assertion_message"] = _node.existsAssertionMessage.has_value()
		? njson(_node.existsAssertionMessage.value())
		: njson(nullptr);
}

void AWSTSerializer::serializeFields(awst::AppAccountStateExpression const& _node, njson& _json)
{
	_json["key"] = serializeExpression(*_node.key);
	_json["account"] = serializeExpression(*_node.account);
	_json["exists_assertion_message"] = _node.existsAssertionMessage.has_value()
		? njson(_node.existsAssertionMessage.value())
		: njson(nullptr);
}

void AWSTSerializer::serializeFields(awst::BoxValueExpression const& _node, njson& _json)
{
	_json["key"] = serializeExpression(*_node.key);
	_json["exists_assertion_message"] = _node.existsAssertionMessage.has_value()
		? njson(_node.existsAssertionMessage.value())
		: njson(nullptr);
}

void AWSTSerializer::serializeFields(awst::StateGet const& _node, njson& _json)
{
	emitStateField(*_node.field, _json);
	_json["default"] = serializeExpression(*_node.defaultValue);
}

void AWSTSerializer::serializeFields(awst::StateExists const& _node, njson& _json)
{
	emitStateField(*_node.field, _json);
}

void AWSTSerializer::serializeFields(awst::StateDelete const& _node, njson& _json)
{
	_json["field"] = serializeExpression(*_node.field);
}

void AWSTSerializer::serializeFields(awst::StateGetEx const& _node, njson& _json)
{
	emitStateField(*_node.field, _json);
}

void AWSTSerializer::serializeFields(awst::NewArray const& _node, njson& _json)
{
	njson values = njson::array();
	for (auto const& v: _node.values)
		values.push_back(serializeExpression(*v));
	_json["values"] = values;
}

void AWSTSerializer::serializeFields(awst::ArrayLength const& _node, njson& _json)
{
	_json["array"] = serializeExpression(*_node.array);
}

void AWSTSerializer::serializeFields(awst::ArrayPop const& _node, njson& _json)
{
	_json["base"] = serializeExpression(*_node.base);
}

void AWSTSerializer::serializeFields(awst::ArrayConcat const& _node, njson& _json)
{
	_json["left"] = serializeExpression(*_node.left);
	_json["right"] = serializeExpression(*_node.right);
}

void AWSTSerializer::serializeFields(awst::ArrayExtend const& _node, njson& _json)
{
	_json["base"] = serializeExpression(*_node.base);
	_json["other"] = serializeExpression(*_node.other);
}

void AWSTSerializer::serializeFields(awst::ConvertArray const& _node, njson& _json)
{
	_json["expr"] = serializeExpression(*_node.expr);
}

void AWSTSerializer::serializeFields(awst::NewStruct const& _node, njson& _json)
{
	njson vals = njson::object();
	for (auto const& [k, v]: _node.values)
		vals[k] = serializeExpression(*v);
	_json["values"] = vals;
}

void AWSTSerializer::serializeFields(awst::NamedTupleExpression const& _node, njson& _json)
{
	njson vals;
	for (auto const& [k, v]: _node.values)
		vals[k] = serializeExpression(*v);
	_json["values"] = vals;
}

void AWSTSerializer::serializeFields(awst::CreateInnerTransaction const& _node, njson& _json)
{
	njson fields;
	for (auto const& [k, v]: _node.fields)
		fields[k] = serializeExpression(*v);
	_json["fields"] = fields;
}

void AWSTSerializer::serializeFields(awst::SubmitInnerTransaction const& _node, njson& _json)
{
	njson itxns = njson::array();
	for (auto const& itxn: _node.itxns)
		itxns.push_back(serializeExpression(*itxn));
	_json["itxns"] = itxns;
}

void AWSTSerializer::serializeFields(awst::InnerTransactionField const& _node, njson& _json)
{
	_json["itxn"] = serializeExpression(*_node.itxn);
	_json["field"] = _node.field;
	_json["array_index"] = _node.arrayIndex ? serializeExpression(*_node.arrayIndex) : njson(nullptr);
}

void AWSTSerializer::serializeFields(awst::CommaExpression const& _node, njson& _json)
{
	njson exprs = njson::array();
	for (auto const& ex: _node.expressions)
		exprs.push_back(serializeExpression(*ex));
	_json["expressions"] = exprs;
}

void AWSTSerializer::serializeFields(awst::MethodConstant const& _node, njson& _json)
{
	njson methodSig;
	methodSig["_type"] = "MethodSignatureString";
	methodSig["source_location"] = serializeSourceLocation(_node.sourceLocation);
	methodSig["value"] = _node.value;
	_json["value"] = std::move(methodSig);
}

void AWSTSerializer::serializeFields(awst::AddressConstant const& _node, njson& _json)
{
	_json["value"] = _node.value;
}

void AWSTSerializer::serializeFields(awst::PuyaLibCall const& _node, njson& _json)
{
	_json.erase("wtype");
	_json["func"] = _node.func;
	njson args = njson::array();
	for (auto const& arg: _node.args)
		args.push_back(serializeCallArg(arg));
	_json["args"] = args;
}

njson AWSTSerializer::serializeStatement(awst::Statement const& _stmt)
{
	njson j;
	j["_type"] = _stmt.nodeType();
	j["source_location"] = serializeSourceLocation(_stmt.sourceLocation);

#define PUYASOL_AWST_SERIALIZE_ARM(Node) \
	if (auto const* node = dynamic_cast<awst::Node const*>(&_stmt)) \
	{ \
		serializeFields(*node, j); \
		return j; \
	}
	PUYASOL_AWST_STATEMENT_NODES(PUYASOL_AWST_SERIALIZE_ARM)
#undef PUYASOL_AWST_SERIALIZE_ARM

	return j;
}

void AWSTSerializer::serializeFields(awst::Block const& _node, njson& _json)
{
	_json = serializeBlock(_node);
}

void AWSTSerializer::serializeFields(awst::ExpressionStatement const& _node, njson& _json)
{
	_json["expr"] = serializeExpression(*_node.expr);
}

void AWSTSerializer::serializeFields(awst::ReturnStatement const& _node, njson& _json)
{
	_json["value"] = _node.value ? serializeExpression(*_node.value) : njson(nullptr);
}

void AWSTSerializer::serializeFields(awst::IfElse const& _node, njson& _json)
{
	_json["condition"] = serializeExpression(*_node.condition);
	_json["if_branch"] = serializeBlock(*_node.ifBranch);
	_json["else_branch"] = _node.elseBranch ? serializeBlock(*_node.elseBranch) : njson(nullptr);
}

void AWSTSerializer::serializeFields(awst::WhileLoop const& _node, njson& _json)
{
	_json["condition"] = serializeExpression(*_node.condition);
	_json["loop_body"] = serializeBlock(*_node.loopBody);
}

void AWSTSerializer::serializeFields(awst::LoopExit const&, njson&)
{
	// no extra fields
}

void AWSTSerializer::serializeFields(awst::LoopContinue const&, njson&)
{
	// no extra fields
}

void AWSTSerializer::serializeFields(awst::AssignmentStatement const& _node, njson& _json)
{
	_json["target"] = serializeExpression(*_node.target);
	_json["value"] = serializeExpression(*_node.value);
}

void AWSTSerializer::serializeFields(awst::Goto const& _node, njson& _json)
{
	_json["target"] = _node.target;
}

void AWSTSerializer::serializeFields(awst::Switch const& _node, njson& _json)
{
	_json["value"] = serializeExpression(*_node.value);
	njson cases = njson::array();
	for (auto const& [expr, block]: _node.cases)
	{
		njson c = njson::array();
		c.push_back(serializeExpression(*expr));
		c.push_back(serializeBlock(*block));
		cases.push_back(c);
	}
	_json["cases"] = cases;
	_json["default_case"] = _node.defaultCase ? serializeBlock(*_node.defaultCase) : njson(nullptr);
}

void AWSTSerializer::serializeFields(awst::ForInLoop const& _node, njson& _json)
{
	_json["sequence"] = serializeExpression(*_node.sequence);
	_json["items"] = serializeExpression(*_node.items);
	_json["loop_body"] = serializeBlock(*_node.loopBody);
}

void AWSTSerializer::serializeFields(awst::UInt64AugmentedAssignment const& _node, njson& _json)
{
	_json["target"] = serializeExpression(*_node.target);
	_json["op"] = uint64BinOpToString(_node.op);
	_json["value"] = serializeExpression(*_node.value);
}

void AWSTSerializer::serializeFields(awst::BigUIntAugmentedAssignment const& _node, njson& _json)
{
	_json["target"] = serializeExpression(*_node.target);
	_json["op"] = bigUIntBinOpToString(_node.op);
	_json["value"] = serializeExpression(*_node.value);
}

njson AWSTSerializer::serializeSourceLocation(awst::SourceLocation const& _loc)
{
	njson j;
	j["file"] = _loc.file.empty() ? njson(nullptr) : njson(_loc.file);
	j["line"] = std::max(_loc.line, 1);
	j["end_line"] = std::max(_loc.endLine, 1);
	j["comment_lines"] = _loc.commentLines;
	j["column"] = _loc.column.has_value() ? njson(_loc.column.value()) : njson(nullptr);
	j["end_column"] = _loc.endColumn.has_value() ? njson(_loc.endColumn.value()) : njson(nullptr);
	return j;
}

njson AWSTSerializer::serializeWType(awst::WType const* _type)
{
	if (!_type)
		return njson(nullptr);

	njson j;
	j["_type"] = _type->jsonType();
	j["name"] = _type->name();
	j["immutable"] = _type->immutable();

	switch (_type->kind())
	{
	case awst::WTypeKind::Bytes:
	{
		auto const* bt = static_cast<awst::BytesWType const*>(_type);
		j["length"] = bt->length().has_value() ? njson(bt->length().value()) : njson(nullptr);
		break;
	}
	case awst::WTypeKind::ARC4UIntN:
	{
		auto const* at = static_cast<awst::ARC4UIntN const*>(_type);
		j["n"] = at->n();
		j["arc4_alias"] = at->arc4Alias().empty() ? njson(nullptr) : njson(at->arc4Alias());
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::ARC4UFixedNxM:
	{
		auto const* at = static_cast<awst::ARC4UFixedNxM const*>(_type);
		j["n"] = at->n();
		j["m"] = at->m();
		j["arc4_alias"] = nullptr;
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::ARC4Tuple:
	{
		auto const* at = static_cast<awst::ARC4Tuple const*>(_type);
		njson types = njson::array();
		for (auto const* t: at->types())
			types.push_back(serializeWType(t));
		j["types"] = types;
		j["arc4_alias"] = nullptr;
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::ARC4DynamicArray:
	{
		auto const* at = static_cast<awst::ARC4DynamicArray const*>(_type);
		j["element_type"] = serializeWType(at->elementType());
		j["arc4_alias"] = at->arc4Alias().empty() ? nlohmann::json(nullptr) : nlohmann::json(at->arc4Alias());
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* at = static_cast<awst::ARC4StaticArray const*>(_type);
		j["element_type"] = serializeWType(at->elementType());
		j["array_size"] = at->arraySize();
		j["arc4_alias"] = at->arc4Alias().empty() ? nlohmann::json(nullptr) : nlohmann::json(at->arc4Alias());
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* at = static_cast<awst::ARC4Struct const*>(_type);
		// puya 5.x expects fields as WTypeField array (port of e293832fe).
		njson fields = njson::array();
		for (auto const& [k, v]: at->fields())
		{
			njson field;
			field["name"] = k;
			field["wtype"] = serializeWType(v);
			field["description"] = nullptr;
			fields.push_back(std::move(field));
		}
		j["fields"] = std::move(fields);
		j["frozen"] = at->frozen();
		j["arc4_alias"] = nullptr;
		j["source_location"] = nullptr;
		j["desc"] = nullptr;
		break;
	}
	case awst::WTypeKind::ReferenceArray:
	{
		auto const* at = static_cast<awst::ReferenceArray const*>(_type);
		j["element_type"] = serializeWType(at->elementType());
		if (at->arraySize().has_value())
			j["array_size"] = at->arraySize().value();
		else
			j["array_size"] = nullptr;
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::WTuple:
	{
		auto const* at = static_cast<awst::WTuple const*>(_type);
		njson types = njson::array();
		for (auto const* t: at->types())
			types.push_back(serializeWType(t));
		j["types"] = types;
		if (at->names().has_value())
			j["names"] = njson(at->names().value());
		else
			j["names"] = nullptr;
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::WInnerTransactionFields:
	{
		auto const* itf = static_cast<awst::WInnerTransactionFields const*>(_type);
		j["transaction_type"] = itf->transactionType().has_value()
			? njson(itf->transactionType().value()) : njson(nullptr);
		break;
	}
	case awst::WTypeKind::WInnerTransaction:
	{
		auto const* it = static_cast<awst::WInnerTransaction const*>(_type);
		j["transaction_type"] = it->transactionType().has_value()
			? njson(it->transactionType().value()) : njson(nullptr);
		break;
	}
	default:
		if (std::string_view{_type->jsonType()} == "ARC4Type")
		{
			j["arc4_alias"] = nullptr;
			j["source_location"] = nullptr;
		}
		break;
	}

	return j;
}

njson AWSTSerializer::serializeARC4MethodConfig(awst::ARC4MethodConfig const& _config)
{
	njson j;

	if (auto const* bare = std::get_if<awst::ARC4BareMethodConfig>(&_config))
	{
		j["_type"] = "ARC4BareMethodConfig";
		j["source_location"] = serializeSourceLocation(bare->sourceLocation);
		j["allowed_completion_types"] = njson(bare->allowedCompletionTypes);
		j["create"] = bare->create;
	}
	else if (auto const* abi = std::get_if<awst::ARC4ABIMethodConfig>(&_config))
	{
		j["_type"] = "ARC4ABIMethodConfig";
		j["source_location"] = serializeSourceLocation(abi->sourceLocation);
		j["allowed_completion_types"] = njson(abi->allowedCompletionTypes);
		j["create"] = abi->create;
		j["name"] = abi->name;
		j["readonly"] = abi->readonly;
		j["default_args"] = njson(abi->defaultArgs);
		j["resource_encoding"] = "value";
		j["validate_encoding"] = nullptr;
	}

	return j;
}

njson AWSTSerializer::serializeSubroutineTarget(awst::SubroutineTarget const& _target)
{
	njson j;

	if (auto const* sub = std::get_if<awst::SubroutineID>(&_target))
	{
		j["_type"] = "SubroutineID";
		j["target"] = sub->target;
	}
	else if (auto const* inst = std::get_if<awst::InstanceMethodTarget>(&_target))
	{
		j["_type"] = "InstanceMethodTarget";
		j["member_name"] = inst->memberName;
	}
	else if (auto const* sup = std::get_if<awst::InstanceSuperMethodTarget>(&_target))
	{
		j["_type"] = "InstanceSuperMethodTarget";
		j["member_name"] = sup->memberName;
	}
	else if (auto const* ct = std::get_if<awst::ContractMethodTarget>(&_target))
	{
		j["_type"] = "ContractMethodTarget";
		j["cref"] = ct->cref;
		j["member_name"] = ct->memberName;
	}

	return j;
}

njson AWSTSerializer::serializeAppStorageDefinition(awst::AppStorageDefinition const& _def)
{
	njson j;
	j["_type"] = "AppStorageDefinition";
	j["source_location"] = serializeSourceLocation(_def.sourceLocation);
	j["member_name"] = _def.memberName;
	j["storage_wtype"] = serializeWType(_def.storageWType);
	j["key"] = _def.key ? serializeExpression(*_def.key) : njson(nullptr);
	j["key_wtype"] = _def.isMap
		? serializeWType(awst::WType::boxKeyType())
		: njson(nullptr);
	j["description"] = _def.description.has_value()
		? njson(_def.description.value())
		: njson(nullptr);

	switch (_def.storageKind)
	{
	case awst::AppStorageKind::AppGlobal:
		j["kind"] = 1;
		break;
	case awst::AppStorageKind::AccountLocal:
		j["kind"] = 2;
		break;
	case awst::AppStorageKind::Box:
		j["kind"] = 3;
		break;
	}

	return j;
}

njson AWSTSerializer::serializeMethodDocumentation(awst::MethodDocumentation const& _doc)
{
	njson j;
	j["description"] = _doc.description.has_value()
		? njson(_doc.description.value())
		: njson(nullptr);
	j["args"] = njson(_doc.args);
	j["returns"] = _doc.returns.has_value()
		? njson(_doc.returns.value())
		: njson(nullptr);
	return j;
}

njson AWSTSerializer::serializeCallArg(awst::CallArg const& _arg)
{
	njson j;
	j["name"] = _arg.name.has_value() ? njson(_arg.name.value()) : njson(nullptr);
	j["value"] = serializeExpression(*_arg.value);
	return j;
}

njson AWSTSerializer::serializeSubroutineArgument(awst::SubroutineArgument const& _arg)
{
	njson j;
	j["_type"] = "SubroutineArgument";
	j["name"] = _arg.name;
	j["source_location"] = serializeSourceLocation(_arg.sourceLocation);
	j["wtype"] = serializeWType(_arg.wtype);
	return j;
}

njson AWSTSerializer::serializeBlock(awst::Block const& _block)
{
	njson j;
	j["_type"] = "Block";
	j["source_location"] = serializeSourceLocation(_block.sourceLocation);
	njson body = njson::array();
	for (auto const& stmt: _block.body)
		body.push_back(serializeStatement(*stmt));
	j["body"] = body;
	j["label"] = _block.label.has_value() ? njson(_block.label.value()) : njson(nullptr);
	j["comment"] = _block.comment.has_value() ? njson(_block.comment.value()) : njson(nullptr);
	return j;
}

} // namespace puyasol::json
