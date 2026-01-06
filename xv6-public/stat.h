#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

// Permission (T_DIR, T_FILE인 경우에만 struct inode의 minor 필드 사용)
#define S_IR 0400
#define S_IW 0200
#define S_IX 0100

#define MODE_DIR (S_IR | S_IW)
#define MODE_FILE (S_IR | S_IW | S_IX)
#define MODE_RDONLY (S_IR)

struct stat {
  short type;  // Type of file
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short nlink; // Number of links to file
  uint size;   // Size of file in bytes
  short mode; // T_DIR/T_FILE인 경우 minor 필드
};
