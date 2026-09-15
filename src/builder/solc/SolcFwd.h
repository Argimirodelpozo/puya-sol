#pragma once

/// Forward declarations for the solc AST and type nodes that this codebase
/// names only through pointers and references.
///
/// solc ships `libsolidity/ast/ASTForward.h` for AST nodes but has no
/// equivalent for `Types.h`, which is why the two heavy headers leaked across
/// almost the whole build: `Types.h` reached 128 of 139 builder translation
/// units while only 13 included it directly, and `AST.h` reached 126 while 40
/// included it directly. A handful of hub headers were pulling ~300k
/// preprocessed lines into everything downstream of them.
///
/// Include this instead when a header only needs the NAME of a solc type.
/// A header that stores one by value, or touches a nested type/enum
/// (`InlineAssemblyAnnotation::ExternalIdentifierInfo`, `Type::Category`,
/// `FunctionType::Kind`), still needs the real header — nested names cannot be
/// forward-declared.
/// For AST NODES (Identifier, FunctionCall, MemberAccess, ...) prefer solc's own
/// `<libsolidity/ast/ASTForward.h>`: it declares all of them and costs ~60k
/// preprocessed lines against AST.h's ~302k. The types below are the gap —
/// Types.h has no forward header at all.
namespace solidity::frontend
{

// AST nodes, also available from solc's ASTForward.h; kept here so a header
// needing one or two of them alongside a Type does not need both includes.
class ContractDefinition;
class Expression;
class FunctionDefinition;
class VariableDeclaration;

// AST nodes solc's own ASTForward.h does NOT declare, despite declaring their
// siblings. Only reachable from AST.h upstream.
class FunctionCallOptions;
class IndexRangeAccess;
class RevertStatement;

// Types.h. These have no upstream forward header.
class AddressType;
class ArrayType;
class BoolType;
class EnumType;
class FixedBytesType;
class FunctionType;
class IntegerType;
class MappingType;
class StructType;
class Type;

}
