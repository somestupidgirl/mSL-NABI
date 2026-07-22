/* freestanding: fstat a known file and confirm the aarch64 stat layout is
 * right - st_mode reads back as a regular file (S_IFREG) with a nonzero size.
 * Guards against struct l_newstat regressing to the x86-64 field order, which
 * lands st_mode on the link count (see PORTING-arm64.md 3.5 / the stat fix). */
static long sys6(long n,long a,long b,long c,long d,long e,long f){
  register long x8 asm("x8")=n;
  register long x0 asm("x0")=a; register long x1 asm("x1")=b;
  register long x2 asm("x2")=c; register long x3 asm("x3")=d;
  register long x4 asm("x4")=e; register long x5 asm("x5")=f;
  asm volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");
  return x0;
}
#define SYS_openat 56
#define SYS_write  64
#define SYS_fstat  80
#define SYS_exit   93
#define AT_FDCWD   -100
#define S_IFMT     0170000
#define S_IFREG    0100000

/* asm-generic (aarch64) struct stat */
struct kstat {
  unsigned long st_dev, st_ino;
  unsigned int  st_mode, st_nlink, st_uid, st_gid;
  unsigned long st_rdev, __pad1;
  long          st_size;
  int           st_blksize, __pad2;
  long          st_blocks;
  long st_atime; unsigned long st_atime_nsec;
  long st_mtime; unsigned long st_mtime_nsec;
  long st_ctime; unsigned long st_ctime_nsec;
  unsigned int __unused4, __unused5;
};
static void put(const char*m){int i=0;while(m[i])i++;sys6(SYS_write,1,(long)m,i,0,0,0);}

void _start(void){
  long fd = sys6(SYS_openat, AT_FDCWD, (long)"/stattest", 0, 0,0,0);
  if (fd < 0){ put("open FAIL\n"); sys6(SYS_exit,1,0,0,0,0,0); }
  struct kstat st;
  long r = sys6(SYS_fstat, fd, (long)&st, 0,0,0,0);
  if (r < 0){ put("fstat FAIL\n"); sys6(SYS_exit,2,0,0,0,0,0); }
  if ((st.st_mode & S_IFMT) == S_IFREG && st.st_size > 0) put("stat ok\n");
  else                                                    put("stat FAIL\n");
  sys6(SYS_exit, ((st.st_mode&S_IFMT)==S_IFREG && st.st_size>0)?0:3, 0,0,0,0,0);
}
