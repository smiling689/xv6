//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

#ifdef LAB_MMAP
// 判断地址区间是否重叠
static int
vmaoverlap(uint64 a0, uint64 a1, struct vma *v)
{
  // 当前 VMA 区间
  uint64 b0 = v->addr;
  uint64 b1 = v->addr + v->length;
  return a0 < b1 && b0 < a1;
}

// 按虚拟地址查 VMA
static struct vma*
findvma(struct proc *p, uint64 va)
{
  // 顺序扫描固定表
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].valid && va >= p->vmas[i].addr &&
       va < p->vmas[i].addr + p->vmas[i].length)
      return &p->vmas[i];
  }
  return 0;
}

// MAP_SHARED 页写回文件
static int
mmapwriteback(struct proc *p, struct vma *v, uint64 addr, uint64 length)
{
  pte_t *pte;
  uint64 a, end, pa, off;
  int max, n, n1, r;

  // private 映射不用回写
  if((v->flags & MAP_SHARED) == 0)
    return 0;

  // 每次事务可写的最大大小
  max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  end = addr + length;

  // 逐页检查已加载页
  for(a = addr; a < end; a += PGSIZE){
    // 懒加载未访问的页直接跳过
    if((pte = walk(p->pagetable, a, 0)) == 0 || (*pte & PTE_V) == 0)
      continue;

    // 计算文件偏移
    pa = PTE2PA(*pte);
    off = v->offset + (a - v->addr);
    n = PGSIZE;
    if(a + n > end)
      n = end - a;

    // 分段写回文件
    for(int i = 0; i < n; i += r){
      n1 = n - i;
      if(n1 > max)
        n1 = max;
      begin_op();
      ilock(v->file->ip);
      r = writei(v->file->ip, 0, pa + i, off + i, n1);
      iunlock(v->file->ip);
      end_op();
      if(r != n1)
        return -1;
    }
  }
  return 0;
}

// mmap 专用 unmap，允许页不存在
static void
mmapuvmunmap(pagetable_t pagetable, uint64 addr, uint64 length)
{
  pte_t *pte;
  uint64 a, end;

  end = addr + length;

  // 逐页取消真实映射
  for(a = addr; a < end; a += PGSIZE){
    // 没建页表项就跳过
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;
    // mmap lazy 页可能还没加载
    if((*pte & PTE_V) == 0)
      continue;
    uvmunmap(pagetable, a, 1, 1);
  }
}

// 取消一个 VMA 的一段
static int
mmapunmapvma(struct proc *p, struct vma *v, uint64 addr, uint64 length)
{
  uint64 oldaddr, oldend, end;

  // 参数基本检查
  if((addr % PGSIZE) != 0 || length == 0)
    return -1;

  // 对齐取消范围
  length = PGROUNDUP(length);
  oldaddr = v->addr;
  oldend = v->addr + v->length;
  end = addr + length;

  // 必须落在 VMA 内
  if(addr < oldaddr || end > oldend)
    return -1;
  // 实验保证不从中间挖洞
  if(addr != oldaddr && end != oldend)
    return -1;

  // shared 页先写回
  if(mmapwriteback(p, v, addr, length) < 0)
    return -1;

  // 再释放页表映射
  mmapuvmunmap(p->pagetable, addr, length);

  // 整段取消
  if(addr == oldaddr && end == oldend){
    fileclose(v->file);
    memset(v, 0, sizeof(*v));
  // 取消头部
  } else if(addr == oldaddr){
    v->addr = end;
    v->offset += length;
    v->length = oldend - end;
  // 取消尾部
  } else {
    v->length = addr - oldaddr;
  }
  return 0;
}

// munmap 公共入口
int
mmapunmap(uint64 addr, uint64 length)
{
  struct proc *p = myproc();

  // 按起始地址找 VMA
  struct vma *v = findvma(p, addr);

  if(v == 0)
    return -1;

  // 复用统一 unmap 逻辑
  return mmapunmapvma(p, v, addr, length);
}

// 进程退出时关闭所有 mmap
void
mmapclose(struct proc *p)
{
  // exit 时清理所有 VMA
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].valid)
      mmapunmapvma(p, &p->vmas[i], p->vmas[i].addr, p->vmas[i].length);
  }
}

