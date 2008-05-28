//===- PoolAllocatorBitMask.cpp - Implementation of poolallocator runtime -===//
// 
//                       The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file is one possible implementation of the LLVM pool allocator runtime
// library.
//
// This uses the 'Ptr1' field to maintain a linked list of slabs that are either
// empty or are partially allocated from.  The 'Ptr2' field of the PoolTy is
// used to track a linked list of slabs which are full, ie, all elements have
// been allocated from them.
//
//===----------------------------------------------------------------------===//

#include "PoolAllocator.h"
#include "PageManager.h"
#include "adl_splay.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
//#include <sys/ucontext.h>
#define DEBUG(x) 

// global variable declarations
extern unsigned PPageSize;
static unsigned globalallocID = 0;
static unsigned globalfreeID = 0;
static void * globalTemp = 0;
static unsigned alertNum = 0;
static PoolTy dummyPool;
static unsigned dummyInitialized = 0;
unsigned poolmemusage = 0;


/* Set to 1 to log object registrations */
static /*const*/ unsigned logregs = 0;

// signal handler
static void bus_error_handler(int, siginfo_t *, void *);

// creates a new PtrMetaData structure to record pointer information
static inline void updatePtrMetaData(PDebugMetaData, unsigned, void *);
static PDebugMetaData createPtrMetaData (unsigned,
                                         unsigned,
                                         void *,
                                         void *,
                                         void *);

//===----------------------------------------------------------------------===//
//
//  PoolSlab implementation
//
//===----------------------------------------------------------------------===//

// PoolSlab Structure - Hold multiple objects of the current node type.
// Invariants: FirstUnused <= UsedEnd
//
struct PoolSlab {
  PoolSlab **PrevPtr, *Next;
  bool isSingleArray;   // If this slab is used for exactly one array

private:
  // FirstUnused - First empty node in slab
  unsigned short FirstUnused;

  // UsedBegin - The first node in the slab that is used.
  unsigned short UsedBegin;

  // UsedEnd - 1 past the last allocated node in slab. 0 if slab is empty
  unsigned short UsedEnd;

  // NumNodesInSlab - This contains the number of nodes in this slab, which
  // effects the size of the NodeFlags vector, and indicates the number of nodes
  // which are in the slab.
  unsigned int NumNodesInSlab;

  // NodeFlagsVector - This array contains two bits for each node in this pool
  // slab.  The first (low address) bit indicates whether this node has been
  // allocated, and the second (next higher) bit indicates whether this is the
  // start of an allocation.
  //
  // This is a variable sized array, which has 2*NumNodesInSlab bits (rounded up
  // to 4 bytes).
  unsigned NodeFlagsVector[1];
  
  bool isNodeAllocated(unsigned NodeNum) {
    return NodeFlagsVector[NodeNum/16] & (1 << (NodeNum & 15));
  }

  void markNodeAllocated(unsigned NodeNum) {
    NodeFlagsVector[NodeNum/16] |= 1 << (NodeNum & 15);
  }

  void markNodeFree(unsigned NodeNum) {
    NodeFlagsVector[NodeNum/16] &= ~(1 << (NodeNum & 15));
  }

  void setStartBit(unsigned NodeNum) {
    NodeFlagsVector[NodeNum/16] |= 1 << ((NodeNum & 15)+16);
  }

  bool isStartOfAllocation(unsigned NodeNum) {
    return NodeFlagsVector[NodeNum/16] & (1 << ((NodeNum & 15)+16));
  }
  
  void clearStartBit(unsigned NodeNum) {
    NodeFlagsVector[NodeNum/16] &= ~(1 << ((NodeNum & 15)+16));
  }

  void assertOkay (void) {
    assert (FirstUnused <= UsedEnd);
    assert ((UsedEnd == getSlabSize()) || (!isNodeAllocated(UsedEnd)));
    assert ((FirstUnused == getSlabSize()) || (!isNodeAllocated(FirstUnused)));
  }
public:
  // create - Create a new (empty) slab and add it to the end of the Pools list.
  static PoolSlab *create(PoolTy *Pool);

  // createSingleArray - Create a slab for a large singlearray with NumNodes
  // entries in it, returning the pointer into the pool directly.
  static void *createSingleArray(PoolTy *Pool, unsigned NumNodes);

  // getSlabSize - Return the number of nodes that each slab should contain.
  static unsigned getSlabSize(PoolTy *Pool) {
    // We need space for the header...
    unsigned NumNodes = PageSize-sizeof(PoolSlab);
    
    // We need space for the NodeFlags...
    unsigned NodeFlagsBytes = NumNodes/Pool->NodeSize * 2 / 8;
    NumNodes -= (NodeFlagsBytes+3) & ~3;  // Round up to int boundaries.

    // Divide the remainder among the nodes!
    return NumNodes / Pool->NodeSize;
  }

  void addToList(PoolSlab **PrevPtrPtr) {
    PoolSlab *InsertBefore = *PrevPtrPtr;
    *PrevPtrPtr = this;
    PrevPtr = PrevPtrPtr;
    Next = InsertBefore;
    if (InsertBefore) InsertBefore->PrevPtr = &Next;
  }

  void unlinkFromList() {
    *PrevPtr = Next;
    if (Next) Next->PrevPtr = PrevPtr;
  }

  unsigned getSlabSize() const {
    return NumNodesInSlab;
  }

  // destroy - Release the memory for the current object.
  void destroy();

  // isEmpty - This is a quick check to see if this slab is completely empty or
  // not.
  bool isEmpty() const { return UsedEnd == 0; }

  // isFull - This is a quick check to see if the slab is completely allocated.
  //
  bool isFull() const { return isSingleArray || FirstUnused == getSlabSize(); }

  // allocateSingle - Allocate a single element from this pool, returning -1 if
  // there is no space.
  int allocateSingle();

  // allocateMultiple - Allocate multiple contiguous elements from this pool,
  // returning -1 if there is no space.
  int allocateMultiple(unsigned Num);

  // getElementAddress - Return the address of the specified element.
  void *getElementAddress(unsigned ElementNum, unsigned ElementSize) {
    char *Data = (char*)&NodeFlagsVector[((unsigned)NumNodesInSlab+15)/16];
    return &Data[ElementNum*ElementSize];
  }
  
  const void *getElementAddress(unsigned ElementNum, unsigned ElementSize)const{
    const char *Data =
      (const char *)&NodeFlagsVector[(unsigned)(NumNodesInSlab+15)/16];
    return &Data[ElementNum*ElementSize];
  }

  // containsElement - Return the element number of the specified address in
  // this slab.  If the address is not in slab, return -1.
  int containsElement(void *Ptr, unsigned ElementSize) const;

  // freeElement - Free the single node, small array, or entire array indicated.
  void freeElement(unsigned short ElementIdx);
  
  // getSize --- size of an allocation
  unsigned getSize(void *Node, unsigned ElementSize);
  
  // lastNodeAllocated - Return one past the last node in the pool which is
  // before ScanIdx, that is allocated.  If there are no allocated nodes in this
  // slab before ScanIdx, return 0.
  unsigned lastNodeAllocated(unsigned ScanIdx);
};

// create - Create a new (empty) slab and add it to the end of the Pools list.
PoolSlab *
PoolSlab::create(PoolTy *Pool) {
  unsigned NodesPerSlab = getSlabSize(Pool);

  unsigned Size = sizeof(PoolSlab) + 4*((NodesPerSlab+15)/16) +
    Pool->NodeSize*getSlabSize(Pool);
  assert(Size <= PageSize && "Trying to allocate a slab larger than a page!");
  PoolSlab *PS = (PoolSlab*)AllocatePage();

  PS->NumNodesInSlab = NodesPerSlab;
  PS->isSingleArray = 0;  // Not a single array!
  PS->FirstUnused = 0;    // Nothing allocated.
  PS->UsedBegin   = 0;    // Nothing allocated.
  PS->UsedEnd     = 0;    // Nothing allocated.

  for (unsigned i = 0; i < PS->getSlabSize(); ++i)
  {
    PS->markNodeFree(i);
    PS->clearStartBit(i);
  }

  // Add the slab to the list...
  PS->addToList((PoolSlab**)&Pool->Ptr1);
  //  printf(" creating a slab %x\n", PS);
  return PS;
}

