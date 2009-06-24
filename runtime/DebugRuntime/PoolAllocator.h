//===- PoolAllocator.h - Pool allocator runtime interface file --*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file defines the interface which is implemented by the LLVM pool
// allocator runtime library.
//
//===----------------------------------------------------------------------===//

#ifndef _SC_POOLALLOCATOR_RUNTIME_H_
#define _SC_POOLALLOCATOR_RUNTIME_H_

#include "SafeCodeRuntime.h"

#include "llvm/ADT/DenseMap.h"

NAMESPACE_SC_BEGIN

extern DebugPoolTy dummyPool;

// Splay tree of external objects
extern RangeSplaySet<> ExternalObjects;

// Records Out of Bounds pointer rewrites; also used by OOB rewrites for
// exactcheck() calls
extern DebugPoolTy OOBPool;

// Map between rewrite pointer and source file information
extern llvm::DenseMap<void *, const char*>    RewriteSourcefile;
extern llvm::DenseMap<void *, unsigned> RewriteLineno;
extern llvm::DenseMap<const void *, const void *>   RewrittenPointers;

// Record from which object an OOB pointer originates
extern llvm::DenseMap<void *, std::pair<const void *, const void * > > RewrittenObjs;


NAMESPACE_SC_END
#endif
