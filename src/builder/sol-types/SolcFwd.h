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
namespace solidity::frontend
{

class ContractDefinition;
class Expression;
class FunctionDefinition;
class FunctionType;
class StructType;
class Type;
class VariableDeclaration;

}
