#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "fs.h"
#include "stat.h"
#include "sleeplock.h"
#include "file.h"
#include "buf.h"
#include "snapshot.h"

// 전역 변수
static struct sleeplock snapshot_lock;
extern struct superblock sb;

//
void snapshot_iclear_r(struct inode *dp);
int snapshot_icopy_r(struct inode *dst_dp, struct inode *src_ip, char *name, int rollback_flag);

// util
static void int_to_str(int id, char name[DIRSIZ]);
static struct inode *snapshot_mkdir_id(struct inode *dp, char name[DIRSIZ]);

extern struct inode *snapshot_iget(uint dev, uint inum);

// 부팅 시 snapshot_lock 초기화
void
snapshot_init(void)
{
 initsleeplock(&snapshot_lock, "snapshot");
 cprintf("init: snapshot\n");
}

int
snapshot_create(void)
{
    int snapshot_id;
    char snapshot_dirname[DIRSIZ];
    struct inode *snapshot_root, *snapshot_dir, *root;

    acquiresleep(&snapshot_lock);

    // /snapshot 디렉토리 찾기
    if (!(snapshot_root = namei("/snapshot/"))){
        releasesleep(&snapshot_lock);
        cprintf("Error: No /snapshot directory.\n");
        return -1;
    }

    ilock(snapshot_root);
    begin_op(); // transaction 시작 (디스크 수정)

    // new id 할당
    snapshot_id = sb.snapshot_next_id++;
    writesb(ROOTDEV);

    int_to_str(snapshot_id, snapshot_dirname);

    if (!(snapshot_dir = snapshot_mkdir_id(snapshot_root, snapshot_dirname))) { // lock 잡고 들어옴
        iunlockput(snapshot_root);
        end_op();
        releasesleep(&snapshot_lock);
        return -1;
    }
  
    iunlockput(snapshot_root);

    root = snapshot_iget(ROOTDEV, ROOTINO);
    ilock(root);

    end_op();

    uint off = 0;
    struct dirent de;
    while (readi(root, (char *)&de, off, sizeof(de)) == sizeof(de)) {
        begin_op();

        struct inode *child = 0;
        off += sizeof(de);

        if (de.inum == 0) {
            end_op();
            continue;
        }
        if (!strncmp(de.name, ".", DIRSIZ) || !strncmp(de.name, "..", DIRSIZ)){
            end_op();
            continue;
        }
        // snapshot 제외
        if (!strncmp(de.name, "snapshot", DIRSIZ)){
            end_op();
            continue;
        }
        
        if (!(child = snapshot_iget(root->dev, de.inum))) {
            iunlockput(root);
            iunlockput(snapshot_dir);
            end_op();
            releasesleep(&snapshot_lock);
            // remove <snapshot_id> directory
            snapshot_delete(snapshot_id);
            return -1;
        }

        ilock(child);
        if (child->type == T_DEV){
            iunlockput(child);
            end_op();
            continue;
        }
        if (snapshot_icopy_r(snapshot_dir, child, de.name, 0) < 0) {
            iunlockput(child);
            iunlockput(root);
            iunlockput(snapshot_dir);
            end_op();
            releasesleep(&snapshot_lock);
            // remove <snapshot_id> directory
            snapshot_delete(snapshot_id);
            return -1;
        }

        iunlockput(child);

        end_op();
    }

    begin_op();
    iunlockput(root);
    iunlockput(snapshot_dir);

    end_op(); // 디스크 수정 작업 완료 후 transaction commit

    releasesleep(&snapshot_lock);

    return snapshot_id;
}

