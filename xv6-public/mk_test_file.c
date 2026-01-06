#include "types.h"
#include "fcntl.h"
#include "user.h"

int main(int argc, char *argv[]){
    int fd;
    if (argc < 2){
        printf(1, "need argv[1]\n");
        exit();
    }
    if ((fd = open(argv[1], O_CREATE | O_WRONLY)) < 0){
        printf(1, "open error for %s\n", argv[0]);
        exit();
    }
    char buf[512];
    for (int i=1; i<511; i++) buf[i] = 0;
    buf[511] = '\n';
    for (int i= 0; i<12; i++){
        buf[0] = i%10 + '0';
        write(fd, buf, 512);
    }
    char *str = "hello\n";
    write(fd, str, 6);
    close(fd);
    exit();
}