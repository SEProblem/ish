#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "emu/memory.h"

addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    return sys_mmap(addr, len, prot, flags, fd_no, offset << PAGE_BITS);
}

static addr_t do_mmap(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    int err;
    pages_t pages = PAGE_ROUND_UP(len);
    page_t page;
    if (addr == 0) {
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE)
            return _ENOMEM;
    } else {
        if (PGOFFSET(addr) != 0)
            return _EINVAL;
        page = PAGE(addr);
    }
    if (flags & MMAP_ANONYMOUS) {
        if (!(flags & MMAP_PRIVATE)) {
            TODO("MMAP_SHARED");
            return _EINVAL;
        }
        if ((err = pt_map_nothing(current->mem, page, pages, prot)) < 0)
            return err;
    } else {
        // fd must be valid
        struct fd *fd = f_get(fd_no);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops->mmap == NULL)
            return _ENODEV;
        if ((err = fd->ops->mmap(fd, current->mem, page, pages, offset, prot, flags)) < 0)
            return err;
    }
    return page << PAGE_BITS;
}

addr_t sys_mmap(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    STRACE("mmap(0x%x, 0x%x, 0x%x, 0x%x, %d, %d)", addr, len, prot, flags, fd_no, offset);
    if (len == 0)
        return _EINVAL;
    if (prot & ~(P_READ | P_WRITE | P_EXEC))
        return _EINVAL;

    write_wrlock(&current->mem->lock);
    addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    write_wrunlock(&current->mem->lock);
    return res;
}

int_t sys_munmap(addr_t addr, uint_t len) {
    STRACE("munmap(0x%x, 0x%x)", addr, len);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    write_wrlock(&current->mem->lock);
    int err = pt_unmap(current->mem, PAGE(addr), PAGE_ROUND_UP(len), 0);
    write_wrunlock(&current->mem->lock);
    if (err < 0)
        return _EINVAL;
    return 0;
}

int_t sys_mprotect(addr_t addr, uint_t len, int_t prot) {
    STRACE("mprotect(0x%x, 0x%x, 0x%x)", addr, len, prot);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (prot & ~(P_READ | P_WRITE | P_EXEC))
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    write_wrlock(&current->mem->lock);
    int err = pt_set_flags(current->mem, PAGE(addr), pages, prot);
    write_wrunlock(&current->mem->lock);
    return err;
}

dword_t sys_madvise(addr_t addr, dword_t len, dword_t advice) {
    // portable applications should not rely on linux's destructive semantics for MADV_DONTNEED.
    return 0;
}

addr_t sys_brk(addr_t new_brk) {
    STRACE("brk(0x%x)", new_brk);
    struct mem *mem = current->mem;

    if (new_brk != 0 && new_brk < mem->start_brk)
        return _EINVAL;
    write_wrlock(&mem->lock);
    addr_t old_brk = mem->brk;
    if (new_brk == 0) {
        write_wrunlock(&mem->lock);
        return old_brk;
    }
    // TODO check for not going too high

    if (new_brk > old_brk) {
        // expand heap: map region from old_brk to new_brk
        int err = pt_map_nothing(mem, PAGE_ROUND_UP(old_brk),
                PAGE_ROUND_UP(new_brk) - PAGE_ROUND_UP(old_brk), P_WRITE);
        if (err < 0) {
            write_wrunlock(&mem->lock);
            return err;
        }
    } else if (new_brk < old_brk) {
        // shrink heap: unmap region from new_brk to old_brk
        // first page to unmap is PAGE(new_brk)
        // last page to unmap is PAGE(old_brk)
        pt_unmap(mem, PAGE(new_brk), PAGE(old_brk), PT_FORCE);
    }

    mem->brk = new_brk;
    write_wrunlock(&mem->lock);
    return new_brk;
}
