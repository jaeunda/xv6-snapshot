#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]){
    int snap_id;
    if (argc < 2 || atoi(argv[1]) <= 0) {
        printf(2, "[USAGE] snap_delete <id>\n");
        exit();
    }

    snap_id = atoi(argv[1]);
    if (snapshot_delete(snap_id) != snap_id){
        printf(2, "SNAPSHOT DELETE <ID: %d> Failed.\n", snap_id);
    } else {
        printf(1, "SNAPSHOT DELETE <ID: %d> Done.\n", snap_id);
    }
    exit();
}