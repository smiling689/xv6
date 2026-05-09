#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
#ifdef LAB_TRAPS
  backtrace();
#endif
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
uint64
sys_pgaccess(void)
{
  // 解析参数
  uint64 start, dst;
  int len;
  unsigned int mask = 0;
  struct proc *p = myproc();

  argaddr(0, &start);
  argint(1, &len);
  argaddr(2, &dst);

  // 限制 bitmask 大小
  if(len < 0 || len > 32)
    return -1;

  // 扫描每个页面
  for(int i = 0; i < len; i++){
    pte_t *pte = walk(p->pagetable, start + i * PGSIZE, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;

    // 记录并清除访问位
    if(*pte & PTE_A){
      mask |= (1U << i);
      *pte &= ~PTE_A;
    }
  }

  // 返回访问掩码
  if(copyout(p->pagetable, dst, (char *)&mask, sizeof(mask)) < 0)
    return -1;

  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
    int mask;
    // argint函数用于获取系统调用的第一个参数，并将其存储在mask变量中
    argint(0, &mask);
    myproc()->trace_mask = mask;
    return 0;
}

uint64
sys_sysinfo(void)
{
    struct sysinfo info;
    info.freemem = freemem();
    info.nproc = nproc();
    uint64 addr;
    argaddr(0, &addr);
    if (copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info)) < 0) {
        return -1;
    }
    return 0;
}

uint64
sys_sigalarm(void)
{
  // 从参数中取出interval和handler
  int interval;
  uint64 handler;
  struct proc *p = myproc();

  argint(0, &interval);
  argaddr(1, &handler);

  if(interval < 0)
    return -1;

  // 存到proc中
  p->alarm_interval = interval;
  p->alarm_handler = handler;
  // 计时器清零
  p->alarm_ticks = 0;

  return 0;
}

uint64
sys_sigreturn(void)
{
  // 先把a0保存下来，因为之后会修改trapframe
  struct proc *p = myproc();
  uint64 a0 = p->alarm_tf.a0;

  // 把 alarm_tf 中的寄存器恢复到 trapframe 中
  *(p->trapframe) = p->alarm_tf;
  p->alarm_handler_running = 0;
  p->alarm_ticks = 0;

  // 返回保存的 a0
  return a0;
}