void *
PoolSlab::createSingleArray(PoolTy *Pool, unsigned NumNodes) {
  // FIXME: This wastes memory by allocating space for the NodeFlagsVector
  unsigned NodesPerSlab = getSlabSize(Pool);
  assert(NumNodes > NodesPerSlab && "No need to create a single array!");

  unsigned NumPages = (NumNodes+NodesPerSlab-1)/NodesPerSlab;
  PoolSlab *PS = (PoolSlab*)AllocateNPages(NumPages);

  assert(PS && "poolalloc: Could not allocate memory!");

  if (Pool->NumSlabs > AddrArrSize)
    Pool->Slabs->insert((void*)PS);
  else if (Pool->NumSlabs == AddrArrSize) {
    // Create the hash_set
    Pool->Slabs = new hash_set<void *>;
    Pool->Slabs->insert((void *)PS);
    for (unsigned i = 0; i < AddrArrSize; ++i)
      Pool->Slabs->insert((void *) Pool->SlabAddressArray[i]);
  } else {
    // Insert it in the array
    Pool->SlabAddressArray[Pool->NumSlabs] = (unsigned) PS;
  }
  Pool->NumSlabs++;
  
  PS->addToList((PoolSlab**)&Pool->LargeArrays);

  PS->isSingleArray = 1;
  PS->NumNodesInSlab = NumPages * PageSize;
  *(unsigned*)&PS->FirstUnused = NumPages;
  return PS->getElementAddress(0, 0);
}

void
PoolSlab::destroy() {
  if (isSingleArray)
    for (unsigned NumPages = *(unsigned*)&FirstUnused; NumPages != 1;--NumPages)
      FreePage((char*)this + (NumPages-1)*PageSize);

  FreePage(this);
}

// allocateSingle - Allocate a single element from this pool, returning -1 if
// there is no space.
int
PoolSlab::allocateSingle() {
  // If the slab is a single array, go on to the next slab.  Don't allocate
  // single nodes in a SingleArray slab.
  if (isSingleArray) return -1;

  unsigned SlabSize = getSlabSize();

  // Check to see if there are empty entries at the end of the slab...
  if (UsedEnd < SlabSize) {
    // Mark the returned entry used
    unsigned short UE = UsedEnd;
    markNodeAllocated(UE);
    setStartBit(UE);
    
    // If we are allocating out the first unused field, bump its index also
    if (FirstUnused == UE) {
      FirstUnused++;
    }
    
    // Return the entry, increment UsedEnd field.
    ++UsedEnd;
    assertOkay();
    return UE;
  }
  
  // If not, check to see if this node has a declared "FirstUnused" value that
  // is less than the number of nodes allocated...
  //
  if (FirstUnused < SlabSize) {
    // Successfully allocate out the first unused node
    unsigned Idx = FirstUnused;
    markNodeAllocated(Idx);
    setStartBit(Idx);
    
    // Increment FirstUnused to point to the new first unused value...
    // FIXME: this should be optimized
    unsigned short FU = FirstUnused;
    do {
      ++FU;
    } while ((FU != SlabSize) && (isNodeAllocated(FU)));
    FirstUnused = FU;
    
    assertOkay();
    return Idx;
  }
  
  assertOkay();
  return -1;
}

// allocateMultiple - Allocate multiple contiguous elements from this pool,
// returning -1 if there is no space.
int
PoolSlab::allocateMultiple(unsigned Size) {
  // Do not allocate small arrays in SingleArray slabs
  if (isSingleArray) return -1;

  // For small array allocation, check to see if there are empty entries at the
  // end of the slab...
  if (UsedEnd+Size <= getSlabSize()) {
    // Mark the returned entry used and set the start bit
    unsigned UE = UsedEnd;
    setStartBit(UE);
    for (unsigned i = UE; i != UE+Size; ++i)
      markNodeAllocated(i);
    
    // If we are allocating out the first unused field, bump its index also
    if (FirstUnused == UE)
      FirstUnused += Size;

    // Increment UsedEnd
    UsedEnd += Size;

    // Return the entry
    assertOkay();
    return UE;
  }

  // If not, check to see if this node has a declared "FirstUnused" value
  // starting which Size nodes can be allocated
  //
  unsigned Idx = FirstUnused;
  while (Idx+Size <= getSlabSize()) {
    assert(!isNodeAllocated(Idx) && "FirstUsed is not accurate!");

    // Check if there is a continuous array of Size nodes starting FirstUnused
    unsigned LastUnused = Idx+1;
    for (; (LastUnused != Idx+Size) && (!isNodeAllocated(LastUnused)); ++LastUnused)
      /*empty*/;

    // If we found an unused section of this pool which is large enough, USE IT!
    if (LastUnused == Idx+Size) {
      setStartBit(Idx);
      // FIXME: this loop can be made more efficient!
      for (unsigned i = Idx; i != Idx + Size; ++i)
        markNodeAllocated(i);

      // This should not be allocating on the end of the pool, so we don't need
      // to bump the UsedEnd pointer.
      assert(Idx != UsedEnd && "Shouldn't allocate at end of pool!");

      // If we are allocating out the first unused field, bump its index also.
      if (Idx == FirstUnused) {
        unsigned SlabSize = getSlabSize();
        unsigned i;
        for (i = FirstUnused+Size; i < UsedEnd; ++i) {
          if (!isNodeAllocated(i)) {
            break;
          }
        }
        FirstUnused = i;
        if (isNodeAllocated(i))
          FirstUnused = SlabSize;
      }
      
      // Return the entry
      assertOkay();
      return Idx;
    }

    // Otherwise, try later in the pool.  Find the next unused entry.
    Idx = LastUnused;
    while (Idx+Size <= getSlabSize() && isNodeAllocated(Idx))
      ++Idx;
  }

  assertOkay();
  return -1;
}

// getSize
unsigned PoolSlab::getSize(void *Ptr, unsigned ElementSize) {
  const void *FirstElement = getElementAddress(0, 0);
  if (FirstElement <= Ptr) {
    unsigned Delta = (char*)Ptr-(char*)FirstElement;
    unsigned Index = Delta/ElementSize;
    
    if (Index < getSlabSize()) {
      //we have the index now do something like free
      assert(isStartOfAllocation(Index) &&
        "poolrealloc: Attempt to realloc from the middle of allocated array\n");
      unsigned short ElementEndIdx = Index + 1;
      
      // FIXME: This should use manual strength reduction to produce decent code.
      unsigned short UE = UsedEnd;
      while (ElementEndIdx != UE &&
          !isStartOfAllocation(ElementEndIdx) && 
          isNodeAllocated(ElementEndIdx)) {
        ++ElementEndIdx;
      }
      return (ElementEndIdx - Index);
    }
  }
  if (logregs)
  {
    fprintf(stderr, "PoolSlab::getSize failed!\n");
    fflush(stderr);
  }
  abort();
}


// containsElement - Return the element number of the specified address in
// this slab.  If the address is not in slab, return -1.
int
PoolSlab::containsElement(void *Ptr, unsigned ElementSize) const {
  const void *FirstElement = getElementAddress(0, 0);
  if (FirstElement <= Ptr) {
    unsigned Delta = (char*)Ptr-(char*)FirstElement;
    if (isSingleArray) 
      if (Delta < NumNodesInSlab) return Delta/ElementSize;
    unsigned Index = Delta/ElementSize;
    if (Index < getSlabSize()) {
      if (Delta % ElementSize != 0) {
        fprintf(stderr, "Freeing pointer into the middle of an element!\n");
        fflush(stderr);
    abort();
      }
      
      return Index;
    }
  }
  return -1;
}


