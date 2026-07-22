/* freestanding: mmap a page, write into it, write() it to stdout, munmap, exit */
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
#define SYS_mmap 222
#define SYS_munmap 215
#define SYS_exit 93
void _start(void){
  /* mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0) */
  char *p = (char*)sys6(SYS_mmap, 0, 4096, 0x3, 0x22, -1, 0);
  if ((long)p < 0 && (long)p > -4096) { sys6(SYS_exit,1,0,0,0,0,0); }
  const char msg[] = "mmap+munmap ok\n";
  int i; for(i=0;msg[i];i++) p[i]=msg[i];
  sys6(SYS_write, 1, (long)p, i, 0,0,0);
  sys6(SYS_munmap, (long)p, 4096, 0,0,0,0);
  sys6(SYS_exit, 0, 0,0,0,0,0);
}
