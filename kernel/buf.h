struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint lastuse;     // tick when the last reference was released
  struct buf *next; // hash bucket chain
  uchar data[BSIZE];
};

