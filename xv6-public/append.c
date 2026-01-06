#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int
main(int argc, char *argv[]){
    if (argc != 3){
        printf(2, "Usage: append filename string\n");
        exit();
    }
    // open the file for read+write, create if needed
    int fd = open(argv[1], O_RDWR | O_CREATE);
    if (fd < 0){
        printf(2, "append: cannot open %s\n", argv[1]);
        exit();
    }
    // move offset to the end by reading until EOF
    char buf[1];
    while (read(fd, buf, 1) == 1); // drain to EOF
    // now at end, write the string
    if (write(fd, argv[2], strlen(argv[2])) < 0){
        printf(2, "append: write failed\n");
        close(fd);
        exit();
    }
    close(fd);
    exit();
}