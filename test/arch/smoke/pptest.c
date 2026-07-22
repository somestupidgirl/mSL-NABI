/* freestanding: exercise ppoll (aarch64 73) over a self-pipe. First the timeout
 * path (nothing ready, short relative timespec -> 0), then the ready path (write
 * a byte, ppoll reports POLLIN). See PORTING-arm64.md 3.2. */
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
#define SYS_ppoll 73
#define SYS_pipe2 59
#define POLLIN 0x1
struct pollfd { int fd; short events, revents; };
struct ts { long sec; long nsec; };
static void put(const char*m){int i=0;while(m[i])i++;sys6(SYS_write,1,(long)m,i,0,0,0);}
void _start(void){
  int ok=1, fds[2];
  if (sys6(SYS_pipe2,(long)fds,0,0,0,0,0) < 0){ put("pipe FAIL\n"); sys6(SYS_exit,1,0,0,0,0,0); }

  struct pollfd p = { fds[0], POLLIN, 0 };
  struct ts t = { 0, 10*1000*1000 };
  long r = sys6(SYS_ppoll, (long)&p, 1, (long)&t, 0, 0, 0);
  if (r != 0){ put("ppoll timeout FAIL\n"); ok=0; } else put("ppoll timeout ok\n");

  char c = 'x'; sys6(SYS_write, fds[1], (long)&c, 1, 0,0,0);
  p.revents = 0;
  r = sys6(SYS_ppoll, (long)&p, 1, 0, 0, 0, 0);
  if (r != 1 || !(p.revents & POLLIN)){ put("ppoll ready FAIL\n"); ok=0; } else put("ppoll ready ok\n");

  sys6(SYS_exit, ok?0:1, 0,0,0,0,0);
}
