//===- ExactCheckOpt.cpp -------------------------------------------------- --//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
//  This pass tries to lower bounds checks and load/store checks to exact
//  checks, that is checks whose bounds information can be determined easily,
//  say, allocations inside a function or global variables. Therefore SAFECode
//  does not need to register stuffs in the meta-data.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "exactcheck-opt"

#include "llvm/ADT/Statistic.h"
#include "safecode/AllocatorInfo.h"
#include "safecode/OptimizeChecks.h"
#include "safecode/Utility.h"

#include <queue>

namespace llvm {

static RegisterPass<ExactCheckOpt> X ("exactcheck-opt", "Exact check optimization", true);

// Pass Statistics
namespace {
  STATISTIC (ExactChecks ,    "The number of checks lowered to exactcheck");
}

char ExactCheckOpt::ID = 0;

//
// Method: runOnModule()
//
// Description:
//  This method is the entry point for the transform pass.
//
// Inputs:
//  M - The LLVM module to transform.
//
// Return value:
//  true  - The module was modified.
//  false - The module was not modified.
//
bool
ExactCheckOpt::runOnModule(Module & M) {
  //
  // Add a prototype for the exactcheck function.
  //
  Type * VoidTy    = Type::getVoidTy (M.getContext());
  Type * VoidPtrTy = getVoidPtrType (M.getContext());
  Type * Int32Type = IntegerType::getInt32Ty(M.getContext());
  ExactCheck2 = cast<Function>(M.getOrInsertFunction ("exactcheck2",
                                                      VoidPtrTy,
                                                      VoidPtrTy,
                                                      VoidPtrTy,
                                                      VoidPtrTy,
                                                      Int32Type,
                                                      NULL));

  FastLSCheck = cast<Function>(M.getOrInsertFunction ("fastlscheck",
                                                      VoidTy,
                                                      VoidPtrTy,
                                                      VoidPtrTy,
                                                      Int32Type,
                                                      Int32Type,
                                                      NULL));

  //
  // Add the readnone attribute to the fast checks; they don't use global state
  // to determine if a pointer passes the check.
  //
  // To clarify, these fuction have Attribute::ReadNone because they are
  // purely functions of their input parameters -- unlike boundscheck()
  // (which has Attribute::ReadOnly) whose output can be influenced by 
  // changes in the heap.
  //
  ExactCheck2->addFnAttr (Attribute::ReadNone);
  FastLSCheck->addFnAttr (Attribute::ReadNone);

  //
  // Scan through all the intrinsics and process those that perform run-time
  // checks.
  //
  for (unsigned index = 0; index < numChecks; ++index) {
    //
    // Skip function checks and string checks.
    //
    if ((RuntimeChecks[index].checkType == funccheck) ||
        (RuntimeChecks[index].checkType == strcheck)) {
      continue;
    }

    //
    // Also skip gepchecks and memchecks that that we have inserted
    // in this pass.  At present this means that we skip calls to
    // exactcheck2() and fastlscheck(), and their debug versions
    // exactcheck2_debug() and fastlscheck_debug().
    //
    // Note the use of strncmp() to match both regular and debug 
    // versions.
    //
    if (((RuntimeChecks[index].checkType == gepcheck) &&
         (strncmp("exactcheck2", RuntimeChecks[index].name,
                  strlen("exactcheck2")) == 0)) ||
        ((RuntimeChecks[index].checkType == memcheck) &&
         (strncmp("fastlscheck", RuntimeChecks[index].name,
                  strlen("fastlscheck")) == 0))) {
      continue;
    }

    //
    // Clear the list of calls to intrinsics that must be removed.
    //
    checkingIntrinsicsToBeRemoved.clear();

    //
    // Scan through all uses of this run-time checking function and process
    // each call to it.
    //
    Function * F = M.getFunction (RuntimeChecks[index].name);
    if (F) {
      for (Value::use_iterator UI = F->use_begin(), E = F->use_end();
           UI != E;
           ++UI) {
        if (CallInst * CI = dyn_cast<CallInst>(*UI)) {
          visitCheckingIntrinsic(CI, RuntimeChecks[index]);
        }
      }
    }

    //
    // Update statistics if anything has changed.  We don't want to update
    // the statistics variable if nothing has happened because we don't want
    // it to appear in the output if it is zero.
    //
    if (checkingIntrinsicsToBeRemoved.size())
      ExactChecks += checkingIntrinsicsToBeRemoved.size();

    //
    // Remove checking intrinsics that have been optimized
    //
    for (std::vector<CallInst*>::const_iterator i = checkingIntrinsicsToBeRemoved.begin(), e = checkingIntrinsicsToBeRemoved.end(); i != e; ++i) {
      (*i)->eraseFromParent();
    }
  }

  //
  // Conservatively assume that we have changed something in the module.
  //
  return true;
}

//
// Function: findObject()
//
// Description:
//  Find the singular memory object to which this pointer points (if such a
//  singular object exists and is easy to find).
//
static Value *
findObject (Value * obj) {
  // Values that we have already examined
  std::set<Value *> exploredObjects;

  // Values that could potentiall by the memory object
  std::set<Value *> objects;

  // Queue of values to examine next
  std::queue<Value *> queue;

  //
  // Start with the initial value.
  //
  queue.push(obj);
  while (!queue.empty()) {
    //
    // Take an element off the queue.  Strip all pointer casts as we just
    // skip through them.
    //
    Value * o = queue.front()->stripPointerCasts();
    queue.pop();

    //
    // If we have already explore this object, skip it.
    //
    if (exploredObjects.count(o)) continue;
    exploredObjects.insert(o);

    if (ConstantExpr * CE = dyn_cast<ConstantExpr >(o)) {
      switch (CE->getOpcode()) {
        case Instruction::GetElementPtr: {
          Value * Operand = CE->getOperand(0);
          if (!isa<ConstantPointerNull>(Operand))
            queue.push(Operand);
          break;
        }

        case Instruction::Select:
        default:
          objects.insert(o);
          break;
      }
    } else if (isa<GetElementPtrInst>(o)) {
      queue.push(cast<GetElementPtrInst>(o)->getPointerOperand());
    } else if (isa<PHINode>(o)) {
      PHINode * p = cast<PHINode>(o);
      for (unsigned int i = 0; i < p->getNumIncomingValues(); ++i) {
        queue.push(p->getIncomingValue(i));
      }
    } else if (SelectInst * SI = dyn_cast<SelectInst>(o)) {
      if (!isa<ConstantPointerNull>(SI->getTrueValue()))
        queue.push(SI->getTrueValue());
      if (!isa<ConstantPointerNull>(SI->getFalseValue()))
        queue.push(SI->getFalseValue());
    } else {
      objects.insert(o);
    }
  }

  return objects.size() == 1 ? *(objects.begin()) : NULL;
}

//
// Function: visitCheckingIntrinsic()
//
// Description:
//  Attempts to rewrite an extensive check into an efficient, accurate array
//  bounds check which will not use meta-data information.
//
// Inputs:
//  CI    - A pointer to the instruction that performs a run-time check.
//  Info  - A reference to a structure containing information on the
//         run-time check.
//
// Return value:
//  true  - Successfully rewrite the check into an exact check.
//  false - Cannot perform the optimization.
//
bool
ExactCheckOpt::visitCheckingIntrinsic (CallInst * CI,
                                       const struct CheckInfo & Info) {
  //
  // Get the pointer that is checked by this run-time check.
  //
  Value * CheckPtr = Info.getCheckedPointer(CI)->stripPointerCasts();
  Value * CheckLen = Info.getCheckedLength(CI);

  //
  // Try to find the source of the pointer.
  //
  Value * BasePtr = findObject (CheckPtr);
  if (!BasePtr) return false;

  //
  // If the run time check is a gepcheck, get the source pointer and
  // store it in SrcPtr.  Note that this value may be NULL.  This will
  // typically be the resulf of a program trying to do a GEP off of 
  // a NULL pointer to calculate an offset or because it hasn't checked 
  // a malloc() call for a NULL return value.  I am given to understand
  // that we allow GEPs to index within the first 4096 bytes of memory 
  // because it's unmapped; if it indexes beyond that, it generates a 
  // rewrite pointer.
  //
  // Otherwise, set SrcPtr to NULL.
  //
  // Note that we make no attempt to strip casts or otherwise
  // lookup the source of this pointer -- unlike with the BasePtr.
  //
  Value * SrcPtr = NULL;
  if ( Info.isGEPCheck() ) {
    SrcPtr = Info.getSourcePointer(CI);
  }

  //
  // Do not use exactchecks on global variables that are defined in other
  // compilation units.
  //
  if (GlobalValue * GV = dyn_cast<GlobalValue>(BasePtr)) {
    if (GV->isDeclaration()) {
      return false;
    }
  }

  //
  // If the call is to a memory checking function, then we cannot blindly
  // convert a check that operates on a heap object; the heap object might be
  // deallocated between the time it was allocated and the time of the check.
  // Other checks can be converted since they don't try to detect dangling
  // pointers.
  //
  // So, if this is a memory check, make sure that the object cannot be freed
  // before the check.  Global variables and stack allocations cannot be freed.
  //
  Argument * AI;
  if ((!(Info.isMemCheck())) ||
      ((isa<AllocaInst>(BasePtr)) || isa<GlobalVariable>(BasePtr)) ||
      ((AI = dyn_cast<Argument>(BasePtr)) && (AI->hasByValAttr()))){

    //
    // Attempt to get the size of the pointer.  If a size is returned, we know
    // that the base pointer points to the beginning of an object, and we can
    // do a run-time check without a lookup.
    //
    AllocatorInfoPass & AIP = getAnalysis<AllocatorInfoPass>();
    if (Value * Size = AIP.getObjectSize(BasePtr)) {
      rewriteToExactCheck(Info.isMemCheck(), CI, SrcPtr, BasePtr, CheckPtr, CheckLen, Size);
      return true;
    }
  }

  //
  // We were not able to insert a call to exactcheck().
  //
  return false;
}

//
// Method: rewriteToExactCheck()
//
// Description:
//  Rewrite a check into an exact check
//
// Inputs:
//  isMemCheck    - Flags if we are replacing a load/store check.
//  SourcePointer - If target call is a gepcheck, this is the source pointer
//                  of the indexing operation (the GEP).  This value is
//                  obtained from the parameter list of the target gepcheck.
//                  NULL otherwise.
//  BasePointer   - An LLVM Value representing the base of the object to check.
//  ResultPointer - An LLVM Value representing the pointer to check.
//  ResultLength  - An LLVM Value representing the length of the memory access.
//                  This can be NULL when not applicable to the check.
//  Bounds        - An LLVM Value representing the bounds of the check.
//
void
ExactCheckOpt::rewriteToExactCheck(bool isMemCheck, CallInst * CI,
                                   Value * SourcePointer, 
                                   Value * BasePointer, 
                                   Value * ResultPointer,
                                   Value * ResultLength,
                                   Value * Bounds) {

  // The LLVM type for a void *
  Type *VoidPtrType = getVoidPtrType(CI->getContext()); 
  Type * Int32Type = IntegerType::getInt32Ty(CI->getContext());

  //
  // For readability, make sure that both the base pointer and the result
  // pointer have names.
  //
  if ((SourcePointer != NULL) &&
      (!(SourcePointer->hasName()))) SourcePointer->setName("source");
  if (!(BasePointer->hasName())) BasePointer->setName("base");
  if (!(ResultPointer->hasName())) ResultPointer->setName("result");

  //
  // Cast the operands to the correct type.
  //
  if (SourcePointer != NULL) {
    SourcePointer = castTo (SourcePointer, VoidPtrType,
                            SourcePointer->getName()+".ec.casted",
                            CI);
  }

  BasePointer = castTo (BasePointer, VoidPtrType,
                        BasePointer->getName()+".ec.casted",
                        CI);

  ResultPointer = castTo (ResultPointer, VoidPtrType,
                          ResultPointer->getName()+".ec.casted",
                          CI);

  Value * CastBounds = Bounds;
  if (Bounds->getType() != Int32Type) {
    LLVMContext & Context = Int32Type->getContext();
    CastBounds = CastInst::CreateIntegerCast (CastBounds,
                                              Type::getInt32Ty(Context),
                                              false,
                                              CastBounds->getName(),
                                              CI);
  }

  //
  // Create the call to exactcheck2() or to fastlscheck().
  //
  std::vector<Value *> args;

  if (!isMemCheck) { // push first argument for exactcheck2()
    args.push_back(SourcePointer);
  }
  args.push_back(BasePointer);
  args.push_back(ResultPointer);
  args.push_back(CastBounds);
  if (ResultLength) args.push_back(ResultLength);
  Function * Check = (isMemCheck) ? FastLSCheck : ExactCheck2;
  CallInst * ExactCheckCI = CallInst::Create (Check, args, "", CI);

  //
  // Copy the debug metadata from the original check to the exactcheck.
  //
  if (MDNode * MD = CI->getMetadata ("dbg"))
    ExactCheckCI->setMetadata ("dbg", MD);

  //
  // boundscheck / exactcheck return an out of bound pointer when REWRITE_OOB is
  // enabled. We need to replace all uses to make the optimization correct, but
  // we don't need do anything for load / store checks.
  //
  // We can test the condition above by simply testing the return types of the
  // checking functions.
  //
  if (ExactCheckCI->getType() == CI->getType()) {
    CI->replaceAllUsesWith(ExactCheckCI);
  }

  checkingIntrinsicsToBeRemoved.push_back(CI);
}

}
