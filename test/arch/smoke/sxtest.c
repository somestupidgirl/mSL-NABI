/* freestanding: exercise statx and prlimit64. statx a known file and confirm it
 * reads back as a regular file with nonzero size; prlimit64 queries RLIMIT_NOFILE
 * and confirms a nonzero soft limit. Both are aarch64 syscalls (291, 261) that
 * had no handler before - see PORTING-arm64.md 3.2. */
static long sys6(long n,long a,long b,long c,long d,long e,long f){
  register long x8 asm("x8")=n;
  register long x0 asm("x0")=a; register long x1 asm("x1")=b;
  register long x2 asm("x2")=c; register long x3 asm("x3")=d;
  register long x4 asm("x4")=e; register long x5 asm("x5")=f;
  asm volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");
  return x0;
}
#define SYS_write 64
#define SYS_exit  93
#define SYS_prlimit64 261
#define SYS_statx 291
#define AT_FDCWD  -100
#define STATX_BASIC_STATS 0x7ff
#define RLIMIT_NOFILE 7
#define S_IFMT  0170000
#define S_IFREG 0100000

struct statx_ts { long tv_sec; unsigned tv_nsec; int __r; };
struct statx {
  unsigned stx_mask, stx_blksize; unsigned long stx_attributes;
  unsigned stx_nlink, stx_uid, stx_gid; unsigned short stx_mode, __sp0[1];
  unsigned long stx_ino, stx_size, stx_blocks, stx_attributes_mask;
  struct statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
  unsigned stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
  unsigned long stx_mnt_id; unsigned stx_dio1, stx_dio2; unsigned long __sp3[12];
};
struct rlim64 { unsigned long cur, max; };
static void put(const char*m){int i=0;while(m[i])i++;sys6(SYS_write,1,(long)m,i,0,0,0);}

void _start(void){
  int ok = 1;
  struct statx sx;
  long r = sys6(SYS_statx, AT_FDCWD, (long)"/sxtest", 0, STATX_BASIC_STATS, (long)&sx, 0);
  if (r < 0 || (sx.stx_mode & S_IFMT) != S_IFREG || sx.stx_size == 0){ put("statx FAIL\n"); ok=0; }
  else put("statx ok\n");

  struct rlim64 old;
  r = sys6(SYS_prlimit64, 0, RLIMIT_NOFILE, 0, (long)&old, 0, 0);
  if (r < 0 || old.cur == 0){ put("prlimit64 FAIL\n"); ok=0; }
  else put("prlimit64 ok\n");

  sys6(SYS_exit, ok?0:1, 0,0,0,0,0);
}