// mmap lazy page fault
int
mmapfault(uint64 va, int write)
{
  char *mem;
  int perm;
  uint64 a, offset;
  struct proc *p = myproc();
  struct vma *v = findvma(p, va);

  // 必须命中某个 VMA
  if(v == 0)
    return -1;
  // 写缺页需要写权限
  if(write && (v->prot & PROT_WRITE) == 0)
    return -1;
  // 读缺页需要可读或可写
  if(!write && (v->prot & (PROT_READ | PROT_WRITE)) == 0)
    return -1;

  // 按页对齐 fault 地址
  a = PGROUNDDOWN(va);
  // 已经映射则不是 mmap lazy fault
  if(walkaddr(p->pagetable, a) != 0)
    return -1;

  // 分配一页物理内存
  if((mem = kalloc()) == 0)
    return -1;
  memset(mem, 0, PGSIZE);

  // 从文件读入对应页
  offset = v->offset + (a - v->addr);
  ilock(v->file->ip);
  readi(v->file->ip, 0, (uint64)mem, offset, PGSIZE);
  iunlock(v->file->ip);

  // 根据 prot 生成 PTE 权限
  perm = PTE_U;
  if(v->prot & PROT_READ)
    perm |= PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_R | PTE_W;
  if(v->prot & PROT_EXEC)
    perm |= PTE_X;

  // 建立用户页映射
  if(mappages(p->pagetable, a, PGSIZE, (uint64)mem, perm) < 0){
    kfree(mem);
    return -1;
  }
  return 0;
}

// mmap 系统调用
uint64
sys_mmap(void)
{
  int prot, flags;
  uint64 addr, length, offset, mapaddr;
  struct file *f;
  struct proc *p = myproc();
  struct vma *freevma = 0;

  // 取 mmap 参数
  argaddr(0, &addr);
  argaddr(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argaddr(5, &offset);
  if(argfd(4, 0, &f) < 0)
    return -1;

  // 只支持实验要求的文件映射
  if(addr != 0 || length == 0 || f->type != FD_INODE)
    return -1;
  // prot 合法性检查
  if((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return -1;
  // flags 合法性检查
  if(flags != MAP_SHARED && flags != MAP_PRIVATE)
    return -1;
  // mmap 需要可读文件
  if(f->readable == 0)
    return -1;
  // shared writable 需要文件可写
  if((flags & MAP_SHARED) && (prot & PROT_WRITE) && f->writable == 0)
    return -1;

  // VMA 长度按页对齐
  length = PGROUNDUP(length);

  // 找空闲 VMA 槽
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].valid == 0){
      freevma = &p->vmas[i];
      break;
    }
  }
  if(freevma == 0)
    return -1;

  // 从高地址向下找空洞
  mapaddr = TRAPFRAME - length;
  for(;;){
    int overlap = 0;

    // 避开已有 VMA
    for(int i = 0; i < NVMA; i++){
      if(p->vmas[i].valid && vmaoverlap(mapaddr, mapaddr + length, &p->vmas[i])){
        if(p->vmas[i].addr < length)
          return -1;
        mapaddr = p->vmas[i].addr - length;
        overlap = 1;
        break;
      }
    }
    if(overlap == 0)
      break;
  }

  // 不和普通用户内存冲突
  if(mapaddr < p->sz || mapaddr + length > TRAPFRAME)
    return -1;

  // 记录 VMA，不分配物理页
  freevma->valid = 1;
  freevma->addr = mapaddr;
  freevma->length = length;
  freevma->prot = prot;
  freevma->flags = flags;
  freevma->offset = offset;
  freevma->file = filedup(f);

  // 返回映射起始地址
  return mapaddr;
}

// munmap 系统调用
uint64
sys_munmap(void)
{
  uint64 addr, length;

  // 取 munmap 参数
  argaddr(0, &addr);
  argaddr(1, &length);

  // 交给统一 unmap 逻辑
  return mmapunmap(addr, length);
}
#endif

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    // 默认跟随符号链接
    if(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)){
      // 最多跟随 10 层
      for(int i = 0; i < 10; i++){
        // 读出目标路径
        int len = readi(ip, 0, (uint64)path, 0, MAXPATH);
        if(len <= 0){
          iunlockput(ip);
          end_op();
          return -1;
        }
        path[MAXPATH-1] = 0;
        iunlockput(ip);
        // 打开目标 inode
        if((ip = namei(path)) == 0){
          end_op();
          return -1;
        }
        ilock(ip);
        // 找到普通文件就停止
        if(ip->type != T_SYMLINK)
          break;
      }
      // 仍然是链接，认为成环或太深
      if(ip->type == T_SYMLINK){
        iunlockput(ip);
        end_op();
        return -1;
      }
    }
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;
  int len;

  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();
  // 创建链接 inode
  if((ip = create(path, T_SYMLINK, 0, 0)) == 0){
    end_op();
    return -1;
  }

  // 写入目标路径
  len = strlen(target) + 1;
  if(writei(ip, 0, (uint64)target, 0, len) != len){
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
