/* freestanding: install SIGUSR1 handler, raise it, verify handler ran + resume */
typedef unsigned long ulong;
static long sys6(long n,long a,long b,long c,long d,long e,long f){
  register long x8 asm("x8")=n;
  register long x0 asm("x0")=a; register long x1 asm("x1")=b;
  register long x2 asm("x2")=c; register long x3 asm("x3")=d;
  register long x4 asm("x4")=e; register long x5 asm("x5")=f;
  asm volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");
  return x0;
}
#define SYS_write 64
#define SYS_exit 93
#define SYS_rt_sigaction 134
#define SYS_getpid 172
#define SYS_kill 129
#define SIGUSR1 10

static volatile long caught = 0;
static void handler(int s){ caught = s; }

static void put(const char *m){ int i=0; while(m[i])i++; sys6(SYS_write,1,(long)m,i,0,0,0); }

void _start(void){
  long act[4];
  act[0]=(long)handler; /* sa_handler */
  act[1]=0;             /* sa_flags   */
  act[2]=0;             /* sa_restorer (unused on aarch64) */
  act[3]=0;             /* sa_mask    */
  sys6(SYS_rt_sigaction, SIGUSR1, (long)act, 0, 8, 0, 0);
  long pid = sys6(SYS_getpid, 0,0,0,0,0,0);
  sys6(SYS_kill, pid, SIGUSR1, 0,0,0,0);
  /* execution resumes here after the handler + sigreturn */
  if (caught == SIGUSR1) put("signal ok\n");
  else                   put("signal FAIL\n");
  sys6(SYS_exit, caught==SIGUSR1?0:1, 0,0,0,0,0);
}
