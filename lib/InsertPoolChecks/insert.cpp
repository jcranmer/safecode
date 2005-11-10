#include "InsertPoolChecks.h"
#include "llvm/Instruction.h"
#include "llvm/Module.h"

using namespace llvm;
RegisterOpt<InsertPoolChecks> ipc("ipc", "insert runtime checks");

bool InsertPoolChecks::runOnModule(Module &M) {
  cuaPass = &getAnalysis<ConvertUnsafeAllocas>();
  //  budsPass = &getAnalysis<CompleteBUDataStructures>();
  paPass = &getAnalysis<PoolAllocate>();
  equivPass = &(paPass->getECGraphs());
  efPass = &getAnalysis<EmbeCFreeRemoval>();
  //add the new poolcheck prototype 
  addPoolCheckProto(M);
  //Replace old poolcheck with the new one 
  addPoolChecks(M);
  return true;
}

void InsertPoolChecks::addPoolChecks(Module &M) {
  std::vector<Instruction *> & UnsafeGetElemPtrs = cuaPass->getUnsafeGetElementPtrsFromABC();
  std::vector<Instruction *>::const_iterator iCurrent = UnsafeGetElemPtrs.begin(), iEnd = UnsafeGetElemPtrs.end();
  for (; iCurrent != iEnd; ++iCurrent) {
    //we have the GetElementPtr
    //    assert(isa<GetElementPtrInst>(*iCurrent) && " we don't yet handle runtime checks for trusted fns");
    if (!isa<GetElementPtrInst>(*iCurrent)) {
      //Then this must be some trusted call we cant prove safety
      std::cerr << "WARNING : DID NOT HANDLE  \n";
      (*iCurrent)->dump();
      continue;
    }
    GetElementPtrInst *GEP = cast<GetElementPtrInst>(*iCurrent);
    Function *F = GEP->getParent()->getParent();
    // Now we need to decide if we need to pass in the alignmnet
    //for the poolcheck
    assert(!getDSNodeOffset(GEP->getPointerOperand(), F) && " we don't handle middle of structs yet\n");
    
    PA::FuncInfo *FI = paPass->getFuncInfoOrClone(*F);
    Instruction *Casted = GEP;
    if (!FI->ValueMap.empty()) {
      assert(FI->ValueMap.count(GEP) && "Instruction not in the value map \n");
      Instruction *temp = dyn_cast<Instruction>(FI->ValueMap[GEP]);
      assert(temp && " Instruction  not there in the Value map");
      Casted  = temp;
    }
    if (GetElementPtrInst *GEPNew = dyn_cast<GetElementPtrInst>(Casted)) {
      Value *PH = getPoolHandle(GEP, F, *FI);
      if (PH && isa<ConstantPointerNull>(PH)) continue;
    if (!PH) {
      Value *PointerOperand = GEPNew->getPointerOperand();
      if (ConstantExpr *cExpr = dyn_cast<ConstantExpr>(PointerOperand)) {
	if (cExpr->getOpcode() == Instruction::Cast)
	  PointerOperand = cExpr->getOperand(0);
      }
      if (GlobalVariable *GV = dyn_cast<GlobalVariable>(PointerOperand)) {
	if (const ArrayType *AT = dyn_cast<ArrayType>(GV->getType()->getElementType())) {
	  //we need to insert an actual check
	  //It could be a select instruction
	  //First get the size
	  //This only works for one or two dimensional arrays
	  if (GEPNew->getNumOperands() == 2) {
	    Value *secOp = GEPNew->getOperand(1);
	    if (secOp->getType() != Type::UIntTy) {
	      secOp = new CastInst(secOp, Type::UIntTy, secOp->getName()+".casted",
				   Casted);
	    }

	    std::vector<Value *> args(1,secOp);
	    const Type* csiType = Type::getPrimitiveType(Type::IntTyID);
	    args.push_back(ConstantSInt::get(csiType,AT->getNumElements()));
	    CallInst *newCI = new CallInst(ExactCheck,args,"", Casted);
	    DEBUG(std::cerr << "Inserted exact check call Instruction \n");
	    continue;
	  } else if (GEPNew->getNumOperands() == 3) {
	    if (ConstantSInt *COP = dyn_cast<ConstantSInt>(GEPNew->getOperand(1))) {
	      //FIXME assuming that the first array index is 0
	      assert((COP->getRawValue() == 0) && "non zero array index\n");
	      Value * secOp = GEPNew->getOperand(2);
	      if (secOp->getType() != Type::UIntTy) {
		secOp = new CastInst(secOp, Type::UIntTy, secOp->getName()+".casted",
				   Casted);
	      }
	      std::vector<Value *> args(1,secOp);
	      const Type* csiType = Type::getPrimitiveType(Type::IntTyID);
	      args.push_back(ConstantSInt::get(csiType,AT->getNumElements()));
	      CallInst *newCI = new CallInst(ExactCheck,args,"", Casted->getNext());
	      continue;
	    } else {
	      //Handle non constant index two dimensional arrays later
	      abort();
	    }
	  } else {
	    //Handle Multi dimensional cases later
	    std::cerr << "WARNING: Handle multi dimensional globals later\n";
	    (*iCurrent)->dump();
	  }
	}
	std::cerr << " Global variable ok \n";
      }
      //      These must be real unknowns and they will be handled anyway
      //      std::cerr << " WARNING, DID NOT HANDLE   \n";
      //      (*iCurrent)->dump();
      continue ;
    } else {
      if (Casted->getType() != PointerType::get(Type::SByteTy)) {
	Casted = new CastInst(Casted,PointerType::get(Type::SByteTy),
			      (Casted)->getName()+".casted",(Casted)->getNext());
      }
      std::vector<Value *> args(1, PH);
      args.push_back(Casted);
      //Insert it
      CallInst * newCI = new CallInst(PoolCheck,args, "",Casted->getNext());
      std::cerr << "inserted instrcution \n";
    }
    }
  }
}