// freeElement - Free the single node, small array, or entire array indicated.
void
PoolSlab::freeElement(unsigned short ElementIdx) {
  if (!isNodeAllocated(ElementIdx)) return;
  //  assert(isNodeAllocated(ElementIdx) &&
  //         "poolfree: Attempt to free node that is already freed\n");
#if 0
  assert(!isSingleArray && "Cannot free an element from a single array!");
#else
#if 0
  if (isSingleArray) {
    this->addToList((PoolSlab**)&Pool->FreeLargeArrays);
    return;
  }
#endif
#endif

  // Mark this element as being free!
  markNodeFree(ElementIdx);

  // If this slab is not a SingleArray
  assert(isStartOfAllocation(ElementIdx) &&
         "poolfree: Attempt to free middle of allocated array\n");
  
  // Free the first cell
  clearStartBit(ElementIdx);
  markNodeFree(ElementIdx);
  
  // Free all nodes if this was a small array allocation.
  unsigned short ElementEndIdx = ElementIdx + 1;

  // FIXME: This should use manual strength reduction to produce decent code.
  unsigned short UE = UsedEnd;
  while (ElementEndIdx != UE &&
         !isStartOfAllocation(ElementEndIdx) && 
         isNodeAllocated(ElementEndIdx)) {
    markNodeFree(ElementEndIdx);
    ++ElementEndIdx;
  }
  
  // Update the first free field if this node is below the free node line
  if (ElementIdx < FirstUnused) FirstUnused = ElementIdx;

  // Update the first used field if this node was the first used.
  if (ElementIdx == UsedBegin) UsedBegin = ElementEndIdx;
  
  // If we are freeing the last element in a slab, shrink the UsedEnd marker
  // down to the last used node.
  if (ElementEndIdx == UE) {
#if 0
      printf("FU: %d, UB: %d, UE: %d  FREED: [%d-%d)",
           FirstUnused, UsedBegin, UsedEnd, ElementIdx, ElementEndIdx);
#endif

    // If the user is freeing the slab entirely in-order, it's quite possible
    // that all nodes are free in the slab.  If this is the case, simply reset
    // our pointers.
    if (UsedBegin == UE) {
      //printf(": SLAB EMPTY\n");
      FirstUnused = 0;
      UsedBegin = 0;
      UsedEnd = 0;
    } else if (FirstUnused == ElementIdx) {
      // Freed the last node(s) in this slab.
      FirstUnused = ElementIdx;
      UsedEnd = ElementIdx;
    } else {
      UsedEnd = lastNodeAllocated(ElementIdx);
      assert(FirstUnused <= UsedEnd+1 &&
             "FirstUnused field was out of date!");
    }
  }
  assertOkay();
}

unsigned
PoolSlab::lastNodeAllocated(unsigned ScanIdx) {
  // Check the last few nodes in the current word of flags...
  unsigned CurWord = ScanIdx/16;
  unsigned short Flags = NodeFlagsVector[CurWord] & 0xFFFF;
  if (Flags) {
    // Mask off nodes above this one
    Flags &= (1 << ((ScanIdx & 15)+1))-1;
    if (Flags) {
      // If there is still something in the flags vector, then there is a node
      // allocated in this part.  The goto is a hack to get the uncommonly
      // executed code away from the common code path.
      //printf("A: ");
      goto ContainsAllocatedNode;
    }
  }

  // Ok, the top word doesn't contain anything, scan the whole flag words now.
  --CurWord;
  while (CurWord != ~0U) {
    Flags = NodeFlagsVector[CurWord] & 0xFFFF;
    if (Flags) {
      // There must be a node allocated in this word!
      //printf("B: ");
      goto ContainsAllocatedNode;
    }
    CurWord--;
  }
  return 0;

ContainsAllocatedNode:
  // Figure out exactly which node is allocated in this word now.  The node
  // allocated is the one with the highest bit set in 'Flags'.
  //
  // This should use __builtin_clz to get the value, but this builtin is only
  // available with GCC 3.4 and above.  :(
  assert(Flags && "Should have allocated node!");
  
  unsigned short MSB;
#if GCC3_4_EVENTUALLY
  MSB = 16 - ::__builtin_clz(Flags);
#else
  for (MSB = 15; (Flags & (1U << MSB)) == 0; --MSB)
    /*empty*/;
#endif

  assert((1U << MSB) & Flags);   // The bit should be set
  assert((~(1U << MSB) & Flags) < Flags);// Removing it should make flag smaller
  ScanIdx = CurWord*16 + MSB;
  assert(isNodeAllocated(ScanIdx));
  return ScanIdx;
}


//===----------------------------------------------------------------------===//
//
//  Pool allocator library implementation
//
//===----------------------------------------------------------------------===//

void
pool_init_runtime (unsigned Dangling) {
  //
  // Configure the allocator.
  //
  extern ConfigData ConfigData;
  ConfigData.RemapObjects = Dangling;

  //
  // Install hooks for catching allocations outside the scope of SAFECode
  //
#if 0
  extern void installAllocHooks(void);
  installAllocHooks();
#endif
  return;
}

// poolinit - Initialize a pool descriptor to empty
//
void
poolinit(PoolTy *Pool, unsigned NodeSize) {
  assert(Pool && "Null pool pointer passed into poolinit!\n");
  DEBUG(printf("pool init %x, %d\n", Pool, NodeSize);)

  // Ensure the page manager is initialized
  InitializePageManager();

  // We must alway return unique pointers, even if they asked for 0 bytes
  Pool->NodeSize = NodeSize ? NodeSize : 1;
  // Initialize the splay tree
  Pool->Objects = Pool->OOB = Pool->DPTree = 0;
  Pool->DPTree = 0;
  Pool->Ptr1 = Pool->Ptr2 = 0;
  Pool->LargeArrays = 0;
  // For SAFECode, we set FreeablePool to 0 always
  //  Pool->FreeablePool = 0;
  Pool->AllocadPool = -1;
  Pool->allocaptr = 0;
  Pool->lastUsed = 0;
  Pool->prevPage[0] = 0;
  Pool->prevPage[1] = 0;
  // Initialize the SlabAddressArray to zero
  for (int i = 0; i < AddrArrSize; ++i) {
    Pool->SlabAddressArray[i] = 0;
  }
  Pool->NumSlabs = 0;
#if 0
  Pool->RegNodes = new std::map<void*,unsigned>;
#endif
  
  if (dummyInitialized == 1)
    return;
  
  dummyPool.NodeSize = NodeSize ? NodeSize : 1;
  // Initialize the splay tree
  dummyPool.Objects = dummyPool.OOB = dummyPool.DPTree = 0;
  dummyPool.Ptr1 = dummyPool.Ptr2 = 0;
  dummyPool.LargeArrays = 0;
  // For SAFECode, we set FreeablePool to 0 always
  //  Pool->FreeablePool = 0;
  dummyPool.AllocadPool = -1;
  dummyPool.allocaptr = 0;
  dummyPool.lastUsed = 0;
  dummyPool.prevPage[0] = 0;
  dummyPool.prevPage[1] = 0;
  // Initialize the SlabAddressArray to zero
  for (int i = 0; i < AddrArrSize; ++i) {
    dummyPool.SlabAddressArray[i] = 0;
  }
  dummyPool.NumSlabs = 0;
#if 0
  dummyPool.RegNodes = new std::map<void*,unsigned>;
#endif
  dummyInitialized = 1;
}

void
poolmakeunfreeable(PoolTy *Pool) {
  assert(Pool && "Null pool pointer passed in to poolmakeunfreeable!\n");
  //  Pool->FreeablePool = 0;
}

// pooldestroy - Release all memory allocated for a pool
//
// FIXME: determine how to adjust debug logs when 
//        pooldestroy is called
void
pooldestroy(PoolTy *Pool) {
  assert(Pool && "Null pool pointer passed in to pooldestroy!\n");
  adl_splay_delete_tag(&Pool->Objects, Pool);
  //adl_splay_delete_tag(&Pool->DPTree, Pool);
  if (Pool->AllocadPool) return;

  // Remove all registered pools
#if 0
  delete Pool->RegNodes;
#endif

  if (Pool->NumSlabs > AddrArrSize) {
    Pool->Slabs->clear();
    delete Pool->Slabs;
  }

  // Free any partially allocated slabs
  PoolSlab *PS = (PoolSlab*)Pool->Ptr1;
  while (PS) {
    PoolSlab *Next = PS->Next;
    PS->destroy();
    PS = Next;
  }

  // Free the completely allocated slabs
  PS = (PoolSlab*)Pool->Ptr2;
  while (PS) {
    PoolSlab *Next = PS->Next;
    PS->destroy();
    PS = Next;
  }

  // Free the large arrays
  PS = (PoolSlab*)Pool->LargeArrays;
  while (PS) {
    PoolSlab *Next = PS->Next;
    PS->destroy();
    PS = Next;
  }

}


