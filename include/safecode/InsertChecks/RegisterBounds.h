//===- InsertChecks/RegisterBounds.h ----------------------------*- C++ -*----//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// Various passes to register the bound information of variables into the pools
//
//===----------------------------------------------------------------------===//

#ifndef _REGISTER_BOUNDS_H_
#define _REGISTER_BOUNDS_H_

#include "safecode/SAFECode.h"
#include "safecode/Intrinsic.h"
#include "safecode/PoolHandles.h"

#include "llvm/Pass.h"
#include "llvm/Module.h"
#include "llvm/Target/TargetData.h"

NAMESPACE_SC_BEGIN

/// Base class of all passes which register variables into pools.
class RegisterVariables : public llvm::ModulePass {
public:
  RegisterVariables(uint32_t id) : llvm::ModulePass(id) {}
  virtual ~RegisterVariables();
  virtual bool runOnModule(llvm::Module & M) = 0;
protected:
  void init(llvm::Module & M);
  /// Helper function to register the bound information of a variable into a
  /// particular pool.
  void RegisterVariableIntoPool(llvm::Value * PH, llvm::Value * val, llvm::Value * AllocSize, llvm::Instruction * InsertBefore);

  /// Helper function to create the body of sc.register_globals /
  /// sc.register_main. It inserts an empty basicblock and a ret void
  /// instruction into the function.
  ///
  /// Return the last instruction of the function body.
  llvm::Instruction * CreateRegistrationFunction(llvm::Function * F);

  InsertSCIntrinsic * intrinsic;
  Function * PoolRegisterFunc;

};

/// Register the bound information of global variables.
/// All registeration are placed at sc.register_globals
class RegisterGlobalVariables : public RegisterVariables {
public:
  static char ID;
  RegisterGlobalVariables() : RegisterVariables((uintptr_t) &ID) {}
  const char * getPassName() const 
  { return "Register Global Variables into Pools"; }

  virtual bool runOnModule(llvm::Module & M);

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.addRequired<InsertSCIntrinsic>();
    AU.addRequired<llvm::TargetData>();
    AU.addRequired<DSNodePass>();
    DSNodePass::preservePAandDSA(AU);
    AU.setPreservesCFG();
  }


private:
  void registerGV(GlobalVariable * GV, Instruction * InsertBefore);
  DSNodePass * dsnPass;

  TargetData * TD;
};

/// Register the bound information of argv[] in main().
/// All registeration are placed at sc.register_main_args
class RegisterMainArgs : public RegisterVariables {
public:
  static char ID;
  const char * getPassName() const { return "Register argv[] into Pools";}
  RegisterMainArgs() : RegisterVariables((uintptr_t) &ID) {}
  virtual bool runOnModule(llvm::Module & M);
  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.addRequired<InsertSCIntrinsic>();
    AU.setPreservesAll();
  }
};

/// Register the bound information of custom allocators
/// such as kmem_cache_alloc.
///
/// FIXME: Haohui
/// Ideally, the pass should be organized as a FunctionPass. It should ask other
/// analysis passes for all allocation sites, and register them.
/// Now I hard-coded the allocation inside the pass since it is only used by SVA
/// kernel, and DSA does not have the functionality to point out all allocation
/// site yet.
///
/// Now the pass scan through all uses of customized allocators and add
/// registration right after them.

class AllocatorInfo;

class RegisterCustomizedAllocation : public RegisterVariables {
public:
  static char ID;
  const char * getPassName() const 
  { return "Register customized allocations into Pools";}
  RegisterCustomizedAllocation() : RegisterVariables((uintptr_t) &ID) {}
  virtual bool runOnModule(llvm::Module & M);
  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.addRequired<InsertSCIntrinsic>();
    AU.addRequired<DSNodePass>();
    AU.addRequiredTransitive<PoolAllocateGroup>();
    AU.setPreservesAll();
  }

private:
  DSNodePass * dsnPass;
  Function * PoolUnregisterFunc;
  PoolAllocateGroup * paPass;
  void registerAllocationSite(llvm::CallInst * AllocSite, AllocatorInfo * info);
  void registerFreeSite(llvm::CallInst * FreeSite, AllocatorInfo * info);
  void proceedAllocator(llvm::Module * M, AllocatorInfo * info);
};

// Pass to register byval arguments
class RegisterFunctionByvalArguments : public RegisterVariables {
public:
  static char ID;
  const char * getPassName() const { return "Register byval arguments of functions";}
  RegisterFunctionByvalArguments() : RegisterVariables((uintptr_t) &ID) {}
  virtual bool runOnModule(llvm::Module & M);
  virtual bool runOnFunction(llvm::Function & F);
  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.addRequired<InsertSCIntrinsic>();
    AU.addRequired<llvm::TargetData>();
    AU.addRequired<DSNodePass>();
    // Pretend we do nothing
    AU.setPreservesAll();
  }
private:
  InsertSCIntrinsic * intrinsic;
  DSNodePass * dsnPass;
  TargetData * TD;
  Function * StackFree;
};

NAMESPACE_SC_END

#endif
