//===- RegisterStackObjPass.cpp - Pass to Insert Stack Object Registration ---//
// 
//                          The SAFECode Compiler 
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass instruments code to register stack objects with the appropriate
// pool.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "stackreg"

#include "safecode/SAFECode.h"

#include "SCUtils.h"
#include "InsertPoolChecks.h"
#include "llvm/Instruction.h"
#include "llvm/Module.h"
#include "llvm/ADT/VectorExtras.h"
#include "llvm/ADT/Statistic.h"
#include "safecode/VectorListHelper.h"

NAMESPACE_SC_BEGIN

char RegisterStackObjPass::ID = 0;

static RegisterPass<RegisterStackObjPass> passRegStackObj ("reg-stack-obj", "register stack objects into pools");

// Pass Statistics
namespace {
  // Object registration statistics
  STATISTIC (StackRegisters,      "Stack registrations");
  STATISTIC (SavedRegAllocs,      "Stack registrations avoided");
}

////////////////////////////////////////////////////////////////////////////
// Static Functions
////////////////////////////////////////////////////////////////////////////

// Prototypes of the poolunregister function
static Constant * StackFree = 0;

//
// Function: insertPoolFrees()
//
// Description:
//  This function takes a list of alloca instructions and inserts code to
//  unregister them at every unwind and return instruction.
//
// Inputs:
//  PoolRegisters - The list of calls to poolregister() inserted for stack
//                  objects.
//  ExitPoints    - The list of instructions that can cause the function to
//                  return.
//  Context       - The LLVM Context in which to insert instructions.
//
static void
insertPoolFrees (const std::vector<CallInst *> & PoolRegisters,
                 const std::vector<Instruction *> & ExitPoints,
                 LLVMContext * Context) {
  // List of alloca instructions we create to store the pointers to be
  // deregistered.
  std::vector<AllocaInst *> PtrList;

  // List of pool handles; this is a parallel array to PtrList
  std::vector<Value *> PHList;

  // The infamous void pointer type
  PointerType * VoidPtrTy = PointerType::getUnqual(Type::Int8Ty);

  //
  // Create alloca instructions for every registered alloca.  These will hold
  // a pointer to the registered stack objects and will be referenced by
  // poolunregister().
  //
  for (unsigned index = 0; index < PoolRegisters.size(); ++index) {
    //
    // Take the first element off of the worklist.
    //
    CallInst * CI = PoolRegisters[index];

    //
    // Get the pool handle and allocated pointer from the poolregister() call.
    //
    Value * PH  = CI->getOperand(1);
    Value * Ptr = CI->getOperand(2);

    //
    // Create a place to store the pointer returned from alloca.  Initialize it
    // with a null pointer.
    //
    BasicBlock & EntryBB = CI->getParent()->getParent()->getEntryBlock();
    Instruction * InsertPt = &(EntryBB.front());
    AllocaInst * PtrLoc = new AllocaInst (VoidPtrTy,
                                          Ptr->getName() + ".st",
                                          InsertPt);
    Value * NullPointer = ConstantPointerNull::get(VoidPtrTy);
    new StoreInst (NullPointer, PtrLoc, InsertPt);

    //
    // Store the registered pointer into the memory we allocated in the entry
    // block.
    //
    new StoreInst (Ptr, PtrLoc, CI);

    //
    // Record the alloca that stores the pointer to deregister.
    // Record the pool handle with it.
    //
    PtrList.push_back (PtrLoc);
    PHList.push_back (PH);
  }

  //
  // For each point where the function can exit, insert code to deregister all
  // stack objects.
  //
  for (unsigned index = 0; index < ExitPoints.size(); ++index) {
    //
    // Take the first element off of the worklist.
    //
    Instruction * Return = ExitPoints[index];

    //
    // Deregister each registered stack object.
    //
    for (unsigned i = 0; i < PtrList.size(); ++i) {
      //
      // Get the location holding the pointer and the pool handle associated
      // with it.
      //
      AllocaInst * PtrLoc = PtrList[i];
      Value * PH = PHList[i];

      //
      // Generate a load instruction to get the registered pointer.
      //
      LoadInst * Ptr = new LoadInst (PtrLoc, "", Return);

      //
      // Create the call to poolunregister().
      //
      std::vector<Value *> args;
      args.push_back (PH);
      args.push_back (Ptr);
      CallInst::Create (StackFree, args.begin(), args.end(), "", Return);
    }
  }
}

////////////////////////////////////////////////////////////////////////////
// RegisterStackObjPass Methods
////////////////////////////////////////////////////////////////////////////
 