// Function: poolallocarray()
//
// Description:
//  This is a helper function used to implement poolalloc() when the number of
//  nodes to allocate is not 1.
//
// Inputs:
//  Pool - A pointer to the pool from which to allocate.
//  Size - The number of nodes to allocate.
//
// FIXME: look into globalTemp, make it a pass by reference arg instead of
//          a global variable.
// FIXME: determine whether Size is bytes or number of nodes.
static void *
poolallocarray(PoolTy* Pool, unsigned Size) {
  assert(Pool && "Null pool pointer passed into poolallocarray!\n");
  
  // check to see if we need to allocate a single large array
  if (Size > PoolSlab::getSlabSize(Pool)) {
    if (logregs) {
      fprintf(stderr, " poolallocarray:694: Size = %d, SlabSize = %d\n", Size, PoolSlab::getSlabSize(Pool));
      fflush(stderr);
    }
    globalTemp = (PoolSlab*) PoolSlab::createSingleArray(Pool, Size);
    unsigned offset = (unsigned)globalTemp & (PPageSize - 1);
    void * retAddress = (void *)((unsigned)(globalTemp) & ~(PPageSize - 1));

    if (logregs) {
      fprintf(stderr, " poolallocarray:704: globalTemp = 0x%08x, offset = 0x%08x, retAddress = 0x%08x\n",
          (unsigned)globalTemp, offset, (unsigned)retAddress);
      fflush(stderr);
    }
    return (void*) ((unsigned)retAddress + offset);
  }
 
  PoolSlab *PS = (PoolSlab*)Pool->Ptr1;
  unsigned offset;

  // Loop through all of the slabs looking for one with an opening
  for (; PS; PS = PS->Next) {
    int Element = PS->allocateMultiple(Size);
    if (Element != -1) {
    // We allocated an element.  Check to see if this slab has been completely
    // filled up.  If so, move it to the Ptr2 list.
      if (PS->isFull()) {
        PS->unlinkFromList();
        PS->addToList((PoolSlab**)&Pool->Ptr2);
      }
      
      // insert info into adl splay tree for poolcheck runtime
      //unsigned NodeSize = Pool->NodeSize;
      globalTemp = PS->getElementAddress(Element, Pool->NodeSize);
      offset = (unsigned)globalTemp & (PPageSize - 1); 
      //adl_splay_insert(&(Pool->Objects), globalTemp, 
      //          (unsigned)((Size*NodeSize) - NodeSize + 1), (Pool));
      if(logregs) {fprintf(stderr, " poolallocarray:731:before RemapObject\n");}
      //  remap the page to get a shadow page (dangling pointer detection library)
      PS = (PoolSlab *) RemapObject(globalTemp, Size);
      if (logregs) {
        fprintf(stderr, " poolallocarray:735: globalTemp = 0x%x\n", (unsigned)globalTemp);
        fprintf(stderr ," poolallocarray:736: PS = 0x%08x, offset = 0x%08x, retAddress = 0x%08x\n",
              (unsigned)PS, offset, (unsigned)PS + offset);
      }
      return (void*) ((unsigned)PS + offset);
    }
  }
  
  PoolSlab *New = PoolSlab::create(Pool);
  //  printf("new slab created %x \n", New);
  if (Pool->NumSlabs > AddrArrSize)
    Pool->Slabs->insert((void *)New);
  else if (Pool->NumSlabs == AddrArrSize) {
    // Create the hash_set
    Pool->Slabs = new hash_set<void *>;
    Pool->Slabs->insert((void *)New);
    for (unsigned i = 0; i < AddrArrSize; ++i)
      Pool->Slabs->insert((void *)Pool->SlabAddressArray[i]);
  }
  else {
    // Insert it in the array
    Pool->SlabAddressArray[Pool->NumSlabs] = (unsigned) New;
  }
  
  Pool->NumSlabs++;
  
  int Idx = New->allocateMultiple(Size);
  assert(Idx == 0 && "New allocation didn't return zero'th node?");
  
  // insert info into adl splay tree for poolcheck runtime
  //unsigned NodeSize = Pool->NodeSize;
  globalTemp = New->getElementAddress(0, 0);
  //adl_splay_insert(&(Pool->Objects), globalTemp, 
  //          (unsigned)((Size*NodeSize) - NodeSize + 1), (Pool));
  
  // remap page to get a shadow page (dangling pointer detection library)
  New = (PoolSlab *) RemapObject(globalTemp, Size);
  offset = (unsigned)globalTemp & (PPageSize - 1);
  if (logregs) {
    fprintf(stderr, " poolallocarray:774: globalTemp = 0x%x\n, offset = 0x%x\n", (unsigned)globalTemp, offset);
    fprintf(stderr, " poolallocarray:775: New = 0x%08x, Size = %d, retAddress = 0x%08x\n",
        (unsigned)New, Size, (unsigned)New + offset);
  }
  return (void*) ((unsigned)New + offset);
}

void
poolregister(PoolTy *Pool, void * allocaptr, unsigned NumBytes) {
  // If the pool is NULL, don't do anything.
  if (!Pool) return;

#if 0
  if (Pool->AllocadPool != -1) {
    if (Pool->AllocadPool == 0) {
      printf(" Handle this case later\n");
      exit(-1);
    } else {
      printf(" An allocad pool, you can only allocate once \n");
      exit(-1);
    }
  } else {
    Pool->AllocadPool = NumBytes;
    Pool->allocaptr = allocaptr;
  }
#else
#if 0
  Pool->RegNodes->insert (std::make_pair(allocaptr,NumBytes));
#endif
  adl_splay_insert(&(Pool->Objects), allocaptr, NumBytes, 0);
  if (logregs) {
    fprintf (stderr, "poolregister: %x %d\n", (unsigned)allocaptr, NumBytes);
  }
#endif
}

void
poolunregister(PoolTy *Pool, void * allocaptr) {
  if (!Pool) return;
  adl_splay_delete(&Pool->Objects, allocaptr);
  if (logregs) {
    fprintf (stderr, "pooluregister: %x\n", allocaptr);
  }
}

