/* freestanding: mimic glibc's fork() clone flags on aarch64 -
 * clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD, stack=0, parent_tid=0,
 * tls=0 [x3], child_tid=&ctid [x4]). The child's tid must land in ctid, which
 * only happens if the last two clone args are read in aarch64 (CLONE_BACKWARDS)
 * order rather than x86-64's. See src/proc/fork.c. */
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
#define SYS_clone 220
#define SYS_wait4 260
#define SIGCHLD 17
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
static int ctid;
static void put(const char*m){int i=0;while(m[i])i++;sys6(SYS_write,1,(long)m,i,0,0,0);}
void _start(void){
  long flags = CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD;
  long pid = sys6(SYS_clone, flags, 0, 0, 0 /*tls*/, (long)&ctid /*child_tid*/, 0);
  if (pid == 0) {                 /* child */
    if (ctid != 0) put("settid ok\n"); else put("settid FAIL\n");
    sys6(SYS_exit, ctid != 0 ? 0 : 3, 0,0,0,0,0);
  }
  if (pid < 0) { put("clone FAIL\n"); sys6(SYS_exit, 1, 0,0,0,0,0); }
  long status = 0;
  sys6(SYS_wait4, -1, (long)&status, 0, 0, 0, 0);
  sys6(SYS_exit, (status>>8)&0xff, 0,0,0,0,0);
}