int
snapshot_rollback(int snapshot_id) 
{
    // 현재 파일시스템을 id의 스냅샷으로 복구
    // 복구 시 새로운 inode를 할당받아 복구
    // 올바르지 않은 id 입력 시 -1 리턴
    // 복구 완료 시 id 리턴

    // ID 검증
    if (snapshot_id <= 0) return -1;

    char snapshot_dirname[DIRSIZ];
    int_to_str(snapshot_id, snapshot_dirname);

    // snapshot_lock
    acquiresleep(&snapshot_lock);

    // rollback할 스냅샷이 존재하는지 확인
    // /snapshot
    struct inode *snapshot_root;
    if (!(snapshot_root = namei("/snapshot"))){
        releasesleep(&snapshot_lock);
        cprintf("Error: No /snapshot directory.\n");
        return -1;
    }
    ilock(snapshot_root);
    
    // <id>에 해당하는 디렉토리
    struct inode *rollback_dir;
    if (!(rollback_dir = dirlookup(snapshot_root, snapshot_dirname, 0))) {
        iunlockput(snapshot_root);
        releasesleep(&snapshot_lock);
        cprintf("Error: No such snapshot directory.\n");
        return -1;
    }
    iunlockput(snapshot_root);
    ilock(rollback_dir);

    // 디스크 수정 시작 (transaction)
    begin_op();

    // ROLLBACK
    // 1. root 지우기 (snapshot, ., ..는 유지)
    struct inode *root;
    if (!(root = snapshot_iget(ROOTDEV, ROOTINO))) {
        iunlockput(rollback_dir);
        end_op();
        releasesleep(&snapshot_lock);
        return -1;
    }
    ilock(root);
  
    end_op();

    // clear root
    snapshot_iclear_r(root);

    // 2. rollback_dir를 root로 복사
    struct dirent de;
    uint off = 0;

    while (readi(rollback_dir, (char *)&de, off, sizeof(de)) == sizeof(de)){
        begin_op();

        struct inode *child = (struct inode *)0;
        off += sizeof(de);

        if (de.inum == 0) {
            end_op();
            continue;
        }
        if (!strncmp(de.name, ".", DIRSIZ) || !strncmp(de.name, "..", DIRSIZ)){
            end_op();
            continue;
        }
        // snapshot 디렉토리, T_DEV 파일은 root에 남아있으며, 스냅샷 생성 시에도 복사 대상에서 제외

        // child
        if (!(child = snapshot_iget(rollback_dir->dev, de.inum))) {
            iunlockput(root);
            iunlockput(rollback_dir);
            end_op();
            releasesleep(&snapshot_lock);
            return -1;
        }
        ilock(child);

        snapshot_icopy_r(root, child, de.name, 1);

        iunlockput(child);

        end_op();
    }

    begin_op();

    iunlockput(root);
    iunlockput(rollback_dir);

    end_op();
    releasesleep(&snapshot_lock);

    return snapshot_id;
}
int
snapshot_delete(int snapshot_id)
{
    // 지정한 id의 스냅샷 삭제
    // 올바르지 않은 id 입력 시 -1 리턴
    // 삭제 완료 시 id 리턴

    // ID 검증
    if (snapshot_id <= 0) return -1;

    char snapshot_dirname[DIRSIZ];
    int_to_str(snapshot_id, snapshot_dirname);

    // snapshot_lock
    acquiresleep(&snapshot_lock);

    // 삭제할 스냅샷이 존재하는지 확인
    struct inode *snapshot_root;
    if (!(snapshot_root = namei("/snapshot"))){
        releasesleep(&snapshot_lock);
        cprintf("Error: No /snapshot directory.\n");
        return -1;
    }
    ilock(snapshot_root);

    // id에 해당하는 디렉토리 존재 여부 확인
    struct inode *delete_dir;
    if (!(delete_dir = dirlookup(snapshot_root, snapshot_dirname, 0))) {
        iunlockput(snapshot_root);
        releasesleep(&snapshot_lock);
        cprintf("Error: No such snapshot directory.\n");
        return -1;
    }
    ilock(delete_dir);

    // DELETE
    snapshot_iclear_r(delete_dir); // delete_dir의 inode 재귀적으로 초기화

    // 디스크 수정 (transaction)
    begin_op();

    // delete_dir의 nlink 처리
    if (snapshot_root->nlink > 0){ // delete_dir의 .. 엔트리 삭제
        snapshot_root->nlink--;
        iupdate(snapshot_root);
    }
    if (delete_dir->nlink > 0){ // delete_dir의 . 엔트리 삭제
        delete_dir->nlink--;
        iupdate(delete_dir);
    }
    
    // snapshot_root에서 delete_dir 엔트리 삭제
    struct dirent de;
    uint off = 0;
    while (readi(snapshot_root, (char *)&de, off, sizeof(de)) == sizeof(de)){
        if (de.inum == delete_dir->inum){
            memset(&de, 0, sizeof(de));
            writei(snapshot_root, (char *)&de, off, sizeof(de)); // 초기화한 entry로 덮어씀
            break;
        }
        off += sizeof(de);
    }

    // snapshot_root에서 delete_dir 링크 끊기
    if (delete_dir->nlink > 0){
        delete_dir->nlink--;
        iupdate(delete_dir);
    }

    iunlockput(delete_dir);
    iunlockput(snapshot_root);

    end_op();

    releasesleep(&snapshot_lock);

    return snapshot_id;
}

