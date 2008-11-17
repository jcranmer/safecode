#ifndef CSTDLIB_H
#define CSTDLIB_H

#include "llvm/Analysis/Passes.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Pass.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InstVisitor.h"

#include <algorithm>
#include <vector>

#include <fcntl.h>

using namespace llvm;

// Statistics counters
/*
STATISTIC(stat_missed_strcpy, "Total strcpy() calls missed");
*/

// From KTransform.h
namespace llvm {
/*
ModulePass * createSCPass(RegSCPass * KP);
*/
ModulePass * createStringTransformPass();
}

namespace {
	class StringTransform : public ModulePass {

	public:
		static char ID;
		StringTransform() : ModulePass((intptr_t)&ID) {}
                virtual bool runOnModule(Module &M);
                bool strcpyTransform(Module &M);

		virtual void getAnalysisUsage(AnalysisUsage &AU) const {
			AU.setPreservesAll();
		}
		virtual void print(std::ostream &O, const Module *M) const {}
	};

	RegisterPass<StringTransform> X("string_transform", "Secure C standard string library calls");
}

#endif
