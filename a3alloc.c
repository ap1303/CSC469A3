#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include "mm_thread.h"
#include "memlib.h"

#define HEAP_MAX_SIZE 20
#define PAGE_SIZE 4 * 1024

#define STATE_ACTIVE 0
#define STATE_FULL 1
#define STATE_PARTIAL 2
#define STATE_EMPTY 3

#define SUPERPAGE_NUM 4 // every superpage contains 4 base pages

//name_t myName = {
	/* Allocator Name */
//	"Lockless Memalloc",
	/* Team Member */
//	"Peizhi Zhang",
//	"peizhi.zhang@mail.utoronto.com"
//};


typedef struct {
	int avail; // the index of the first available page in the superpage
	int count; // the number of unreserved pages in the superpage,
	int state; // the state of the superpage
	int tag; // a placeholder variable for preventing the ABA problem
} anchor;

// Superpage descriptor structure
typedef struct descriptor {
	anchor Anchor;
	struct descriptor *Next; // unknown type name
	void *sb; // pointer to superpage
	int sz; // page size
	int maxcount; // superpage size/sz
        int procheap_num; // the index of the processor heap that it belongs to 
        int index; 
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
        int index; // the index of the heap in heap_array
} procheap;

descriptor *DescAvail;
procheap *heap_array;

int min(int a, int b) { return a > b ? b : a; }

void *AllocNewSBFromMemory(int sbsize) {
     ptrdiff_t increment = sbsize;
     return mem_sbrk(increment);
}

descriptor *AllocNewSB(int sbsize) { 
    descriptor *desc = (descriptor *) mem_sbrk(sizeof(descriptor));
    desc->sb = AllocNewSBFromMemory(sbsize);
    return desc;
}

descriptor *DescAlloc() {
    _Atomic(descriptor *) placeholder;
    atomic_init(&placeholder, DescAvail);
    descriptor *desc  = DescAvail;
    while (1) {
       if (desc) {
           struct descriptor *next = desc->Next;  
           if (atomic_compare_exchange_strong(&placeholder,&desc,next)) break; 
       } else {
           desc = AllocNewSB(SUPERPAGE_NUM * PAGE_SIZE);
	   DescAvail = desc;
           atomic_thread_fence(memory_order_seq_cst);
           descriptor *temp = NULL;
           struct descriptor *next = desc->Next;
           if (atomic_compare_exchange_weak(&placeholder,&temp,next)) break;
       }
    }
    return desc;
}

void DescRetire(descriptor *desc) {
    _Atomic(descriptor *) placeholder;
    atomic_init(&placeholder, DescAvail);
    descriptor *oldhead = desc;
    do {
       oldhead = DescAvail;
       desc->Next = oldhead;
       atomic_thread_fence(memory_order_seq_cst);
    } while(!atomic_compare_exchange_weak(&placeholder,&oldhead,desc)); // invalid initializer
}

void ListPutPartial(descriptor *partial, procheap *heap, int sz) {
     descriptor *temp;
     if (sz) {
         temp = heap->sc->Partial;
     } else {
	 temp = heap->Partial;
     }
     while((temp != NULL) && (temp->Next != NULL)) {
	temp = temp->Next;
     }
     temp->Next = partial;
}

descriptor *ListGetPartial(sizeclass *sc) {
    descriptor *temp = sc->Partial;
    sc->Partial = sc->Partial->Next;
    return temp;
}

void ListRemoveEmptyDesc(sizeclass *sc) {
     descriptor *temp = sc->Partial;
     while (1) {
	 if (temp->sb == NULL) {
	     sc->Partial = sc->Partial->Next;
	     temp = sc->Partial;
	     continue;
	 } else {
	     temp = temp->Next;
	     continue;
	 }
	 if (temp->Next == NULL) {
	     break;
	 }
      }
}

void HeapPutPartial(descriptor *desc) {
     descriptor *prev;
     procheap heap = heap_array[desc->procheap_num];
     _Atomic(descriptor *) placeholder; 
     atomic_init(&placeholder, heap.Partial);
     do {
	  prev = heap.Partial;
     } while (!atomic_compare_exchange_weak(&placeholder,&prev,desc)); 
     if (prev) ListPutPartial(prev, heap_array + desc->procheap_num, 1);
}

