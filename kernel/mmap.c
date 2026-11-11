#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "stat.h"

static struct vma*
vmafind(struct proc *p, uint64 addr)
{
  for(int i = 0; i < NVMA; i++){
    struct vma *v = &p->vmas[i];
    if(v->file && addr >= v->addr && addr - v->addr < PGROUNDUP(v->length))
      return v;
  }
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 addr, length, offset, end = TRAPFRAME;
  int prot, flags, fd;
  struct proc *p = myproc();
  struct vma *v = 0;
  struct file *f;

  if(argaddr(0, &addr) < 0 || argaddr(1, &length) < 0 ||
     argint(2, &prot) < 0 || argint(3, &flags) < 0 ||
     argint(4, &fd) < 0 || argaddr(5, &offset) < 0)
    return -1;
  if(addr != 0 || offset != 0 || length == 0 || length > TRAPFRAME ||
     prot == 0 || (prot & ~(PROT_READ | PROT_WRITE)) ||
     (flags != MAP_SHARED && flags != MAP_PRIVATE) ||
     fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0 ||
     f->type != FD_INODE || !f->readable ||
     (flags == MAP_SHARED && (prot & PROT_WRITE) && !f->writable))
    return -1;

  ilock(f->ip);
  int regular = f->ip->type == T_FILE;
  iunlock(f->ip);
  if(!regular)
    return -1;

  for(int i = 0; i < NVMA; i++)
    if(p->vmas[i].file == 0){
      v = &p->vmas[i];
      break;
    }
  if(v == 0)
    return -1;

  // Search downward from the trapframe, reusing gaps between mappings.
  uint64 size = PGROUNDUP(length);
  for(int i = 0; i < NVMA; i++){
    struct vma *other = &p->vmas[i];
    if(size > end || end - size < PGROUNDUP(p->sz))
      return -1;
    if(other->file && other->addr < end &&
       end - size < other->addr + PGROUNDUP(other->length)){
      end = other->addr;
      i = -1;
    }
  }

  v->addr = end - size;
  v->length = length;
  v->offset = 0;
  v->prot = prot;
  v->flags = flags;
  v->file = filedup(f);
  return v->addr;
}

// Install a file page only when it is first accessed by user code.
int
vmafault(struct proc *p, uint64 addr, int write)
{
  struct vma *v = vmafind(p, addr);
  if(v == 0 || (write && !(v->prot & PROT_WRITE)))
    return -1;

  addr = PGROUNDDOWN(addr);
  pte_t *pte = walk(p->pagetable, addr, 0);
  if(pte && (*pte & PTE_V))
    return -1;
  char *mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);

  uint64 off = v->offset + addr - v->addr;
  struct inode *ip = v->file->ip;
  ilock(ip);
  int n = 0;
  if(off < ip->size)
    n = readi(ip, 0, (uint64)mem, off, PGSIZE);
  iunlock(ip);

  // RISC-V requires readable PTEs for writable pages.
  int perm = PTE_U | PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_W;
  if(n < 0 || mappages(p->pagetable, addr, PGSIZE, (uint64)mem, perm) < 0){
    kfree(mem);
    return -1;
  }
  return 0;
}

// Keep writeback within both the mapping and the file's existing size.
static int
vmawrite(struct vma *v, uint64 addr, uint64 pa)
{
  struct inode *ip = v->file->ip;
  uint64 off = v->offset + addr - v->addr;
  uint length = PGSIZE;
  if(v->length - (addr - v->addr) < length)
    length = v->length - (addr - v->addr);

  // A block per transaction keeps writes within the log reservation.
  for(uint i = 0; i < length; ){
    uint n = length - i;
    if(n > BSIZE)
      n = BSIZE;
    begin_op(ip->dev);
    ilock(ip);
    if(off + i >= ip->size)
      n = 0;
    else if(n > ip->size - (off + i))
      n = ip->size - (off + i);
    int r = n ? writei(ip, 0, pa + i, off + i, n) : 0;
    iunlock(ip);
    end_op(ip->dev);
    if(r != n)
      return -1;
    if(n == 0)
      break;
    i += n;
  }
  return 0;
}

int
vmaunmap(struct proc *p, uint64 addr, uint64 length)
{
  if(addr % PGSIZE || length == 0 || length > TRAPFRAME || addr >= TRAPFRAME)
    return -1;
  uint64 size = PGROUNDUP(length);
  struct vma *v = vmafind(p, addr);
  if(v == 0 || size > v->addr + PGROUNDUP(v->length) - addr ||
     (addr != v->addr && addr + size != v->addr + PGROUNDUP(v->length)))
    return -1;

  int result = 0;
  for(uint64 a = addr; a < addr + size; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;
    if(v->flags == MAP_SHARED && (*pte & PTE_D) &&
       vmawrite(v, a, PTE2PA(*pte)) < 0)
      result = -1;
    uvmunmap(p->pagetable, a, PGSIZE, 1);
  }

  if(addr == v->addr){
    if(size == PGROUNDUP(v->length)){
      fileclose(v->file);
      memset(v, 0, sizeof(*v));
    } else {
      v->addr += size;
      v->offset += size;
      v->length -= size;
    }
  } else {
    v->length = addr - v->addr;
  }
  return result;
}

uint64
sys_munmap(void)
{
  uint64 addr, length;
  if(argaddr(0, &addr) < 0 || argaddr(1, &length) < 0)
    return -1;
  return vmaunmap(myproc(), addr, length);
}

void
vmafree(struct proc *p)
{
  for(int i = 0; i < NVMA; i++)
    if(p->vmas[i].file)
      vmaunmap(p, p->vmas[i].addr, p->vmas[i].length);
}
