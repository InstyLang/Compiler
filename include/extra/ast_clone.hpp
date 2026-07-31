#pragma once

#include "extra/ast.hpp"

namespace AST {

// Structural deep copy of an AST subtree.
//
// The clone is byte-for-byte equivalent to the source: no type spellings are
// substituted and no names are rewritten. The only thing that changes is node
// *identity*.
//
// That is exactly what monomorphization needs. A generic class declares its
// methods once, and sema checks that one body once per instantiation with a
// different generic substitution active (Checker::checkGenericClassMethod).
// Every per-node fact sema records -- most importantly `SemaResult::exprTypes`,
// which the backend reads to type locals and temporaries -- is keyed by the
// `ExprAST*`. Checking a shared body twice therefore overwrites the first
// instantiation's types with the second's, and the backend emits every
// instantiation using the last one's types: `Vector<T>.grow()` allocates and
// strides with the wrong element size for `T`. Giving each instantiation its own copy of
// the body makes those keys distinct, so each instantiation records and reads
// its own types. The substitution itself stays where it already works, in
// `Checker::currentSubst_`.
NodePtr cloneNode(const NodePtr& node);
NodeList cloneNodeList(const NodeList& list);

// Deep copy of a method, including its body. Signature strings are copied
// verbatim (still spelled in terms of the generic parameters).
Method cloneMethod(const Method& method);

// Deep copy of a function declaration, including its body. Used to give each
// generic function instantiation its own body for the same reason as above.
std::shared_ptr<FunctionDeclaration> cloneFunctionDeclaration(
    const FunctionDeclaration& fn);

}  // namespace AST
