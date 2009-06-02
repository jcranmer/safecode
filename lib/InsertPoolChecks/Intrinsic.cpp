//===- Intrinsic.cpp - Insert declaration of SAFECode intrinsics -------------//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements a module pass to insert declarations of the SAFECode
// intrinsics into the bitcode file. It also provides interfaces for later
// passes which use these intrinsics.
//
//===----------------------------------------------------------------------===//

#include "safecode/Intrinsic.h"
#include "safecode/VectorListHelper.h"
#include "safecode/SAFECodeConfig.h"
#include "safecode/Support/AllocatorInfo.h"

#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Constants.h"

using namespace llvm;
NAMESPACE_SC_BEGIN

#define REG_FUNC(type, name, index, ret,  ...) do { addIntrinsic(type, name, FunctionType::get(ret, args<const Type*>::list(__VA_ARGS__), false), index); } while (0)

//
// Method: runOnModule()
//
// Description:
//  This is the entry point for this Module Pass.  It will insert the necessary
//  SAFECode run-time functions into the Module.
//
// Inputs:
//  M - A reference to the Module to modify.
//
// Return value:
//  true  - The module was modified.
//  false - The module was not modified.
//
bool
InsertSCIntrinsic::runOnModule(Module & M) {
  currentModule = &M;
  TD = &getAnalysis<TargetData>();
  static const Type * VoidTy = Type::VoidTy;
  static const Type * Int32Ty = Type::Int32Ty;
  static const Type * vpTy = PointerType::getUnqual(Type::Int8Ty);

  REG_FUNC(SC_INTRINSIC_MEMCHECK,	"sc.lscheck",		1, VoidTy, vpTy, vpTy);
  REG_FUNC(SC_INTRINSIC_MEMCHECK,	"sc.lscheckui",	1, VoidTy, vpTy, vpTy);
  REG_FUNC(SC_INTRINSIC_MEMCHECK,	"sc.lscheckalign", 1, VoidTy, vpTy, vpTy, Int32Ty);
  REG_FUNC(SC_INTRINSIC_MEMCHECK,	"sc.lscheckalignui", 1, VoidTy, vpTy, vpTy, Int32Ty);
  REG_FUNC(SC_INTRINSIC_GEPCHECK,	"sc.boundscheck",	 2, vpTy, vpTy, vpTy, vpTy);
  REG_FUNC(SC_INTRINSIC_GEPCHECK,	"sc.boundscheckui",2, vpTy, vpTy, vpTy, vpTy);
  REG_FUNC(SC_INTRINSIC_GEPCHECK,	"sc.exactcheck", 	 2, vpTy, Int32Ty, Int32Ty, vpTy);
  REG_FUNC(SC_INTRINSIC_GEPCHECK,	"sc.exactcheck2",  1, vpTy, vpTy, vpTy, Int32Ty);
  REG_FUNC(SC_INTRINSIC_MEMCHECK,	"sc.funccheck", 	1, VoidTy, Int32Ty, vpTy, vpTy);
  REG_FUNC(SC_INTRINSIC_OOB,	        "sc.get_actual_val",	0, vpTy, vpTy, vpTy);
  REG_FUNC(SC_INTRINSIC_MISC,		"sc.pool_register",	1, VoidTy, vpTy, vpTy, Int32Ty);
  REG_FUNC(SC_INTRINSIC_MISC,		"sc.pool_unregister",	1, VoidTy, vpTy, vpTy);
  REG_FUNC(SC_INTRINSIC_MISC,		"sc.register_globals",	0, VoidTy);
  REG_FUNC(SC_INTRINSIC_MISC,		"sc.init_runtime",	0, VoidTy);
  REG_FUNC(SC_INTRINSIC_MISC,		"sc.init_pool_runtime" ,0, VoidTy, Int32Ty, Int32Ty, Int32Ty);

  // We always change the module.
  return true;
}

//
// Method: addIntrinsic()
//
// Description:
//  Create and register a new function as a SAFECode intrinsic function.
//
// Inputs:
//  type     - The type of intrinsic check.
//  name     - The name of the function.
//  FTy      - The LLVM type of the intrinsic function.
//  ptrindex - The index of the operand to the function which is used to take
//             the pointer which the intrinsic checks.  This is unused for
//             non-run-time checking intrinsics.
//
//
void
InsertSCIntrinsic::addIntrinsic (IntrinsicType type,
                                 const std::string & name,
                                 FunctionType * FTy,
                                 unsigned ptrindex) {
  //
  // Check that this pass has already analyzed an LLVM Module.
  //
  assert (currentModule && "No LLVM Module!");

  // Structure describing the new intrinsic function
  IntrinsicInfoTy info;

  //
  // Create the new intrinsic function and configure its SAFECode attributes.
  //
  info.type = type;
  info.F = dyn_cast<Function> (currentModule->getOrInsertFunction(name, FTy));
  info.ptrindex = ptrindex;

  //
  // Map the function name and LLVM Function pointer to its SAFECode attributes.
  //
  intrinsics.push_back (info);
  intrinsicNameMap.insert (std::make_pair (name, intrinsics.size() - 1));
}

const InsertSCIntrinsic::IntrinsicInfoTy &
InsertSCIntrinsic::getIntrinsic(const std::string & name) const {
  std::map<std::string, uint32_t>::const_iterator it = intrinsicNameMap.find(name);
  assert(it != intrinsicNameMap.end() && "Intrinsic should be defined before uses!");
  return intrinsics[it->second];
}