int
snapshot_icopy_r(struct inode *dst_dp, struct inode *src_ip, char *name, int rollback_flag)
{
    // dst_ip(dir)에 src_ip를 name이라는 이름으로 복사(재귀적으로)
    // dst_dp src_ip lock 잡은 상태로 호출
    // RETURN VALUE
    // On success 0, on failure -1.

    struct inode *new_ip;

    // 1. 새 inode 할당
    if ((new_ip = ialloc(src_ip->dev, src_ip->type)) == 0) {
        cprintf("Hint: Run 'snap_delete' to clean up.\n");
        return -1;
    }
    
    // 2. 새 inode 초기화
    ilock(new_ip);
    new_ip->major = src_ip->major;
    if (new_ip->type == T_DIR)
        new_ip->minor = rollback_flag ? MODE_DIR : MODE_RDONLY;
    else if (new_ip->type == T_FILE)
        new_ip->minor = rollback_flag ? MODE_FILE : MODE_RDONLY;
    else
        new_ip->minor = src_ip->minor;
    iupdate(new_ip);
    
    // DIR가 아닌 경우에만 블록 복사 및 refcnt 증가
    if (src_ip->type != T_DIR) {
        new_ip->size = src_ip->size;
        // 데이터 블록 포인터 복사 (물리적으로 블록 공유)
        memmove(new_ip->addrs, src_ip->addrs, sizeof(src_ip->addrs));

        // update refcnt (DIRECT BLOCKS)
        for (uint bn = 0; bn < NDIRECT; bn++){
            uint addr, cnt;
            if ((addr = new_ip->addrs[bn])){
                if (read_refcnt(addr, &cnt) < 0){
                    cprintf("Error: cannot read snapshot metadata.\n");
                    return -1;
                }
                if (write_refcnt(addr, cnt + 1) < 0){
                    cprintf("Error: cannot write snapshot metadata.\n");
                    return -1;
                }
            }
        }
    
        // update refcnt (INDIRECT BLOCKS)
        uint addr;
        if ((addr = new_ip->addrs[NDIRECT])){
            uint cnt;
            if (read_refcnt(addr, &cnt) < 0) {
                cprintf("Error: cannot read snapshot metadata.\n");
                return -1;
            }
            if (write_refcnt(addr, cnt + 1) < 0) {
                cprintf("Error: cannot write snapshot metadata.\n");
                return -1;
            }

            struct buf *bp;
            bp = bread(src_ip->dev, addr);
            uint *data_addr = (uint *)bp->data;
            // Update refcnt (INDATA)
            for (uint bn = 0; bn < NINDIRECT; bn++){ // direct와 동일한 방식으로
                if ((addr = data_addr[bn])){
                    if (read_refcnt(addr, &cnt) < 0) {
                        cprintf("Error: cannot read snapshot metadata.\n");
                        return -1;
                    }
                    if (write_refcnt(addr, cnt + 1) < 0) {
                        cprintf("Error: cannot write snapshot metadata.\n");
                        return -1;
                    }
                }
            }
            brelse(bp); // When done with buffer, call relse
        } 
    } else {
            // T_DIR인 경우 블록 복사 안 함, 사이즈 0부터 시작
            new_ip->size = 0;
            memset(new_ip->addrs, 0, sizeof(new_ip->addrs));
    }

    new_ip->nlink = 1;
    iupdate(new_ip);

    // 4. 부모 디렉토리에 링크
    // dirlink(): write a new directory entry (name, inum) into the directory dp.
    if (dirlink(dst_dp, name, new_ip->inum) < 0) {
        cprintf("Error: cannot link the directory %s.", name);
        return -1;
    }

    // 5. src_ip가 디렉토리라면 내부 엔트리 재귀 복사
    if (src_ip->type == T_DIR){
        struct dirent de;
        uint off = 0;

        if (dirlink(new_ip, ".", new_ip->inum) < 0){
            cprintf("Error: cannot link the . entry.\n");
            return -1;
        }
        if (dirlink(new_ip, "..", dst_dp->inum) < 0){
            cprintf("Error: cannot link the .. entry.\n");
            return -1;
        }
        
        dst_dp->nlink++;
        iupdate(dst_dp);

        while (readi(src_ip, (char *)&de, off, sizeof(de)) == sizeof(de)){
            struct inode *child = 0;
            off += sizeof(de);
            
            if (de.inum == 0) continue;
            if (!strncmp(de.name, ".", DIRSIZ) || !strncmp(de.name, "..", DIRSIZ)) continue;
            // snapshot 디렉토리 제외
            if (src_ip->inum == ROOTINO && !strncmp(de.name, "snapshot", DIRSIZ)) continue;

            if (!(child = snapshot_iget(src_ip->dev, de.inum))) {
                cprintf("Error: cannot get the inode of directory %s.\n", de.name);
                return -1;
            }

            ilock(child);
            if (child->type == T_DEV){
              iunlockput(child);
              continue;
            }
            // 재귀 호출
            snapshot_icopy_r(new_ip, child, de.name, rollback_flag);

            iunlockput(child);
        }
    }
    iunlockput(new_ip);
    return 0;
}

