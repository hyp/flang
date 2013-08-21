//===--- CGCall.cpp - Encapsulate calling convention details ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// These classes wrap the information about a call or function
// definition used to handle ABI compliancy.
//
//===----------------------------------------------------------------------===//

#include "CGCall.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "flang/AST/Decl.h"
#include "flang/Frontend/CodeGenOptions.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Transforms/Utils/Local.h"

namespace flang {
namespace CodeGen {

llvm::Type *CodeGenTypes::ConvertReturnType(QualType T,
                                            CGFunctionInfo::RetInfo &ReturnInfo) {
  switch(ReturnInfo.ABIInfo.getKind()) {
  case ABIRetInfo::Nothing:
    return CGM.VoidTy;

  case ABIRetInfo::CharacterValueAsArg:
    ReturnInfo.ReturnArgInfo.ABIInfo = ABIArgInfo(ABIArgInfo::Value);
    return CGM.VoidTy;
  default:
    break;
  }

  ReturnInfo.Kind = T->isComplexType()? CGFunctionInfo::RetInfo::ComplexValue :
                                        CGFunctionInfo::RetInfo::ScalarValue;
  return ConvertType(T);
}

void CodeGenTypes::ConvertArgumentType(SmallVectorImpl<llvm::Type*> &ArgTypes,
                                       SmallVectorImpl<llvm::Type*> &AdditionalArgTypes,
                                       QualType T,
                                       CGFunctionInfo::ArgInfo &ArgInfo) {
  switch(ArgInfo.ABIInfo.getKind()) {
  case ABIArgInfo::Value:
    ArgTypes.push_back(ConvertType(T));
    break;

  case ABIArgInfo::Reference:
    ArgTypes.push_back(llvm::PointerType::get(ConvertType(T), 0));
    break;

  case ABIArgInfo::ReferenceAsVoidExtraSize:
    ArgTypes.push_back(CGM.VoidPtrTy);
    ArgTypes.push_back(CGM.Int32Ty);
    break;

  case ABIArgInfo::Expand:
    if(T->isComplexType()) {
      auto ElementType = ConvertType(Context.getComplexTypeElementType(T));
      ArgTypes.push_back(ElementType);
      ArgTypes.push_back(ElementType);
    } else if(T->isCharacterType()) {
      ArgTypes.push_back(CGM.Int8PtrTy); //FIXME: character kinds
      ArgTypes.push_back(CGM.SizeTy);
    } else
      llvm_unreachable("invalid expand abi");
    break;

  case ABIArgInfo::ExpandCharacterPutLengthToAdditionalArgsAsInt:
    assert(T->isCharacterType());
    ArgTypes.push_back(CGM.Int8PtrTy);
    AdditionalArgTypes.push_back(CGM.Int32Ty);
    break;

  case ABIArgInfo::ComplexValueAsVector:
    assert(T->isComplexType());
    ArgTypes.push_back(GetComplexTypeAsVector(
                         ConvertType(Context.getComplexTypeElementType(T))));
    break;
  }
}

CGFunctionInfo *CGFunctionInfo::Create(ASTContext &C,
                                       llvm::CallingConv::ID CC,
                                       llvm::FunctionType *Type,
                                       ArrayRef<ArgInfo> Arguments,
                                       RetInfo Returns) {
  auto Info = new(C) CGFunctionInfo;
  Info->Type = Type;
  Info->CC = CC;
  Info->NumArgs = Arguments.size();
  Info->Args = new(C) ArgInfo[Info->NumArgs];
  for(unsigned I = 0; I < Info->NumArgs; ++I)
    Info->Args[I] = Arguments[I];
  Info->ReturnInfo = Returns;
  return Info;
}

RValueTy CodeGenFunction::EmitCall(const CallExpr *E) {
  CallArgList ArgList;
  return EmitCall(E->getFunction(), ArgList, E->getArguments());
}

RValueTy CodeGenFunction::EmitCall(const FunctionDecl *Function,
                                   CallArgList &ArgList,
                                   ArrayRef<Expr*> Arguments,
                                   bool ReturnsNothing) {
  if(Function->isStatementFunction())
    // statement functions are inlined.
    return EmitStatementFunctionCall(Function, Arguments);
  CGFunction CGFunc = CGM.GetFunction(Function);
  return EmitCall(CGFunc.getFunction(), CGFunc.getInfo(),
                  ArgList, Arguments, ReturnsNothing);
}

RValueTy CodeGenFunction::EmitCall(llvm::Value *Callee,
                                   const CGFunctionInfo *FuncInfo,
                                   CallArgList &ArgList,
                                   ArrayRef<Expr*> Arguments,
                                   bool ReturnsNothing) {
  auto ArgumentInfo = FuncInfo->getArguments();
  for(size_t I = 0; I < Arguments.size(); ++I)
    EmitCallArg(ArgList, Arguments[I], ArgumentInfo[I]);
  auto ReturnInfo = FuncInfo->getReturnInfo().ABIInfo.getKind();
  if(ReturnInfo == ABIRetInfo::CharacterValueAsArg)
    EmitCallArg(ArgList, ArgList.getReturnValueArg().asCharacter(),
                FuncInfo->getReturnInfo().ReturnArgInfo);

  auto Result = Builder.CreateCall(Callee,
                                   ArgList.createValues(), "call");
  Result->setCallingConv(FuncInfo->getCallingConv());

  if(ReturnsNothing ||
     ReturnInfo == ABIRetInfo::Nothing)
    return RValueTy();
  else if(ReturnInfo == ABIRetInfo::Value &&
          FuncInfo->getReturnInfo().Kind == CGFunctionInfo::RetInfo::ComplexValue)
    return ExtractComplexValue(Result);
  else if(ReturnInfo == ABIRetInfo::CharacterValueAsArg)
    return ArgList.getReturnValueArg();
  return Result;
}

RValueTy CodeGenFunction::EmitCall(CGFunction Func,
                                   ArrayRef<RValueTy> Arguments) {
  auto FuncInfo = Func.getInfo();
  auto ArgumentInfo = FuncInfo->getArguments();

  CallArgList ArgList;
  for(size_t I = 0; I < Arguments.size(); ++I) {
    if(Arguments[I].isScalar())
      EmitCallArg(ArgList, Arguments[I].asScalar(), ArgumentInfo[I]);
    else if(Arguments[I].isComplex())
      EmitCallArg(ArgList, Arguments[I].asComplex(), ArgumentInfo[I]);
    else
      EmitCallArg(ArgList, Arguments[I].asCharacter(), ArgumentInfo[I]);
  }

  auto Result = Builder.CreateCall(Func.getFunction(),
                                   ArgList.createValues(), "call");
  Result->setCallingConv(FuncInfo->getCallingConv());
  if(FuncInfo->getReturnInfo().Kind == CGFunctionInfo::RetInfo::ComplexValue)
    return ExtractComplexValue(Result);
  return Result;
}

void CodeGenFunction::EmitCallArg(CallArgList &Args,
                                  const Expr *E, CGFunctionInfo::ArgInfo ArgInfo) {
  if(E->getType()->isCharacterType()) {
    EmitCallArg(Args, EmitCharacterExpr(E), ArgInfo);
    return;
  } else if(E->getType()->isArrayType()) {
    EmitArrayCallArg(Args, E, ArgInfo);
    return;
  }
  switch(ArgInfo.ABIInfo.getKind()) {
  case ABIArgInfo::Value:
    Args.add(EmitScalarExpr(E));
    break;

  case ABIArgInfo::Reference:
    Args.add(EmitCallArgPtr(E));
    break;

  case ABIArgInfo::ReferenceAsVoidExtraSize: {
    auto EType = E->getType();
    auto Ptr = EmitCallArgPtr(E);
    Args.add(Builder.CreateBitCast(Ptr, CGM.VoidPtrTy));
    Args.add(Builder.getInt32(getContext().getTypeKindBitWidth(
                                getContext().getArithmeticOrLogicalTypeKind(
                                  EType.getExtQualsPtrOrNull(), EType))/8));
    break;
  }
  }
}

void CodeGenFunction::EmitArrayCallArg(CallArgList &Args,
                                       const Expr *E, CGFunctionInfo::ArgInfo ArgInfo) {
  switch(ArgInfo.ABIInfo.getKind()) {
  case ABIArgInfo::Value:
    Args.add(EmitArrayPtr(E));
    break;

  default:
    llvm_unreachable("invalid array ABI");
  }
}

void CodeGenFunction::EmitCallArg(CallArgList &Args,
                                  llvm::Value *Value, CGFunctionInfo::ArgInfo ArgInfo) {
  assert(ArgInfo.ABIInfo.getKind() == ABIArgInfo::Value);
  Args.add(Value);
}

void CodeGenFunction::EmitCallArg(CallArgList &Args,
                                  ComplexValueTy Value, CGFunctionInfo::ArgInfo ArgInfo) {
  assert(ArgInfo.ABIInfo.getKind() != ABIArgInfo::Reference);
  switch(ArgInfo.ABIInfo.getKind()) {
  case ABIArgInfo::Value:
    Args.add(CreateComplexAggregate(Value));
    break;

  case ABIArgInfo::Expand:
    Args.add(Value.Re);
    Args.add(Value.Im);
    break;

  case ABIArgInfo::ComplexValueAsVector:
    Args.add(CreateComplexVector(Value));
    break;
  }
}

void CodeGenFunction::EmitCallArg(CallArgList &Args,
                                  CharacterValueTy Value, CGFunctionInfo::ArgInfo ArgInfo) {
  assert(ArgInfo.ABIInfo.getKind() != ABIArgInfo::Reference);
  switch(ArgInfo.ABIInfo.getKind()) {
  case ABIArgInfo::Value:
    Args.add(CreateCharacterAggregate(Value));
    break;

  case ABIArgInfo::Expand:
    Args.add(Value.Ptr);
    Args.add(Value.Len);
    break;

  case ABIArgInfo::ExpandCharacterPutLengthToAdditionalArgsAsInt:
    Args.add(Value.Ptr);
    Args.addAditional(Builder.CreateSExtOrTrunc(Value.Len, CGM.Int32Ty));
    break;
  }
}

CharacterValueTy CodeGenFunction::GetCharacterArg(const VarDecl *Arg) {
  auto Result = CharacterArgs.find(Arg);
  if(Result == CharacterArgs.end()) {
    CharacterValueTy Val;
    switch(GetArgInfo(Arg).ABIInfo.getKind()) {
    case ABIArgInfo::Value:
      Val = ExtractCharacterValue(GetVarPtr(Arg));
      break;

    case ABIArgInfo::Expand: {
      auto ExpansionInfo = GetExpandedArg(Arg);
      Val = CharacterValueTy(ExpansionInfo.A1,
                             ExpansionInfo.A2);
      break;
    }

    case ABIArgInfo::ExpandCharacterPutLengthToAdditionalArgsAsInt: {
      auto ExpansionInfo = GetExpandedArg(Arg);
      Val = CharacterValueTy(ExpansionInfo.A1,
                             Builder.CreateSExtOrTrunc(ExpansionInfo.A2,
                                                       CGM.SizeTy));
      break;
    }

    }
    CharacterArgs.insert(std::make_pair(Arg, Val));
    return Val;
  }

  return Result->second;
}

llvm::Value *CodeGenFunction::EmitCallArgPtr(const Expr *E) {
  if(auto Var = dyn_cast<VarExpr>(E)) {
    auto VD = Var->getVarDecl();
    if(!VD->isParameter())
      return GetVarPtr(VD);
  }
  else if(auto ArrEl = dyn_cast<ArrayElementExpr>(E))
    return EmitArrayElementPtr(ArrEl->getTarget(), ArrEl->getSubscripts());

  auto Value = EmitRValue(E);
  auto Temp  = CreateTempAlloca(ConvertType(E->getType()));
  EmitAssignment(Temp, Value);
  return Temp;
}


llvm::CallInst *CodeGenFunction::EmitRuntimeCall(llvm::Value *Func) {
  auto Result = Builder.CreateCall(Func);
  Result->setCallingConv(CGM.getRuntimeCC());
  return Result;
}

llvm::CallInst *CodeGenFunction::EmitRuntimeCall(llvm::Value *Func, llvm::ArrayRef<llvm::Value*> Args) {
  auto Result = Builder.CreateCall(Func, Args);
  Result->setCallingConv(CGM.getRuntimeCC());
  return Result;
}

llvm::CallInst *CodeGenFunction::EmitRuntimeCall2(llvm::Value *Func, llvm::Value *A1, llvm::Value *A2) {
  auto Result = Builder.CreateCall2(Func, A1, A2);
  Result->setCallingConv(CGM.getRuntimeCC());
  return Result;
}

class StatementFunctionInliningScope {
public:
  CodeGenFunction *CGF;
  const FunctionDecl *Func;
  const StatementFunctionInliningScope *Previous;
  llvm::DenseMap<const VarDecl*, const Expr*> Args;

