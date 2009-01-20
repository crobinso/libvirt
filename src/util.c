/*
 * utils.c: common, generic utility functions
 *
 * Copyright (C) 2006, 2007, 2008 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 * Copyright (C) 2006, 2007 Binary Karma
 * Copyright (C) 2006 Shuveb Hussain
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 * File created Jul 18, 2007 - Shuveb Hussain <shuveb@binarykarma.com>
 */

#include <config.h>

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#if HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <string.h>
#include <signal.h>
#if HAVE_TERMIOS_H
#include <termios.h>
#endif
#include "c-ctype.h"

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif
#include <netdb.h>

#include "virterror_internal.h"
#include "logging.h"
#include "event.h"
#include "buf.h"
#include "util.h"
#include "memory.h"

#ifndef NSIG
# define NSIG 32
#endif

#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define virLog(msg...) fprintf(stderr, msg)


#define ReportError(conn, code, fmt...)                                      \
        virReportErrorHelper(conn, VIR_FROM_NONE, code, __FILE__,          \
                               __FUNCTION__, __LINE__, fmt)

/* Like read(), but restarts after EINTR */
int saferead(int fd, void *buf, size_t count)
{
        size_t nread = 0;
        while (count > 0) {
                ssize_t r = read(fd, buf, count);
                if (r < 0 && errno == EINTR)
                        continue;
                if (r < 0)
                        return r;
                if (r == 0)
                        return nread;
                buf = (char *)buf + r;
                count -= r;
                nread += r;
        }
        return nread;
}

/* Like write(), but restarts after EINTR */
ssize_t safewrite(int fd, const void *buf, size_t count)
{
        size_t nwritten = 0;
        while (count > 0) {
                ssize_t r = write(fd, buf, count);

                if (r < 0 && errno == EINTR)
                        continue;
                if (r < 0)
                        return r;
                if (r == 0)
                        return nwritten;
                buf = (const char *)buf + r;
                count -= r;
                nwritten += r;
        }
        return nwritten;
}

#ifndef PROXY

int virFileStripSuffix(char *str,
                       const char *suffix)
{
    int len = strlen(str);
    int suffixlen = strlen(suffix);

    if (len < suffixlen)
        return 0;

    if (!STREQ(str + len - suffixlen, suffix))
        return 0;

    str[len-suffixlen] = '\0';

    return 1;
}

char *
virArgvToString(const char *const *argv)
{
    int len, i;
    char *ret, *p;

    for (len = 1, i = 0; argv[i]; i++)
        len += strlen(argv[i]) + 1;

    if (VIR_ALLOC_N(ret, len) < 0)
        return NULL;
    p = ret;

    for (i = 0; argv[i]; i++) {
        if (i != 0)
            *(p++) = ' ';

        strcpy(p, argv[i]);
        p += strlen(argv[i]);
    }

    *p = '\0';

    return ret;
}

int virSetNonBlock(int fd) {
#ifndef WIN32
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) < 0)
        return -1;
    flags |= O_NONBLOCK;
    if ((fcntl(fd, F_SETFL, flags)) < 0)
        return -1;
#else
    unsigned long flag = 1;

    /* This is actually Gnulib's replacement rpl_ioctl function.
     * We can't call ioctlsocket directly in any case.
     */
    if (ioctl (fd, FIONBIO, (void *) &flag) == -1)
        return -1;
#endif
    return 0;
}


#ifndef WIN32

static int virSetCloseExec(int fd) {
    int flags;
    if ((flags = fcntl(fd, F_GETFD)) < 0)
        return -1;
    flags |= FD_CLOEXEC;
    if ((fcntl(fd, F_SETFD, flags)) < 0)
        return -1;
    return 0;
}

