#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]){
    int snap_id;
    snap_id = snapshot_create();

    if (snap_id > 0)
        printf(1, "SNAPSHOT CREATE <ID: %d> Done.\n", snap_id);
    else
        printf(2, "SNAPSHOT CREATE Failed.\n");
    exit();
}