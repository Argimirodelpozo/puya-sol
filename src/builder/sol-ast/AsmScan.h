/// @file AsmScan.h
/// Recursive "does this subtree use inline assembly" query.

#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include <map>
#include <set>

namespace puyasol::builder
{

/// True iff the subtree contains an InlineAssembly node at ANY depth.
/// A naive `body().statements()` scan misses asm nested in a block
/// (`unchecked { assembly { ... } }` or a plain `{ }`), silently flipping
/// the asm-specific param/decode/storage-ref gates — every "does this
/// function use assembly" check must go through here so the gates agree.
inline bool containsInlineAssembly(solidity::frontend::ASTNode const& _node)
{
	class Scan: public solidity::frontend::ASTConstVisitor
	{
	public:
		bool found = false;
		bool visit(solidity::frontend::InlineAssembly const&) override
		{
			found = true;
			return false;
		}
	};
	Scan s;
	_node.accept(s);
	return s.found;
}

/// Param indices of `_func` that are STORAGE-ref STRUCT params referenced via
/// `.slot` in an inline-assembly block — the solady storage-library idiom
/// `function op(S storage s) { assembly { sload(s.slot) } }`. Such params must
/// travel as a box-key handle (not a struct value) so `s.slot` resolves; see
/// memory asm-slot-storage-ref-param. Returns the set of matching indices.
inline std::set<size_t> structRefParamsUsedAsAsmSlot(
	solidity::frontend::FunctionDefinition const& _func)
{
	using namespace solidity::frontend;
	std::set<size_t> result;
	if (!_func.isImplemented())
		return result;
	std::map<int64_t, size_t> paramById; // storage-ref struct params only
	for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
	{
		auto const& p = _func.parameters()[pi];
		if (p && p->referenceLocation() == VariableDeclaration::Location::Storage
			&& dynamic_cast<StructType const*>(p->type()))
			paramById[p->id()] = pi;
	}
	if (paramById.empty())
		return result;

	class Scan: public ASTConstVisitor
	{
	public:
		std::map<int64_t, size_t> const& byId;
		std::set<size_t>& out;
		Scan(std::map<int64_t, size_t> const& _b, std::set<size_t>& _o): byId(_b), out(_o) {}
		bool visit(InlineAssembly const& _asm) override
		{
			for (auto const& [yulId, extInfo]: _asm.annotation().externalReferences)
			{
				(void)yulId;
				if (extInfo.suffix != "slot" || !extInfo.declaration) continue;
				auto it = byId.find(extInfo.declaration->id());
				if (it != byId.end())
					out.insert(it->second);
			}
			return true;
		}
	};
	Scan s(paramById, result);
	_func.body().accept(s);
	return result;
}

} // namespace puyasol::builder