static int
__virExec(virConnectPtr conn,
          const char *const*argv,
          const char *const*envp,
          const fd_set *keepfd,
          pid_t *retpid,
          int infd, int *outfd, int *errfd,
          int flags) {
    pid_t pid;
    int null, i, openmax;
    int pipeout[2] = {-1,-1};
    int pipeerr[2] = {-1,-1};
    int childout = -1;
    int childerr = -1;
    sigset_t oldmask, newmask;
    struct sigaction sig_action;

    /*
     * Need to block signals now, so that child process can safely
     * kill off caller's signal handlers without a race.
     */
    sigfillset(&newmask);
    if (pthread_sigmask(SIG_SETMASK, &newmask, &oldmask) != 0) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("cannot block signals: %s"),
                    strerror(errno));
        return -1;
    }

    if ((null = open("/dev/null", O_RDONLY)) < 0) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("cannot open %s: %s"),
                    "/dev/null", strerror(errno));
        goto cleanup;
    }

    if (outfd != NULL) {
        if (*outfd == -1) {
            if (pipe(pipeout) < 0) {
                ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                            _("cannot create pipe: %s"), strerror(errno));
                goto cleanup;
            }

            if ((flags & VIR_EXEC_NONBLOCK) &&
                virSetNonBlock(pipeout[0]) == -1) {
                ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                            "%s", _("Failed to set non-blocking file descriptor flag"));
                goto cleanup;
            }

            if (virSetCloseExec(pipeout[0]) == -1) {
                ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                            "%s", _("Failed to set close-on-exec file descriptor flag"));
                goto cleanup;
            }

            childout = pipeout[1];
        } else {
            childout = *outfd;
        }
#ifndef ENABLE_DEBUG
    } else {
        childout = null;
#endif
    }

    if (errfd != NULL) {
        if (*errfd == -1) {
            if (pipe(pipeerr) < 0) {
                ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                            _("Failed to create pipe: %s"), strerror(errno));
                goto cleanup;
            }

            if ((flags & VIR_EXEC_NONBLOCK) &&
                virSetNonBlock(pipeerr[0]) == -1) {
                ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                            "%s", _("Failed to set non-blocking file descriptor flag"));
                goto cleanup;
            }

            if (virSetCloseExec(pipeerr[0]) == -1) {
                ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                            "%s", _("Failed to set close-on-exec file descriptor flag"));
                goto cleanup;
            }

            childerr = pipeerr[1];
        } else {
            childerr = *errfd;
        }
#ifndef ENABLE_DEBUG
    } else {
        childerr = null;
#endif
    }

    if ((pid = fork()) < 0) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("cannot fork child process: %s"), strerror(errno));
        goto cleanup;
    }

    if (pid) { /* parent */
        close(null);
        if (outfd && *outfd == -1) {
            close(pipeout[1]);
            *outfd = pipeout[0];
        }
        if (errfd && *errfd == -1) {
            close(pipeerr[1]);
            *errfd = pipeerr[0];
        }

        /* Restore our original signal mask now child is safely
           running */
        if (pthread_sigmask(SIG_SETMASK, &oldmask, NULL) != 0) {
            ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                        _("cannot unblock signals: %s"),
                        strerror(errno));
            return -1;
        }

        *retpid = pid;
        return 0;
    }

    /* child */

    /* Don't want to report errors against this accidentally, so
       just discard it */
    conn = NULL;
    /* Remove any error callback too, so errors in child now
       get sent to stderr where they stand a fighting chance
       of being seen / logged */
    virSetErrorFunc(NULL, NULL);

    /* Clear out all signal handlers from parent so nothing
       unexpected can happen in our child once we unblock
       signals */
    sig_action.sa_handler = SIG_DFL;
    sig_action.sa_flags = 0;
    sigemptyset(&sig_action.sa_mask);

    for (i = 1 ; i < NSIG ; i++)
        /* Only possible errors are EFAULT or EINVAL
           The former wont happen, the latter we
           expect, so no need to check return value */
        sigaction(i, &sig_action, NULL);

    /* Unmask all signals in child, since we've no idea
       what the caller's done with their signal mask
       and don't want to propagate that to children */
    sigemptyset(&newmask);
    if (pthread_sigmask(SIG_SETMASK, &newmask, NULL) != 0) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("cannot unblock signals: %s"),
                    strerror(errno));
        return -1;
    }

    openmax = sysconf (_SC_OPEN_MAX);
    for (i = 3; i < openmax; i++)
        if (i != infd &&
            i != null &&
            i != childout &&
            i != childerr &&
            (!keepfd ||
             !FD_ISSET(i, keepfd)))
            close(i);

    if (flags & VIR_EXEC_DAEMON) {
        if (setsid() < 0) {
            ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                        _("cannot become session leader: %s"),
                        strerror(errno));
            _exit(1);
        }

        if (chdir("/") < 0) {
            ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                        _("cannot change to root directory: %s"),
                        strerror(errno));
            _exit(1);
        }

        pid = fork();
        if (pid < 0) {
            ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                        _("cannot fork child process: %s"),
                        strerror(errno));
            _exit(1);
        }

        if (pid > 0)
            _exit(0);
    }


    if (dup2(infd >= 0 ? infd : null, STDIN_FILENO) < 0) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("failed to setup stdin file handle: %s"), strerror(errno));
        _exit(1);
    }
    if (childout > 0 &&
        dup2(childout, STDOUT_FILENO) < 0) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("failed to setup stdout file handle: %s"), strerror(errno));
        _exit(1);
    }
    if (childerr > 0 &&
        dup2(childerr, STDERR_FILENO) < 0) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("failed to setup stderr file handle: %s"), strerror(errno));
        _exit(1);
    }

    close(null);
    if (childout > 0)
        close(childout);
    if (childerr > 0 &&
        childerr != childout)
        close(childerr);

    if (envp)
        execve(argv[0], (char **) argv, (char**)envp);
    else
        execvp(argv[0], (char **) argv);

    ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                _("cannot execute binary '%s': %s"),
                argv[0], strerror(errno));

    _exit(1);

    return 0;

 cleanup:
    /* This is cleanup of parent process only - child
       should never jump here on error */

    /* NB we don't ReportError() on any failures here
       because the code which jumped hre already raised
       an error condition which we must not overwrite */
    if (pipeerr[0] > 0)
        close(pipeerr[0]);
    if (pipeerr[1] > 0)
        close(pipeerr[1]);
    if (pipeout[0] > 0)
        close(pipeout[0]);
    if (pipeout[1] > 0)
        close(pipeout[1]);
    if (null > 0)
        close(null);
    return -1;
}