//
// Method: runOnFunction()
//
// Description:
//  This is the entry point for this LLVM function pass.  The pass manager will
//  call this method for every function in the Module that will be transformed.
//
// Inputs:
//  F - A reference to the function to transform.
//
// Outputs:
//  F - The function will be modified to register and unregister stack objects.
//
// Return value:
//  true  - The function was modified.
//  false - The function was not modified.
//
bool
RegisterStackObjPass::runOnFunction(Function & F) {
  //
  // Get prerequisite analysis information.
  //
  TD = &getAnalysis<TargetData>();
  LI = &getAnalysis<LoopInfo>();
  intrinsic = &getAnalysis<InsertSCIntrinsic>();
  dsnPass = &getAnalysis<DSNodePass>();
  paPass = dsnPass->paPass;

  //
  // Get pointers to the functions for registering and unregistering pointers.
  //
  PoolRegister = intrinsic->getIntrinsic("sc.pool_register").F;  
  StackFree = intrinsic->getIntrinsic("sc.pool_unregister").F;  

  // The set of registered stack objects
  std::vector<CallInst *> PoolRegisters;

  // The set of stack objects within the function.
  std::vector<AllocaInst *> AllocaList;

  // The set of instructions that can cause the function to return to its
  // caller.
  std::vector<Instruction *> ExitPoints;

  //
  // Scan the function to register allocas and find locations where registered
  // allocas to be deregistered.
  //
  for (Function::iterator BI = F.begin(); BI != F.end(); ++BI) {
    //
    // Create a list of alloca instructions to register.  Note that we create
    // the list ahead of time because registerAllocaInst() will create new
    // alloca instructions.
    //
    for (BasicBlock::iterator I = BI->begin(); I != BI->end(); ++I) {
      if (AllocaInst * AI = dyn_cast<AllocaInst>(I)) {
        //
        // Ensure that the alloca is not within a loop; we don't support yet.
        //
        if (LI->getLoopFor (BI)) {
          assert (0 &&
                  "Register Stack Objects: No support for alloca in loop!\n");
          abort();
        }
        AllocaList.push_back (AI);
      }
    }

    //
    // Add calls to register the allocated stack objects.
    //
    while (AllocaList.size()) {
      AllocaInst * AI = AllocaList.back();
      AllocaList.pop_back();
      if (CallInst * CI = registerAllocaInst (AI))
        PoolRegisters.push_back(CI);
    }

    //
    // If the terminator instruction of this basic block can return control
    // flow back to the caller, mark it as a place where a deregistration
    // is needed.
    //
    Instruction * Terminator = BI->getTerminator();
    if ((isa<ReturnInst>(Terminator)) || (isa<UnwindInst>(Terminator))) {
      ExitPoints.push_back (Terminator);
    }
  }

  //
  // Insert poolunregister calls for all of the registered allocas.
  //
  insertPoolFrees (PoolRegisters, ExitPoints, &getGlobalContext());

  //
  // Conservatively assume that we've changed the function.
  //
  return true;
}

