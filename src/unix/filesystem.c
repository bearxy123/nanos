#include <unix_internal.h>
#include <filesystem.h>
#include <storage.h>

sysreturn sysreturn_from_fs_status(fs_status s)
{
    switch (s) {
    case FS_STATUS_NOSPACE:
        return -ENOSPC;
    case FS_STATUS_IOERR:
        return -EIO;
    case FS_STATUS_NOENT:
        return -ENOENT;
    case FS_STATUS_EXIST:
        return -EEXIST;
    case FS_STATUS_INVAL:
        return -EINVAL;
    case FS_STATUS_NOTDIR:
        return -ENOTDIR;
    case FS_STATUS_NOMEM:
        return -ENOMEM;
    case FS_STATUS_LINKLOOP:
        return -ELOOP;
    default:
        return 0;
    }
}

sysreturn sysreturn_from_fs_status_value(status s)
{
    if (is_ok(s))
        return 0;
    u64 fss;
    sysreturn rv;

    /* block r/w errors won't include an fs status, so assume I/O error if none found */
    if (get_u64(s, sym(fsstatus), &fss))
        rv = sysreturn_from_fs_status(fss);
    else
        rv = -EIO;
    return rv;
}

/* If the file path being resolved crosses a filesystem boundary (i.e. a mount
 * point), the 'fs' argument (if non-null) is updated to point to the new
 * filesystem. */
// fused buffer wrap, split, and resolve
int resolve_cstring(filesystem *fs, tuple cwd, const char *f, tuple *entry,
                    tuple *parent)
{
    if (!f)
        return -EFAULT;
    return sysreturn_from_fs_status(filesystem_resolve_cstring(fs, cwd, f, entry, parent));
}

/* If the file path being resolved crosses a filesystem boundary (i.e. a mount
 * point), the 'fs' argument (if non-null) is updated to point to the new
 * filesystem. */
int resolve_cstring_follow(filesystem *fs, tuple cwd, const char *f, tuple *entry,
        tuple *parent)
{
    if (!f)
        return -EFAULT;
    return sysreturn_from_fs_status(filesystem_resolve_cstring_follow(fs, cwd, f, entry, parent));
}

void file_readahead(file f, u64 offset, u64 len)
{
    u64 ra_size = 0;
    switch (f->fadv) {
    case POSIX_FADV_NORMAL:
        ra_size = FILE_READAHEAD_DEFAULT;
        break;
    case POSIX_FADV_RANDOM: /* no read-ahead */
        break;
    case POSIX_FADV_SEQUENTIAL:
        ra_size = 2 * FILE_READAHEAD_DEFAULT;
        break;
    }
    if (ra_size > 0)
        pagecache_node_fetch_pages(fsfile_get_cachenode(f->fsf),
            irangel(offset + len, ra_size));
}

fs_status filesystem_chdir(process p, const char *path)
{
    process_lock(p);
    filesystem fs = p->cwd_fs;
    fs_status fss;
    tuple n;
    fss = filesystem_resolve_cstring_follow(&fs, p->cwd, path, &n, 0);
    if (fss != FS_STATUS_OK)
        goto out;
    if (!is_dir(n)) {
        fss = FS_STATUS_NOENT;
        goto out;
    }
    p->cwd_fs = fs;
    p->cwd = n;
  out:
    process_unlock(p);
    return fss;
}

closure_function(4, 1, void, fs_sync_complete,
                 filesystem, fs, pagecache_node, pn, status_handler, sh, boolean, fs_flushed,
                 status, s)
{
    if (is_ok(s) && !bound(fs_flushed)) {
        bound(fs_flushed) = true;
        if (bound(pn))
            pagecache_sync_node(bound(pn), (status_handler)closure_self());
        else
            pagecache_sync_volume(filesystem_get_pagecache_volume(bound(fs)),
                (status_handler)closure_self());
        return;
    }
    apply(bound(sh), s);
    closure_finish();
}

static void filesystem_sync_internal(filesystem fs, pagecache_node pn,
                                     status_handler sh)
{
    status_handler sync_complete = closure(heap_general(get_kernel_heaps()),
        fs_sync_complete, fs, pn, sh, false);
    if (sync_complete == INVALID_ADDRESS) {
        apply(sh, timm("result", "cannot allocate closure"));
        return;
    }
    filesystem_flush(fs, sync_complete);
}

void filesystem_sync(filesystem fs, status_handler sh)
{
    filesystem_sync_internal(fs, 0, sh);
}