int
virExec(virConnectPtr conn,
        const char *const*argv,
        const char *const*envp,
        const fd_set *keepfd,
        pid_t *retpid,
        int infd, int *outfd, int *errfd,
        int flags) {
    char *argv_str;

    if ((argv_str = virArgvToString(argv)) == NULL) {
        ReportError(conn, VIR_ERR_NO_MEMORY, "%s", _("command debug string"));
        return -1;
    }
    DEBUG0(argv_str);
    VIR_FREE(argv_str);

    return __virExec(conn, argv, envp, keepfd, retpid, infd, outfd, errfd,
                     flags);
}

static int
virPipeReadUntilEOF(virConnectPtr conn, int outfd, int errfd,
                    char **outbuf, char **errbuf) {

    struct pollfd fds[2];
    int i;
    int finished[2];

    fds[0].fd = outfd;
    fds[0].events = POLLIN;
    finished[0] = 0;
    fds[1].fd = errfd;
    fds[1].events = POLLIN;
    finished[1] = 0;

    while(!(finished[0] && finished[1])) {

        if (poll(fds, ARRAY_CARDINALITY(fds), -1) < 0) {
            if ((errno == EAGAIN) || (errno == EINTR))
                continue;
            goto pollerr;
        }

        for (i = 0; i < ARRAY_CARDINALITY(fds); ++i) {
            char data[1024], **buf;
            int got, size;

            if (!(fds[i].revents))
                continue;
            else if (fds[i].revents & POLLHUP)
                finished[i] = 1;

            if (!(fds[i].revents & POLLIN)) {
                if (fds[i].revents & POLLHUP)
                    continue;

                ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                            "%s", _("Unknown poll response."));
                goto error;
            }

            got = read(fds[i].fd, data, sizeof(data));

            if (got == 0) {
                finished[i] = 1;
                continue;
            }
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN)
                    break;
                goto pollerr;
            }

            buf = ((fds[i].fd == outfd) ? outbuf : errbuf);
            size = (*buf ? strlen(*buf) : 0);
            if (VIR_REALLOC_N(*buf, size+got+1) < 0) {
                ReportError(conn, VIR_ERR_NO_MEMORY, NULL);
                goto error;
            }
            memmove(*buf+size, data, got);
            (*buf)[size+got] = '\0';
        }
        continue;

    pollerr:
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("poll error: %s"), strerror(errno));
        goto error;
    }

    return 0;

error:
    VIR_FREE(*outbuf);
    VIR_FREE(*errbuf);
    return -1;
}

/**
 * @conn connection to report errors against
 * @argv NULL terminated argv to run
 * @status optional variable to return exit status in
 *
 * Run a command without using the shell.
 *
 * If status is NULL, then return 0 if the command run and
 * exited with 0 status; Otherwise return -1
 *
 * If status is not-NULL, then return 0 if the command ran.
 * The status variable is filled with the command exit status
 * and should be checked by caller for success. Return -1
 * only if the command could not be run.
 */
