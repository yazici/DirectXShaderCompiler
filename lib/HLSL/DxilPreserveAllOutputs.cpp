///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilPreserveAllOutputs.cpp                                                //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Ensure we store to all elements in the output signature.                  //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/HLSL/DxilGenerationPass.h"
#include "dxc/HLSL/DxilOperations.h"
#include "dxc/HLSL/DxilSignatureElement.h"
#include "dxc/HLSL/DxilModule.h"
#include "dxc/Support/Global.h"
#include "dxc/HLSL/DxilInstructions.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Pass.h"
#include "llvm/IR/IRBuilder.h"
#include <llvm/ADT/DenseSet.h>

using namespace llvm;
using namespace hlsl;

namespace {
class OutputWrite {
public:
  explicit OutputWrite(CallInst *call)
    : m_Call(call)
  {
    assert(DxilInst_StoreOutput(call) || DxilInst_StorePatchConstant(call));
  }

  unsigned GetSignatureID() const {
    Value *id = m_Call->getOperand(SignatureIndex);
    return cast<ConstantInt>(id)->getLimitedValue();
  }

  DxilSignatureElement &GetSignatureElement(DxilModule &DM) const {
    if (DxilInst_StorePatchConstant(m_Call))
      return DM.GetPatchConstantSignature().GetElement(GetSignatureID());
    else
      return DM.GetOutputSignature().GetElement(GetSignatureID());
  }

  CallInst *GetStore() const {
    return m_Call;
  }

  Value *GetValue() const {
    return m_Call->getOperand(ValueIndex);
  }

  Value *GetRow() const {
    return m_Call->getOperand(RowIndex);
  }
  
  Value *GetColumn() const {
    return m_Call->getOperand(ColumnIndex);
  }

  void DeleteStore() {
    m_Call->eraseFromParent();
    m_Call = nullptr;
  }

private:
  CallInst *m_Call;
  enum OperandIndex {
    SignatureIndex = 1,
    RowIndex = 2,
    ColumnIndex = 3,
    ValueIndex = 4,
  };
};

class OutputElement {
public:
  explicit OutputElement(const DxilSignatureElement &outputElement)
    : m_OutputElement(outputElement)
    , m_Rows(outputElement.GetRows())
    , m_Columns(outputElement.GetCols())
  {
  }

  void CreateAlloca(IRBuilder<> &builder) {
    LLVMContext &context = builder.getContext();
    Type *elementType = m_OutputElement.GetCompType().GetLLVMType(context);
    Type *allocaType = nullptr;
    if (IsSingleElement())
      allocaType = elementType;
    else
      allocaType = ArrayType::get(elementType, NumElements());
    m_Alloca = builder.CreateAlloca(allocaType, nullptr, m_OutputElement.GetName());
  }

  void StoreTemp(IRBuilder<> &builder, Value *row, Value *col, Value *value) const {
    Value *addr = GetTempAddr(builder, row, col);
    builder.CreateStore(value, addr);
  }

  void StoreOutput(IRBuilder<> &builder, DxilModule &DM) const {
    for (unsigned row = 0; row < m_Rows; ++row)
      for (unsigned col = 0; col < m_Columns; ++col) {
        StoreOutput(builder, DM, row, col);
      }
  }

  unsigned NumElements() const {
    return m_Rows * m_Columns;
  }

private:
  const DxilSignatureElement &m_OutputElement;
  unsigned m_Rows;
  unsigned m_Columns;
  AllocaInst *m_Alloca;

  bool IsSingleElement() const {
    return m_Rows == 1 && m_Columns == 1;
  }

  Value *GetAsI32(IRBuilder<> &builder, Value *col) const {
    assert(col->getType()->isIntegerTy());
    Type *i32Ty = builder.getInt32Ty();
    if (col->getType() != i32Ty) {
      if (col->getType()->getScalarSizeInBits() > i32Ty->getScalarSizeInBits())
        col = builder.CreateTrunc(col, i32Ty);
      else
        col = builder.CreateZExt(col, i32Ty);
    }

    return col;
  }

  Value *GetTempAddr(IRBuilder<> &builder, Value *row, Value *col) const {
    // Load directly from alloca for non-array output.
    if (IsSingleElement())
      return m_Alloca;
    else
      return CreateGEP(builder, row, col);
  }

  Value *CreateGEP(IRBuilder<> &builder, Value *row, Value *col) const {
    assert(m_Alloca);
    Constant *rowStride = ConstantInt::get(row->getType(), m_Columns);
    Value *rowOffset = builder.CreateMul(row, rowStride);
    Value *index     = builder.CreateAdd(rowOffset, GetAsI32(builder, col));
    return builder.CreateInBoundsGEP(m_Alloca, {builder.getInt32(0), index});
  }
  
  Value *LoadTemp(IRBuilder<> &builder, Value *row,  Value *col) const {
    Value *addr = GetTempAddr(builder, row, col);
    return builder.CreateLoad(addr);
  }
  
  void StoreOutput(IRBuilder<> &builder, DxilModule &DM, unsigned row, unsigned col) const {
    Value *opcodeV = builder.getInt32(static_cast<unsigned>(GetOutputOpCode()));
    Value *sigID = builder.getInt32(m_OutputElement.GetID());
    Value *rowV = builder.getInt32(row);
    Value *colV = builder.getInt8(col);
    Value *val = LoadTemp(builder, rowV, colV);
    Value *args[] = { opcodeV, sigID, rowV, colV, val };
    Function *Store = GetOutputFunction(DM);
    builder.CreateCall(Store, args);
  }

