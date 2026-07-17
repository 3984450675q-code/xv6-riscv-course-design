//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
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
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
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

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

static struct vma *
findvma(struct proc *p, uint64 va)
{
  for (int i = 0; i < NVMA; i++) {
    struct vma *v = &p->vmas[i];
    if (v->used && va >= v->addr && va < v->addr + v->length)
      return v;
  }
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 addr, length, offset, maplen, top, mapaddr;
  int prot, flags;
  struct file *f;
  struct proc *p = myproc();
  struct vma *slot = 0;

  argaddr(0, &addr);
  argaddr(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argaddr(5, &offset);

  if (argfd(4, 0, &f) < 0 || addr != 0 || length == 0 || offset != 0)
    return -1;
  if ((prot & ~(PROT_READ | PROT_WRITE)) != 0 ||
      (prot & (PROT_READ | PROT_WRITE)) == 0)
    return -1;
  if (flags != MAP_SHARED && flags != MAP_PRIVATE)
    return -1;
  if (f->type != FD_INODE || !f->readable)
    return -1;
  if (flags == MAP_SHARED && (prot & PROT_WRITE) && !f->writable)
    return -1;

  for (int i = 0; i < NVMA; i++) {
    if (!p->vmas[i].used && slot == 0)
      slot = &p->vmas[i];
  }
  if (slot == 0)
    return -1;

  maplen = PGROUNDUP(length);
  if (maplen < length)
    return -1;
  top = vmalimit(p);
  if (top < maplen)
    return -1;
  mapaddr = top - maplen;
  if (mapaddr < PGROUNDUP(p->sz))
    return -1;

  slot->used = 1;
  slot->addr = mapaddr;
  slot->length = maplen;
  slot->prot = prot;
  slot->flags = flags;
  slot->offset = offset;
  slot->file = filedup(f);
  return mapaddr;
}

uint64
mmapfault(pagetable_t pagetable, uint64 va, int read)
{
  int n, perm;
  uint64 mem;
  struct proc *p = myproc();
  struct vma *v;

  if (pagetable != p->pagetable || (v = findvma(p, va)) == 0)
    return 0;
  if ((read && !(v->prot & PROT_READ)) ||
      (!read && !(v->prot & PROT_WRITE)))
    return 0;

  if ((mem = (uint64)kalloc()) == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);

  ilock(v->file->ip);
  n = readi(v->file->ip, 0, mem, v->offset + (va - v->addr), PGSIZE);
  iunlock(v->file->ip);
  if (n < 0) {
    kfree((void *)mem);
    return 0;
  }

  perm = PTE_U;
  if (v->prot & PROT_READ)
    perm |= PTE_R;
  if (v->prot & PROT_WRITE)
    perm |= PTE_R | PTE_W;
  if (mappages(pagetable, va, PGSIZE, mem, perm) < 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

static int
vmawriteback(struct vma *v, uint64 va, pte_t *pte)
{
  int i = 0;
  int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
  uint64 pa = PTE2PA(*pte);
  uint64 off = v->offset + (va - v->addr);

  while (i < PGSIZE) {
    int n = PGSIZE - i;
    if (n > max)
      n = max;

    begin_op();
    ilock(v->file->ip);
    int written = writei(v->file->ip, 0, pa + i, off + i, n);
    iunlock(v->file->ip);
    end_op();
    if (written != n)
      return -1;
    i += written;
  }
  return 0;
}

int
vmaunmap(struct proc *p, uint64 addr, uint64 length)
{
  uint64 start, end, vend;
  int error = 0;
  struct vma *v;

  if (length == 0 || addr % PGSIZE != 0 || addr + length < addr)
    return -1;
  start = addr;
  end = PGROUNDUP(addr + length);
  if (end < addr || (v = findvma(p, start)) == 0)
    return -1;
  vend = v->addr + v->length;
  if (end > vend || (start != v->addr && end != vend))
    return -1;

  for (uint64 va = start; va < end; va += PGSIZE) {
    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte == 0 || (*pte & PTE_V) == 0)
      continue;
    if (v->flags == MAP_SHARED && (v->prot & PROT_WRITE) &&
        (*pte & PTE_D) && vmawriteback(v, va, pte) < 0)
      error = -1;
    uvmunmap(p->pagetable, va, 1, 1);
  }

  if (start == v->addr && end == vend) {
    fileclose(v->file);
    memset(v, 0, sizeof(*v));
  } else if (start == v->addr) {
    uint64 removed = end - start;
    v->addr = end;
    v->length -= removed;
    v->offset += removed;
  } else {
    v->length = start - v->addr;
  }
  return error;
}

uint64
sys_munmap(void)
{
  uint64 addr, length;

  argaddr(0, &addr);
  argaddr(1, &length);
  return vmaunmap(myproc(), addr, length);
}

void
vmafree(struct proc *p)
{
  for (int i = 0; i < NVMA; i++) {
    if (p->vmas[i].used) {
      uint64 addr = p->vmas[i].addr;
      uint64 length = p->vmas[i].length;
      vmaunmap(p, addr, length);
    }
  }
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
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
  if (argfd(0, 0, &f) < 0)
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
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
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
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
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

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
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

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR) {
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

static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) { // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if (type == T_DIR) {
    // now that success is guaranteed:
    dp->nlink++; // for ".."
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
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;
  int len;

  if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = create(path, T_SYMLINK, 0, 0)) == 0) {
    end_op();
    return -1;
  }

  len = strlen(target) + 1;
  if (writei(ip, 0, (uint64)target, 0, len) != len) {
    ip->nlink = 0;
    iupdate(ip);
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();
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
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);

    for (int depth = 0; ip->type == T_SYMLINK && !(omode & O_NOFOLLOW);
         depth++) {
      if (depth >= 10 ||
          readi(ip, 0, (uint64)path, 0, sizeof(path)) <= 0) {
        iunlockput(ip);
        end_op();
        return -1;
      }
      path[MAXPATH - 1] = 0;
      iunlockput(ip);
      if ((ip = namei(path)) == 0) {
        end_op();
        return -1;
      }
      ilock(ip);
    }

    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
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
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
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
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
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
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
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
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) <
        0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_connect(void)
{
  struct file *f;
  uint32 raddr;
  int fd, addr, lport, rport;

  argint(0, &addr);
  argint(1, &lport);
  argint(2, &rport);
  raddr = addr;

  if (sockalloc(&f, raddr, lport, rport) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0) {
    fileclose(f);
    return -1;
  }
  return fd;
}
