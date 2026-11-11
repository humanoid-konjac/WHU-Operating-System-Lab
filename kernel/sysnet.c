//
// network system calls.
//

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
#include "net.h"

struct sock {
  struct sock *next; // the next socket in the list
  uint32 raddr;      // the remote IPv4 address
  uint16 lport;      // the local UDP port number
  uint16 rport;      // the remote UDP port number
  struct mbufq rxq;  // a queue of packets waiting to be received
};

static struct spinlock lock; // protects the socket list and all receive queues
static struct sock *sockets;

void
sockinit(void)
{
  initlock(&lock, "socktbl");
}

int
sockalloc(struct file **f, uint32 raddr, uint16 lport, uint16 rport)
{
  struct sock *si, *pos;

  si = 0;
  *f = 0;
  if ((*f = filealloc()) == 0)
    goto bad;
  if ((si = (struct sock*)kalloc()) == 0)
    goto bad;

  // initialize objects
  si->raddr = raddr;
  si->lport = lport;
  si->rport = rport;
  mbufq_init(&si->rxq);

  // add to list of sockets
  acquire(&lock);
  pos = sockets;
  while (pos) {
    if (pos->raddr == raddr &&
        pos->lport == lport &&
	pos->rport == rport) {
      release(&lock);
      goto bad;
    }
    pos = pos->next;
  }
  (*f)->type = FD_SOCK;
  (*f)->readable = 1;
  (*f)->writable = 1;
  (*f)->sock = si;
  si->next = sockets;
  sockets = si;
  release(&lock);
  return 0;

bad:
  if (si)
    kfree((char*)si);
  if (*f)
    fileclose(*f);
  *f = 0;
  return -1;
}

void
sockclose(struct sock *si)
{
  struct sock **pos;
  struct mbuf *m;

  acquire(&lock);
  for (pos = &sockets; *pos != si; pos = &(*pos)->next)
    ;
  *pos = si->next;
  release(&lock);

  // The last file reference is gone, and receivers can no longer find si.
  while ((m = mbufq_pophead(&si->rxq)) != 0)
    mbuffree(m);
  kfree(si);
}

int
sockread(struct sock *si, uint64 addr, int n)
{
  struct proc *p = myproc();
  struct mbuf *m;

  if (n <= 0)
    return n == 0 ? 0 : -1;
  acquire(&lock);
  while (mbufq_empty(&si->rxq)) {
    if (p->killed) {
      release(&lock);
      return -1;
    }
    sleep(&si->rxq, &lock);
  }
  m = mbufq_pophead(&si->rxq);
  release(&lock);

  if ((uint)n > m->len)
    n = m->len;
  if (copyout(p->pagetable, addr, m->head, n) < 0)
    n = -1;
  mbuffree(m);
  return n;
}

int
sockwrite(struct sock *si, uint64 addr, int n)
{
  struct mbuf *m;

  if (n < 0 || n > MBUF_SIZE - MBUF_DEFAULT_HEADROOM)
    return -1;
  m = mbufalloc(MBUF_DEFAULT_HEADROOM);
  if (m == 0)
    return -1;
  if (copyin(myproc()->pagetable, mbufput(m, n), addr, n) < 0) {
    mbuffree(m);
    return -1;
  }
  net_tx_udp(m, si->raddr, si->lport, si->rport);
  return n;
}

// called by protocol handler layer to deliver UDP packets
void
sockrecvudp(struct mbuf *m, uint32 raddr, uint16 lport, uint16 rport)
{
  acquire(&lock);
  for (struct sock *si = sockets; si; si = si->next) {
    if (si->raddr == raddr && si->lport == lport && si->rport == rport) {
      mbufq_pushtail(&si->rxq, m);
      wakeup(&si->rxq);
      release(&lock);
      return;
    }
  }
  release(&lock);
  mbuffree(m);
}