//Pool->AllocadPool -1 : unused so far
//Pool->AllocadPool 0 : used only for mallocs
//Pool->AllocadPool >0 : used for only allocas indicating the size 
void *
poolalloc(PoolTy *Pool, unsigned NumBytes) {
  void *retAddress = NULL;
  if (!Pool) {
    fprintf(stderr, "Null pool pointer passed in to poolalloc!, FAILING\n");
    exit(-1);
  }

#if 0
  // Ensure that stack objects and heap objects d not belong to the same pool.
  if (Pool->AllocadPool != -1) {
    if (Pool->AllocadPool != 0) {
    printf(" Did not Handle this case, an alloa and malloc point to");
    printf("same DSNode, Will happen in stack safety \n");
    exit(-1);
    }
  }
  else {
    Pool->AllocadPool = 0;
  }
#endif
   
  // Ensure that we're always allocating at least 1 byte.
  if (NumBytes == 0)
    NumBytes = 1;

  unsigned NodeSize = Pool->NodeSize;
  unsigned NodesToAllocate = (NumBytes + NodeSize - 1)/NodeSize;
  unsigned offset = 0;
  PDebugMetaData debugmetadataPtr;
  
  // Call a helper function if we need to allocate more than 1 node.
  if (NodesToAllocate > 1) {
    if (logregs) {
      fprintf(stderr, " poolalloc:848: Allocating more than 1 node for %d bytes\n", NumBytes); fflush(stderr);
    }
    retAddress = poolallocarray(Pool, NodesToAllocate);

    //
    // Record information about this allocation in the global debugging
    // structure.
    globalallocID++;
    debugmetadataPtr = createPtrMetaData(globalallocID, globalfreeID, __builtin_return_address(0), 0, globalTemp);
    adl_splay_insert(&(dummyPool.DPTree), retAddress, NumBytes, (void *) debugmetadataPtr);
    if (logregs) {
      fprintf(stderr, " poolalloc:856: after inserting to dummyPool\n");
      fflush (stderr);
    }

    // Register the object in the splay tree.  Keep track of its debugging data
    // with the splay node tag so that we can quickly map shadow address back
    // to the canonical address.
    //
    // globalTemp is the canonical page address
    adl_splay_insert(&(Pool->Objects), retAddress, NumBytes, debugmetadataPtr);
    
    //if ((unsigned)retAddress > 0x2f000000 && logregs == 0)
    //  logregs = 1;
    
    if (logregs) {
      fprintf(stderr, " poolalloc:863: retAddress = 0x%08x NumBytes = %d globalTemp = 0x%08x\n",
          (unsigned)retAddress, NumBytes, (unsigned)globalTemp); fflush(stderr);
    }
    assert (retAddress && "poolalloc(1): Returning NULL!\n");
    return retAddress;
  }
  // Special case the most common situation, where a single node is being
  // allocated.
  PoolSlab *PS = (PoolSlab*)Pool->Ptr1;

  if (__builtin_expect(PS != 0, 1)) {
    int Element = PS->allocateSingle();
    if (__builtin_expect(Element != -1, 1)) {
      // We allocated an element.  Check to see if this slab has been
      // completely filled up.  If so, move it to the Ptr2 list.
      if (__builtin_expect(PS->isFull(), false)) {
        PS->unlinkFromList();
        PS->addToList((PoolSlab**)&Pool->Ptr2);
      }
      
      globalTemp = PS->getElementAddress(Element, NodeSize);
      offset = (unsigned)globalTemp & (PPageSize - 1);
      if (logregs) {
        fprintf(stderr, " poolalloc:885: canonical page = 0x%08x offset = 0x%08x\n", (unsigned)globalTemp, offset);
      }
      //adl_splay_insert(&(Pool->Objects), globalTemp, NumBytes, (Pool));
      
      // remap page to get a shadow page for dangling pointer library
      PS = (PoolSlab *) RemapObject(globalTemp, NumBytes);
      retAddress = (void*) ((unsigned)PS + offset);
      // for the use of dangling pointer runtime
      globalallocID++;
      debugmetadataPtr = createPtrMetaData(globalallocID, globalfreeID,
                          __builtin_return_address(0), 0, globalTemp);
      adl_splay_insert(&(dummyPool.DPTree), retAddress, NumBytes, debugmetadataPtr);
      
      adl_splay_insert(&(Pool->Objects), retAddress, NumBytes, debugmetadataPtr);
      if (logregs) {
        fprintf(stderr, " poolalloc:900: retAddress = 0x%08x, NumBytes = %d\n", (unsigned)retAddress, NumBytes);
      }
      assert (retAddress && "poolalloc(2): Returning NULL!\n");
      return retAddress;
    }

    // Loop through all of the slabs looking for one with an opening
    for (PS = PS->Next; PS; PS = PS->Next) {
      int Element = PS->allocateSingle();
      if (Element != -1) {
        // We allocated an element.  Check to see if this slab has been
        // completely filled up.  If so, move it to the Ptr2 list.
        if (PS->isFull()) {
          PS->unlinkFromList();
          PS->addToList((PoolSlab**)&Pool->Ptr2);
        }
        
        globalTemp = PS->getElementAddress(Element, NodeSize);
        offset = (unsigned)globalTemp & (PPageSize - 1);
        //adl_splay_insert(&(Pool->Objects), globalTemp, NumBytes, (Pool));
      
        // remap page to get a shadow page for dangling pointer library
        PS = (PoolSlab *) RemapObject(globalTemp, NumBytes);
        retAddress = (void*) ((unsigned)PS + offset);
        //printf(" returning the address %x",retAddress);
        // for the use of dangling pointer runtime
        globalallocID++;
        debugmetadataPtr = createPtrMetaData(globalallocID, globalfreeID,
                            __builtin_return_address(0), 0, globalTemp);
        adl_splay_insert(&(dummyPool.DPTree), retAddress, NumBytes, debugmetadataPtr);
        
        adl_splay_insert(&(Pool->Objects), retAddress, NumBytes, debugmetadataPtr);
        if (logregs) {
          fprintf (stderr, " poolalloc:932: PS = 0x%08x, retAddress = 0x%08x, NumBytes = %d, offset = 0x%08x\n",
                (unsigned)PS, (unsigned)retAddress, NumBytes, offset);
        }
        assert (retAddress && "poolalloc(3): Returning NULL!\n");
        return retAddress;
      }
    }
  }

  // Otherwise we must allocate a new slab and add it to the list
  PoolSlab *New = PoolSlab::create(Pool);
  //
  // Ensure that we're always allocating at least 1 byte.
  //
  if (NumBytes == 0)
    NumBytes = 1;
  
  if (Pool->NumSlabs > AddrArrSize)
    Pool->Slabs->insert((void *)New);
  else if (Pool->NumSlabs == AddrArrSize) {
    // Create the hash_set
    Pool->Slabs = new hash_set<void *>;
    Pool->Slabs->insert((void *)New);
    for (unsigned i = 0; i < AddrArrSize; ++i)
      Pool->Slabs->insert((void *)Pool->SlabAddressArray[i]);
  }
  else {
    // Insert it in the array
    Pool->SlabAddressArray[Pool->NumSlabs] = (unsigned) New;
  }
  Pool->NumSlabs++;


  int Idx = New->allocateSingle();
  assert(Idx == 0 && "New allocation didn't return zero'th node?");
  if (logregs) {
    fprintf(stderr, " poolalloc:967: canonical page at 0x%08x from underlying allocator\n", (unsigned)New);
  }
  globalTemp = New->getElementAddress(0, 0);
  offset = (unsigned)globalTemp & (PPageSize - 1);
  
  if (logregs) {
    fprintf(stderr, " poolalloc:973: element at 0x%08x, offset=0x%08x\n", (unsigned)globalTemp, offset);
  }
  //adl_splay_insert(&(Pool->Objects), globalTemp, NumBytes, (Pool));
  
  // remap  page to get a shadow page for dangling pointer library
  New = (PoolSlab *) RemapObject(globalTemp, NumBytes);
  offset = (unsigned)globalTemp & (PPageSize - 1);
  //printf(" shadow page at 0x%x through remapping\n", (unsigned)New);
  retAddress = (void*) ((unsigned)New + offset);
  //printf(" returning the address 0x%x\n", (unsigned)retAddress);
  // for the use of dangling pointer runtime
  globalallocID++;
  debugmetadataPtr = createPtrMetaData(globalallocID, globalfreeID, __builtin_return_address(0), 0, globalTemp);
  adl_splay_insert(&(dummyPool.DPTree), retAddress, NumBytes, (void *) debugmetadataPtr);
  
  adl_splay_insert(&(Pool->Objects), retAddress, NumBytes, debugmetadataPtr);
  if (logregs) {
    fprintf (stderr, " poolalloc:990: New = 0x%08x, retAddress = 0x%08x, NumBytes = %d, offset = 0x%08x\n",
          (unsigned)New, (unsigned)retAddress, NumBytes, offset);
  }
  assert (retAddress && "poolalloc(4): Returning NULL!\n");
  return retAddress;
}

void *
poolrealloc(PoolTy *Pool, void *Node, unsigned NumBytes) {
  if (Node == 0) return poolalloc(Pool, NumBytes);
  if (NumBytes == 0) {
    poolfree(Pool, Node);
    return 0;
  }

  void *New = poolalloc(Pool, NumBytes);
  //    unsigned Size =
  //FIXME the following may not work in all cases  
  memcpy(New, Node, NumBytes);
  poolfree(Pool, Node);
  return New;
}

void *
poolcalloc (PoolTy *Pool, unsigned Number, unsigned NumBytes) {
  void * New = poolalloc (Pool, Number * NumBytes);
  if (New) bzero (New, Number * NumBytes);
  return New;
}

void *
poolstrdup(PoolTy *Pool, char *Node) {
  if (Node == 0) return 0;

  unsigned int NumBytes = strlen(Node) + 1;
  void *New = poolalloc(Pool, NumBytes);
  if (New) {
    memcpy(New, Node, NumBytes+1);
  }
  return New;
}