int
virRun(virConnectPtr conn,
       const char *const*argv,
       int *status) {
    pid_t childpid;
    int exitstatus, execret, waitret;
    int ret = -1;
    int errfd = -1, outfd = -1;
    char *outbuf = NULL;
    char *errbuf = NULL;
    char *argv_str = NULL;

    if ((argv_str = virArgvToString(argv)) == NULL) {
        ReportError(conn, VIR_ERR_NO_MEMORY, "%s", _("command debug string"));
        goto error;
    }
    DEBUG0(argv_str);

    if ((execret = __virExec(conn, argv, NULL, NULL,
                             &childpid, -1, &outfd, &errfd,
                             VIR_EXEC_NONE)) < 0) {
        ret = execret;
        goto error;
    }

    if (virPipeReadUntilEOF(conn, outfd, errfd, &outbuf, &errbuf) < 0)
        goto error;

    if (outbuf)
        DEBUG("Command stdout: %s", outbuf);
    if (errbuf)
        DEBUG("Command stderr: %s", errbuf);

    while ((waitret = waitpid(childpid, &exitstatus, 0) == -1) &&
            errno == EINTR);
    if (waitret == -1) {
        ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                    _("cannot wait for '%s': %s"),
                    argv[0], strerror(errno));
        goto error;
    }

    if (status == NULL) {
        errno = EINVAL;
        if (WIFEXITED(exitstatus) && WEXITSTATUS(exitstatus) != 0) {

            ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                        _("'%s' exited with non-zero status %d and "
                          "signal %d: %s"), argv_str,
                        WIFEXITED(exitstatus) ? WEXITSTATUS(exitstatus) : 0,
                        WIFSIGNALED(exitstatus) ? WTERMSIG(exitstatus) : 0,
                        (errbuf ? errbuf : ""));
            goto error;
        }
    } else {
        *status = exitstatus;
    }

    ret = 0;

  error:
    VIR_FREE(outbuf);
    VIR_FREE(errbuf);
    VIR_FREE(argv_str);
    if (outfd != -1)
        close(outfd);
    if (errfd != -1)
        close(errfd);
    return ret;
}

#else /* __MINGW32__ */

int
virRun(virConnectPtr conn,
       const char *const *argv ATTRIBUTE_UNUSED,
       int *status)
{
    if (status)
        *status = ENOTSUP;
    else
        ReportError (conn, VIR_ERR_INTERNAL_ERROR, __FUNCTION__);
    return -1;
}

int
virExec(virConnectPtr conn,
        const char *const*argv ATTRIBUTE_UNUSED,
        const char *const*envp ATTRIBUTE_UNUSED,
        const fd_set *keepfd ATTRIBUTE_UNUSED,
        int *retpid ATTRIBUTE_UNUSED,
        int infd ATTRIBUTE_UNUSED,
        int *outfd ATTRIBUTE_UNUSED,
        int *errfd ATTRIBUTE_UNUSED,
        int flags ATTRIBUTE_UNUSED)
{
    ReportError (conn, VIR_ERR_INTERNAL_ERROR, __FUNCTION__);
    return -1;
}

#endif /* __MINGW32__ */

/* Like gnulib's fread_file, but read no more than the specified maximum
   number of bytes.  If the length of the input is <= max_len, and
   upon error while reading that data, it works just like fread_file.  */
static char *
fread_file_lim (FILE *stream, size_t max_len, size_t *length)
{
    char *buf = NULL;
    size_t alloc = 0;
    size_t size = 0;
    int save_errno;

    for (;;) {
        size_t count;
        size_t requested;

        if (size + BUFSIZ + 1 > alloc) {
            alloc += alloc / 2;
            if (alloc < size + BUFSIZ + 1)
                alloc = size + BUFSIZ + 1;

            if (VIR_REALLOC_N(buf, alloc) < 0) {
                save_errno = errno;
                break;
            }
        }

        /* Ensure that (size + requested <= max_len); */
        requested = MIN (size < max_len ? max_len - size : 0,
                         alloc - size - 1);
        count = fread (buf + size, 1, requested, stream);
        size += count;

        if (count != requested || requested == 0) {
            save_errno = errno;
            if (ferror (stream))
                break;
            buf[size] = '\0';
            *length = size;
            return buf;
        }
    }

    free (buf);
    errno = save_errno;
    return NULL;
}

