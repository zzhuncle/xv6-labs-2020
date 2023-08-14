// lab8 q2
// 为 kernel/buf.h 添加 lastuse 字段，便于使用 LRU 机制
struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
  uint lastuse;
};