  StatementFunctionInliningScope(CodeGenFunction *cgf,
                                 const FunctionDecl *Function,
                                 ArrayRef<Expr*> Arguments)
    : CGF(cgf), Func(Function), Previous(cgf->CurInlinedStmtFunc) {
      cgf->CurInlinedStmtFunc = this;
      for(size_t I = 0; I < Arguments.size(); ++I)
        Args.insert(std::make_pair(Function->getArguments()[I], Arguments[I]));
    }
  ~StatementFunctionInliningScope() {
    CGF->CurInlinedStmtFunc = Previous;
  }
  const Expr *getArgValue(const VarDecl *Arg) const {
    auto result = Args.find(Arg);
    if(result != Args.end())
      return result->second;
    return Previous->getArgValue(Arg);
  }
};

RValueTy CodeGenFunction::EmitStatementFunctionCall(const FunctionDecl *Function,
                                                    ArrayRef<Expr*> Arguments) {
  StatementFunctionInliningScope Scope(this, Function, Arguments);
  return EmitRValue(Function->getBodyExpr());
}

bool CodeGenFunction::IsInlinedArgument(const VarDecl *VD) {
  if(VD->isArgument())
    return cast<FunctionDecl>(VD->getDeclContext())->isStatementFunction();
  return false;
}

const Expr *CodeGenFunction::GetInlinedArgumentValue(const VarDecl *VD) {
  return CurInlinedStmtFunc->getArgValue(VD);
}

}
} // end namespace flang
