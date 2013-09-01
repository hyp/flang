//===--- CGArray.cpp - Emit LLVM Code for Array operations and Expr -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Array subscript expressions and operations.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "CGArray.h"
#include "flang/AST/ASTContext.h"
#include "flang/AST/ExprVisitor.h"
#include "flang/AST/StmtVisitor.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"

namespace flang {
namespace CodeGen {

llvm::ArrayType *CodeGenTypes::GetFixedSizeArrayType(const ArrayType *T,
                                                     uint64_t Size) {
  return llvm::ArrayType::get(ConvertTypeForMem(T->getElementType()),
                              Size);
}

llvm::Type *CodeGenTypes::ConvertArrayType(const ArrayType *T) {
  return llvm::PointerType::get(ConvertTypeForMem(T->getElementType()), 0);
}

llvm::ArrayType *CodeGenTypes::ConvertArrayTypeForMem(const ArrayType *T) {
  uint64_t ArraySize;
  if(T->EvaluateSize(ArraySize, Context))
    return GetFixedSizeArrayType(T, ArraySize);
  llvm_unreachable("invalid memory array type");
  return nullptr;
}

llvm::Value *CodeGenFunction::CreateArrayAlloca(QualType T,
                                                const llvm::Twine &Name,
                                                bool IsTemp) {
  auto ATy = cast<ArrayType>(T.getTypePtr());
  uint64_t ArraySize;
  if(ATy->EvaluateSize(ArraySize, getContext())) {
    auto Ty = getTypes().GetFixedSizeArrayType(ATy, ArraySize);
    if(IsTemp)
      return CreateTempAlloca(Ty, Name);
    else
      return Builder.CreateAlloca(Ty, nullptr, Name);
  }
  // FIXME variable size stack/heap allocation
  return nullptr;
}

llvm::Value *CodeGenFunction::CreateTempHeapArrayAlloca(QualType T,
                                                        ArrayRef<ArraySection> Sections) {
  auto ETy = getTypes().ConvertTypeForMem(T.getSelfOrArrayElementType());
  auto PTy = llvm::PointerType::get(ETy, 0);
  auto Size = EmitArraySize(Sections);
  Size = Builder.CreateMul(Size, llvm::ConstantInt::get(Size->getType(), CGM.getDataLayout().getTypeStoreSize(ETy)));
  return CreateTempHeapAlloca(Size, PTy);
}

void CodeGenFunction::GetArrayDimensionsInfo(QualType T, SmallVectorImpl<ArrayDimensionValueTy> &Dims) {
  auto ATy = cast<ArrayType>(T.getTypePtr());
  auto Dimensions = ATy->getDimensions();

  for(size_t I = 0; I < Dimensions.size(); ++I) {
    llvm::Value *LB = nullptr;
    llvm::Value *UB = nullptr;
    auto LowerBound = Dimensions[I]->getLowerBoundOrNull();
    auto UpperBound = Dimensions[I]->getUpperBoundOrNull();
    if(LowerBound) {
      int64_t ConstantLowerBound;
      if(LowerBound->EvaluateAsInt(ConstantLowerBound, getContext())) {
        LB = llvm::ConstantInt::get(ConvertType(getContext().IntegerTy),
                                                ConstantLowerBound);
      } else LB = EmitScalarExpr(LowerBound);
    }
    if(UpperBound) {
      int64_t ConstantUpperBound;
      if(UpperBound->EvaluateAsInt(ConstantUpperBound, getContext())) {
        UB = llvm::ConstantInt::get(ConvertType(getContext().IntegerTy),
                                                ConstantUpperBound);
      } else UB = EmitScalarExpr(UpperBound);
    }
    Dims.push_back(ArrayDimensionValueTy(LB, UB));
  }
}

llvm::Value *CodeGenFunction::EmitDimSize(const ArrayDimensionValueTy &Dim) {
  // UB - LB + 1
  if(Dim.hasLowerBound()) {
    return Builder.CreateAdd(Builder.CreateSub(Dim.UpperBound,
                                               Dim.LowerBound),
                             llvm::ConstantInt::get(Dim.LowerBound->getType(),
                                                    1));
  }
  // UB - LB + 1 => UB - 1 + 1 => UB
  return Dim.UpperBound;
}

llvm::Value *CodeGenFunction::EmitDimSubscript(llvm::Value *Subscript,
                                               const ArrayDimensionValueTy &Dim) {
  // S - LB
  auto LB = Dim.hasLowerBound()? Dim.LowerBound :
                                 llvm::ConstantInt::get(Subscript->getType(), 1);
  return Builder.CreateSub(Subscript, LB);
}

llvm::Value *CodeGenFunction::EmitNthDimSubscript(llvm::Value *Subscript,
                                                  const ArrayDimensionValueTy &Dim,
                                                  llvm::Value *DimSizeProduct) {
  // (Sn - LBn) * product of sizes of previous dimensions.
  return Builder.CreateMul(EmitDimSubscript(Subscript, Dim),
                           DimSizeProduct);
}

ArraySection CodeGenFunction::EmitDimSection(const ArrayDimensionValueTy &Dim) {
  auto Offset = Dim.hasOffset()? Dim.Offset : nullptr;
  auto Size = EmitDimSize(Dim);
  return ArraySection(ArrayRangeSection(Offset, Size,
                        Dim.hasStride()? Dim.Stride : nullptr),
                      Size);
}

llvm::Value *CodeGenFunction::EmitArraySize(ArrayRef<ArraySection> Sections) {
  llvm::Value *Size = nullptr;
  for(auto I : Sections) {
    if(I.isRangeSection()) {
      Size = Size? Builder.CreateMul(I.getRangeSection().Size, Size):
                   I.getRangeSection().Size;
    } else if(I.isVectorSection()) {
      Size = Size? Builder.CreateMul(I.getVectorSection().Size, Size):
                   I.getVectorSection().Size;
    }
  }
  return Size;
}

class ArrayValueExprEmitter
  : public ConstExprVisitor<ArrayValueExprEmitter> {
  CodeGenFunction &CGF;
  CGBuilderTy &Builder;
  llvm::LLVMContext &VMContext;

  SmallVector<ArrayDimensionValueTy, 8> Dims;
  llvm::Value *Ptr;
  bool GetPointer;
public:

  ArrayValueExprEmitter(CodeGenFunction &cgf, bool getPointer = true);

  void EmitExpr(const Expr *E);
  void VisitVarExpr(const VarExpr *E);
  void VisitArrayConstructorExpr(const ArrayConstructorExpr *E);

  ArrayRef<ArrayDimensionValueTy> getResultInfo() const {
    return Dims;
  }
  llvm::Value *getResultPtr() const {
    return Ptr;
  }
};

ArrayValueExprEmitter::ArrayValueExprEmitter(CodeGenFunction &cgf, bool getPointer)
  : CGF(cgf), Builder(cgf.getBuilder()),
    VMContext(cgf.getLLVMContext()), GetPointer(getPointer) {
}

void ArrayValueExprEmitter::EmitExpr(const Expr *E) {
  Visit(E);
}

void ArrayValueExprEmitter::VisitVarExpr(const VarExpr *E) {
  auto VD = E->getVarDecl();
  if(CGF.IsInlinedArgument(VD))
    return EmitExpr(CGF.GetInlinedArgumentValue(VD));
  if(VD->isParameter())
    return EmitExpr(VD->getInit());
  if(VD->isArgument()) {
    CGF.GetArrayDimensionsInfo(VD->getType(), Dims);
    if(GetPointer)
      Ptr = CGF.GetVarPtr(VD);
    return;
  }
  CGF.GetArrayDimensionsInfo(VD->getType(), Dims);
  if(GetPointer)
    Ptr = Builder.CreateConstInBoundsGEP2_32(CGF.GetVarPtr(VD), 0, 0);
}

void ArrayValueExprEmitter::VisitArrayConstructorExpr(const ArrayConstructorExpr *E) {
  CGF.GetArrayDimensionsInfo(E->getType(), Dims);
  if(GetPointer)
    Ptr = CGF.EmitArrayConstructor(E);
}

class ArraySectionsEmmitter
  : public ConstExprVisitor<ArraySectionsEmmitter, void> {
  CodeGenFunction &CGF;
  SmallVector<ArraySection, 8> Sections;
  llvm::Value *Ptr;
  bool GetPointer;
public:
  ArraySectionsEmmitter(CodeGenFunction &cgf, bool getPointer = true);

  void EmitExpr(const Expr *E);
  void VisitExpr(const Expr *E);

  ArrayRef<ArraySection> getSections() const {
    return Sections;
  }
  llvm::Value *getPointer() const {
    return Ptr;
  }
};

ArraySectionsEmmitter::ArraySectionsEmmitter(CodeGenFunction &cgf, bool getPointer)
  : CGF(cgf), GetPointer(getPointer) {}

void ArraySectionsEmmitter::EmitExpr(const Expr *E) {
  Visit(E);
}

void ArraySectionsEmmitter::VisitExpr(const Expr *E) {
  ArrayValueExprEmitter EV(CGF, GetPointer);
  EV.EmitExpr(E);
  if(GetPointer)
   Ptr = EV.getResultPtr();
  for(auto I : EV.getResultInfo())
    Sections.push_back(CGF.EmitDimSection(I));
}

/// \brief Gathers the array sections which are needed for
/// an standalone array expression.
class StandaloneArrayValueSectionGatherer
  : public ConstExprVisitor<StandaloneArrayValueSectionGatherer> {
  CodeGenFunction &CGF;

  SmallVector<ArraySection, 8> Sections;
  bool Gathered;

  void GatherSections(const Expr *E);
public:

  StandaloneArrayValueSectionGatherer(CodeGenFunction &cgf);
  void EmitExpr(const Expr *E);

  void VisitVarExpr(const VarExpr *E);
  void VisitArrayConstructorExpr(const ArrayConstructorExpr *E);
  void VisitBinaryExpr(const BinaryExpr *E);
  void VisitUnaryExpr(const UnaryExpr *E);
  void VisitImplicitCastExpr(const ImplicitCastExpr *E);
  void VisitIntrinsicCallExpr(const IntrinsicCallExpr *E);

  ArrayRef<ArraySection> getSections() const {
    return Sections;
  }
};

StandaloneArrayValueSectionGatherer::StandaloneArrayValueSectionGatherer(CodeGenFunction &cgf)
  : CGF(cgf), Gathered(false) {
}

void StandaloneArrayValueSectionGatherer::EmitExpr(const Expr *E) {
  if(Gathered) return;
  if(E->getType()->isArrayType())
    Visit(E);
}

void StandaloneArrayValueSectionGatherer::GatherSections(const Expr *E) {
  ArraySectionsEmmitter EV(CGF, false);
  EV.EmitExpr(E);
  for(auto I : EV.getSections())
    Sections.push_back(I);
  Gathered = true;
}

void StandaloneArrayValueSectionGatherer::VisitVarExpr(const VarExpr *E) {
  GatherSections(E);
}

void StandaloneArrayValueSectionGatherer::VisitArrayConstructorExpr(const ArrayConstructorExpr *E) {
  GatherSections(E);
}

void StandaloneArrayValueSectionGatherer::VisitBinaryExpr(const BinaryExpr *E) {
  EmitExpr(E->getLHS());
  EmitExpr(E->getRHS());
}

void StandaloneArrayValueSectionGatherer::VisitUnaryExpr(const UnaryExpr *E) {
  EmitExpr(E->getExpression());
}

void StandaloneArrayValueSectionGatherer::VisitImplicitCastExpr(const ImplicitCastExpr *E) {
  EmitExpr(E->getExpression());
}

void StandaloneArrayValueSectionGatherer::VisitIntrinsicCallExpr(const IntrinsicCallExpr *E) {
  // FIXME
  EmitExpr(E->getArguments()[0]);
}

//
// Scalar values and array sections emmitter for an array operations.
//

ArrayValueTy ArrayOperation::getArrayValue(const Expr *E) {
  auto Arr = Arrays[E];
  return ArrayValueTy(llvm::makeArrayRef(Sections.begin() + Arr.SectionsOffset,
                        E->getType()->asArrayType()->getDimensionCount()),
                      Arr.Ptr);
}

void ArrayOperation::EmitArraySections(CodeGenFunction &CGF, const Expr *E) {
  if(Arrays.find(E) != Arrays.end())
    return;

  ArraySectionsEmmitter EV(CGF);
  EV.EmitExpr(E);

  StoredArrayValue ArrayValue;
  ArrayValue.SectionsOffset = Sections.size();
  ArrayValue.Ptr = EV.getPointer();
  Arrays[E] = ArrayValue;

  for(auto S : EV.getSections())
    Sections.push_back(S);
}

RValueTy ArrayOperation::getScalarValue(const Expr *E) {
  return Scalars[E];
}

void ArrayOperation::EmitScalarValue(CodeGenFunction &CGF, const Expr *E) {
  if(Scalars.find(E) != Scalars.end())
    return;

  Scalars[E] = CGF.EmitRValue(E);
}

class ScalarEmmitterAndSectionGatherer : public ConstExprVisitor<ScalarEmmitterAndSectionGatherer> {
  CodeGenFunction &CGF;
  ArrayOperation &ArrayOp;
  const Expr *LastArrayEmmitted;
public:

