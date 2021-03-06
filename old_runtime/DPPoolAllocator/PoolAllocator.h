/*===- PoolAllocator.h - Pool allocator runtime interface file --*- C++ -*-===*/
/* 
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
//===----------------------------------------------------------------------===*/

#ifndef POOLALLOCATOR_RUNTIME_H
#define POOLALLOCATOR_RUNTIME_H

typedef struct PoolTy {
  /* Ptr1, Ptr2 - Implementation specified data pointers. */
  void *Ptr1, *Ptr2;

  /* NodeSize - Keep track of the object size tracked by this pool */
  unsigned NodeSize;

  /* FreeablePool - Set to false if the memory from this pool cannot be freed
  // before destroy.
  */
  void *Ptr3; /*to keep track of unmapped list*/ 
} PoolTy;

#ifdef __cplusplus
extern "C" {
#endif
  void poolinit(PoolTy *Pool, unsigned NodeSize);
  void poolmakeunfreeable(PoolTy *Pool);
  void pooldestroy(PoolTy *Pool);
  
  void *poolalloc(PoolTy *Pool, unsigned NumBytes);
  void *poolrealloc(PoolTy *Pool, void *Node, unsigned NumBytes);
  void *poolstrdup(PoolTy *Pool, char *Node);
  void poolfree(PoolTy *Pool, void *Node);
  void poolcheck(PoolTy *Pool, void *Node);
#ifdef __cplusplus
}
#endif
#endif