/* A wrapper around fread_file_lim that maps a failure due to
   exceeding the maximum size limitation to EOVERFLOW.  */
static int virFileReadLimFP(FILE *fp, int maxlen, char **buf)
{
    size_t len;
    char *s = fread_file_lim (fp, maxlen+1, &len);
    if (s == NULL)
        return -1;
    if (len > maxlen || (int)len != len) {
        VIR_FREE(s);
        /* There was at least one byte more than MAXLEN.
           Set errno accordingly. */
        errno = EOVERFLOW;
        return -1;
    }
    *buf = s;
    return len;
}

/* Like virFileReadLimFP, but use a file descriptor rather than a FILE*.  */
int virFileReadLimFD(int fd_arg, int maxlen, char **buf)
{
    int fd = dup (fd_arg);
    if (fd >= 0) {
        FILE *fp = fdopen (fd, "r");
        if (fp) {
            int len = virFileReadLimFP (fp, maxlen, buf);
            int saved_errno = errno;
            fclose (fp);
            errno = saved_errno;
            return len;
        } else {
            int saved_errno = errno;
            close (fd);
            errno = saved_errno;
        }
    }
    return -1;
}

int virFileReadAll(const char *path, int maxlen, char **buf)
{
    FILE *fh = fopen(path, "r");
    if (fh == NULL) {
        virLog("Failed to open file '%s': %s\n",
               path, strerror(errno));
        return -1;
    }

    int len = virFileReadLimFP (fh, maxlen, buf);
    fclose(fh);
    if (len < 0) {
        virLog("Failed to read '%s': %s\n", path, strerror (errno));
        return -1;
    }

    return len;
}

int virFileMatchesNameSuffix(const char *file,
                             const char *name,
                             const char *suffix)
{
    int filelen = strlen(file);
    int namelen = strlen(name);
    int suffixlen = strlen(suffix);

    if (filelen == (namelen + suffixlen) &&
        STREQLEN(file, name, namelen) &&
        STREQLEN(file + namelen, suffix, suffixlen))
        return 1;
    else
        return 0;
}

int virFileHasSuffix(const char *str,
                     const char *suffix)
{
    int len = strlen(str);
    int suffixlen = strlen(suffix);

    if (len < suffixlen)
        return 0;

    return STREQ(str + len - suffixlen, suffix);
}

#define SAME_INODE(Stat_buf_1, Stat_buf_2) \
  ((Stat_buf_1).st_ino == (Stat_buf_2).st_ino \
   && (Stat_buf_1).st_dev == (Stat_buf_2).st_dev)

/* Return nonzero if checkLink and checkDest
   refer to the same file.  Otherwise, return 0.  */
int virFileLinkPointsTo(const char *checkLink,
                        const char *checkDest)
{
    struct stat src_sb;
    struct stat dest_sb;

    return (stat (checkLink, &src_sb) == 0
            && stat (checkDest, &dest_sb) == 0
            && SAME_INODE (src_sb, dest_sb));
}

int virFileExists(const char *path)
{
    struct stat st;

    if (stat(path, &st) >= 0)
        return(1);
    return(0);
}

int virFileMakePath(const char *path)
{
    struct stat st;
    char parent[PATH_MAX];
    char *p;
    int err;

    if (stat(path, &st) >= 0)
        return 0;

    strncpy(parent, path, PATH_MAX);
    parent[PATH_MAX - 1] = '\0';

    if (!(p = strrchr(parent, '/')))
        return EINVAL;

    if (p != parent) {
        *p = '\0';
        if ((err = virFileMakePath(parent)))
            return err;
    }

    if (mkdir(path, 0777) < 0 && errno != EEXIST)
        return errno;

    return 0;
}

/* Build up a fully qualfiied path for a config file to be
 * associated with a persistent guest or network */
int virFileBuildPath(const char *dir,
                     const char *name,
                     const char *ext,
                     char *buf,
                     unsigned int buflen)
{
    if ((strlen(dir) + 1 + strlen(name) + (ext ? strlen(ext) : 0) + 1) >= (buflen-1))
        return -1;

    strcpy(buf, dir);
    strcat(buf, "/");
    strcat(buf, name);
    if (ext)
        strcat(buf, ext);
    return 0;
}