void
snapshot_iclear_r(struct inode *dp)
{
    // dp 하위 파일 모두 삭제
    // snapshot 디렉토리와 T_DEV 파일은 제외
    struct dirent de;
    uint off = 0;
    while (readi(dp, (char *)&de, off, sizeof(de)) == sizeof(de)){
        struct inode *child = (struct inode *)0;

        if (de.inum == 0){
            off += sizeof(de);
            continue;
        }
        if (!strncmp(de.name, ".", DIRSIZ) || !strncmp(de.name, "..", DIRSIZ)) {
            off += sizeof(de);
            continue;
        }
        if (dp->inum == ROOTINO && !strncmp(de.name, "snapshot", DIRSIZ)) {
            off += sizeof(de);
            continue;
        }
        
        if (!(child = snapshot_iget(dp->dev, de.inum))) {
            cprintf("Error: cannot get the inode of directory %s.\n", de.name);
            return;
        }
        ilock(child);

        // T_DEV 파일이면 skip
        if (child->type == T_DEV) {
            iunlockput(child);
            off += sizeof(de);
            continue;
        }

        // T_DIR이면 재귀적으로 삭제
        if (child->type == T_DIR){
            snapshot_iclear_r(child);

            // 마지막 단계에서 memset으로 밀어버릴 거임
            // dp의 nlilnk 감소 (자식의 .. link 삭제)
            begin_op();
            if (dp->nlink > 0){
                dp->nlink--;
                iupdate(dp);
            }
            // child의 nlink 감소(자식의 . link 삭제)
            if (child->nlink > 0){
                child->nlink--;
                iupdate(child);
            }
            end_op();
        }

        begin_op();
        // 삭제 (unlink)
        if (child->nlink > 0) {
            child->nlink--;
            iupdate(child);
        }
        iunlockput(child);

        // dirent 지우기 (0으로 초기화)
        memset(&de, 0, sizeof(de));
        if (writei(dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
            end_op();
            cprintf("Error: failed to writei\n");
            return;
        }

        end_op();
        off += sizeof(de);
    }
}
// /snapshot 아래에 [ID] 디렉토리를 생성하는 함수
// struct inode *dp는 locked 상태로 들어와야 하며 
// locked 상태의 새로운 struct inode * 반환
static struct inode *
snapshot_mkdir_id(struct inode *dp, char name[DIRSIZ])
{
    struct inode *snapshot_ip;

    // 이미 존재하는지 체크
    if ((snapshot_ip = dirlookup(dp, name, 0))){
        return (struct inode *)0;
    }

    // 존재하지 않는다면 새 디렉토리 inode 할당
    snapshot_ip = ialloc(dp->dev, T_DIR);
    if (!snapshot_ip) {
        cprintf("Hint: Run 'snap_delete' to clean up.\n");
        return 0;
    }

    // inode 초기화
    ilock(snapshot_ip);
    snapshot_ip->nlink = 1;
    snapshot_ip->minor = MODE_RDONLY;
    iupdate(snapshot_ip);

    // . .. entry
        // dirlink(struct inode *dp, char *name, uint inum)
        // write a new directory entry (name, inum) into the directory dp.
    if (dirlink(snapshot_ip, ".", snapshot_ip->inum) < 0){
        iunlockput(snapshot_ip);
        cprintf("Error: cannot link the . entry.\n");
        return 0;
    }
    if (dirlink(snapshot_ip, "..", dp->inum) < 0){
        iunlockput(snapshot_ip);
        cprintf("Error: cannot link the .. entry.\n");
        return 0;
    }

    dp->nlink++;
    iupdate(dp);
    
    // dp에 추가
    if (dirlink(dp, name, snapshot_ip->inum) < 0) {
        iunlockput(snapshot_ip);
        cprintf("Error: cannot link the directory %s.\n", name);
        return 0;
    }

    return snapshot_ip; // lock 잡은 상태로 반환
}
static void
int_to_str(int id, char name[DIRSIZ])
{
    char buf[16];
    int i=0, j;

    if (id <= 0) id = 0;
    if (!id) buf[i++] = '0';
    else {
        int x = id;
        char tmp[16];
        j = 0;
        // 일의 자리부터 tmp 버퍼에 push
        while (x > 0 && j < 15){
            tmp[j++] = '0' + (x % 10);
            x /= 10;
        }
        // 역순으로 복사
        while (j > 0)
            buf[i++] = tmp[--j];
    }
    buf[i] = (char)0;

    if (i < 2){
        int padding = 2 - i;
        int k;

        for (k=i; k>=0; k--)
            buf[k+padding] = buf[k];
        for (k=0; k<padding; k++)
            buf[k]='0';
        
        i+=padding;
    }

    // name 버퍼에 복사
    memset(name, 0, DIRSIZ);
    for (j = 0; j < DIRSIZ && buf[j]; j++)
        name[j] = buf[j];
}