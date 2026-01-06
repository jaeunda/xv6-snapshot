#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"

#define MAX_ADDRS (NDIRECT + 1 + NINDIRECT)

int 
main(int argc, char *argv[])
{
    int fd;
    uint addrs[MAX_ADDRS];

    if (argc != 2){
        printf(2, "Usage: print_addr <filename>\n");
        exit();
    }

    if ((fd = open(argv[1], O_RDONLY)) < 0){
        printf(2, "print_addr: cannot open the file %s\n", argv[1]);
        exit();
    }
    if (get_addr(fd, addrs) < 0){
        printf(2, "print_addr: get_addr failed\n");
        close(fd);
        exit();
    }

    // direct block
    for (int i=0; i<NDIRECT; i++){
        if (addrs[i] != 0){
            printf(1, "addr[%d]: %x\n", i, addrs[i]);
        }
    }

    // indirect block
    if (addrs[NDIRECT] != 0)
        printf(1, "addr[%d]: %x (INDIRECT POINTER)\n", NDIRECT, addrs[NDIRECT]);

    for (int i=0; i<NINDIRECT; i++){
        uint val = addrs[NDIRECT + 1 + i];
        if (val != 0){
            printf(1, "addr[%d] -> [%d] (bn : %d) : %x\n",NDIRECT, i, NDIRECT+i, val);
        }
    }
    close(fd);
    exit();
}