#ifdef __linux__
int virFileOpenTty(int *ttymaster,
                   char **ttyName,
                   int rawmode)
{
    int rc = -1;

    if ((*ttymaster = posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK)) < 0)
        goto cleanup;

    if (unlockpt(*ttymaster) < 0)
        goto cleanup;

    if (grantpt(*ttymaster) < 0)
        goto cleanup;

    if (rawmode) {
        struct termios ttyAttr;
        if (tcgetattr(*ttymaster, &ttyAttr) < 0)
            goto cleanup;

        cfmakeraw(&ttyAttr);

        if (tcsetattr(*ttymaster, TCSADRAIN, &ttyAttr) < 0)
            goto cleanup;
    }

    if (ttyName) {
        char tempTtyName[PATH_MAX];
        if (ptsname_r(*ttymaster, tempTtyName, sizeof(tempTtyName)) < 0)
            goto cleanup;

        if ((*ttyName = strdup(tempTtyName)) == NULL) {
            errno = ENOMEM;
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    if (rc != 0 &&
        *ttymaster != -1) {
        close(*ttymaster);
    }

    return rc;

}
#else
int virFileOpenTty(int *ttymaster ATTRIBUTE_UNUSED,
                   char **ttyName ATTRIBUTE_UNUSED,
                   int rawmode ATTRIBUTE_UNUSED)
{
    return -1;
}
#endif


char* virFilePid(const char *dir, const char* name)
{
    char *pidfile;
    virAsprintf(&pidfile, "%s/%s.pid", dir, name);
    return pidfile;
}


int virFileWritePid(const char *dir,
                    const char *name,
                    pid_t pid)
{
    int rc;
    int fd;
    FILE *file = NULL;
    char *pidfile = NULL;

    if ((rc = virFileMakePath(dir)))
        goto cleanup;

    if (!(pidfile = virFilePid(dir, name))) {
        rc = ENOMEM;
        goto cleanup;
    }

    if ((fd = open(pidfile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR)) < 0) {
        rc = errno;
        goto cleanup;
    }

    if (!(file = fdopen(fd, "w"))) {
        rc = errno;
        close(fd);
        goto cleanup;
    }

    if (fprintf(file, "%d", pid) < 0) {
        rc = errno;
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (file &&
        fclose(file) < 0) {
        rc = errno;
    }

    VIR_FREE(pidfile);
    return rc;
}

int virFileReadPid(const char *dir,
                   const char *name,
                   pid_t *pid)
{
    int rc;
    FILE *file;
    char *pidfile = NULL;
    *pid = 0;

    if (!(pidfile = virFilePid(dir, name))) {
        rc = ENOMEM;
        goto cleanup;
    }

    if (!(file = fopen(pidfile, "r"))) {
        rc = errno;
        goto cleanup;
    }

    if (fscanf(file, "%d", pid) != 1) {
        rc = EINVAL;
        goto cleanup;
    }

    if (fclose(file) < 0) {
        rc = errno;
        goto cleanup;
    }

    rc = 0;

 cleanup:
    VIR_FREE(pidfile);
    return rc;
}

int virFileDeletePid(const char *dir,
                     const char *name)
{
    int rc = 0;
    char *pidfile = NULL;

    if (!(pidfile = virFilePid(dir, name))) {
        rc = ENOMEM;
        goto cleanup;
    }

    if (unlink(pidfile) < 0 && errno != ENOENT)
        rc = errno;

cleanup:
    VIR_FREE(pidfile);
    return rc;
}



/* Like strtol, but produce an "int" result, and check more carefully.
   Return 0 upon success;  return -1 to indicate failure.
   When END_PTR is NULL, the byte after the final valid digit must be NUL.
   Otherwise, it's like strtol and lets the caller check any suffix for
   validity.  This function is careful to return -1 when the string S
   represents a number that is not representable as an "int". */
int
virStrToLong_i(char const *s, char **end_ptr, int base, int *result)
{
    long int val;
    char *p;
    int err;

    errno = 0;
    val = strtol(s, &p, base);
    err = (errno || (!end_ptr && *p) || p == s || (int) val != val);
    if (end_ptr)
        *end_ptr = p;
    if (err)
        return -1;
    *result = val;
    return 0;
}

/* Just like virStrToLong_i, above, but produce an "unsigned int" value.  */
int
virStrToLong_ui(char const *s, char **end_ptr, int base, unsigned int *result)
{
    unsigned long int val;
    char *p;
    int err;

    errno = 0;
    val = strtoul(s, &p, base);
    err = (errno || (!end_ptr && *p) || p == s || (unsigned int) val != val);
    if (end_ptr)
        *end_ptr = p;
    if (err)
        return -1;
    *result = val;
    return 0;
}

/* Just like virStrToLong_i, above, but produce an "long long" value.  */
int
virStrToLong_ll(char const *s, char **end_ptr, int base, long long *result)
{
    long long val;
    char *p;
    int err;

    errno = 0;
    val = strtoll(s, &p, base);
    err = (errno || (!end_ptr && *p) || p == s || (long long) val != val);
    if (end_ptr)
        *end_ptr = p;
    if (err)
        return -1;
    *result = val;
    return 0;
}

/* Just like virStrToLong_i, above, but produce an "unsigned long long" value.  */
int
virStrToLong_ull(char const *s, char **end_ptr, int base, unsigned long long *result)
{
    unsigned long long val;
    char *p;
    int err;

    errno = 0;
    val = strtoull(s, &p, base);
    err = (errno || (!end_ptr && *p) || p == s || (unsigned long long) val != val);
    if (end_ptr)
        *end_ptr = p;
    if (err)
        return -1;
    *result = val;
    return 0;
}
#endif /* PROXY */

/**
 * virSkipSpaces:
 * @str: pointer to the char pointer used
 *
 * Skip potential blanks, this includes space tabs, line feed,
 * carriage returns and also '\\' which can be erronously emitted
 * by xend
 */
void
virSkipSpaces(const char **str)
{
    const char *cur = *str;

    while ((*cur == ' ') || (*cur == '\t') || (*cur == '\n') ||
           (*cur == '\r') || (*cur == '\\'))
        cur++;
    *str = cur;
}

/**
 * virParseNumber:
 * @str: pointer to the char pointer used
 *
 * Parse an unsigned number
 *
 * Returns the unsigned number or -1 in case of error. @str will be
 *         updated to skip the number.
 */
int
virParseNumber(const char **str)
{
    int ret = 0;
    const char *cur = *str;

    if ((*cur < '0') || (*cur > '9'))
        return (-1);

    while (c_isdigit(*cur)) {
        unsigned int c = *cur - '0';

        if ((ret > INT_MAX / 10) ||
            ((ret == INT_MAX / 10) && (c > INT_MAX % 10)))
            return (-1);
        ret = ret * 10 + c;
        cur++;
    }
    *str = cur;
    return (ret);
}

/**
 * virAsprintf
 *
 * like glibc's_asprintf but makes sure *strp == NULL on failure
 */
int
virAsprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);

    if ((ret = vasprintf(strp, fmt, ap)) == -1)
        *strp = NULL;

    va_end(ap);
    return ret;
}

