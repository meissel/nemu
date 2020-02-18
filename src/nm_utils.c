#include <nm_core.h>
#include <nm_utils.h>
#include <nm_vector.h>
#include <nm_ncurses.h>
#include <nm_vm_control.h>

#include <sys/wait.h>
#include <sys/socket.h>

enum {
    NM_BLKSIZE      = 131072, /* 128KiB */
    NM_SOCK_READLEN = 1024,
};

#if defined (NM_OS_LINUX) && defined (NM_WITH_SENDFILE)
# include <sys/sendfile.h>
#endif

#if defined (NM_OS_LINUX) && defined (NM_WITH_SENDFILE)
static void nm_copy_file_sendfile(int in_fd, int out_fd);
#else
static void nm_copy_file_default(int in_fd, int out_fd);
#endif

void nm_bug(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);

    nm_curses_deinit();

    vfprintf(stderr, fmt, args);
    putc('\n', stderr);
    va_end(args);

    exit(NM_ERR);
}

void *nm_alloc(size_t size)
{
    void *p;

    if ((p = malloc(size)) == NULL)
        nm_bug("malloc: %s\n", strerror(errno));

    return p;
}

void *nm_calloc(size_t nmemb, size_t size)
{
    void *p;

    if ((p = calloc(nmemb, size)) == NULL)
        nm_bug("cmalloc: %s\n", strerror(errno));

    return p;
}

void *nm_realloc(void *p, size_t size)
{
    void *p_new;

    if ((p_new = realloc(p, size)) == NULL)
        nm_bug("realloc: %s\n", strerror(errno));

    return p_new;
}

void nm_map_file(nm_file_map_t *file)
{
    struct stat stat;

    if ((file->fd = open(file->name->data, O_RDONLY)) == -1)
        nm_bug(_("Cannot open file %s:%s"),
               file->name->data, strerror(errno));

    if (fstat(file->fd, &stat) == -1) {
        close(file->fd);
        nm_bug(_("Cannot get file info %s:%s"),
               file->name->data, strerror(errno));
    }

    file->size = stat.st_size;
    file->mp = mmap(0, file->size, PROT_READ, MAP_PRIVATE, file->fd, 0);

    if (file->mp == MAP_FAILED) {
        close(file->fd);
        nm_bug(_("%s: cannot map file %s:%s"),
               __func__, file->name->data, strerror(errno));
    }
}

void nm_unmap_file(const nm_file_map_t *file)
{
    munmap(file->mp, file->size);
    close(file->fd);
}

void nm_copy_file(const nm_str_t *src, const nm_str_t *dst)
{
    int in_fd, out_fd;

    if ((in_fd = open(src->data, O_RDONLY)) == -1)
        nm_bug("%s: cannot open file %s: %s",
               __func__, src->data, strerror(errno));

    if ((out_fd = open(dst->data, O_WRONLY | O_CREAT | O_EXCL, 0644)) == -1) {
        close(in_fd);
        nm_bug("%s: cannot open file %s: %s",
               __func__, dst->data, strerror(errno));
    }

#if defined (NM_OS_LINUX) && defined (NM_WITH_SENDFILE)
    nm_copy_file_sendfile(in_fd, out_fd);
#else
    nm_copy_file_default(in_fd, out_fd);
#endif

    close(in_fd);
    close(out_fd);
}

#if defined (NM_OS_LINUX) && defined (NM_WITH_SENDFILE)
static void nm_copy_file_sendfile(int in_fd, int out_fd)
{
    off_t offset = 0;
    struct stat file_info;
    int rc;

    memset(&file_info, 0, sizeof(file_info));

    if (fstat(in_fd, &file_info) != 0)
        nm_bug("%s: cannot get file info %d: %s", __func__, in_fd,
               strerror(errno));

    while (offset < file_info.st_size) {
        if ((rc = sendfile(out_fd, in_fd, &offset, file_info.st_size)) == -1)
            nm_bug("%s: cannot copy file: %s", __func__, strerror(errno));

        if (rc == 0)
            break;
    }

    if (offset != file_info.st_size)
        nm_bug("%s: incomplete transfer from sendfile", __func__);
}
#else
static void nm_copy_file_default(int in_fd, int out_fd)
{
    char *buf = nm_alloc(NM_BLKSIZE);
    ssize_t nread;

    posix_fadvise(in_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    while ((nread = read(in_fd, buf, NM_BLKSIZE)) > 0) {
        ssize_t nwrite;
        char *bufsp = buf;

        do {
            nwrite = write(out_fd, bufsp, NM_BLKSIZE);

            if (nwrite >= 0) {
                nread -= nwrite;
                bufsp += nwrite;
            } else if (errno != EINTR) {
                nm_bug("%s: copy file failed: %s", __func__, strerror(errno));
            }
        } while (nread > 0);
    }

    if (nread != 0)
        nm_bug("%s: copy was not compleete.", __func__);

    free(buf);
}
#endif

int nm_spawn_process(const nm_vect_t *argv, nm_str_t *answer)
{
    int rc = NM_OK;
    int fd[2];
    pid_t child_pid = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == -1)
        nm_bug("%s: error create socketpair: %s", __func__, strerror(errno));

    switch (child_pid = fork()) {
    case (-1):  /* error*/
        nm_bug("%s: fork: %s", __func__, strerror(errno));
        break;

    case (0):   /* child */
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);

        execvp(((char *const *)argv->data)[0], (char *const *)argv->data);
        nm_bug("%s: unreachable reached", __func__);
        break;

    default:    /* parent */
    {
        int wstatus = 0;
        pid_t w_rc;
        char buf[NM_SOCK_READLEN] = { 0 };

        close(fd[1]);
        w_rc = waitpid(child_pid, &wstatus, 0);

        if ((w_rc == child_pid) && (WEXITSTATUS(wstatus) != 0)) {
            nm_str_t err_msg = NM_INIT_STR;
            while (read(fd[0], buf, sizeof(buf) - 1) > 0) {
                nm_str_add_text(&err_msg, buf);
                memset(&buf, 0, sizeof(buf));
            }
            nm_vmctl_log_last(&err_msg);
            nm_debug("exec_error: %s", err_msg.data);
            nm_str_free(&err_msg);
            rc = NM_ERR;
        } else if (answer && (w_rc == child_pid) &&
                   (WEXITSTATUS(wstatus) == 0)) {
            while (read(fd[0], buf, sizeof(buf) - 1) > 0) {
                nm_str_add_text(answer, buf);
                memset(buf, 0, sizeof(buf));
            }
        }

        close(fd[0]);
    }
    }

    return rc;
}

void nm_debug(const char *fmt, ...)
{
#ifdef NM_DEBUG
    va_list args;
    FILE *fp;

    if ((fp = fopen("/tmp/nemu_debug.log", "a+")) == NULL)
        return;

    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fclose(fp);
#else
    (void)fmt;
#endif
}

void nm_cmd_str(nm_str_t *str, const nm_vect_t *argv)
{
    if (str->len > 0)
        nm_str_trunc(str, 0);

    for (size_t m = 0; m < argv->n_memb; m++)
        nm_str_append_format(str, "%s ", (char *)nm_vect_at(argv, m));
}

/* vim:set ts=4 sw=4: */
