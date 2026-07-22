/* freestanding: fork (clone with SIGCHLD), the child exits with a known code,
 * the parent wait4()s it and checks the status. Exercises the whole fork path:
 * vCPU snapshot, hv_vm_destroy, host fork, and reentry (VM rebuild + stage-2
 * replay + vCPU restore) on both sides. See PORTING-arm64.md 3.x / Phase 4. */
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
static void put(const char*m){int i=0;while(m[i])i++;sys6(SYS_write,1,(long)m,i,0,0,0);}
void _start(void){
  long pid = sys6(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);   /* fork */
  if (pid == 0) {                 /* child */
    put("child\n");
    sys6(SYS_exit, 7, 0,0,0,0,0);
  }
  if (pid < 0) { put("fork FAIL\n"); sys6(SYS_exit, 1, 0,0,0,0,0); }
  long status = 0;
  sys6(SYS_wait4, -1, (long)&status, 0, 0, 0, 0);
  put("parent\n");
  int code = (status >> 8) & 0xff;                       /* WEXITSTATUS */
  sys6(SYS_exit, code==7 ? 0 : 2, 0,0,0,0,0);
}