descriptor *HeapGetPartial(procheap *heap) {
    descriptor *desc;
    descriptor *Partial = heap->Partial;
    _Atomic(descriptor *) placeholder;
    atomic_init(&placeholder, Partial); 
    do {
        desc = heap->Partial;
        if (desc == NULL)
          return ListGetPartial(heap->sc);
    } while (!atomic_compare_exchange_weak(&placeholder,&desc,NULL)); // invalid initializer
    return desc;
}

void RemoveEmptyDesc(procheap *heap,descriptor *desc) {
  descriptor *partial = heap->Partial;
  _Atomic(descriptor *) placeholder;
  atomic_init(&placeholder, partial);
  descriptor *temp = NULL;
  if (atomic_compare_exchange_weak(&placeholder,&desc,temp)) { // invalid initializer
      DescRetire(desc);
  } else {
      ListRemoveEmptyDesc(heap->sc);
  }    
}

void UpdateActive(procheap *heap, descriptor *desc, int morecredits) {
     active curActive = heap_array[desc->procheap_num].Active;
     curActive.credits = morecredits - 1;
     active *temp = NULL;
     _Atomic(active) placeholder;
     atomic_init(&placeholder, heap->Active); 
     if (atomic_compare_exchange_weak(&placeholder,temp,curActive)) { // size mismatch
	exit(0);
     }
     anchor prevAnchor;
     anchor curAnchor;
     _Atomic(anchor) placeholder1;
     atomic_init(&placeholder1, desc->Anchor);
     do {
	prevAnchor = desc->Anchor;
	curAnchor = prevAnchor;
	curAnchor.count += morecredits;
	curAnchor.state = STATE_PARTIAL;
     } while(!atomic_compare_exchange_weak(&placeholder1, &prevAnchor, curAnchor));
     HeapPutPartial(desc);
}

void* MallocFromActive(procheap *heap) {
      active *prevActive = &(heap->Active);
      active *curActive = prevActive;
      _Atomic(active) placeholder;
      atomic_init(&placeholder, heap->Active); 
      do {
	    *prevActive = heap->Active;
	    curActive = prevActive;
            active* temp = NULL;
	    if (prevActive == temp) return NULL; 
	    if (prevActive->credits == 0)
	       curActive = NULL; 
	    else
	       curActive->credits--;
      } while(!atomic_compare_exchange_weak(&placeholder, prevActive, *curActive));// invalid initializer

      descriptor *desc = prevActive->desc;
      int morecredits = 0;
      anchor prevAnchor = desc->Anchor; 
      anchor curAnchor = prevAnchor;
      _Atomic(anchor) placeholder1 = ATOMIC_VAR_INIT(desc->Anchor); 
      do {
            prevAnchor = desc->Anchor;
	    curAnchor = prevAnchor;
            curAnchor.avail = prevAnchor.avail + 1;
            curAnchor.tag++;
            if (prevActive->credits == 0) {
                if (prevAnchor.count == 0) {
                    curAnchor.state = STATE_FULL;
                } else {
                    morecredits = min(prevAnchor.count,SUPERPAGE_NUM);
                    curAnchor.count -= morecredits;
                }
            }
     } while (!atomic_compare_exchange_weak(&placeholder1,&prevAnchor,curAnchor));

     if (prevActive->credits==0 && prevAnchor.count>0)
	 UpdateActive(heap, desc, morecredits);

      void *addr = desc->sb;
      return addr;

}

