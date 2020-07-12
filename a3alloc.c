#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <sched.h>
#include <stdlib.h>
#include "memlib.h"
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include "mm_thread.h"

#define HEAP_MAX_SIZE 20
#define PAGE_SIZE 4 * 1024

#define STATE_ACTIVE 0
#define STATE_FULL 1
#define STATE_PARTIAL 2
#define STATE_EMPTY 3

#define SUPERPAGE_NUM 4 // every superpage contains 4 base pages

name_t myName = {
	/* Allocator Name */
	"Lockless Memalloc",
	/* Team Member */
	"Peizhi Zhang",
	"peizhi.zhang@mail.utoronto.com"
};


typedef struct {
	int avail; // the index of the first available page in the superpage
	int count; // the number of unreserved pages in the superpage,
	int state; // the state of the superpage
	int tag; // a placeholder variable for preventing the ABA problem
} anchor;

// Superpage descriptor structure
typedef struct descriptor {
	anchor Anchor;
	descriptor *Next;
	void *sb; // pointer to superpage
	procheap *heap; // pointer to owner procheap
	int sz; // page size
	int maxcount; // superpage size/sz
} descriptor;

// primarily a pointer to the descriptor of the superpage
typedef struct active {
	descriptor *desc; // descriptor of the active superpage owned by the processor heap
	int credits; // the number of pages available for reservation in the active superpage minus one
} active;

typedef struct sizeclass {
	descriptor *Partial; // initially empty
	int sz; // page size
	int sbsize; // superpage size
} sizeclass;

typedef struct procheap {
	active Active; // initially NULL
	descriptor *Partial; // initially NULL
	sizeclass *sc; // pointer to parent sizeclass
} procheap;

descriptor* DescAvail; // initially NULL
procheap *heap_array;

void *AllocNewSBFromMemory(int sbsize) {
	   ptrdiff_f increment = sbsize
	   return mem_sbrk(increment);
}

descriptor *AllocNewSB(int sbsize) {
	  descriptor *desc;
		ptrdiff_f increment = sbsize
		desc->sb = AllocNewSBFromMemory(increment);
    return desc
}

descriptor* DescAlloc() {
    while (1) {
			descriptor *desc = DescAvail;
      if (desc) {
          next = desc->Next;
          if atomic_compare_exchange_strong(&DescAvail,desc,next) break;
      } else {
          desc = AllocNewSB(SUPERPAGE_NUM * PAGE_SIZE);
					DescAvail->next = desc;
          atomic_thread_fence(memory_order_seq_cst);
          if atomic_compare_exchange_weak(&DescAvail,NULL,desc->Next)) break;
      }
   }
   return desc;
}

void DescRetire(descrptor *desc) {
    do {
       oldhead = DescAvail;
       desc->Next = oldhead;
       atomic_thread_fence(memory_order_seq_cst);
    } while(!atomic_compare_exchange_weak(&DescAvail,oldhead,desc));
}

void ListPutPartial(descriptor *partial, procheap *heap, int sz) {
	   descriptor *temp;
     if (sz) {
			 temp = heap->sz->Partial;
		 } else {
			 temp = heap->Partial;
		 }
		 while(temp != NULL & temp->next != NULL) {
				 temp = temp->next;
		 }
		 temp->next = partial;
}

descriptor *ListGetPartial(sizeclass *sc) {
	   descriptor *temp = sc->Partial;
		 sc->partial = sc->Partial->next;
		 return temp;
}

void ListRemoveEmptyDesc(sizeclass *sc) {
	   descriptor *temp = sc->Partial;
	   while (1) {
			 if (temp->sb == NULL) {
				   sc->Partial = sc->Partial->next;
					 temp = sc->Partial;
					 continue;
			 } else {
				   temp = temp->next;
					 continue;
			 }
			 if (temp->next == NULL) {
				   break;
			 }
	   }
}

void HeapPutPartial(descriptor *desc) {
     do {
	       prev = desc->heap->Partial;
     } while (!atomic_compare_exchange_weak(&desc->heap->Partial,prev,desc))
     if (prev) ListPutPartial(prev, desc->heap, 1);
}

descriptor *HeapGetPartial(procheap *heap) {
    do {
        desc = heap->Partial;
        if (desc == NULL)
          return ListGetPartial(heap->sc);
    } while (!atomic_compare_exchange_weak(&heap->Partial,desc,NULL))
    return desc;
}

void RemoveEmptyDesc(procheap *heap,descriptor *desc) {
  if atomic_compare_exchange_weak(&heap->Partial,desc,NULL) {
	   DescRetire(desc);
  } else {
		 ListRemoveEmptyDesc(heap->sc);
	}
}

void UpdateActive(procheap *heap, descriptor *desc, int morecredits) {
	curActive = desc;
  curActive.credits = morecredits - 1;
  if atomic_compare_exchange_weak(&heap->Active,NULL,curActive)
	    return;
  do {
		anchor prevAnchor = desc->Anchor;
		anchor curAnchor = prevanchor;
		curAnchor.count += morecredits;
		curAnchor.state = STATE_PARTIAL;
	} while(!atomic_compare_exchange_weak(&desc->Anchor,prevAnchor,curAnchor))
  HeapPutPartial();
}