// SearchForContainingSlab - Do a brute force search through the list of
// allocated slabs for the node in question.
//
static PoolSlab *
SearchForContainingSlab(PoolTy *Pool, void *Node, unsigned &TheIndex) {
  PoolSlab *PS = (PoolSlab*)Pool->Ptr1;
  unsigned NodeSize = Pool->NodeSize;

  // Search the partially allocated slab list for the slab that contains this
  // node.
  int Idx = -1;
  if (PS) {               // Pool->Ptr1 could be null if Ptr2 isn't
    for (; PS; PS = PS->Next) {
      Idx = PS->containsElement(Node, NodeSize);
      if (Idx != -1) break;
    }
  }

  // If the partially allocated slab list doesn't contain it, maybe the
  // completely allocated list does.
  if (PS == 0) {
    PS = (PoolSlab*)Pool->Ptr2;
    assert(Idx == -1 && "Found node but don't have PS?");
    
    while (PS) {
      assert(PS && "poolfree: node being free'd not found in allocation "
             " pool specified!\n");
      Idx = PS->containsElement(Node, NodeSize);
      if (Idx != -1) break;
      PS = PS->Next;
    }
  }
  
  // Otherwise, maybe its a block within LargeArrays
  if(PS == 0) {
    PS = (PoolSlab*)Pool->LargeArrays;
    assert(Idx == -1  && "Found node but don't have PS?");
    
    while (PS) {
      assert(PS && "poolfree: node being free'd not found in allocation "
             " pool specified!\n");
      Idx = PS->containsElement(Node, NodeSize);
      if (Idx != -1) break;
      PS = PS->Next;
    }
  }

  TheIndex = Idx;
  return PS;
}

void
poolcheck(PoolTy *Pool, void *Node) {
  void* S = Node;
  unsigned len = 0;
  if (!Pool) return;
  int fs = adl_splay_retrieve(&(Pool->Objects), &S, &len, 0);
  if ((fs) && (S <= Node) && (((char*)S + len) > (char*)Node)) {
    return;
  }

  /*
   * The node is not found or is not within bounds; fail!
   */
  fprintf (stderr, "Poolcheck failed(%x:%x): %x %x from %x\n", 
      (unsigned)Pool, fs, (unsigned)Node, len, (unsigned)__builtin_return_address(0));
  fflush (stderr);
  abort ();
  return;
}

void
poolcheckui (PoolTy *Pool, void *Node) {
  // Splay tree of external objects
  extern void * ExternalObjects;

  void* S = Node;
  unsigned len = 0;
  if (!Pool) return;

  /*
   * Look for the object within the pool's splay tree.
   */
  int fs = adl_splay_retrieve(&(Pool->Objects), &S, &len, 0);
  if ((fs) && (S <= Node) && (((char*)S + len) > (char*)Node)) {
    return;
  }

  /*
   * Look for the object within the splay tree of external objects.
   */
  S = Node;
  len = 0;
  fs = adl_splay_retrieve (&(ExternalObjects), &S, &len, 0);
  if ((fs) && (S <= Node) && (((char*)S + len) > (char*)Node)) {
    return;
  }

  /*
   * The node is not found or is not within bounds.  Report a warning but keep
   * going.
   */
  fprintf (stderr, "PoolcheckUI failed(%x:%x): %x %x from %x\n", 
      (unsigned)Pool, fs, (unsigned)Node, len, (unsigned)__builtin_return_address(0));
  fflush (stderr);
  return;
}

/*
 * Function: boundscheck()
 *
 * Description:
 *  Perform a precise bounds check.  Ensure that Source is within a valid
 *  object within the pool and that Dest is within the bounds of the same
 *  object.
 */
static unsigned char * invalidptr = 0;

void *
boundscheck (PoolTy * Pool, void * Source, void * Dest) {
  void* S = Source;
  unsigned len = 0;
  int fs = adl_splay_retrieve(&(Pool->Objects), &S, &len, 0);
  if ((fs) && (S <= Dest) && (((char*)S + len) > (char*)Dest)) {
    return Dest;
  }

  /*
   * Handle the case where a pointer is allowed to move just beyond the end of
   * the allocated space.
   */
  if ((fs) && (((char *) Dest) == ((char*)S + len))) {
    if (logregs)
      fprintf (stderr, "boundscheck  : rewrite: %x %x %x %x, pc=%x\n",
               S, Source, Dest, len, (void*)__builtin_return_address(0));
    if (invalidptr == 0) invalidptr = (unsigned char*)InvalidLower;
    ++invalidptr;
    void* P = invalidptr;
    if ((unsigned)P & ~(InvalidUpper - 1)) {
      fprintf (stderr, "boundscheck: out of rewrite ptrs: %x %x, pc=%x\n",
               Source, Dest, (void*)__builtin_return_address(0));
      fflush (stderr);
#if 0
      abort();
#else
      return Dest;
#endif
    }

    adl_splay_insert (&(Pool->OOB), P, 1, Dest);
    return invalidptr;
  }

  /*
   * The node is not found or is not within bounds; fail!
   */
  if (fs) {
    fprintf (stderr, "Boundscheck failed(%x:%x): Out of object: %x %x %x from %x esp=%x\n",
             (unsigned)Pool, fs, (unsigned)Source, (unsigned)Dest, len,
       (unsigned)__builtin_return_address(0), (unsigned)__builtin_frame_address(0));

  } else {
    fprintf (stderr, "Boundscheck failed(%x:%x): No object: %x %x %x from %x esp=%x\n",
             (unsigned)Pool, fs, (unsigned)Source, (unsigned)Dest, len,
       (unsigned)__builtin_return_address(0), (unsigned)__builtin_frame_address(0));
  }
  fflush (stderr);
  abort ();
  return Dest;
}

void *
boundscheckui (PoolTy * Pool, void * Source, void * Dest) {
  // Splay tree of external objects
  extern void * ExternalObjects;

  void* S = Source;
  unsigned len = 0;
  int fs = adl_splay_retrieve(&(Pool->Objects), &S, &len, 0);
  if (fs) {
    if ((S <= Dest) && (((char*)S + len) > (char*)Dest)) {
      return Dest;
    } else if (((char *) Dest) == ((char*)S + len)) {
      if (logregs)
        fprintf (stderr, "boundscheckui: rewrite: %x %x %x %x, pc=%x\n",
                 S, Source, Dest, len, (void*)__builtin_return_address(0));
      if (invalidptr == 0) invalidptr = (unsigned char*)InvalidLower;
      ++invalidptr;
      void* P = invalidptr;
      if ((unsigned)P & ~(InvalidUpper - 1)) {
        fprintf (stderr, "boundscheckui: out of rewrite ptrs: %x %x, pc=%x\n",
                 Source, Dest, (void*)__builtin_return_address(0));
        fflush (stderr);
#if 0
        abort();
#else
        return Dest;
#endif
      }
      adl_splay_insert (&(Pool->OOB), P, 1, Dest);
      return invalidptr;
    }
  }

  if (!Source) {
    fprintf (stderr, "Boundscheck failed(%x:%x): Out of object: %x %x %x from %x esp=%x\n",
             (unsigned)Pool, fs, (unsigned)Source, (unsigned)Dest, len,
       (unsigned)__builtin_return_address(0), (unsigned)__builtin_frame_address(0));
    abort();
  }

  /*
   * Attempt to look for the object in the external object splay tree.
   */
  S = Source;
  len = 0;
  fs = adl_splay_retrieve (&(ExternalObjects), &S, &len, 0);
  if (fs) {
    if ((S <= Dest) && (((char*)S + len) > (char*)Dest)) {
      return Dest;
    } else {
      fprintf (stderr, "Boundscheckui failed(%x:%x): Out of object: %x %x %x from %x esp=%x\n", (unsigned)Pool, fs, (unsigned)Source, (unsigned)Dest, len,
     (unsigned)__builtin_return_address(0), (unsigned)__builtin_frame_address(0));
      fflush (stderr);
    }
  }

  /*
   * We cannot find the object.  Print a warning and continue execution.
   */
#if 0
  fprintf (stderr, "Boundscheck failed(%x:%x): Out of object: %x %x %x from %x esp=%x\n",
           (unsigned)Pool, fs, (unsigned)Source, (unsigned)Dest, len,
     (unsigned)__builtin_return_address(0), (unsigned)__builtin_frame_address(0));
  fflush (stderr);
#endif
  return Dest;
}

