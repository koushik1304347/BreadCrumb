// Private block storage only: ordinary filesystem paths are NOT redirected.
#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "buf.h"

enum shadowstate { SH_LOADING, SH_READY };

// One kalloc page per entry. Unlinked/free entries are FREE implicitly.
// Linked allocation avoids a fixed pointer array and keeps wait channels stable.
struct shadow {
  struct shadow *next;
  uint dev;
  uint blockno;
  enum shadowstate state;
  uchar data[BSIZE];
};
typedef char shadow_fits_page[(sizeof(struct shadow) <= PGSIZE) ? 1 : -1];

#ifdef SHADOW_TEST
static volatile uint fail_alloc;
static volatile uint hold_loader;
static volatile uint waiting;
static volatile uint pages;
#endif

static struct shadow*
find(struct spec_context *sc, uint dev, uint blockno)
{
  struct shadow *s;
  for(s = sc->resources; s; s = s->next)
    if(s->dev == dev && s->blockno == blockno)
      return s;
  return 0;
}

static char*
entry_alloc(void)
{
  char *p;
#ifdef SHADOW_TEST
  if(__sync_bool_compare_and_swap(&fail_alloc, 1, 0))
    return 0;
#endif
  p = kalloc();
  if(p){
    memset(p, 0, PGSIZE);
#ifdef SHADOW_TEST
    __sync_fetch_and_add(&pages, 1);
#endif
  }
  return p;
}

static void
entry_free(struct shadow *s)
{
#ifdef SHADOW_TEST
  __sync_fetch_and_sub(&pages, 1);
#endif
  kfree((char*)s);
}

int
shadow_count(struct spec_context *sc)
{
  int n = 0;
  struct shadow *s;
  acquire(&sc->resource_lock);
  for(s = sc->resources; s; s = s->next)
    n++;
  release(&sc->resource_lock);
  return n;
}

// Only completion's exclusive reference or the final reclaimer may call this.
// No other user may hold an entry reference; free outside both spinlocks.
void
shadow_discard(struct spec_context *sc)
{
  struct shadow *s, *next;
  acquire(&sc->resource_lock);
  s = sc->resources;
  sc->resources = 0;
  release(&sc->resource_lock);
  for(; s; s = next){
    next = s->next;
    entry_free(s);
  }
}

// Return 1 if copied/updated, 0 for a read with no private version, -1 on
// invalid input, capacity/allocation failure, or cancellation. Never public-write.
static int
access(int id, uint dev, uint blockno, uint off, void *data, uint n, int write)
{
  struct spec_context *sc, *active;
  struct shadow *s, *candidate = 0;
  struct buf *b;
  int result = -1, count;

  if(dev != ROOTDEV || blockno >= FSSIZE || off > BSIZE || n > BSIZE-off ||
     data == 0 || n == 0)
    return -1;
  if((sc = spec_pin(id)) == 0)
    return -1;
  acquire(&sc->resource_lock);
  for(;;){
    if(myproc()->killed)
      goto out;
    s = find(sc, dev, blockno);
    if(s && s->state == SH_LOADING){
#ifdef SHADOW_TEST
      __sync_fetch_and_add(&waiting, 1);
#endif
      // Cancellation check and sleep admission are atomic under ptable.lock.
      result = spec_wait(sc, sc, &sc->resource_lock);
#ifdef SHADOW_TEST
      __sync_fetch_and_sub(&waiting, 1);
#endif
      if(result < 0){
        result = -1;
        goto out;
      }
      continue;  // entry may have disappeared after failed initialization
    }
    if(s){
      if(write)
        memmove(s->data+off, data, n);
      else
        memmove(data, s->data+off, n);
      result = 1;
      goto out;
    }
    if(!write){
      result = 0;
      goto out;
    }
    count = 0;
    for(s = sc->resources; s; s = s->next)
      count++;
    if(count >= NSHADOW){
      result = -1;
      goto out;
    }
    if(candidate == 0){
      release(&sc->resource_lock);
      candidate = (struct shadow*)entry_alloc();
      acquire(&sc->resource_lock);
      if(candidate == 0){
        result = -1;
        goto out;
      }
      // Recheck after allocation: another member may have reserved this block.
      continue;
    }
    s = candidate;
    candidate = 0;
    s->dev = dev;
    s->blockno = blockno;
    s->state = SH_LOADING;
    s->next = sc->resources;
    sc->resources = s;
    release(&sc->resource_lock);

#ifdef SHADOW_TEST
    while(__sync_val_compare_and_swap(&hold_loader, 0, 0) && !myproc()->killed)
      yield();
#endif
    // This remains the original cache path in Step 3. Never call while holding
    // resource_lock or a source buffer lock. Step 4 must preserve a raw path.
    b = bread(dev, blockno);
    memmove(s->data, b->data, BSIZE);
    brelse(b);
    // A pin keeps memory alive but is not permission to admit work after abort.
    active = spec_pin(id);
    acquire(&sc->resource_lock);
    if(active == 0 || myproc()->killed){
      struct shadow **link;
      for(link = (struct shadow**)&sc->resources; *link != s; link = &(*link)->next)
        ;
      *link = s->next;
      release(&sc->resource_lock);
      entry_free(s);
      wakeup(sc);
      if(active)
        spec_unpin(active);
      spec_unpin(sc);
      return -1;
    }
    memmove(s->data+off, data, n);
    s->state = SH_READY;
    release(&sc->resource_lock);
    wakeup(sc);
    spec_unpin(active);
    spec_unpin(sc);
    return 1;
  }
out:
  release(&sc->resource_lock);
  if(candidate)
    entry_free(candidate);
  spec_unpin(sc);
  return result;
}