void* MallocFromActive(procheap *heap) {
   do {
		active prevActive = heap->Active;
	  active curActice = prevActive;
		if (!prevActive) return NULL;
		if (prevActive.credits == 0)
				curActive = NULL;
		else
				curActive.credits--;
	} while(!atomic_compare_exchange_weak(&heap->Active, prevActive, curActive))

  descriptor *desc = prevActive->desc;

  do {
		 anchor prevAnchor = desc->Anchor;
	   anchor curAnchor = prevAnchor;
     void *addr = desc->sb + prevAnchor.avail * desc->sz;
     void next = *(unsigned*)addr;
     curAnchor.avail = next;
     curAnchor.tag++;
     if (prevActive.credits == 0) {
         if (prevAnchor.count == 0)
             curAnchor.state = STATE_FULL;
          else {
             morecredits = min(prevAnchor.count,SUPERPAGE_NUM);
             curAnchor.count -= morecredits;
         }
		  }
	 } while (!atomic_compare_exchange_weak(&desc->Anchor,prevAnchor,curAnchor))

   if (prevActive.credits==0 && prevAnchor.count>0)
	    UpdateActive(heap, desc, morecredits);

	 *addr = desc;
	 return addr;

}

void *MallocFromPartial(procheap *heap) {
	    do {
						descriptor *desc = HeapGetPartial(heap);
	          if (!desc) return NULL;
	          desc->heap = heap;
	          // reserve blocks
						anchor prevAnchor = desc->Anchor;
						anchor curAnchor = prevAnchor;
		        if (prevAnchor.state == STATE_EMPTY) {
		            DescRetire(desc);
								continue;
		         }

						int morecredits = min(prevAnchor.count-1,SUPERPAGE_NUM);
            curAnchor.count -= morecredits+1;
            curAnchor.state = (morecredits > 0) ? STATE_ACTIVE : STATE_FULL;
			} while (!atomic_compare_exchange_weak(&desc->Anchor,prevAnchor,curAnchor))

      do { // pop reserved block
				  anchor prevAnchor = desc->Anchor;
          anchor curAnchor = prevAnchor;
          void *addr = desc->sb + prevAnchor.avail * desc->sz;
          curAnchor.avail = * (unsigned*)addr;
          curAnchor.tag++;
      } while (!atomic_compare_exchange_weak(&desc->Anchor,prevAnchor,curAnchor))

      if (morecredits > 0)
          UpdateActive(heap,desc,morecredits);

      *addr = desc;
			return addr;
}

void *MallocFromNewSB(procheap *heap) {
      descrptor* desc = DescAlloc();
      desc->sb = AllocNewSB(heap->sc->sbsize);
      desc->heap = heap;
      desc->Anchor.avail = 1;
      desc->sz = heap->sc->sz;
      desc->maxcount = heap->sc->sbsize / desc->sz;
      active curActive = desc;
      curActive.credits = min(desc->maxcount-1, SUPERPAGE_NUM) - 1;
      desc->Anchor.count = (desc->maxcount-1) - (curActive.credits+1);
      desc->Anchor.state = STATE_ACTIVE;

			if (heap->Partial == NULL) {
				  heap->Partial = desc;
			} else {
				  ListPutPartial(desc, heap, 0);
			}
      atomic_thread_fence(memory_order_seq_cst);
      if atomic_compare_exchange_weak((&heap->Active,NULL,curActive) {
         void *addr = desc->sb;
         *addr = desc;
         return addr;
      } else {
         DescRetire(desc);
				 return NULL;
      }
}

void mm_init() {
	   mem_init();

		 // initialize processor
		 sizeclass *sc;
		 sc->sz = PAGE_SIZE;
		 sc->sbsize = SUPERPAGE_NUM * PAGE_SIZE;

     int num = getNumProcessors();
		 for(int i = 0; i < num; i++) {
         heap_array->sc = sc;
		 }

		 // as a simplification, pin execution to one processor
		 // setCPU(0);
}

void mm_malloc() {
     int cpu = sched_getcpu();
		 procheap *heap = heap_array + cpu;
		 while(1) {
			 addr = MallocFromActive(heap);
			 if (addr) return addr;
			 addr = MallocFromPartial(heap);
			 if (addr) return addr;
			 addr = MallocFromNewSB(heap);
			 if (addr) return addr;
		 }

}

void mm_free(void *ptr) {
	   if (!ptr) return;
     descriptor *desc = *(descriptor**)ptr;
     void *sb = desc->sb;
     do {
			  anchor prevAnchor = desc->Anchor
        anchor curAnchor = prevAnchor;
        *(unsigned*)ptr = prevAnchor.avail;
        curAnchor.avail = (ptr->sb) / desc->sz;
        if (prevAnchor.state == STATE_FULL)
            curAnchor.state = STATE_PARTIAL;
        if (prevAnchor.count == desc->maxcount - 1) {
            procheap *heap = desc->heap;
            curAnchor.state = STATE_EMPTY;
        } else {
					  curAnchor.count++;
					  atomic_thread_fence(memory_order_seq_cst);
				}
      } while(!CAS(&desc->Anchor,prevAnchor,prevAnchor));

      if (prevAnchor.state == STATE_EMPTY) {
          RemoveEmptyDesc(heap,desc);
      } else if (prevAnchor.state == STATE_FULL) {
          HeapPutPartial(desc);
      }
}