/*
 * Function: getActualValue()
 *
 * Description:
 *  If src is an out of object pointer, get the original value.
 */
void *
pchk_getActualValue (PoolTy * Pool, void * src) {
  if ((unsigned)src <= InvalidLower) return src;

  void* tag = 0;

  /* outside rewrite zone */
  if ((unsigned)src & ~(InvalidUpper - 1)) return src;
  if (adl_splay_retrieve(&(Pool->OOB), &src, 0, &tag)) {
    return tag;
  }

  /* Lookup has failed */
  fprintf (stderr, "GetActualValue failure: src = %x, pc = %x", (unsigned) src,
           (unsigned) __builtin_return_address(0));
  fflush (stderr);
  abort ();
  return tag;
}

// Check that Node falls within the pool and within start and (including)
// end offset
void
poolcheckalign (PoolTy *Pool, void *Node, unsigned StartOffset, 
                unsigned EndOffset) {
  PoolSlab *PS;
  if (StartOffset >= Pool->NodeSize || EndOffset >= Pool->NodeSize) {
    printf("Error: Offset specified exceeded node size");
    exit(-1);
  }

  if (Pool->AllocadPool > 0) {
    if (Pool->allocaptr <= Node) {
     unsigned diffPtr = (unsigned)Node - (unsigned)Pool->allocaptr;
     unsigned offset = diffPtr % Pool->NodeSize;
     if ((diffPtr  < (unsigned)Pool->AllocadPool ) && (offset >= StartOffset) &&
         (offset <= EndOffset))
       return;
    }
    assert(0 && "poolcheckalign failure FAILING \n");
    exit(-1);    
  }
  
  PS = (PoolSlab*)((long)Node & ~(PageSize-1));

  if (Pool->NumSlabs > AddrArrSize) {
    hash_set<void*> &theSlabs = *Pool->Slabs;
    if (theSlabs.find((void*)PS) == theSlabs.end()) {
      // Check the LargeArrays
      if (Pool->LargeArrays) {
        PoolSlab *PSlab = (PoolSlab*) Pool->LargeArrays;
        int Idx = -1;
        while (PSlab) {
          assert(PSlab && "poolcheck: node being free'd not found in "
                          "allocation pool specified!\n");
          Idx = PSlab->containsElement(Node, Pool->NodeSize);
          if (Idx != -1) {
            Pool->prevPage[Pool->lastUsed] = PS;
            Pool->lastUsed = (Pool->lastUsed + 1) % 4;
            break;
          }
          PSlab = PSlab->Next;
        }

        if (Idx == -1) {
          fprintf(stderr, "poolcheck1: node being checked not found in pool with right"
           " alignment\n");
          fflush(stderr);
      abort();
          exit(-1);
        } else {
          //exit(-1);
        }
      } else {
        fprintf(stderr, "poolcheck2: node being checked not found in pool with right"
               " alignment\n");
        fflush(stderr);
    abort();
        exit(-1);
      }
    } else {
      unsigned long startaddr = (unsigned long)PS->getElementAddress(0,0);
      if (startaddr > (unsigned long) Node) {
        fprintf(stderr, "poolcheck: node being checked points to meta-data \n");
        fflush(stderr);
    abort();
        exit(-1);
      }
      unsigned long offset = ((unsigned long) Node - (unsigned long) startaddr) % Pool->NodeSize;
      if ((offset < StartOffset) || (offset > EndOffset)) {
        fprintf(stderr, "poolcheck3: node being checked does not have right alignment\n");
        fflush(stderr);
    abort();
        exit(-1);
      }
      Pool->prevPage[Pool->lastUsed] = PS;
      Pool->lastUsed = (Pool->lastUsed + 1) % 4;
    }
  } else {
    bool found = false;
    for (unsigned i = 0; i < AddrArrSize && !found; ++i) {
      if ((unsigned)Pool->SlabAddressArray[i] == (unsigned) PS) {
        found = true;
        Pool->prevPage[Pool->lastUsed] = PS;
        Pool->lastUsed = (Pool->lastUsed + 1) % 4;
      }
    } 

    if (found) {
      // Check that Node does not point to PoolSlab meta-data
      unsigned long startaddr = (unsigned long)PS->getElementAddress(0,0);
      if (startaddr > (unsigned long) Node) {
        printf("poolcheck: node being checked points to meta-data \n");
        exit(-1);
      }
      unsigned long offset = ((unsigned long) Node - (unsigned long) startaddr) % Pool->NodeSize;
      if ((offset < StartOffset) || (offset > EndOffset)) {
        fprintf(stderr, "poolcheck4: node being checked does not have right alignment\n");
        fflush(stderr);
    abort();
        exit(-1);
      }
    } else {
      // Check the LargeArrays
      if (Pool->LargeArrays) {
        PoolSlab *PSlab = (PoolSlab*) Pool->LargeArrays;
        int Idx = -1;
        while (PSlab) {
          assert(PSlab && "poolcheck: node being free'd not found in "
           "allocation pool specified!\n");
          Idx = PSlab->containsElement(Node, Pool->NodeSize);
          if (Idx != -1) {
            Pool->prevPage[Pool->lastUsed] = PS;
            Pool->lastUsed = (Pool->lastUsed + 1) % 4;
            break;
          }
          PSlab = PSlab->Next;
        }
        if (Idx == -1) {
          fprintf(stderr, "poolcheck6: node being checked not found in pool with right"
           " alignment\n");
          fflush(stderr);
      abort();
          exit(-1);
        }
      } else {
        fprintf(stderr, "poolcheck5: node being checked not found in pool with right"
               " alignment %x %x\n", (unsigned)Pool, (unsigned)Node);
        fflush(stderr);
    abort();
      }
    }
  }
}