void filesystem_sync_node(filesystem fs, pagecache_node pn, status_handler sh)
{
    filesystem_sync_internal(fs, pn, sh);
}

closure_function(2, 2, void, fs_op_complete,
                 thread, t, file, f,
                 fsfile, fsf, fs_status, s)
{
    thread t = bound(t);
    sysreturn ret = sysreturn_from_fs_status(s);
    thread_log(current, "%s: %d", __func__, ret);

    bound(f)->length = fsfile_get_length(fsf);
    fdesc_put(&bound(f)->f);
    syscall_return(t, ret);
    closure_finish();
}

static sysreturn symlink_internal(filesystem fs, tuple cwd, const char *path,
        const char *target)
{
    if (!validate_user_string(path) || !validate_user_string(target)) {
        return set_syscall_error(current, EFAULT);
    }
    tuple parent;
    int ret = resolve_cstring(&fs, cwd, path, 0, &parent);
    if ((ret != -ENOENT) || !parent) {
        return set_syscall_return(current, ret);
    }
    if (filesystem_symlink(fs, parent, filename_from_path(path),
        target))
        return 0;
    else
        return -ENOSPC;
}

sysreturn symlink(const char *target, const char *linkpath)
{
    thread_log(current, "symlink %s -> %s", linkpath, target);
    filesystem cwd_fs;
    tuple cwd;
    process_get_cwd(current->p, &cwd_fs, &cwd);
    return symlink_internal(cwd_fs, cwd, linkpath, target);
}

sysreturn symlinkat(const char *target, int dirfd, const char *linkpath)
{
    thread_log(current, "symlinkat %d %s -> %s", dirfd, linkpath, target);
    filesystem fs;
    tuple cwd = resolve_dir(fs, dirfd, linkpath);
    return symlink_internal(fs, cwd, linkpath, target);
}

static sysreturn utime_internal(const char *filename, timestamp actime,
        timestamp modtime)
{
    tuple t;
    filesystem fs;
    tuple cwd;
    process_get_cwd(current->p, &fs, &cwd);
    int ret = resolve_cstring(&fs, cwd, filename, &t, 0);
    if (ret) {
        return set_syscall_return(current, ret);
    }
    filesystem_set_atime(fs, t, actime);
    filesystem_set_mtime(fs, t, modtime);
    return set_syscall_return(current, 0);
}

sysreturn utime(const char *filename, const struct utimbuf *times)
{
    if (!validate_user_string(filename) ||
        !validate_user_memory(times, sizeof(struct utimbuf), false))
        return set_syscall_error(current, EFAULT);
    timestamp atime = times ? seconds(times->actime) : now(CLOCK_ID_REALTIME);
    timestamp mtime = times ? seconds(times->modtime) : now(CLOCK_ID_REALTIME);
    return utime_internal(filename, atime, mtime);
}

sysreturn utimes(const char *filename, const struct timeval times[2])
{
    if (!validate_user_string(filename) ||
        !validate_user_memory(times, 2 * sizeof(struct timeval), false))
        return set_syscall_error(current, EFAULT);
    /* Sub-second precision is not supported. */
    timestamp atime =
            times ? time_from_timeval(&times[0]) : now(CLOCK_ID_REALTIME);
    timestamp mtime =
            times ? time_from_timeval(&times[1]) : now(CLOCK_ID_REALTIME);
    return utime_internal(filename, atime, mtime);
}

static sysreturn statfs_internal(filesystem fs, tuple t, struct statfs *buf)
{
    if (!buf) {
        return set_syscall_error(current, EFAULT);
    }
    runtime_memset((u8 *) buf, 0, sizeof(*buf));
    if (fs) {
        buf->f_bsize = fs_blocksize(fs);
        buf->f_blocks = fs_totalblocks(fs);
        buf->f_bfree = buf->f_bavail = fs_freeblocks(fs);
    } else {
        buf->f_bsize = PAGESIZE;
    }
    buf->f_frsize = buf->f_bsize;
    u64 id = u64_from_pointer(t);
    buf->f_fsid.val[0] = (int) id;
    buf->f_fsid.val[1] = (int) (id >> 32);
    buf->f_namelen = NAME_MAX;
    return set_syscall_return(current, 0);
}

sysreturn statfs(const char *path, struct statfs *buf)
{
    if (!validate_user_string(path) ||
        !validate_user_memory(buf, sizeof(struct statfs), true))
        return set_syscall_error(current, EFAULT);
    filesystem fs;
    tuple cwd;
    process_get_cwd(current->p, &fs, &cwd);
    tuple t;
    int ret = resolve_cstring(&fs, cwd, path, &t, 0);
    if (ret) {
        return set_syscall_return(current, ret);
    }
    return statfs_internal(fs, t, buf);
}