/* Compare two MAC addresses, ignoring differences in case,
 * as well as leading zeros.
 */
int
virMacAddrCompare (const char *p, const char *q)
{
    unsigned char c, d;
    do {
        while (*p == '0' && c_isxdigit (p[1]))
            ++p;
        while (*q == '0' && c_isxdigit (q[1]))
            ++q;
        c = c_tolower (*p);
        d = c_tolower (*q);

        if (c == 0 || d == 0)
            break;

        ++p;
        ++q;
    } while (c == d);

    if (UCHAR_MAX <= INT_MAX)
        return c - d;

    /* On machines where 'char' and 'int' are types of the same size, the
       difference of two 'unsigned char' values - including the sign bit -
       doesn't fit in an 'int'.  */
    return (c > d ? 1 : c < d ? -1 : 0);
}

/**
 * virParseMacAddr:
 * @str: string representation of MAC address, e.g., "0:1E:FC:E:3a:CB"
 * @addr: 6-byte MAC address
 *
 * Parse a MAC address
 *
 * Return 0 upon success, or -1 in case of error.
 */
int
virParseMacAddr(const char* str, unsigned char *addr)
{
    int i;

    errno = 0;
    for (i = 0; i < VIR_MAC_BUFLEN; i++) {
        char *end_ptr;
        unsigned long result;

        /* This is solely to avoid accepting the leading
         * space or "+" that strtoul would otherwise accept.
         */
        if (!c_isxdigit(*str))
            break;

        result = strtoul(str, &end_ptr, 16);

        if ((end_ptr - str) < 1 || 2 < (end_ptr - str) ||
            (errno != 0) ||
            (0xFF < result))
            break;

        addr[i] = (unsigned char) result;

        if ((i == 5) && (*end_ptr == '\0'))
            return 0;
        if (*end_ptr != ':')
            break;

        str = end_ptr + 1;
    }

    return -1;
}