void
poolfree(PoolTy *Pool, void *Node) {
  assert(Pool && "Null pool pointer passed in to poolfree!\n");
  DEBUG(printf("poolfree  %x %x \n",Pool,Node);)
  PoolSlab *PS;
  int Idx;
  
  
  if (logregs) {
    printf(" poolfree:1368: poolfree to addr 0x%08x\n", (unsigned)Node);
  }
  // update DebugMetaData
  globalfreeID++;

  // FIXME: figure what mykey, NumPPAge and len are for
  void * mykey;
  unsigned len = 1;
  unsigned NumPPage = 0;
  unsigned offset = (unsigned)((long)Node & (PPageSize - 1));
  PDebugMetaData debugmetadataptr;
  mykey = Node;
  
  // Retrieve the debug information about the node.  This will include a
  // pointer to the canonical page.
  adl_splay_retrieve (&(Pool->Objects), &mykey, &len, (void **) &debugmetadataptr);
  
  if (logregs) {
    printf(" poolfree:1387: mykey = 0x%08x offset = 0x%08x\n", (unsigned)mykey, offset);
    printf(" poolfree:1388: len = %d\n", len);
  }

  // figure out how many pages does this object span to
  //  protect the pages. First we sum the offset and len
  //  to get the total size we originally remapped.
  //  Then, we determine if this sum is a multiple of
  //  physical page size. If it is not, then we increment
  //  the number of pages to protect.
  //  FIXME!!!

  NumPPage = (len / PPageSize) + 1;
  if ( (len - (NumPPage-1) * PPageSize) > (PPageSize - offset) )
    NumPPage++;
  
  globalTemp = debugmetadataptr->canonAddr;
  
  if (logregs) {
    printf(" poolfree:1397: NumPPage = %d\n", NumPPage);
    printf(" poolfree:1398: canonical address is 0x%x\n", (unsigned)globalTemp);
  }
  updatePtrMetaData(debugmetadataptr, globalfreeID, __builtin_return_address(0));

  // then we protect the shadow pages
  ProtectShadowPage((void *)((long)Node & ~(PPageSize - 1)), NumPPage);
  
  // then we allow the poolcheck runtime to finish the bookkeping it needs to do.
  adl_splay_delete(&Pool->Objects, Node);
  
  if (1) {                  // THIS SHOULD BE SET FOR SAFECODE!
    unsigned TheIndex;
    PS = SearchForContainingSlab(Pool, globalTemp, TheIndex);
    Idx = TheIndex;
  }
  else {
    // Since it is undefined behavior to free a node which has not been
    // allocated, we know that the pointer coming in has to be a valid node
    // pointer in the pool.  Mask off some bits of the address to find the base
    // of the pool.
    assert((PageSize & PageSize-1) == 0 && "Page size is not a power of 2??");
    PS = (PoolSlab*)((long)Node & ~(PageSize-1));

    if (PS->isSingleArray) {
      PS->unlinkFromList();
      
      // works done to update the DebugMetaData struct for
      //  dangling pointer runtime
      globalfreeID++;
      void * mykey;
      unsigned len = 0;
      PDebugMetaData debugmetadataptr;
      mykey = Node;
      adl_splay_retrieve(&(Pool->DPTree), &mykey, &len, (void **) &debugmetadataptr);
      updatePtrMetaData(debugmetadataptr, globalfreeID, __builtin_return_address(0));      
      return;
    }

    Idx = PS->containsElement(Node, Pool->NodeSize);
    assert((int)Idx != -1 && "Node not contained in slab??");
  }
  
  //  return if PS is NULL
  if (!PS)
    return;
  assert (PS && "PS is NULL!\n");
  
  // If PS was full, it must have been in list #2.  Unlink it and move it to
  // list #1.
  if (PS->isFull()) {
    // Now that we found the node, we are about to free an element from it.
    // This will make the slab no longer completely full, so we must move it to
    // the other list!
    PS->unlinkFromList(); // Remove it from the Ptr2 list.

    PoolSlab **InsertPosPtr = (PoolSlab**)&Pool->Ptr1;

    // If the partially full list has an empty node sitting at the front of the
    // list, insert right after it.
    if ((*InsertPosPtr)->isEmpty())
      InsertPosPtr = &(*InsertPosPtr)->Next;

    PS->addToList(InsertPosPtr);     // Insert it now in the Ptr1 list.
  }

  // Ok, if this slab is empty, we unlink it from the of slabs and either move
  // it to the head of the list, or free it, depending on whether or not there
  // is already an empty slab at the head of the list.
  if (PS->isEmpty()) {
    PS->unlinkFromList();   // Unlink from the list of slabs...
    
    // If we can free this pool, check to see if there are any empty slabs at
    // the start of this list.  If so, delete the FirstSlab!
    PoolSlab *FirstSlab = (PoolSlab*)Pool->Ptr1;
    if (0 && FirstSlab && FirstSlab->isEmpty()) {
      // Here we choose to delete FirstSlab instead of the pool we just freed
      // from because the pool we just freed from is more likely to be in the
      // processor cache.
    FirstSlab->unlinkFromList();
    FirstSlab->destroy();
    //  Pool->Slabs.erase((void *)FirstSlab);
    }
 
    // Link our slab onto the head of the list so that allocations will find it
    // efficiently.    
    PS->addToList((PoolSlab**)&Pool->Ptr1);
  }
 
  //
  // An object has been freed.  Set up a signal handler to catch any dangling
  // pointer references.
  //
  // FIXME:
  //  This code was placed here because it does not appear to work when placed
  //  in poolinit().
  //
  struct sigaction sa;
  sa.sa_sigaction = bus_error_handler;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGBUS, &sa, NULL) == -1) {
    fprintf (stderr, "sigaction installer failed!");
    fflush (stderr);
  }
  return; 
}

//===----------------------------------------------------------------------===//
//
// Dangling pointer runtime functions
//
//===----------------------------------------------------------------------===//

//
// Function: createPtrMetaData()
//  Allocates memory for a DebugMetaData struct and fills up the appropriate
//  fields so to keep a record of the pointer's meta data
//
extern "C" { void * internal_malloc (unsigned int size);}
static PDebugMetaData
createPtrMetaData (unsigned paramAllocID,
                   unsigned paramFreeID,
                   void * paramAllocPC,
                   void * paramFreePC,
                   void * paramCanon) {
  // FIXME:
  //  This only works because DebugMetaData and a splay tree node are of
  //  identical size.
  PDebugMetaData ret = (PDebugMetaData) internal_malloc(sizeof(DebugMetaData));
  ret->allocID = paramAllocID;
  ret->freeID = paramFreeID;
  ret->allocPC = paramAllocPC;
  ret->freePC = paramFreePC;
  ret->canonAddr = paramCanon;

  return ret;
}

static inline void
updatePtrMetaData (PDebugMetaData debugmetadataptr,
                   unsigned globalfreeID,
                   void * paramFreePC) {
  debugmetadataptr->freeID = globalfreeID;
  debugmetadataptr->freePC = paramFreePC;
  return;
}

//
// Function: bus_error_handler()
//
// Description:
//  This is the signal handler that catches bad memory references.
//
static void
bus_error_handler (int sig, siginfo_t * info, void * context) {
  signal(SIGBUS, NULL);
  alertNum++;
  ucontext_t * mycontext = (ucontext_t *) context;

  unsigned len = 0;
  void * faultAddr = info->si_addr;
  PDebugMetaData debugmetadataptr;
  int fs = 0;
  if (0 == (fs = adl_splay_retrieve(&(dummyPool.DPTree), &faultAddr, &len, (void **) &debugmetadataptr)))
  {
    fprintf(stderr, "signal handler: retrieving debug meta data failed");
    fflush(stderr);
    abort();
  }
 
  // FIXME: Correct the semantics for calculating NumPPage 
  unsigned NumPPage;
  unsigned offset = (unsigned) ((long)info->si_addr & (PPageSize - 1) );
  NumPPage = (len / PPageSize) + 1;
  if ( (len - (NumPPage-1) * PPageSize) > (PPageSize - offset) )
    NumPPage++;
 
  // This is necessary so that the program continues execution,
  //  especially in debugging mode 
  UnprotectShadowPage((void *)((long)info->si_addr & ~(PPageSize - 1)), NumPPage);
  
  //void* S = info->si_addr;
  // printing reports
  printf("=======+++++++    SAFECODE RUNTIME ALERT #%04d   +++++++=======\n", alertNum);
  printf("%04d: Invalid access to memory address 0x%08x \n", alertNum, (unsigned)faultAddr);
#if defined(__APPLE__)
#if defined(i386) || defined(__i386__) || defined(__x86__)
  printf("%04d:               at program counter 0x%08x \n", alertNum, (unsigned)mycontext->uc_mcontext->__ss.__eip);
#endif
  printf("%04d:     Object allocated at program counter \t: 0x%08x \n", alertNum, (unsigned)debugmetadataptr->allocPC - 5);
  printf("%04d:     Object allocation generation number \t: %d \n", alertNum, debugmetadataptr->allocID);
  printf("%04d:     Object freed at program counter \t: 0x%08x \n", alertNum, (unsigned)debugmetadataptr->freePC - 5);
  printf("%04d:     Object free generation number \t: %d \n", alertNum, debugmetadataptr->freeID);
#endif
  printf("=======+++++++    end of runtime error report    +++++++=======\n");
  
  // reinstall the signal handler for subsequent faults
  struct sigaction sa;
  sa.sa_sigaction = bus_error_handler;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGBUS, &sa, NULL) == -1)
    printf("sigaction installer failed!");
  
  return;
}

 

//
// Function: funccheck()
//
// Description:
//  Determine whether the specified function pointer is one of the functions
//  in the given list.
//
// Inputs:
//  num - The number of function targets in the DSNode.
//  f   - The function pointer that we are testing.
//  g   - The first function given in the DSNode.
//
void
funccheck (unsigned num, void *f, void *g, ...) {
  va_list ap;
  unsigned i = 0;

  // Test against the first function in the list
  if (f == g) return;
  i++;
  va_start(ap, g);
  for ( ; i != num; ++i) {
    void *h = va_arg(ap, void *);
    if (f == h) {
      return;
    }
  }
  if (logregs) {
  fprintf(stderr, "funccheck failed(num=%d): %x %x\n", num, f, g);
  fflush(stderr);
  }
  abort();
}

void
poolstats() {
  fprintf (stderr, "pool mem usage %d\n", poolmemusage);
  fflush (stderr);
}