sysreturn fstatfs(int fd, struct statfs *buf)
{
    fdesc desc = resolve_fd(current->p, fd);
    file f;
    switch (desc->type) {
    case FDESC_TYPE_REGULAR:
    case FDESC_TYPE_DIRECTORY:
    case FDESC_TYPE_SYMLINK:
        f = (file) desc;
        break;
    default:
        f = 0;
        break;
    }
    sysreturn rv = statfs_internal(f ? f->fs : 0, f ? file_get_meta(f) : 0, buf);
    fdesc_put(desc);
    return rv;
}

sysreturn fallocate(int fd, int mode, long offset, long len)
{
    fdesc desc = resolve_fd(current->p, fd);
    sysreturn rv;
    if (desc->type != FDESC_TYPE_REGULAR) {
        switch (desc->type) {
        case FDESC_TYPE_PIPE:
        case FDESC_TYPE_STDIO:
            rv = -ESPIPE;
            break;
        default:
            rv = -ENODEV;
        }
        goto out;
    } else if (!fdesc_is_writable(desc)) {
        rv = -EBADF;
        goto out;
    }

    heap h = heap_general(get_kernel_heaps());
    file f = (file) desc;
    switch (mode) {
    case 0:
    case FALLOC_FL_KEEP_SIZE:
        filesystem_alloc(f->fsf, offset, len,
                mode == FALLOC_FL_KEEP_SIZE,
                closure(h, fs_op_complete, current, f));
        break;
    case FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE:
        filesystem_dealloc(f->fsf, offset, len,
                closure(h, fs_op_complete, current, f));
        break;
    default:
        rv = -EINVAL;
        goto out;
    }
    return thread_maybe_sleep_uninterruptible(current);
  out:
    fdesc_put(desc);
    return rv;
}

sysreturn fadvise64(int fd, s64 off, u64 len, int advice)
{
    fdesc desc = resolve_fd(current->p, fd);
    sysreturn rv;
    if (desc->type != FDESC_TYPE_REGULAR) {
        switch (desc->type) {
        case FDESC_TYPE_PIPE:
        case FDESC_TYPE_STDIO:
            rv = -ESPIPE;
            break;
        default:
            rv = -EBADF;
        }
        goto out;
    }
    file f = (file)desc;
    switch (advice) {
    case POSIX_FADV_NORMAL:
    case POSIX_FADV_RANDOM:
    case POSIX_FADV_SEQUENTIAL:
        f->fadv = advice;
        break;
    case POSIX_FADV_WILLNEED: {
        pagecache_node pn = fsfile_get_cachenode(f->fsf);
        range r = (len != 0) ? irangel(off, len) :
                irange(off, pagecache_get_node_length(pn));
        pagecache_node_fetch_pages(pn, r);
        break;
    }
    case POSIX_FADV_DONTNEED:
    case POSIX_FADV_NOREUSE:
        break;
    default:
        rv = -EINVAL;
        goto out;
    }
    rv = 0;
  out:
    fdesc_put(desc);
    return rv;
}

void file_release(file f)
{
    release_fdesc(&f->f);
    unix_cache_free(get_unix_heaps(), file, f);
}
KLIB_EXPORT(file_release);

/* file_path is treated as an absolute path. */
fsfile fsfile_open_or_create(buffer file_path)
{
    tuple file, parent;
    filesystem fs = get_root_fs();
    tuple root = filesystem_getroot(fs);
    char *file_str = buffer_to_cstring(file_path);
    int ret = resolve_cstring(0, root, file_str, &file, &parent);
    if (ret == -ENOENT) {
        if (!parent) {
            int separator = buffer_strrchr(file_path, '/');
            file_str[separator] = '\0';
            fs_status s = filesystem_mkdirpath(fs, 0, file_str, true);
            if (s != FS_STATUS_OK)
                return 0;
            file_str[separator] = '/';
            resolve_cstring(0, root, file_str, 0, &parent);
        }
        file = filesystem_creat(fs, parent, filename_from_path(file_str));
    }
    return file ? fsfile_from_node(fs, file) : 0;
}
KLIB_EXPORT(fsfile_open_or_create);

/* Can be used for files in the root filesystem only. */
fs_status fsfile_truncate(fsfile f, u64 len)
{
    return (filesystem_truncate(get_root_fs(), f, len));
}
KLIB_EXPORT(fsfile_truncate);