  DXIL::OpCode GetOutputOpCode() const {
    if (m_OutputElement.IsPatchConstant())
      return DXIL::OpCode::StorePatchConstant;
    else
      return DXIL::OpCode::StoreOutput;
  }

  Function *GetOutputFunction(DxilModule &DM) const {
    hlsl::OP *opInfo = DM.GetOP();
    return opInfo->GetOpFunc(GetOutputOpCode(), m_OutputElement.GetCompType().GetLLVMBaseType(DM.GetCtx()));
  }
    
};

class DxilPreserveAllOutputs : public FunctionPass {
private:

public:
  static char ID; // Pass identification, replacement for typeid
  DxilPreserveAllOutputs() : FunctionPass(ID) {}

  const char *getPassName() const override {
    return "DXIL preserve all outputs";
  }

  bool runOnFunction(Function &F) override;

private:
  typedef std::vector<OutputWrite> OutputVec;
  typedef std::map<unsigned, OutputElement>  OutputMap;
  OutputVec collectOutputStores(Function &F);
  OutputMap generateOutputMap(const OutputVec &calls, DxilModule &DM);
  void createTempAllocas(OutputMap &map, IRBuilder<> &builder);
  void insertTempOutputStores(const OutputVec &calls, const OutputMap &map, IRBuilder<> &builder);
  void insertFinalOutputStores(Function &F, const OutputMap &outputMap, IRBuilder<> &builder, DxilModule &DM);
  void removeOriginalOutputStores(OutputVec &outputStores);
};

bool DxilPreserveAllOutputs::runOnFunction(Function &F) {
  DxilModule &DM = F.getParent()->GetOrCreateDxilModule();
  
  OutputVec outputStores = collectOutputStores(F);
  if (outputStores.empty())
    return false;

  IRBuilder<> builder(F.getEntryBlock().getFirstInsertionPt());
  OutputMap outputMap = generateOutputMap(outputStores, DM);
  createTempAllocas(outputMap, builder);
  insertTempOutputStores(outputStores, outputMap, builder);
  insertFinalOutputStores(F,outputMap, builder, DM);
  removeOriginalOutputStores(outputStores);

  return false;
}

DxilPreserveAllOutputs::OutputVec DxilPreserveAllOutputs::collectOutputStores(Function &F) {
  OutputVec calls;
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *inst = &*I;
    DxilInst_StoreOutput storeOutput(inst);
    DxilInst_StorePatchConstant storePatch(inst);

    if (storeOutput || storePatch)
      calls.emplace_back(cast<CallInst>(inst));
  }
  return calls;
}

DxilPreserveAllOutputs::OutputMap DxilPreserveAllOutputs::generateOutputMap(const OutputVec &calls, DxilModule &DM) {
  OutputMap map;
  for (const OutputWrite &output : calls) {
    unsigned sigID = output.GetSignatureID();
    if (map.count(sigID))
      continue;

    map.insert(std::make_pair(sigID, OutputElement(output.GetSignatureElement(DM))));
  }

  return map;
}

void DxilPreserveAllOutputs::createTempAllocas(OutputMap &outputMap, IRBuilder<> &builder)
{
  for (auto &iter: outputMap) {
    OutputElement &output = iter.second;
    output.CreateAlloca(builder);
  }
}

void DxilPreserveAllOutputs::insertTempOutputStores(const OutputVec &writes, const OutputMap &map, IRBuilder<>& builder)
{
  for (const OutputWrite& outputWrite : writes) {
    OutputMap::const_iterator iter = map.find(outputWrite.GetSignatureID());
    assert(iter != map.end());
    const OutputElement &output = iter->second;

    builder.SetInsertPoint(outputWrite.GetStore());
    output.StoreTemp(builder, outputWrite.GetRow(), outputWrite.GetColumn(), outputWrite.GetValue());
  }
}

void DxilPreserveAllOutputs::insertFinalOutputStores(Function &F, const OutputMap & outputMap, IRBuilder<>& builder, DxilModule & DM)
{
  // Find all return instructions.
  SmallVector<ReturnInst *, 4> returns;
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *inst = &*I;
    if (ReturnInst *ret = dyn_cast<ReturnInst>(inst))
      returns.push_back(ret);
  }

  // Write all outputs before each return. 
  for (ReturnInst *ret : returns) {
    for (const auto &iter : outputMap) {
      const OutputElement &output = iter.second;
      builder.SetInsertPoint(ret);
      output.StoreOutput(builder, DM);
    }
  }
}

void DxilPreserveAllOutputs::removeOriginalOutputStores(OutputVec & outputStores)
{
  for (OutputWrite &write : outputStores) {
    write.DeleteStore();
  }
}

}

char DxilPreserveAllOutputs::ID = 0;

FunctionPass *llvm::createDxilPreserveAllOutputsPass() {
  return new DxilPreserveAllOutputs();
}

INITIALIZE_PASS(DxilPreserveAllOutputs,
                "hlsl-dxil-preserve-all-outputs",
                "DXIL preserve all outputs", false, false)
