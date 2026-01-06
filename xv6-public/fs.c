// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);
static struct inode *iget(uint, uint);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// bmap Read/Write Flags
#define RD_FLAG 0
#define WR_FLAG 1

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// update snapshot id
void
writesb(int dev)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(bp->data, &sb, sizeof(sb));
  log_write(bp);
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Block Reference Count
int
read_refcnt(uint bn, uint *cnt_out) // bn의 refcnt를 읽어옴
{
  // RETURN VALUE
  // On success 0, On Failure -1
  struct inode *ip;

  // REFCNT_INO로 inode 리턴
  if ((ip = iget(ROOTDEV, REFCNT_INO)) == (struct inode *)0){
    cprintf("read_refcnt: cannot get refcnt inode.\n");
    return -1;
  }

  ilock(ip); // inode lock

  // offset으로부터 uint 크기만큼 refcnt 읽어옴
  if (readi(ip, (char *)cnt_out, bn*sizeof(uint), sizeof(uint)) != sizeof(uint)){
    iunlockput(ip);
    cprintf("read_refcnt: read failed for bn %d\n", bn);
    return -1;
  }
  iunlockput(ip);
  return 0;
}

int
write_refcnt(uint bn, uint cnt) // bn의 refcnt 씀
{
  // RETURN VALUE
  // On Success 0, On Failure -1
  struct inode *ip;

  // REFCNT_INO의 inode 리턴
  if ((ip = iget(ROOTDEV, REFCNT_INO)) == (struct inode *)0){
    cprintf("write_refcnt: cannot get refcnt inode\n");
    return -1;
  }

  // inode lock
  ilock(ip);

  // offset으로부터 uint만큼 refcnt 덮어씀
  if (writei(ip, (char *)&cnt, bn*sizeof(uint), sizeof(uint)) != sizeof(uint)){
    iunlockput(ip);
    cprintf("write_refcnt: write failed for bn %d\n", bn);
    return -1;
  }

  iunlockput(ip);
  return 0;
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);

        // 할당하는 블록의 refcnt 1로 설정
        if (write_refcnt(b+bi, 1) < 0){
          cprintf("balloc: failed to write refcnt.\n");
          return 0;
        }

        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  cprintf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;
  // refcnt
  uint refcnt;

  // 1. .brefcnt 파일에서 블록 b의 refcnt 읽어옴
  if (read_refcnt(b, &refcnt) < 0){
    cprintf("bfree: failed to read refcnt.\n");
    return;
  }
  // 2. refcnt가 0 이하면 panic (refcnt가 0이면 이미 해제된 블록)
  if (refcnt <= 0){
    cprintf("bfree: refcnt zero or less.\n");
    return;
  }
  
  // 3. refcnt 감소 후 업데이트
  refcnt--;
  if (write_refcnt(b, refcnt) < 0){
    cprintf("bfree: failed to write refcnt.\n");
    return;
  }

  // 4. 감소 후 refcnt가 0인 경우에만 실제 블록 해제
  if (refcnt == 0){
    // 기존 bfree 로직
    bp = bread(dev, BBLOCK(b, sb));
    bi = b % BPB;
    m = 1 << (bi % 8);
    if ((bp->data[bi/8] & m) == 0) // Is the block free?
    {
      cprintf("freeing free block.\n");
      return;
    }
    bp->data[bi/8] &= ~m; // mark the block is free
    // bp->data[bi/8] |= m; -> mark the block in use
    log_write(bp);
    brelse(bp);
  } 
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      if (type == T_DIR) dip->minor = (MODE_DIR);
      else if (type == T_FILE) dip->minor = (MODE_FILE);
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  cprintf("Error: Out of inodes.\n");
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0){
    cprintf("iget: no inodes\n");
    return 0;
  }

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1){
    cprintf("ilock\n");
    return;
  }

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0){
      cprintf("ilock: no type\n");
      return;
    }
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1){
    cprintf("iunlock\n");
    return;
  }

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// (수정) 필요 시 COW (Copy-On-Write) 수행
static uint
bmap(struct inode *ip, uint bn, uint rdwr_flag)
{
  uint addr, *a;
  struct buf *bp;
  // COW
  uint refcnt;

  // 1. Direct Blocks
  if(bn < NDIRECT){
    // 기존 addr
    if((addr = ip->addrs[bn]) == 0){
      // If there is no such block, bmap allocates one.
      addr = balloc(ip->dev);
      if (!addr) return 0;
      ip->addrs[bn] = addr;
    } else {
      // read만 하는 경우 COW 없이 바로 주소 반환
      if (!rdwr_flag) return addr; // rdwr_flag == RD_FLAG
      // bmap -> read_refcnt -> readi -> bmap 순환 방지
      if (ip->inum == REFCNT_INO) return addr;
      // refcnt 검사
      if (read_refcnt(addr, &refcnt) < 0) {
        cprintf("bmap: read_refcnt failed (direct)\n");
        return 0;
      }
      // COW Trigger
      if (rdwr_flag && refcnt > 1){
        struct buf *old_buf, *new_buf;
        uint new_addr;

        // refcnt 1인 새 블록 할당
        if ((new_addr = balloc(ip->dev)) == 0){
          cprintf("bmap: cow balloc failed (direct)\n");
          return 0;
        }
        
        // 기존 블록 데이터
        if ((old_buf = bread(ip->dev, addr)) == 0){
          cprintf("bmap: cow bread old failed (direct)\n");
          return 0;
        }
        
        // 새로운 블록 데이터 버퍼
        if ((new_buf = bread(ip->dev, new_addr)) == 0){
          cprintf("bmap: cow bread new failed (direct)\n");
          return 0;
        }

        // 기존 데이터 복사
        memmove(new_buf->data, old_buf->data, BSIZE);

        // 새 블록 디스크에 write
        log_write(new_buf);
        brelse(old_buf);
        brelse(new_buf);

        // Inode 포인터 교체
        ip->addrs[bn] = new_addr;

        // 기존 블록 free -> refcnt 감소
        bfree(ip->dev, addr);

        addr = new_addr;
      }
    }
    return addr;
  }

  // 2. Single Indirect Blocks
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0){
      if (!rdwr_flag) return 0;
      addr = balloc(ip->dev);
      if (!addr) return 0;
      ip->addrs[NDIRECT] = addr;
    }

    if (rdwr_flag) {
      uint refcnt_indir;
      if (read_refcnt(addr, &refcnt_indir) < 0){
        cprintf("bmap: read refcnt indirect block\n");
        return 0;
      }
      if (refcnt_indir > 1) {
        uint new_indir = balloc(ip->dev);
        if (!new_indir) {
          cprintf("bmap: cow balloc indirect\n");
        }
        struct buf *old_bp = bread(ip->dev, addr);
        struct buf *new_bp = bread(ip->dev, new_indir);
        memmove(new_bp->data, old_bp->data, BSIZE);
        log_write(new_bp);
        brelse(old_bp);
        brelse(new_bp);

        ip->addrs[NDIRECT] = new_indir;
        bfree(ip->dev, addr);

        addr = new_indir;
      }
    }
    // read the indirect block
    bp = bread(ip->dev, addr);
    if (!bp) {
      cprintf("bmap: bread failed (indirect)\n");
      return 0;
    }

    a = (uint*)bp->data; // indirect block 내의 주소 배열 포인터

    if((addr = a[bn]) == 0){
      if (!rdwr_flag) return 0;
      // 새 데이터 블록 할당
      addr = balloc(ip->dev);
      if (!addr){
        brelse(bp);
        return 0;
      }
      a[bn] = addr;
      log_write(bp);
    } else {
      if (!rdwr_flag) {
        brelse(bp);
        return addr;
      }
      // bmap -> read_refcnt -> readi -> bmap 순환 방지
      if (ip->inum == REFCNT_INO){
        brelse(bp);
        return addr;
      }
      // refcnt 검사
      if (read_refcnt(addr, &refcnt) < 0){
        cprintf("bmap: read_refcnt failed (indirect)\n");
        return 0;
      }
      
      // COW Trigger
      if (rdwr_flag && refcnt > 1){
        struct buf *old_buf, *new_buf;
        uint new_addr;

        // 새 블록 할당
        if ((new_addr = balloc(ip->dev)) == 0){
          cprintf("bmap: cow balloc failed (indirect)\n");
          return 0;
        }

        // 기존 블록 데이터 read
        if ((old_buf = bread(ip->dev, addr)) == 0){
          cprintf("bmap: cow bread old failed (indirect)\n");
          return 0;
        }
        
        // 새 블록 버퍼
        if ((new_buf = bread(ip->dev, new_addr)) == 0){
          cprintf("bmap: cow bread new failed (indirect)\n");
          return 0;
        }
        
        // 새 블록 버퍼에 기존 블록 데이터 복사
        memmove(new_buf->data, old_buf->data, BSIZE);

        // 새 블록 쓰기
        log_write(new_buf);
        brelse(old_buf);
        brelse(new_buf);

        // indirect block 포인터 교체
        a[bn] = new_addr;
        log_write(bp); // 간접 블록 변경사항 로그 기록

        // 기존 블록 free -> refcnt 감소
        bfree(ip->dev, addr);

        addr = new_addr;
      }
    }
    brelse(bp);
    return addr;
  }

  cprintf("bmap: out of range\n");
  return 0;
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
  if (ip->type != T_DEV)
    st->mode = ip->minor;
  else 
    st->mode = (S_IR | S_IW); // T_DEV의 default permission
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE, RD_FLAG));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE, WR_FLAG));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR){
    cprintf("dirlookup not DIR\n");
    return 0;
  }

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de)){
      cprintf("dirlookup read\n");
      return 0;
    }
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de)){
      cprintf("dirlink read.\n");
      return -1;
    }
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de)){
    cprintf("dirlink\n");
    return -1;
  }
  
  return 0;
}

struct inode *
snapshot_iget(uint dev, uint inum)
{
  return iget(dev, inum);
}


//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