void InsertPoolChecks::addPoolCheckProto(Module &M) {
  const Type * VoidPtrType = PointerType::get(Type::SByteTy);
  const Type *PoolDescType = ArrayType::get(VoidPtrType, 50);
  //	StructType::get(make_vector<const Type*>(VoidPtrType, VoidPtrType,
  //                                               Type::UIntTy, Type::UIntTy, 0));
  const Type * PoolDescTypePtr = PointerType::get(PoolDescType);
      
  std::vector<const Type *> Arg(1, PoolDescTypePtr);
  Arg.push_back(VoidPtrType);
  FunctionType *PoolCheckTy =
    FunctionType::get(Type::VoidTy,Arg, false);
  PoolCheck = M.getOrInsertFunction("poolcheck", PoolCheckTy);

  std::vector<const Type *> FArg2(1, Type::UIntTy);
  FArg2.push_back(Type::IntTy);
  FunctionType *ExactCheckTy = FunctionType::get(Type::VoidTy, FArg2, false);
  ExactCheck = M.getOrInsertFunction("exactcheck", ExactCheckTy);
  
}

DSNode* InsertPoolChecks::getDSNode(const Value *V, Function *F) {
  DSGraph &TDG = equivPass->getDSGraph(*F);
  DSNode *DSN = TDG.getNodeForValue((Value *)V).getNode();
  return DSN;
}

unsigned InsertPoolChecks::getDSNodeOffset(const Value *V, Function *F) {
  DSGraph &TDG = equivPass->getDSGraph(*F);
  return TDG.getNodeForValue((Value *)V).getOffset();
}

Value *InsertPoolChecks::getPoolHandle(const Value *V, Function *F, PA::FuncInfo &FI) {
  const DSNode *Node = getDSNode(V,F);
  // Get the pool handle for this DSNode...
  //  assert(!Node->isUnknownNode() && "Unknown node \n");
  if (Node->isUnknownNode()) {
    return 0;
  }
  std::map<const DSNode*, Value*>::iterator I = FI.PoolDescriptors.find(Node);
  map <Function *, set<Value *> > &
    CollapsedPoolPtrs = efPass->CollapsedPoolPtrs;
  
  if (I != FI.PoolDescriptors.end()) {
    // Check that the node pointed to by V in the TD DS graph is not
    // collapsed
    if (CollapsedPoolPtrs.count(F)) {
      Value *v = I->second;
      if (CollapsedPoolPtrs[F].find(I->second) !=
	  CollapsedPoolPtrs[F].end()) {
	std::cerr << "Collapsed pools \n";
	return Constant::getNullValue(PoolAllocate::PoolDescPtrTy);
      } else {
	return v;
      }
    } else {
      return I->second;
    } 
  }
  return 0;
}
     