  ScalarEmmitterAndSectionGatherer(CodeGenFunction &cgf, ArrayOperation &ArrOp)
    : CGF(cgf), ArrayOp(ArrOp) {}

  void Emit(const Expr *E);
  void VisitVarExpr(const VarExpr *E);
  void VisitImplicitCastExpr(const ImplicitCastExpr *E);
  void VisitUnaryExpr(const UnaryExpr *E);
  void VisitBinaryExpr(const BinaryExpr *E);
  void VisitArrayConstructorExpr(const ArrayConstructorExpr *E);

  const Expr *getLastEmmittedArray() const {
    return LastArrayEmmitted;
  }
};

void ScalarEmmitterAndSectionGatherer::Emit(const Expr *E) {
  if(E->getType()->isArrayType())
    Visit(E);
  else ArrayOp.EmitScalarValue(CGF, E);
}

void ScalarEmmitterAndSectionGatherer::VisitVarExpr(const VarExpr *E) {
  ArrayOp.EmitArraySections(CGF, E);
  LastArrayEmmitted = E;
}

void ScalarEmmitterAndSectionGatherer::VisitImplicitCastExpr(const ImplicitCastExpr *E) {
  Emit(E->getExpression());
}

void ScalarEmmitterAndSectionGatherer::VisitUnaryExpr(const UnaryExpr *E) {
  Emit(E->getExpression());
}

void ScalarEmmitterAndSectionGatherer::VisitBinaryExpr(const BinaryExpr *E) {
  Emit(E->getLHS());
  Emit(E->getRHS());
}

void ScalarEmmitterAndSectionGatherer::VisitArrayConstructorExpr(const ArrayConstructorExpr *E) {
  ArrayOp.EmitArraySections(CGF, E);
  LastArrayEmmitted = E;
}

void ArrayOperation::EmitAllScalarValuesAndArraySections(CodeGenFunction &CGF, const Expr *E) {
  ScalarEmmitterAndSectionGatherer EV(CGF, *this);
  EV.Emit(E);
}

ArrayValueTy ArrayOperation::EmitArrayExpr(CodeGenFunction &CGF, const Expr *E) {
  ScalarEmmitterAndSectionGatherer EV(CGF, *this);
  EV.Emit(E);
  return getArrayValue(EV.getLastEmmittedArray());
}

//
// Foreach element in given sections loop emmitter for array operations
//

ArrayLoopEmmitter::ArrayLoopEmmitter(CodeGenFunction &cgf,
                                             ArrayRef<ArraySection> LHS)
  : CGF(cgf), Builder(cgf.getBuilder()), Sections(LHS)
{ }


llvm::Value *ArrayLoopEmmitter::EmitSectionIndex(const ArrayRangeSection &Range,
                                                 int Dimension) {
  // compute dimension index -> index = base + loop_index * stride
  auto StridedIndex = !Range.hasStride()? Elements[Dimension] :
                        Builder.CreateMul(Elements[Dimension], Range.Stride);
  return Range.hasOffset()? Builder.CreateAdd(Range.Offset, StridedIndex) :
                            StridedIndex;
}

llvm::Value *ArrayLoopEmmitter::EmitSectionIndex(const ArraySection &Section,
                                                 int Dimension) {
  if(Section.isRangeSection())
    return EmitSectionIndex(Section.getRangeSection(), Dimension);
  else
    return Section.getElementSection().Index;
}

// FIXME: add support for vector sections.
void ArrayLoopEmmitter::EmitArrayIterationBegin() {
  auto IndexType = CGF.ConvertType(CGF.getContext().IntegerTy);

  Elements.resize(Sections.size());
  Loops.resize(Sections.size());

  // Foreach section from back to front (column major
  // order for efficient memory access).
  for(auto I = Sections.size(); I!=0;) {
    --I;
    if(Sections[I].isRangeSection()) {
      auto Range = Sections[I].getRangeSection();
      auto Var = CGF.CreateTempAlloca(IndexType,"array-dim-loop-counter");
      Builder.CreateStore(llvm::ConstantInt::get(IndexType, 0), Var);
      auto LoopCond = CGF.createBasicBlock("array-dim-loop");
      auto LoopBody = CGF.createBasicBlock("array-dim-loop-body");
      auto LoopEnd = CGF.createBasicBlock("array-dim-loop-end");
      CGF.EmitBlock(LoopCond);
      Builder.CreateCondBr(Builder.CreateICmpULT(Builder.CreateLoad(Var), Range.Size),
                           LoopBody, LoopEnd);
      CGF.EmitBlock(LoopBody);
      Elements[I] = Builder.CreateLoad(Var);

      Loops[I].EndBlock = LoopEnd;
      Loops[I].TestBlock = LoopCond;
      Loops[I].Counter = Var;
    } else {
      Elements[I] = nullptr;
      Loops[I].EndBlock = nullptr;
    }
  }
}

void ArrayLoopEmmitter::EmitArrayIterationEnd() {
  auto IndexType = CGF.ConvertType(CGF.getContext().IntegerTy);

  // foreach loop from front to back.
  for(auto Loop : Loops) {
    if(Loop.EndBlock) {
      Builder.CreateStore(Builder.CreateAdd(Builder.CreateLoad(Loop.Counter),
                                            llvm::ConstantInt::get(IndexType, 1)),
                          Loop.Counter);
      CGF.EmitBranch(Loop.TestBlock);
      CGF.EmitBlock(Loop.EndBlock);
    }
  }
}

llvm::Value *ArrayLoopEmmitter::EmitElementOffset(ArrayRef<ArraySection> Sections) {
  auto Offset = EmitSectionIndex(Sections[0], 0);
  if(Sections.size() > 1) {
    auto SizeProduct = Sections[0].getDimensionSize();
    for(size_t I = 1; I < Sections.size(); ++I) {
      auto Sub = Builder.CreateMul(EmitSectionIndex(Sections[I], I),
                                   SizeProduct);
      Offset = Builder.CreateAdd(Offset, Sub);
      if((I + 1) < Sections.size())
        SizeProduct = Builder.CreateMul(SizeProduct, Sections[I].getDimensionSize());
    }
  }
  return Offset;
}

llvm::Value *ArrayLoopEmmitter::EmitElementPointer(ArrayValueTy Array) {
  return Builder.CreateGEP(Array.Ptr, EmitElementOffset(Array.Sections));
}

//
// Multidimensional loop body emmitter for array operations.
//

ArrayOperationEmmitter::
ArrayOperationEmmitter(CodeGenFunction &cgf, ArrayOperation &Op,
                       ArrayLoopEmmitter &Loop)
  : CGF(cgf), Builder(cgf.getBuilder()), Operation(Op),
    Looper(Loop) {}

RValueTy ArrayOperationEmmitter::Emit(const Expr *E) {
  if(E->getType()->isArrayType())
    return ConstExprVisitor::Visit(E);
  return Operation.getScalarValue(E);
}

RValueTy ArrayOperationEmmitter::VisitVarExpr(const VarExpr *E) {
  return CGF.EmitLoad(Looper.EmitElementPointer(Operation.getArrayValue(E)), ElementType(E));
}

RValueTy ArrayOperationEmmitter::VisitImplicitCastExpr(const ImplicitCastExpr *E) {
  return CGF.EmitImplicitConversion(Emit(E->getExpression()), E->getType().getSelfOrArrayElementType());
}

RValueTy ArrayOperationEmmitter::VisitUnaryExpr(const UnaryExpr *E) {
  return CGF.EmitUnaryExpr(E->getOperator(), Emit(E->getExpression()));
}

RValueTy ArrayOperationEmmitter::VisitBinaryExpr(const BinaryExpr *E) {
  return CGF.EmitBinaryExpr(E->getOperator(), Emit(E->getLHS()), Emit(E->getRHS()));
}

RValueTy ArrayOperationEmmitter::VisitArrayConstructorExpr(const ArrayConstructorExpr *E) {
  return CGF.EmitLoad(Looper.EmitElementPointer(Operation.getArrayValue(E)), ElementType(E));
}

LValueTy ArrayOperationEmmitter::EmitLValue(const Expr *E) {
  return Looper.EmitElementPointer(Operation.getArrayValue(E));
}

static void EmitArrayAssignment(CodeGenFunction &CGF, ArrayOperation &Op,
                                ArrayLoopEmmitter &Looper, ArrayValueTy LHS,
                                const Expr *RHS) {
  ArrayOperationEmmitter EV(CGF, Op, Looper);
  auto Val = EV.Emit(RHS);
  CGF.EmitStore(Val, Looper.EmitElementPointer(LHS), RHS->getType());
}

static void EmitArrayAssignment(CodeGenFunction &CGF, ArrayOperation &Op,
                                ArrayLoopEmmitter &Looper, const Expr *LHS,
                                const Expr *RHS) {
  ArrayOperationEmmitter EV(CGF, Op, Looper);
  auto Val = EV.Emit(RHS);
  CGF.EmitStore(Val, EV.EmitLValue(LHS), RHS->getType());
}

static llvm::Value *EmitArrayConditional(CodeGenFunction &CGF, ArrayOperation &Op,
                                         ArrayLoopEmmitter &Looper, const Expr *Condition) {
  ArrayOperationEmmitter EV(CGF, Op, Looper);
  auto Val = EV.Emit(Condition).asScalar();
  if(Val->getType() != CGF.getModule().Int1Ty)
    return CGF.ConvertLogicalValueToInt1(Val);
  return Val;
}

//
//
//

llvm::Value *CodeGenFunction::EmitArrayElementPtr(const Expr *Target,
                                                  const ArrayRef<Expr*> Subscripts) {
  ArrayValueExprEmitter EV(*this);
  EV.EmitExpr(Target);
  auto ResultDims = EV.getResultInfo();

  llvm::Value *Offset = EmitDimSubscript(EmitScalarExpr(Subscripts[0]), ResultDims[0]);
  if(Subscripts.size() > 1) {
    llvm::Value *SizeProduct = EmitDimSize(ResultDims[0]);
    for(size_t I = 1; I < Subscripts.size(); ++I) {
      auto Sub = EmitNthDimSubscript(EmitScalarExpr(Subscripts[I]),
                                     ResultDims[I], SizeProduct);
      Offset = Builder.CreateAdd(Offset, Sub);
      if((I + 1) != Subscripts.size())
        SizeProduct = Builder.CreateMul(SizeProduct, EmitDimSize(ResultDims[I]));
    }
  }
  return Builder.CreateGEP(EV.getResultPtr(), Offset);
}

llvm::Value *CodeGenFunction::EmitArrayArgumentPointerValueABI(const Expr *E) {
  if(auto Temp = dyn_cast<ImplicitTempArrayExpr>(E)) {
    E = Temp->getExpression();
    StandaloneArrayValueSectionGatherer EV(*this);
    EV.EmitExpr(E);
    auto DestPtr = CreateTempHeapArrayAlloca(E->getType(), EV.getSections());
    ArrayOperation OP;
    OP.EmitAllScalarValuesAndArraySections(*this, E);
    ArrayLoopEmmitter Looper(*this, EV.getSections());
    Looper.EmitArrayIterationBegin();
    CodeGen::EmitArrayAssignment(*this, OP, Looper, ArrayValueTy(EV.getSections(), DestPtr), E);
    Looper.EmitArrayIterationEnd();
    return DestPtr;
  }
  else if(auto Pack = dyn_cast<ImplicitArrayPackExpr>(E)) {
    // FIXME strided array - allocate memory and pack / unpack
  }

  ArrayValueExprEmitter EV(*this);
  EV.EmitExpr(E);
  return EV.getResultPtr();
}

llvm::Value *CodeGenFunction::EmitConstantArrayConstructor(const ArrayConstructorExpr *E) {
  auto Items = E->getItems();
  SmallVector<llvm::Constant*, 16> Values(Items.size());
  for(size_t I = 0; I < Items.size(); ++I)
    Values[I] = EmitConstantExpr(Items[I]);
  auto Arr = llvm::ConstantArray::get(getTypes().ConvertArrayTypeForMem(E->getType()->asArrayType()),
                                      Values);
  return Builder.CreateConstGEP2_64(CGM.EmitConstantArray(Arr), 0, 0);
}

llvm::Value *CodeGenFunction::EmitTempArrayConstructor(const ArrayConstructorExpr *E) {
  // FIXME: implied-do, heap allocations

  auto Items = E->getItems();
  auto ATy = E->getType()->asArrayType();
  auto VMATy = getTypes().ConvertArrayTypeForMem(ATy);
  auto Arr = Builder.CreateConstGEP2_64(CreateTempAlloca(VMATy, "array-constructor-temp"), 0, 0);
  for(uint64_t I = 0, Size = VMATy->getArrayNumElements(); I < Size; ++I) {
    auto Dest = Builder.CreateConstInBoundsGEP1_64(Arr, I);
    EmitStore(EmitRValue(Items[I]), LValueTy(Dest), ATy->getElementType());
  }
  return Arr;
}

llvm::Value *CodeGenFunction::EmitArrayConstructor(const ArrayConstructorExpr *E) {
  if(E->isEvaluatable(getContext()))
    return EmitConstantArrayConstructor(E);
  return EmitTempArrayConstructor(E);
}

void CodeGenFunction::EmitArrayAssignment(const Expr *LHS, const Expr *RHS) {  
  ArrayOperation OP;
  auto LHSArray = OP.EmitArrayExpr(*this, LHS);
  OP.EmitAllScalarValuesAndArraySections(*this, RHS);
  ArrayLoopEmmitter Looper(*this, LHSArray.Sections);
  Looper.EmitArrayIterationBegin();
  // Array = array / scalar
  CodeGen::EmitArrayAssignment(*this, OP, Looper, LHS, RHS);
  Looper.EmitArrayIterationEnd();
}

//
// Masked array assignment emmitter
//

class WhereBodyPreOperationEmmitter : public ConstStmtVisitor<WhereBodyPreOperationEmmitter> {
  CodeGenFunction &CGF;
  ArrayOperation  &Operation;
public:

  WhereBodyPreOperationEmmitter(CodeGenFunction &cgf, ArrayOperation &Op)
    : CGF(cgf), Operation(Op) {}

  void VisitBlockStmt(const BlockStmt *S) {
    for(auto I : S->getStatements())
      Visit(I);
  }
  void VisitAssignmentStmt(const AssignmentStmt *S) {
    Operation.EmitAllScalarValuesAndArraySections(CGF, S->getLHS());
    Operation.EmitAllScalarValuesAndArraySections(CGF, S->getRHS());
  }
  void VisitConstructPartStmt(const ConstructPartStmt*) {}
  void VisitStmt(const Stmt*) {
    llvm_unreachable("invalid where statement!");
  }
};

class WhereBodyEmmitter : public ConstStmtVisitor<WhereBodyEmmitter> {
  CodeGenFunction &CGF;
  ArrayOperation  &Operation;
  ArrayLoopEmmitter &Looper;
public:

  WhereBodyEmmitter(CodeGenFunction &cgf, ArrayOperation &Op,
                    ArrayLoopEmmitter &Loop)
    : CGF(cgf), Operation(Op), Looper(Loop) {}

  void VisitBlockStmt(const BlockStmt *S) {
    for(auto I : S->getStatements())
      Visit(I);
  }
  void VisitAssignmentStmt(const AssignmentStmt *S) {
    EmitArrayAssignment(CGF, Operation, Looper, S->getLHS(), S->getRHS());
  }
};

void CodeGenFunction::EmitWhereStmt(const WhereStmt *S) {
  // FIXME: evaluate the mask array before the loop (only if required?)
  // FIXME: evaluation of else scalars and sections must strictly follow the then body?

  ArrayOperation OP;
  auto MaskArray = OP.EmitArrayExpr(*this, S->getMask());
  WhereBodyPreOperationEmmitter BodyPreEmmitter(*this, OP);
  BodyPreEmmitter.Visit(S->getThenStmt());
  if(S->getElseStmt())
    BodyPreEmmitter.Visit(S->getElseStmt());

  ArrayLoopEmmitter Looper(*this, MaskArray.Sections);
  Looper.EmitArrayIterationBegin();
  auto ThenBB = createBasicBlock("where-true");
  auto EndBB  = createBasicBlock("where-end");
  auto ElseBB = S->hasElseStmt()? createBasicBlock("where-else") : EndBB;
  Builder.CreateCondBr(EmitArrayConditional(*this, OP, Looper, S->getMask()), ThenBB, ElseBB);
  WhereBodyEmmitter BodyEmmitter(*this, OP, Looper);
  EmitBlock(ThenBB);
  BodyEmmitter.Visit(S->getThenStmt());
  EmitBranch(EndBB);
  if(S->hasElseStmt()) {
    EmitBlock(ElseBB);
    BodyEmmitter.Visit(S->getElseStmt());
    EmitBranch(EndBB);
  }
  EmitBlock(EndBB);
  Looper.EmitArrayIterationEnd();
}

}
} // end namespace flang