//
// Method: isSCIntrinsic()
//
// Description:
//  Determine whether the specified LLVM value is a call to a SAFECode
//  intrinsic.
//
// Inputs:
//  inst - The LLVM Value to check.  It can be any value, including
//         non-instruction values.
//
// Return value:
//  true  - The LLVM value is a call to a SAFECode run-time function.
//  false - The LLVM value is not a call to a SAFECode run-time function.
//
bool
InsertSCIntrinsic::isSCIntrinsic(Value * inst) const {
  if (!isa<CallInst>(inst)) 
    return false;

  CallInst * CI = cast<CallInst>(inst);
  Function * F= CI->getCalledFunction();
  if (!F)
    return false;

  const std::string & name = F->getName();

  return intrinsicNameMap.find(name) != intrinsicNameMap.end();
}

//
// Method: isCheckingIntrinsic()
//
// Description:
//  Determine whether the specified LLVM value is a call instruction to a
//  SAFECode run-time check.
//
// Inputs:
//  inst - An LLVM value which may be a call instruction.  It can be any LLVM
//         value (including non-instruction values).
//
// Return value:
//  true  - The LLVM value is a call instruction to a SAFECode run-time check.
//  false - The LLVM value is not a call instruction to a SAFECode run-time
//          check.
//
bool
InsertSCIntrinsic::isCheckingIntrinsic (Value * inst) const {
  //
  // Determine whether the instruction is a call to a SAFECode intrinsic.  If
  // if isn't, we know it's not a checking instruction.
  //
  if (!isSCIntrinsic(inst))
    return false;

  CallInst * CI = dyn_cast<CallInst>(inst);
  assert (CI && "Change in API; Intrinsic is not a CallInst!");

  std::string name = CI->getCalledFunction()->getName();
  const IntrinsicInfoTy & info = getIntrinsic(name);

  //
  // Memory checks and GEP checks are both SAFECode checking intrinsics.
  //
  return (info.type == SC_INTRINSIC_MEMCHECK) ||
         (info.type == SC_INTRINSIC_GEPCHECK);
}

//
// Method: isGEPCheckingIntrinsic()
//
// Description:
//  Determine whether the specified LLVM value is a call instruction to a
//  SAFECode run-time check that does a bounds check.
//
// Inputs:
//  V - An LLVM value which may be a call instruction.  It can be any LLVM
//      value (including non-instruction values).
//
// Return value:
//  true  - The LLVM value is a call instruction to a SAFECode GEP check.
//  false - The LLVM value is not a call instruction to a SAFECode GEP check.
//
bool
InsertSCIntrinsic::isGEPCheckingIntrinsic (Value * V) const {
  //
  // Determine whether the instruction is a call to a SAFECode intrinsic.  If
  // if isn't, we know it's not a checking instruction.
  //
  if (!isSCIntrinsic(V))
    return false;

  CallInst * CI = dyn_cast<CallInst>(V);
  assert (CI && "Change in API; Intrinsic is not a CallInst!");

  std::string name = CI->getCalledFunction()->getName();
  const IntrinsicInfoTy & info = getIntrinsic(name);

  //
  // Memory checks and GEP checks are both SAFECode checking intrinsics.
  //
  return (info.type == SC_INTRINSIC_GEPCHECK);
}

Value *
InsertSCIntrinsic::getCheckedPointer (CallInst * CI) {
  if (isCheckingIntrinsic (CI)) {
    const IntrinsicInfoTy & info = intrinsics[intrinsicNameMap[CI->getCalledFunction()->getName()]];

    //
    // Return the checked pointer in the call.  We use ptrindex + 1 because the
    // index is the index in the function signature, but in a CallInst, the
    // first argument is the function pointer.
    //
    return (CI->getOperand(info.ptrindex+1));
  }

  return 0;
}


//
// Check to see if we're indexing off the beginning of a known object.  If
// so, then find the size of the object.  Otherwise, return NULL.
//
Value *
InsertSCIntrinsic::getObjectSize(Value * V) {
  if (GlobalVariable * GV = dyn_cast<GlobalVariable>(V)) {
    return ConstantInt::get(Type::Int32Ty, TD->getTypeAllocSize (GV->getType()->getElementType()));
  }

  if (AllocationInst * AI = dyn_cast<AllocationInst>(V)) {
    unsigned int type_size = TD->getTypeAllocSize (AI->getAllocatedType());
    if (AI->isArrayAllocation()) {
      if (ConstantInt * CI = dyn_cast<ConstantInt>(AI->getArraySize())) {
        if (CI->getSExtValue() > 0) {
          type_size *= CI->getSExtValue();
        } else {
          return NULL;
        }
      }
    }
    return ConstantInt::get(Type::Int32Ty, type_size);
  }

  // Customized allocators

  if (CallInst * CI = dyn_cast<CallInst>(V)) {
    Function * F = CI->getCalledFunction();
    if (!F)
      return NULL;

    const std::string & name = F->getName();
    for (SAFECodeConfiguration::alloc_iterator it = SCConfig->alloc_begin(),
           end = SCConfig->alloc_end(); it != end; ++it) {
      if ((*it)->isAllocSizeMayConstant(CI) && (*it)->getAllocCallName() == name) {
        return (*it)->getAllocSize(CI);
      }
    }
  }

  return NULL;
}

char InsertSCIntrinsic::ID = 0;
static llvm::RegisterPass<InsertSCIntrinsic> X ("sc-insert-intrinsic", "insert SAFECode's intrinsic");

NAMESPACE_SC_END