int
shadow_read(int id, uint dev, uint blockno, uint off, void *dst, uint n)
{
  return access(id, dev, blockno, off, dst, n, 0);
}

int
shadow_write(int id, uint dev, uint blockno, uint off, const void *src, uint n)
{
  return access(id, dev, blockno, off, (void*)src, n, 1);
}

#ifdef SHADOW_TEST
#include "shadowtest.h"
// Test-only bounded interface: no arbitrary block numbers or user pointers.
int
shadow_test(int op, int slot, int value)
{
  struct spec_context *sc;
  struct buf *b;
  uchar byte;
  int r;
  uint block;
  if(op == ST_PAGES)
    return __sync_val_compare_and_swap(&pages, 0, 0);
  if(op == ST_WAITERS)
    return __sync_val_compare_and_swap(&waiting, 0, 0);
  if(op == ST_FAIL){
    __sync_lock_test_and_set(&fail_alloc, 1);
    return 0;
  }
  if(op == ST_HOLD){
    __sync_lock_test_and_set(&hold_loader, value != 0);
    return 0;
  }
  if(slot < 0 || slot > NSHADOW || value < 0 || value > 255)
    return -1;
  block = FSSIZE-NSHADOW-4+slot;
  if(op == ST_ORIGINAL){
    b = bread(ROOTDEV, block);
    r = b->data[0];
    brelse(b);
    return r;
  }
  if(op == ST_COUNT){
    sc = spec_pin(myproc()->spec);
    if(sc == 0)
      return -1;
    r = shadow_count(sc);
    spec_unpin(sc);
    return r;
  }
  byte = value;
  if(op == ST_WRITE)
    return shadow_write(myproc()->spec, ROOTDEV, block, 0, &byte, 1);
  if(op == ST_READ){
    r = shadow_read(myproc()->spec, ROOTDEV, block, 0, &byte, 1);
    return r == 1 ? byte : (r == 0 ? ST_ABSENT : -1);
  }
  if(op == ST_COPY){
    // No-op modification at byte 511 verifies first-copy preserves byte zero.
    b = bread(ROOTDEV, block);
    byte = b->data[BSIZE-1];
    brelse(b);
    return shadow_write(myproc()->spec, ROOTDEV, block, BSIZE-1, &byte, 1);
  }
  return -1;
}
#endif
