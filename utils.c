#include "jailbreak.h"
#include "kernelrw.h"
#include "utils.h"

void jbc_run_as_root(void(*fn)(void* arg), void* arg, int cwd_mode)
{
    struct jbc_cred cred;
    jbc_get_cred(&cred);
    struct jbc_cred root_cred = cred;
    jbc_jailbreak_cred(&root_cred);
    switch(cwd_mode)
    {
    case CWD_KEEP:
    default:
        root_cred.cdir = cred.cdir;
        break;
    case CWD_ROOT:
        root_cred.cdir = cred.rdir;
        break;
    case CWD_RESET:
        root_cred.cdir = root_cred.rdir;
        break;
    }
    jbc_set_cred(&root_cred);
    fn(arg);
    jbc_set_cred(&cred);
}

enum { EINVAL = 22 };

#define SYSCALL(nr, fn) __attribute__((naked)) fn\
{\
    asm volatile("mov $" #nr ", %rax\nmov %rcx, %r10\nsyscall\nret");\
}

struct iovec
{
    const void* ptr;
    size_t size;
};

SYSCALL(12, static int chdir(char* path))
SYSCALL(22, static int unmount(const char* path, int flags))
SYSCALL(136, static int mkdir(const char* path, int mode))
SYSCALL(137, static int rmdir(const char* path))
SYSCALL(326, static int getcwd(char* buf, size_t sz))
SYSCALL(378, static int nmount(struct iovec* iov, unsigned int niov, int flags))

#define MAX_PATH 1024

struct mount_in_sandbox_param
{
    const char* path;
    const char* name;
    int ans;
};

static void do_mount_in_sandbox(void* op)
{
    struct mount_in_sandbox_param* p = op;
    char path[MAX_PATH+1];
    path[MAX_PATH] = 0;
    int error;
    if((error = getcwd(path, MAX_PATH)))
        goto err_out;
    size_t l1 = 0;
    while(path[l1])
        l1++;
    size_t l2 = 0;
    while(p->name[l2])
        l2++;
    if(l1 + l2 + 1 >= MAX_PATH)
        goto invalid;
    path[l1] = '/';
    int dots = 1;
    for(size_t i = 0; i <= l2; i++)
    {
        if(p->name[i] == '/')
            goto invalid;
        else if(p->name[i] != '.')
            dots = 0;
        path[l1+1+i] = p->name[i];
    }
    if(dots && l2 <= 2)
        goto invalid;
    if(p->path) //mount operation
    {
        if((error = mkdir(path, 0777)))
            goto err_out;
        size_t l3 = 0;
        while(p->path[l3])
            l3++;
        struct iovec data[6] = {
            {"fstype", 7}, {"nullfs", 7},
            {"fspath", 7}, {path, l1+l2+2},
            {"target", 7}, {p->path, l3+1},
        };
        if((error = nmount(data, 6, 0)))
        {
            rmdir(path);
            goto err_out;
        }
    }
    else //unmount operation
    {
        if((error = unmount(path, 0)))
            goto err_out;
        if((error = rmdir(path)))
            goto err_out;
    }
    return;
invalid:
    error = EINVAL;
err_out:
    p->ans = error;
}

int jbc_mount_in_sandbox(const char* system_path, const char* mnt_name)
{
    struct mount_in_sandbox_param op = {
        .path = system_path,
        .name = mnt_name,
        .ans = 0,
    };
    jbc_run_as_root(do_mount_in_sandbox, &op, CWD_ROOT);
    return op.ans;
}

int jbc_unmount_in_sandbox(const char* mnt_name)
{
    return jbc_mount_in_sandbox(0, mnt_name);
}

int jbc_set_proc_name(const char* New_Name)
{
    uintptr_t td = jbc_krw_get_td();
    uintptr_t proc = jbc_krw_read64(td + 8, KERNEL_HEAP);

    return jbc_krw_memcpy(proc + 0x44C, (uintptr_t)New_Name, 0x20, KERNEL_HEAP);
}

int jbc_get_proc_libraries(struct LibraryInfo* out, int maxCount)
{
    uintptr_t td = jbc_krw_get_td();
    uintptr_t proc = jbc_krw_read64(td + 8, KERNEL_HEAP);
    uintptr_t dynlib = jbc_krw_read64(proc + 0x340, KERNEL_HEAP);
    uint32_t objCount = jbc_krw_read32(dynlib + 0x28, KERNEL_HEAP);
    uintptr_t objs = jbc_krw_read64(dynlib, KERNEL_HEAP);

    int objCopied = 0;
    while (objs != 0)
    {
        if (objCopied > maxCount)
            break;

        uintptr_t pathPtr = jbc_krw_read64(objs + 0x8, KERNEL_HEAP);
        jbc_krw_memcpy((uintptr_t)out[objCopied].Path, pathPtr, 256, KERNEL_HEAP);
        out[objCopied].ModuleHandle = jbc_krw_read64(objs + 0x28, KERNEL_HEAP);
        out[objCopied].map_base = jbc_krw_read64(objs + 0x30, KERNEL_HEAP);
        out[objCopied].map_size = jbc_krw_read64(objs + 0x38, KERNEL_HEAP);
        out[objCopied].text_size = jbc_krw_read64(objs + 0x40, KERNEL_HEAP);
        out[objCopied].data_base = jbc_krw_read64(objs + 0x48, KERNEL_HEAP);
        out[objCopied].data_size = jbc_krw_read64(objs + 0x50, KERNEL_HEAP);

        objs = jbc_krw_read64(objs, KERNEL_HEAP); // next obj
        objCopied++;
    }

    return objCount;
}
