// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// Reference count of each physical page, kept in a table (a hash
// table indexed by the physical page number) to support
// copy-on-write fork:
//   0 : page is free (on the free list)
//   1 : page has a single mapping
//   >1: page is shared by several page tables
// kalloc() hands out a page with count 1, krefinc() adds one
// reference (uvmcopy during fork), and kfree() releases one
// reference; the page is returned to the free list only when its
// last reference (count 1) is released.
#define NPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
#define PAGEIDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int ref[NPAGES];
} pageref;

static void
pagerefinit(void)
{
  initlock(&pageref.lock, "pageref");
  for(int i = 0; i < NPAGES; i++)
    pageref.ref[i] = 0;
}

// Add one reference to the physical page pa.
void
krefinc(void *pa)
{
  acquire(&pageref.lock);
  pageref.ref[PAGEIDX(pa)]++;
  release(&pageref.lock);
}

static void
kpush(void *pa)
{
  struct run *r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  pagerefinit();
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kpush(p); // pages start out with reference count 0
}

// Release one reference to the physical page pointed at by pa.
// If this was the page's last reference, put the page back on the
// free list; otherwise just decrement the reference count, since
// other page tables still map the page.
void
kfree(void *pa)
{
  int idx;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  idx = PAGEIDX(pa);
  acquire(&pageref.lock);
  if(pageref.ref[idx] <= 0)
    panic("kfree: not referenced");
  if(pageref.ref[idx] == 1){
    pageref.ref[idx] = 0; // last reference, page becomes free
    release(&pageref.lock);

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
    kpush(pa);
  } else {
    pageref.ref[idx]--;   // still shared, keep the page
    release(&pageref.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    acquire(&pageref.lock);
    pageref.ref[PAGEIDX(r)] = 1;
    release(&pageref.lock);
  }
  return (void*)r;
}
