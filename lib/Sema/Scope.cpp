//===- Scope.cpp - Lexical scope information ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Scope class, which is used for recording information
// about a lexical scope.
//
//===----------------------------------------------------------------------===//

#include "flang/Sema/Scope.h"
#include "flang/AST/Expr.h"
#include <limits>

namespace flang {

static StmtLabelInteger GetStmtLabelValue(const Expr *E) {
  if(const IntegerConstantExpr *IExpr =
     dyn_cast<IntegerConstantExpr>(E)) {
    return StmtLabelInteger(
          IExpr->getValue().getLimitedValue(
            std::numeric_limits<StmtLabelInteger>::max()));
  } else {
    llvm_unreachable("Invalid stmt label expression");
    return 0;
  }
}

/// \brief Declares a new statement label.
void StmtLabelScope::Declare(Expr *StmtLabel, Stmt *Statement) {
  auto Key = GetStmtLabelValue(StmtLabel);
  StmtLabelDeclsInScope.insert(std::make_pair(Key,Statement));
}

/// \brief Tries to resolve a statement label reference.
Stmt *StmtLabelScope::Resolve(Expr *StmtLabel) const {
  auto Key = GetStmtLabelValue(StmtLabel);
  auto Result = StmtLabelDeclsInScope.find(Key);
  if(Result == StmtLabelDeclsInScope.end()) return 0;
  else return Result->second;
}

/// \brief Declares a forward reference of some statement label.
void StmtLabelScope::DeclareForwardReference(StmtLabelForwardDecl Reference) {
  ForwardStmtLabelDeclsInScope.append(1,Reference);
}

/// \brief Removes a forward reference of some statement label.
void StmtLabelScope::RemoveForwardReference(const Stmt *User) {
  for(size_t I = 0; I < ForwardStmtLabelDeclsInScope.size(); ++I) {
    if(ForwardStmtLabelDeclsInScope[I].Statement == User) {
      ForwardStmtLabelDeclsInScope.erase(ForwardStmtLabelDeclsInScope.begin() + I);
      return;
    }
  }
}

/// \brief Returns true is the two statement labels are identical.
bool StmtLabelScope::IsSame(const Expr *StmtLabelA,
                            const Expr *StmtLabelB) const {
  return GetStmtLabelValue(StmtLabelA) == GetStmtLabelValue(StmtLabelB);
}

void StmtLabelScope::reset() {
  StmtLabelDeclsInScope.clear();
  ForwardStmtLabelDeclsInScope.clear();
}

ImplicitTypingScope::ImplicitTypingScope()
  : Parent(nullptr), None(false) {
}

bool ImplicitTypingScope::Apply(const ImplicitStmt::LetterSpec &Spec, QualType T) {
  if(None) return false;
  char Low = toupper((Spec.first->getNameStart())[0]);
  if(Spec.second) {
    char High = toupper((Spec.second->getNameStart())[0]);
    for(; Low <= High; ++Low) {
      llvm::StringRef Key(&Low, 1);
      if(Rules.find(Key) != Rules.end())
        return false;
      Rules[Key] = T;
    }
  } else {
    llvm::StringRef Key(&Low, 1);
    if(Rules.find(Key) != Rules.end())
      return false;
    Rules[Key] = T;
  }
  return true;
}

bool ImplicitTypingScope::ApplyNone() {
  if(Rules.size()) return false;
  None = true;
  return true;
}

std::pair<ImplicitTypingScope::RuleType, QualType>
ImplicitTypingScope::Resolve(const IdentifierInfo *IdInfo) {
  if(None)
    return std::make_pair(NoneRule, QualType());
  char C = toupper(IdInfo->getNameStart()[0]);
  auto Result = Rules.find(llvm::StringRef(&C, 1));
  if(Result != Rules.end())
    return std::make_pair(TypeRule, Result->getValue());
  else if(Parent)
    return Parent->Resolve(IdInfo);
  else return std::make_pair(DefaultRule, QualType());
}


void Scope::Init(Scope *parent, unsigned flags) {
  AnyParent = parent;
  Flags = flags;

  if (parent) {
    Depth          = parent->Depth + 1;
    PrototypeDepth = parent->PrototypeDepth;
    PrototypeIndex = 0;
    FnParent       = parent->FnParent;
  } else {
    Depth = 0;
    PrototypeDepth = 0;
    PrototypeIndex = 0;
    FnParent = 0;
  }

  // If this scope is a function or contains breaks/continues, remember it.
  if (flags & FnScope) FnParent = this;

  DeclsInScope.clear();
  Entity = 0;
  ErrorTrap.reset();
}

} //namespace flang
