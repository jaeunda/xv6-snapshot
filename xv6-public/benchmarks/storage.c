// Controlled, single-writer fixture; built only by measure.py in temporary trees.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"

static char buffer[512];
static void fail(char *message) { printf(2, "FAIL %s\n", message); exit(); }

static void copy_file(char *source, char *dest) {
  int in = open(source, O_RDONLY), out = open(dest, O_CREATE|O_WRONLY), n;
  if(in < 0 || out < 0) fail("copy open");
  while((n = read(in, buffer, sizeof buffer)) > 0)
    if(write(out, buffer, n) != n) fail("copy write");
  if(n < 0) fail("copy read");
  close(in); close(out);
}

int main(int argc, char **argv) {
  int fd, i, block;
  if(argc != 2) fail("usage: storage prepare|copy|save|edit");
  if(strcmp(argv[1], "prepare") == 0) {
    fd = open("payload", O_CREATE|O_WRONLY);
    if(fd < 0) fail("prepare open");
    for(block = 0; block < 128; block++) {
      for(i = 0; i < sizeof buffer; i++) buffer[i] = (i*17+block+23)&255;
      if(write(fd, buffer, sizeof buffer) != sizeof buffer) fail("prepare write");
    }
    close(fd);
  } else if(strcmp(argv[1], "copy") == 0) {
    struct dirent entry;
    // Extra space also permits this guest's stock stat ABI in the extended kernel.
    struct { struct stat value; uint padding[4]; } info;
    char name[DIRSIZ+1], dest[32];
    if(mkdir("saved") < 0) fail("mkdir");
    fd = open("/", O_RDONLY);
    if(fd < 0) fail("root open");
    while(read(fd, &entry, sizeof entry) == sizeof entry) {
      if(!entry.inum) continue;
      memmove(name, entry.name, DIRSIZ); name[DIRSIZ] = 0;
      if(fstat(fd, &info.value) < 0) fail("root stat");
      if(stat(name, &info.value) < 0) fail("entry stat");
      if(info.value.type != T_FILE) continue;
      strcpy(dest, "saved/"); strcpy(dest+6, name);
      copy_file(name, dest);
    }
    close(fd);
} else if(strcmp(argv[1], "save") == 0) {
    int result;
    // Zero-argument snapshot_create is syscall 22; the stock run never calls it.
    asm volatile("int $0x40" : "=a"(result) : "0"(22) : "memory", "cc");
    if(result < 0) fail("snapshot create");
  } else if(strcmp(argv[1], "edit") == 0) {
    fd = open("payload", O_RDWR);
    if(fd < 0) fail("edit open");
    while((i = read(fd, buffer, sizeof buffer)) > 0);
    if(i < 0 || write(fd, "X", 1) != 1) fail("edit");
    close(fd);
  } else fail("unknown action");
  printf(1, "OK %s\n", argv[1]); exit();
}