//
// Method: registerAllocaInst()
//
// Description:
//  Register a single alloca instruction.
//
// Inputs:
//  AI - The alloca which requires registration.
//
// Return value:
//  NULL - The alloca was not registered.
//  Otherwise, the call to poolregister() is returned.
//
CallInst *
RegisterStackObjPass::registerAllocaInst (AllocaInst *AI) {
  //
  // Get the function information for this function.
  //
  Function *F = AI->getParent()->getParent();
  PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
  Value *temp = FI->MapValueToOriginal(AI);
  AllocaInst *AIOrig = AI;
  if (temp)
    AIOrig = dyn_cast<AllocaInst>(temp);

  //
  // Get the pool handle for the node that this contributes to...
  //
  Function *FOrig  = AIOrig->getParent()->getParent();
  DSNode *Node = dsnPass->getDSNode(AIOrig, FOrig);
  assert (Node && "Alloca does not have DSNode!\n");
  assert ((Node->isAllocaNode()) && "DSNode for alloca is missing stack flag!");

  //
  // Only register the stack allocation if it may be the subject of a
  // run-time check.  This can only occur when the object is used like an
  // array because:
  //  1) GEP checks are only done when accessing arrays.
  //  2) Load/Store checks are only done on collapsed nodes (which appear to
  //     be used like arrays).
  //
#if 0
  if (!(Node->isArray()))
    return 0;
#endif

  //
  // Determine if we have ever done a check on this alloca or a pointer
  // aliasing this alloca.  If not, then we can forego the check (even if we
  // can't trace through all the data flow).
  //
  // FIXME:
  //  This implementation is incorrect.  A node in the DSGraph will have
  //  different DSNodes in different functions (because each function has its
  //  own copy of the DSGraph).  We will need to find another way to do this
  //  optimization.
  //
  if (dsnPass->isDSNodeChecked(Node)) {
    ++SavedRegAllocs;
    return 0;
  }

  //
  // Determine if any use (direct or indirect) escapes this function.  If
  // not, then none of the checks will consult the MetaPool, and we can
  // forego registering the alloca.
  //
  bool MustRegisterAlloca = false;
  std::vector<Value *> AllocaWorkList;
  AllocaWorkList.push_back (AI);
  while ((!MustRegisterAlloca) && (AllocaWorkList.size())) {
    Value * V = AllocaWorkList.back();
    AllocaWorkList.pop_back();
    Value::use_iterator UI = V->use_begin();
    for (; UI != V->use_end(); ++UI) {
      // We cannot handle PHI nodes or Select instructions
      if (isa<PHINode>(UI) || isa<SelectInst>(UI)) {
        MustRegisterAlloca = true;
        continue;
      }

      // The pointer escapes if it's stored to memory somewhere.
      StoreInst * SI;
      if ((SI = dyn_cast<StoreInst>(UI)) && (SI->getOperand(0) == V)) {
        MustRegisterAlloca = true;
        continue;
      }

      // GEP instructions are okay, but need to be added to the worklist
      if (isa<GetElementPtrInst>(UI)) {
        AllocaWorkList.push_back (*UI);
        continue;
      }

      // Cast instructions are okay as long as they cast to another pointer
      // type
      if (CastInst * CI = dyn_cast<CastInst>(UI)) {
        if (isa<PointerType>(CI->getType())) {
          AllocaWorkList.push_back (*UI);
          continue;
        } else {
          MustRegisterAlloca = true;
          continue;
        }
      }

#if 0
      if (ConstantExpr *cExpr = dyn_cast<ConstantExpr>(UI)) {
        if (cExpr->getOpcode() == Instruction::Cast) {
          AllocaWorkList.push_back (*UI);
          continue;
        } else {
          MustRegisterAlloca = true;
          continue;
        }
      }
#endif

      CallInst * CI1;
      if ((CI1 = dyn_cast<CallInst>(UI))) {
        if (!(CI1->getCalledFunction())) {
          MustRegisterAlloca = true;
          continue;
        }

        std::string FuncName = CI1->getCalledFunction()->getName();
        if (FuncName == "exactcheck3") {
          AllocaWorkList.push_back (*UI);
          continue;
        } else if ((FuncName == "llvm.memcpy.i32")    || 
                   (FuncName == "llvm.memcpy.i64")    ||
                   (FuncName == "llvm.memset.i32")    ||
                   (FuncName == "llvm.memset.i64")    ||
                   (FuncName == "llvm.memmove.i32")   ||
                   (FuncName == "llvm.memmove.i64")   ||
                   (FuncName == "llva_memcpy")        ||
                   (FuncName == "llva_memset")        ||
                   (FuncName == "llva_strncpy")       ||
                   (FuncName == "llva_invokememcpy")  ||
                   (FuncName == "llva_invokestrncpy") ||
                   (FuncName == "llva_invokememset")  ||
                   (FuncName == "memcmp")) {
          continue;
        } else {
          MustRegisterAlloca = true;
          continue;
        }
      }
    }
  }

  if (!MustRegisterAlloca) {
    ++SavedRegAllocs;
    return 0;
  }

  //
  // Insert the alloca registration.
  //
  Value *PH = dsnPass->getPoolHandle(AIOrig, FOrig, *FI);
  if (PH == 0 || isa<ConstantPointerNull>(PH)) return 0;

  //
  // Create an LLVM Value for the allocation size.  Insert a multiplication
  // instruction if the allocation allocates an array.
  //
  unsigned allocsize = TD->getTypeAllocSize(AI->getAllocatedType());
  Value *AllocSize = getGlobalContext().getConstantInt(Type::Int32Ty, allocsize);
  if (AI->isArrayAllocation())
    AllocSize = BinaryOperator::Create(Instruction::Mul, AllocSize,
                                       AI->getOperand(0), "sizetmp", AI);

  //
  // Attempt to insert the call to register the alloca'ed object after all of
  // the alloca instructions in the basic block.
  //
  Instruction *iptI = AI;
  BasicBlock::iterator InsertPt = AI;
  iptI = ++InsertPt;
  if (AI->getParent() == (&(AI->getParent()->getParent()->getEntryBlock()))) {
    InsertPt = AI->getParent()->begin();
    while (&(*(InsertPt)) != AI)
      ++InsertPt;
    while (isa<AllocaInst>(InsertPt))
      ++InsertPt;
    iptI = InsertPt;
  }

  //
  // Insert a call to register the object.
  //
  PointerType * VoidPtrTy = PointerType::getUnqual(Type::Int8Ty);
  Instruction *Casted = castTo (AI, VoidPtrTy, AI->getName()+".casted", iptI);
  Value * CastedPH    = castTo (PH, VoidPtrTy, PH->getName() + "casted", iptI);
  std::vector<Value *> args;
  args.push_back (CastedPH);
  args.push_back (Casted);
  args.push_back (AllocSize);

  // Update statistics
  ++StackRegisters;
  return CallInst::Create (PoolRegister, args.begin(), args.end(), "", iptI);
}

NAMESPACE_SC_END