void virFormatMacAddr(const unsigned char *addr,
                      char *str)
{
    snprintf(str, VIR_MAC_STRING_BUFLEN,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2],
             addr[3], addr[4], addr[5]);
    str[VIR_MAC_STRING_BUFLEN-1] = '\0';
}

void virGenerateMacAddr(const unsigned char *prefix,
                        unsigned char *addr)
{
    addr[0] = prefix[0];
    addr[1] = prefix[1];
    addr[2] = prefix[2];
    addr[3] = (int)(256*(rand()/(RAND_MAX+1.0)));
    addr[4] = (int)(256*(rand()/(RAND_MAX+1.0)));
    addr[5] = (int)(256*(rand()/(RAND_MAX+1.0)));
}


int virEnumFromString(const char *const*types,
                      unsigned int ntypes,
                      const char *type)
{
    unsigned int i;
    for (i = 0 ; i < ntypes ; i++)
        if (STREQ(types[i], type))
            return i;

    return -1;
}

const char *virEnumToString(const char *const*types,
                            unsigned int ntypes,
                            int type)
{
    if (type < 0 || type >= ntypes)
        return NULL;

    return types[type];
}

/* Translates a device name of the form (regex) "[fhv]d[a-z]+" into
 * the corresponding index (e.g. sda => 0, hdz => 25, vdaa => 26)
 * @param name The name of the device
 * @return name's index, or -1 on failure
 */
int virDiskNameToIndex(const char *name) {
    const char *ptr = NULL;
    int idx = 0;
    static char const* const drive_prefix[] = {"fd", "hd", "vd", "sd", "xvd"};
    unsigned int i;

    for (i = 0; i < ARRAY_CARDINALITY(drive_prefix); i++) {
        if (STRPREFIX(name, drive_prefix[i])) {
            ptr = name + strlen(drive_prefix[i]);
            break;
        }
    }

    if (!ptr)
        return -1;

    for (i = 0; *ptr; i++) {
        idx = (idx + i) * 26;

        if (!c_islower(*ptr))
            return -1;

        idx += *ptr - 'a';
        ptr++;
    }

    return idx;
}

#ifndef AI_CANONIDN
#define AI_CANONIDN 0
#endif

char *virGetHostname(void)
{
    int r;
    char hostname[HOST_NAME_MAX+1], *result;
    struct addrinfo hints, *info;

    r = gethostname (hostname, sizeof(hostname));
    if (r == -1)
        return NULL;
    NUL_TERMINATE(hostname);

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_CANONNAME|AI_CANONIDN;
    hints.ai_family = AF_UNSPEC;
    r = getaddrinfo(hostname, NULL, &hints, &info);
    if (r != 0)
        return NULL;
    if (info->ai_canonname == NULL) {
        freeaddrinfo(info);
        return NULL;
    }

    /* Caller frees this string. */
    result = strdup (info->ai_canonname);
    freeaddrinfo(info);
    return result;
}

/* send signal to a single process */
int virKillProcess(pid_t pid, int sig)
{
    if (pid < 1) {
        errno = ESRCH;
        return -1;
    }

#ifdef WIN32
    /* Mingw / Windows don't have many signals (AFAIK) */
    switch (sig) {
    case SIGINT:
        /* This does a Ctrl+C equiv */
        if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, pid)) {
            errno = ESRCH;
            return -1;
        }
        break;

    case SIGTERM:
        /* Since TerminateProcess is closer to SIG_KILL, we do
         * a Ctrl+Break equiv which is more pleasant like the
         * good old unix SIGTERM/HUP
         */
        if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)) {
            errno = ESRCH;
            return -1;
        }
        break;

    default:
    {
        HANDLE proc;
        proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!proc) {
            errno = ESRCH; /* Not entirely accurate, but close enough */
            return -1;
        }

        /*
         * TerminateProcess is more or less equiv to SIG_KILL, in that
         * a process can't trap / block it
         */
        if (!TerminateProcess(proc, sig)) {
            errno = ESRCH;
            return -1;
        }
        CloseHandle(proc);
    }
    }
    return 0;
#else
    return kill(pid, sig);
#endif
}
