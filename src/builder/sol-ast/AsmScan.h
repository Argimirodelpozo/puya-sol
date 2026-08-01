/// @file AsmScan.h
/// Recursive "does this subtree use inline assembly" query.

#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libyul/AST.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

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


/// If `_func` is a STORAGE-POINTER ALIAS, the aliased parameter index and the
/// wrapper struct's single field name.
///
/// Solidity forbids assigning to a storage pointer, so OZ's StorageSlot library
/// is the sanctioned way to write THROUGH one:
///     function getStringSlot(string storage store)
///         internal pure returns (StringSlot storage r)
///     { assembly { r.slot := store.slot } }
///     ...
///     StorageSlot.getStringSlot(store).value = v;      // means: store = v
/// The entire function is a pointer cast — its result denotes the SAME storage
/// location as the parameter — and the wrapper struct has exactly one field, so
/// `f(x).<field>` IS `x`, as an LVALUE. This is the single call shape that
/// blocked kaito/degen/usde/sdai/ena/aero/velo (all via OZ ShortStrings).
///
/// Deliberately EXACT-SHAPE: one inline-assembly statement holding one Yul
/// assignment `<ret>.slot := <param>.slot`, a one-field storage-struct return,
/// and a field whose type category matches the parameter's. Anything else — say
/// `r.slot := add(store.slot, 1)`, which denotes a DIFFERENT location — fails
/// the match and falls through to the existing loud error rather than aliasing
/// the wrong slot.
inline std::optional<std::pair<size_t, std::string>> storagePointerAliasParam(
	solidity::frontend::FunctionDefinition const& _func)
{
	using namespace solidity::frontend;
	if (!_func.isImplemented() || _func.returnParameters().size() != 1)
		return std::nullopt;
	auto const& rp = _func.returnParameters()[0];
	if (!rp || rp->referenceLocation() != VariableDeclaration::Location::Storage)
		return std::nullopt;
	auto const* st = dynamic_cast<StructType const*>(rp->type());
	if (!st || st->structDefinition().members().size() != 1)
		return std::nullopt;
	auto const& field = st->structDefinition().members()[0];

	auto const& stmts = _func.body().statements();
	if (stmts.size() != 1)
		return std::nullopt;
	auto const* asmStmt = dynamic_cast<InlineAssembly const*>(stmts[0].get());
	if (!asmStmt)
		return std::nullopt;
	auto const& root = asmStmt->operations().root();
	if (root.statements.size() != 1)
		return std::nullopt;
	auto const* assign = std::get_if<solidity::yul::Assignment>(&root.statements[0]);
	if (!assign || assign->variableNames.size() != 1)
		return std::nullopt;
	auto const* rhs = std::get_if<solidity::yul::Identifier>(assign->value.get());
	if (!rhs)
		return std::nullopt;

	// Both sides must be `.slot` external references: LHS the return param,
	// RHS one of the storage parameters.
	Declaration const* lhsDecl = nullptr;
	Declaration const* rhsDecl = nullptr;
	for (auto const& [yulId, extInfo]: asmStmt->annotation().externalReferences)
	{
		if (extInfo.suffix != "slot")
			continue;
		if (yulId == &assign->variableNames[0])
			lhsDecl = extInfo.declaration;
		else if (yulId == rhs)
			rhsDecl = extInfo.declaration;
	}
	if (!lhsDecl || lhsDecl->id() != rp->id() || !rhsDecl)
		return std::nullopt;

	for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
	{
		auto const& p = _func.parameters()[pi];
		if (!p || p->id() != rhsDecl->id())
			continue;
		if (p->referenceLocation() != VariableDeclaration::Location::Storage)
			return std::nullopt;
		if (!field->type() || !p->type()
			|| field->type()->category() != p->type()->category())
			return std::nullopt;
		return std::make_pair(pi, field->name());
	}
	return std::nullopt;
}

} // namespace puyasol::builder
