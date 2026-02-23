#include "json/AWSTSerializer.h"
#include "json/Base85.h"

namespace puyasol::json
{

using njson = nlohmann::json;

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

std::string storageKindToString(awst::AppStorageKind _kind)
{
	switch (_kind)
	{
	case awst::AppStorageKind::AppGlobal: return "app_global";
	case awst::AppStorageKind::AccountLocal: return "account_local";
	case awst::AppStorageKind::Box: return "box";
	}
	return "app_global";
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
	if (auto const* sub = dynamic_cast<awst::Subroutine const*>(&_node))
		return serializeSubroutine(*sub);
	return njson::object();
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

	if (auto const* e = dynamic_cast<awst::IntegerConstant const*>(&_expr))
	{
		// value is stored as string for biguint support, but puya expects int
		try
		{
			j["value"] = std::stoll(e->value);
		}
		catch (...)
		{
			j["value"] = e->value;
		}
		j["teal_alias"] = nullptr;
	}
	else if (auto const* e = dynamic_cast<awst::BoolConstant const*>(&_expr))
	{
		j["value"] = e->value;
	}
	else if (auto const* e = dynamic_cast<awst::BytesConstant const*>(&_expr))
	{
		j["value"] = base85Encode(e->value);
		j["encoding"] = bytesEncodingToString(e->encoding);
	}
	else if (auto const* e = dynamic_cast<awst::StringConstant const*>(&_expr))
	{
		j["value"] = e->value;
	}
	else if (dynamic_cast<awst::VoidConstant const*>(&_expr))
	{
		// no extra fields
	}
	else if (auto const* e = dynamic_cast<awst::VarExpression const*>(&_expr))
	{
		j["name"] = e->name;
	}
	else if (auto const* e = dynamic_cast<awst::UInt64BinaryOperation const*>(&_expr))
	{
		j["left"] = serializeExpression(*e->left);
		j["op"] = uint64BinOpToString(e->op);
		j["right"] = serializeExpression(*e->right);
	}
	else if (auto const* e = dynamic_cast<awst::BigUIntBinaryOperation const*>(&_expr))
	{
		j["left"] = serializeExpression(*e->left);
		j["op"] = bigUIntBinOpToString(e->op);
		j["right"] = serializeExpression(*e->right);
	}
	else if (auto const* e = dynamic_cast<awst::BytesBinaryOperation const*>(&_expr))
	{
		j["left"] = serializeExpression(*e->left);
		j["op"] = bytesBinOpToString(e->op);
		j["right"] = serializeExpression(*e->right);
	}
	else if (auto const* e = dynamic_cast<awst::NumericComparisonExpression const*>(&_expr))
	{
		j["lhs"] = serializeExpression(*e->lhs);
		j["operator"] = numericCompToString(e->op);
		j["rhs"] = serializeExpression(*e->rhs);
	}
	else if (auto const* e = dynamic_cast<awst::BytesComparisonExpression const*>(&_expr))
	{
		j["lhs"] = serializeExpression(*e->lhs);
		j["operator"] = equalityCompToString(e->op);
		j["rhs"] = serializeExpression(*e->rhs);
	}
	else if (auto const* e = dynamic_cast<awst::BooleanBinaryOperation const*>(&_expr))
	{
		j["left"] = serializeExpression(*e->left);
		j["op"] = boolBinOpToString(e->op);
		j["right"] = serializeExpression(*e->right);
	}
	else if (auto const* e = dynamic_cast<awst::Not const*>(&_expr))
	{
		j["expr"] = serializeExpression(*e->expr);
	}
	else if (auto const* e = dynamic_cast<awst::AssertExpression const*>(&_expr))
	{
		j["condition"] = e->condition ? serializeExpression(*e->condition) : njson(nullptr);
		j["error_message"] = e->errorMessage.has_value()
			? njson(e->errorMessage.value())
			: njson(nullptr);
		j["comment"] = e->errorMessage.has_value()
			? njson(e->errorMessage.value())
			: njson(nullptr);
	}
	else if (auto const* e = dynamic_cast<awst::AssignmentExpression const*>(&_expr))
	{
		j["target"] = serializeExpression(*e->target);
		j["value"] = serializeExpression(*e->value);
	}
	else if (auto const* e = dynamic_cast<awst::ConditionalExpression const*>(&_expr))
	{
		j["condition"] = serializeExpression(*e->condition);
		j["true_expr"] = serializeExpression(*e->trueExpr);
		j["false_expr"] = serializeExpression(*e->falseExpr);
	}
	else if (auto const* e = dynamic_cast<awst::SubroutineCallExpression const*>(&_expr))
	{
		j["target"] = serializeSubroutineTarget(e->target);
		njson args = njson::array();
		for (auto const& arg: e->args)
			args.push_back(serializeCallArg(arg));
		j["args"] = args;
	}
	else if (auto const* e = dynamic_cast<awst::IntrinsicCall const*>(&_expr))
	{
		j["op_code"] = e->opCode;
		njson imms = njson::array();
		for (auto const& imm: e->immediates)
		{
			if (auto const* s = std::get_if<std::string>(&imm))
				imms.push_back(*s);
			else if (auto const* i = std::get_if<int>(&imm))
				imms.push_back(*i);
		}
		j["immediates"] = imms;
		njson sargs = njson::array();
		for (auto const& sa: e->stackArgs)
			sargs.push_back(serializeExpression(*sa));
		j["stack_args"] = sargs;
	}
	else if (auto const* e = dynamic_cast<awst::FieldExpression const*>(&_expr))
	{
		j["base"] = serializeExpression(*e->base);
		j["name"] = e->name;
	}
	else if (auto const* e = dynamic_cast<awst::IndexExpression const*>(&_expr))
	{
		j["base"] = serializeExpression(*e->base);
		j["index"] = serializeExpression(*e->index);
	}
	else if (auto const* e = dynamic_cast<awst::TupleExpression const*>(&_expr))
	{
		njson items = njson::array();
		for (auto const& item: e->items)
			items.push_back(serializeExpression(*item));
		j["items"] = items;
	}
	else if (auto const* e = dynamic_cast<awst::TupleItemExpression const*>(&_expr))
	{
		j["base"] = serializeExpression(*e->base);
		j["index"] = e->index;
	}
	else if (auto const* e = dynamic_cast<awst::ARC4Encode const*>(&_expr))
	{
		j["value"] = serializeExpression(*e->value);
		j["error_message"] = nullptr;
	}
	else if (auto const* e = dynamic_cast<awst::ARC4Decode const*>(&_expr))
	{
		j["value"] = serializeExpression(*e->value);
		j["error_message"] = nullptr;
	}
	else if (dynamic_cast<awst::ARC4Router const*>(&_expr))
	{
		// no extra fields
	}
	else if (auto const* e = dynamic_cast<awst::ReinterpretCast const*>(&_expr))
	{
		j["expr"] = serializeExpression(*e->expr);
	}
	else if (auto const* e = dynamic_cast<awst::Copy const*>(&_expr))
	{
		j["value"] = serializeExpression(*e->value);
	}
	else if (auto const* e = dynamic_cast<awst::SingleEvaluation const*>(&_expr))
	{
		j["source"] = serializeExpression(*e->source);
		j["id"] = e->id;
	}
	else if (auto const* e = dynamic_cast<awst::CheckedMaybe const*>(&_expr))
	{
		j["expr"] = serializeExpression(*e->expr);
		j["comment"] = e->comment;
	}
	else if (auto const* e = dynamic_cast<awst::Emit const*>(&_expr))
	{
		j["signature"] = e->signature;
		j["value"] = serializeExpression(*e->value);
	}
	else if (auto const* e = dynamic_cast<awst::BoxPrefixedKeyExpression const*>(&_expr))
	{
		j["prefix"] = serializeExpression(*e->prefix);
		j["key"] = serializeExpression(*e->key);
	}
	else if (auto const* e = dynamic_cast<awst::AppStateExpression const*>(&_expr))
	{
		j["key"] = serializeExpression(*e->key);
		j["exists_assertion_message"] = e->existsAssertionMessage.has_value()
			? njson(e->existsAssertionMessage.value())
			: njson(nullptr);
	}
	else if (auto const* e = dynamic_cast<awst::AppAccountStateExpression const*>(&_expr))
	{
		j["key"] = serializeExpression(*e->key);
		j["account"] = serializeExpression(*e->account);
		j["exists_assertion_message"] = e->existsAssertionMessage.has_value()
			? njson(e->existsAssertionMessage.value())
			: njson(nullptr);
	}
	else if (auto const* e = dynamic_cast<awst::BoxValueExpression const*>(&_expr))
	{
		j["key"] = serializeExpression(*e->key);
		j["exists_assertion_message"] = e->existsAssertionMessage.has_value()
			? njson(e->existsAssertionMessage.value())
			: njson(nullptr);
	}
	else if (auto const* e = dynamic_cast<awst::StateGet const*>(&_expr))
	{
		// wtype is init=False in Puya (derived from field.wtype), so exclude it
		j.erase("wtype");
		j["field"] = serializeExpression(*e->field);
		j["default"] = serializeExpression(*e->defaultValue);
	}
	else if (auto const* e = dynamic_cast<awst::StateExists const*>(&_expr))
	{
		// wtype is init=False in Puya (always bool_wtype), so exclude it
		j.erase("wtype");
		j["field"] = serializeExpression(*e->field);
	}
	else if (auto const* e = dynamic_cast<awst::StateDelete const*>(&_expr))
	{
		j["field"] = serializeExpression(*e->field);
	}
	else if (auto const* e = dynamic_cast<awst::StateGetEx const*>(&_expr))
	{
		// wtype is init=False in Puya (derived from field.wtype), so exclude it
		j.erase("wtype");
		j["field"] = serializeExpression(*e->field);
	}
	else if (auto const* e = dynamic_cast<awst::NewArray const*>(&_expr))
	{
		njson values = njson::array();
		for (auto const& v: e->values)
			values.push_back(serializeExpression(*v));
		j["values"] = values;
	}
	else if (auto const* e = dynamic_cast<awst::ArrayLength const*>(&_expr))
	{
		j["array"] = serializeExpression(*e->array);
	}
	else if (auto const* e = dynamic_cast<awst::ArrayPop const*>(&_expr))
	{
		j["base"] = serializeExpression(*e->base);
	}
	else if (auto const* e = dynamic_cast<awst::ArrayConcat const*>(&_expr))
	{
		j["left"] = serializeExpression(*e->left);
		j["right"] = serializeExpression(*e->right);
	}
	else if (auto const* e = dynamic_cast<awst::ArrayExtend const*>(&_expr))
	{
		j["base"] = serializeExpression(*e->base);
		j["other"] = serializeExpression(*e->other);
	}
	else if (auto const* e = dynamic_cast<awst::NewStruct const*>(&_expr))
	{
		njson vals;
		for (auto const& [k, v]: e->values)
			vals[k] = serializeExpression(*v);
		j["values"] = vals;
	}
	else if (auto const* e = dynamic_cast<awst::NamedTupleExpression const*>(&_expr))
	{
		njson vals;
		for (auto const& [k, v]: e->values)
			vals[k] = serializeExpression(*v);
		j["values"] = vals;
	}
	else if (auto const* e = dynamic_cast<awst::CreateInnerTransaction const*>(&_expr))
	{
		njson fields;
		for (auto const& [k, v]: e->fields)
			fields[k] = serializeExpression(*v);
		j["fields"] = fields;
	}
	else if (auto const* e = dynamic_cast<awst::SubmitInnerTransaction const*>(&_expr))
	{
		njson itxns = njson::array();
		for (auto const& itxn: e->itxns)
			itxns.push_back(serializeExpression(*itxn));
		j["itxns"] = itxns;
	}
	else if (auto const* e = dynamic_cast<awst::InnerTransactionField const*>(&_expr))
	{
		j["itxn"] = serializeExpression(*e->itxn);
		j["field"] = e->field;
		j["array_index"] = e->arrayIndex ? serializeExpression(*e->arrayIndex) : njson(nullptr);
	}
	else if (auto const* e = dynamic_cast<awst::CommaExpression const*>(&_expr))
	{
		njson exprs = njson::array();
		for (auto const& ex: e->expressions)
			exprs.push_back(serializeExpression(*ex));
		j["expressions"] = exprs;
	}
	else if (auto const* e = dynamic_cast<awst::MethodConstant const*>(&_expr))
	{
		j["value"] = e->value;
	}
	else if (auto const* e = dynamic_cast<awst::AddressConstant const*>(&_expr))
	{
		j["value"] = e->value;
	}

	return j;
}

njson AWSTSerializer::serializeStatement(awst::Statement const& _stmt)
{
	njson j;
	j["_type"] = _stmt.nodeType();
	j["source_location"] = serializeSourceLocation(_stmt.sourceLocation);

	if (auto const* s = dynamic_cast<awst::Block const*>(&_stmt))
	{
		return serializeBlock(*s);
	}
	else if (auto const* s = dynamic_cast<awst::ExpressionStatement const*>(&_stmt))
	{
		j["expr"] = serializeExpression(*s->expr);
	}
	else if (auto const* s = dynamic_cast<awst::ReturnStatement const*>(&_stmt))
	{
		j["value"] = s->value ? serializeExpression(*s->value) : njson(nullptr);
	}
	else if (auto const* s = dynamic_cast<awst::IfElse const*>(&_stmt))
	{
		j["condition"] = serializeExpression(*s->condition);
		j["if_branch"] = serializeBlock(*s->ifBranch);
		j["else_branch"] = s->elseBranch ? serializeBlock(*s->elseBranch) : njson(nullptr);
	}
	else if (auto const* s = dynamic_cast<awst::WhileLoop const*>(&_stmt))
	{
		j["condition"] = serializeExpression(*s->condition);
		j["loop_body"] = serializeBlock(*s->loopBody);
	}
	else if (dynamic_cast<awst::LoopExit const*>(&_stmt))
	{
		// no extra fields
	}
	else if (dynamic_cast<awst::LoopContinue const*>(&_stmt))
	{
		// no extra fields
	}
	else if (auto const* s = dynamic_cast<awst::AssignmentStatement const*>(&_stmt))
	{
		j["target"] = serializeExpression(*s->target);
		j["value"] = serializeExpression(*s->value);
	}
	else if (auto const* s = dynamic_cast<awst::Goto const*>(&_stmt))
	{
		j["target"] = s->target;
	}
	else if (auto const* s = dynamic_cast<awst::Switch const*>(&_stmt))
	{
		j["value"] = serializeExpression(*s->value);
		njson cases = njson::array();
		for (auto const& [expr, block]: s->cases)
		{
			njson c = njson::array();
			c.push_back(serializeExpression(*expr));
			c.push_back(serializeBlock(*block));
			cases.push_back(c);
		}
		j["cases"] = cases;
		j["default_case"] = s->defaultCase ? serializeBlock(*s->defaultCase) : njson(nullptr);
	}
	else if (auto const* s = dynamic_cast<awst::ForInLoop const*>(&_stmt))
	{
		j["sequence"] = serializeExpression(*s->sequence);
		j["items"] = serializeExpression(*s->items);
		j["loop_body"] = serializeBlock(*s->loopBody);
	}
	else if (auto const* s = dynamic_cast<awst::UInt64AugmentedAssignment const*>(&_stmt))
	{
		j["target"] = serializeExpression(*s->target);
		j["op"] = uint64BinOpToString(s->op);
		j["value"] = serializeExpression(*s->value);
	}
	else if (auto const* s = dynamic_cast<awst::BigUIntAugmentedAssignment const*>(&_stmt))
	{
		j["target"] = serializeExpression(*s->target);
		j["op"] = bigUIntBinOpToString(s->op);
		j["value"] = serializeExpression(*s->value);
	}

	return j;
}

njson AWSTSerializer::serializeSourceLocation(awst::SourceLocation const& _loc)
{
	njson j;
	j["file"] = _loc.file.empty() ? njson(nullptr) : njson(_loc.file);
	j["line"] = _loc.line;
	j["end_line"] = _loc.endLine;
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
		j["arc4_alias"] = nullptr;
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
		j["arc4_alias"] = nullptr;
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* at = static_cast<awst::ARC4StaticArray const*>(_type);
		j["element_type"] = serializeWType(at->elementType());
		j["array_size"] = at->arraySize();
		j["arc4_alias"] = nullptr;
		j["source_location"] = nullptr;
		break;
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* at = static_cast<awst::ARC4Struct const*>(_type);
		njson fields;
		for (auto const& [k, v]: at->fields())
			fields[k] = serializeWType(v);
		j["fields"] = fields;
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
		// Basic types — no extra fields needed
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
