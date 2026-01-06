#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "types.h"
#include "fs.h"
#include "stat.h"
#include "param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
uint bmap_mkfs(struct dinode *ip, uint bn);

// convert to intel byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);
  sb.snapshot_next_id = xint(1);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

// /snapshot 디렉토리 생성
  // 1. /snapshot 디렉토리 inode 할당
  uint snapshot_ino = ialloc(T_DIR); // inode 2 리턴
  assert(snapshot_ino == 2);
  
  bzero(&din, sizeof(din));
  din.type = xshort(T_DIR);
  din.nlink = xshort(1);
  din.size = xint(0);
  din.minor = xshort(MODE_RDONLY);
  winode(snapshot_ino, &din);

  bzero(&de, sizeof(de));
  de.inum = xshort(snapshot_ino);
  strcpy(de.name, ".");
  iappend(snapshot_ino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino); // 부모 디렉토리 -> root
  strcpy(de.name, ".."); 
  iappend(snapshot_ino, &de, sizeof(de));

  // root 디렉토리에 snapshot 엔트리 추가
  bzero(&de, sizeof(de));
  de.inum = xshort(snapshot_ino);
  strncpy(de.name, "snapshot", DIRSIZ);
  iappend(rootino, &de, sizeof(de));
    
  for(i = 2; i < argc; i++){
    assert(index(argv[i], '/') == 0);

    if((fd = open(argv[i], 0)) < 0){
      perror(argv[i]);
      exit(1);
    }

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(argv[i][0] == '_')
      ++argv[i];

    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, argv[i], DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
  }

  // /snapshot/.brefcnt 파일 생성 및 초기화
  printf("allocating refcount file at inode %d\n", REFCNT_INO);

  // .brefcnt 파일 inode 생성
  struct dinode bref_ino;
  inum = REFCNT_INO;
  bref_ino.type = xshort(T_FILE); // file type 일반 파일
  bref_ino.major = xshort(0); // Major device number (T_DEV only)
  bref_ino.minor = xshort(0); // Minor device number (T_DEV only)
  bref_ino.nlink = xshort(1); // Number of links to inode in file system
  bref_ino.size = xint(0); // bytes
  bzero(bref_ino.addrs, sizeof(bref_ino.addrs)); // data block addresses
  winode(inum, &bref_ino);

  // /snapshot 디렉토리에 ".brefcnt" 엔트리 추가
  bzero(&de, sizeof(de));
  de.inum = xshort(inum); // REFCNT_INO
  strncpy(de.name, ".brefcnt", DIRSIZ);
  iappend(snapshot_ino, &de, sizeof(de)); // snapshot inode에 de(".brefcnt") 엔트리 추가

  // ".brefcnt" 파일에 초기화된 블록 할당 (크기: nblocks * sizeof(uint))
  uint brefcnt_size = FSSIZE * sizeof(uint);
  uint brefcnt_blocks = (brefcnt_size + BSIZE -1)/BSIZE;
  
  char init_buf[BSIZE];
  memset(init_buf, (int)0, BSIZE);

  // 초기화된 블록 할당, .brefcnt inode에 블록 추가
  printf("allocating %u blocks for brefcnt file...\n", brefcnt_blocks);
  for (int i=0; i<brefcnt_blocks; i++){
    iappend(inum, init_buf, BSIZE);
  }

  balloc(freeblock);

  // .brefcnt: 사용 중인 블록의 refcnt 1로 초기화
  printf("initializing refcnt data ... \n");
  uint refval = xint(1);

  // bmap에서 사용 중인 블록 찾기
  for (int i=0; i<sb.nblocks; i++){ // block 번호
    uint bmap_bn = BBLOCK(i, sb); // i가 속한 비트맵 블록 번호
    rsect(bmap_bn, buf); // bmap 블록 읽어옴

    int bi = i % BPB; // block 내 bit offset
    /* IS BLOCK FREE? (fs.c:bzero())
        int m = 1 << (bi % 8);
        if ((bp->data[bi/8] & m) == 0){ ... }

       MARK THE BLOCK IN USE
       bp->data[bi/8] |= m;
    */
    if (buf[bi/8] & (1 << (bi%8))){ // Is block free ?
      // 블록 i가 사용 중이라면 .brecnt 파일에서 이 블록의 refcnt를 1로 설정
      rinode(REFCNT_INO, &bref_ino); // .brefcnt의 inode(REFCNT_INO) 읽기

      // .brefcnt 파일 내 오프셋
      uint bref_off = i * sizeof(uint);
      // .brefcnt 파일의 데이터 블록 넘버 계산
      uint bref_bn = bmap_mkfs(&bref_ino, bref_off / BSIZE);
      if (!bref_bn) {
        fprintf(stderr, "mkfs: refcnt block not allocated.\n");
        exit(1);
      } 

      // 해당 블록을 읽고
      rsect(bref_bn, buf);
      // 버퍼에 refcnt 쓰기 
      uint off_in_block = bref_off % BSIZE;
      memmove(buf + off_in_block, &refval, sizeof(uint));
      // 블록에 쓰기
      wsect(bref_bn, buf);
    }
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  // 위치 수정
  //balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE){
    perror("write");
    exit(1);
  }
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE){
    perror("read");
    exit(1);
  }
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  if (type == T_DIR) 
    din.minor = xshort(MODE_DIR);
  else if (type == T_FILE) 
    din.minor = xshort(MODE_FILE);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

// Return the disk block address of the nth block in inode ip
// If there is no such block, return 0;
uint 
bmap_mkfs(struct dinode *ip, uint bn)
{ // mkfs.c 용 bmap, 블록 할당 기능 삭제
  uint addr, *a;
  char buf[BSIZE];

  if (bn < NDIRECT){
    if ((addr = xint(ip->addrs[bn])) == 0)
      return 0; // balloc 수행하지 않음
    return addr;
  }
  bn -= NDIRECT;

  // 간접 블록
  if (bn < NINDIRECT){
    if ((addr = xint(ip->addrs[NDIRECT])) == 0){
      return 0; // balloc 수행하지 않음
    }
    
      // 간접 블록 읽기
      rsect(addr, buf);
      a = (uint *)buf;

      // 간접 블록 내 주소 반환
      if ((addr = xint(a[bn])) == 0)
        return 0; // balloc 수행하지 않음
      
    return addr; 
  }
  fprintf(stderr, "bmap_mkfs: out of range\n");
  exit(1);
  return 0;
}