void *MallocFromPartial(procheap *heap) {
            descriptor *desc = heap->Partial;
            anchor prevAnchor = desc->Anchor;
	    anchor curAnchor = prevAnchor;
            int morecredits = 0;
            _Atomic(anchor) placeholder = ATOMIC_VAR_INIT(desc->Anchor);
	    do {
		  desc = HeapGetPartial(heap);
	          if (!desc) return NULL;
	          desc->procheap_num = heap->index;
	          // reserve blocks
		  prevAnchor = desc->Anchor;
		  curAnchor = prevAnchor;
		  if (prevAnchor.state == STATE_EMPTY) {
		      DescRetire(desc);
		      continue;
		  }
                  int morecredits = min(prevAnchor.count-1,SUPERPAGE_NUM);
                  curAnchor.count -= morecredits+1;
                  curAnchor.state = (morecredits > 0) ? STATE_ACTIVE : STATE_FULL;
	      } while (!atomic_compare_exchange_weak(&placeholder,&prevAnchor, curAnchor));

              do { // pop reserved block
                   prevAnchor = desc->Anchor;
                   curAnchor = prevAnchor;
                   curAnchor.avail = prevAnchor.avail;
                   curAnchor.tag++;
              } while (!atomic_compare_exchange_weak(&placeholder,&prevAnchor,curAnchor));

              if (morecredits > 0)
                  UpdateActive(heap,desc,morecredits);

              void *addr = desc->sb;
	      return addr;
}

void *MallocFromNewSB(procheap *heap) {
      descriptor* desc = DescAlloc();
      desc->sb = AllocNewSB(heap->sc->sbsize);
      desc->procheap_num = heap->index;
      desc->Anchor.avail = 1;
      desc->sz = heap->sc->sz;
      desc->maxcount = heap->sc->sbsize / desc->sz;
      active curActive = heap->Active;
      curActive.credits = min(desc->maxcount-1, SUPERPAGE_NUM) - 1;
      desc->Anchor.count = (desc->maxcount-1) - (curActive.credits+1);
      desc->Anchor.state = STATE_ACTIVE;

      if (heap->Partial == NULL) {
	  heap->Partial = desc;
      } else {
          ListPutPartial(desc, heap, 0);
      }

      atomic_thread_fence(memory_order_seq_cst);
      active *temp = NULL;
      _Atomic(active) placeholder;
      atomic_init(&placeholder, heap->Active);
      if (atomic_compare_exchange_weak(&placeholder,temp,curActive)){ // size mismatch
         void *addr = desc->sb;
         return addr;
      } else {
         DescRetire(desc);
	 return NULL;
      } // control reaches end of non-void function
}

void mm_init() {
     mem_init();

     // initialize processor
     sizeclass *sc = mem_sbrk((ptrdiff_t) sizeof(sizeclass));
     sc->sz = PAGE_SIZE;
     sc->sbsize = SUPERPAGE_NUM * PAGE_SIZE;

     int num = getNumProcessors();
     for(int i = 0; i < num; i++) {
         heap_array[i].sc = sc;
         heap_array[i].index = i;
     }

     // as a simplification, pin execution to one processor
     // setCPU(0);

}

void *mm_malloc() {
     int cpu = sched_getcpu();
     procheap *heap = heap_array + cpu;
     while (1) {
        void *addr = MallocFromActive(heap);
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
     anchor prevAnchor = desc->Anchor;
     anchor curAnchor = prevAnchor;
     procheap *heap;
     _Atomic(anchor) placeholder = ATOMIC_VAR_INIT(desc->Anchor);
     do {
	prevAnchor = desc->Anchor;
        curAnchor = prevAnchor;
        curAnchor.avail = prevAnchor.avail;
        if (prevAnchor.state == STATE_FULL)
            curAnchor.state = STATE_PARTIAL;
        if (prevAnchor.count == desc->maxcount - 1) {
            heap = heap_array + desc->procheap_num;
            curAnchor.state = STATE_EMPTY;
        } else {
	    curAnchor.count++;
	    atomic_thread_fence(memory_order_seq_cst);
	}
      } while(!atomic_compare_exchange_weak(&placeholder, &prevAnchor,     curAnchor)); // invalid initializer

      if (prevAnchor.state == STATE_EMPTY) {
          RemoveEmptyDesc(heap,desc);
      } else if (prevAnchor.state == STATE_FULL) {
          HeapPutPartial(desc);
      }
}

int main() {
    mm_init();
    void *addr = mm_malloc();
    printf("malloc-ed address is %p\n", addr);
    mm_free(addr);  
}
