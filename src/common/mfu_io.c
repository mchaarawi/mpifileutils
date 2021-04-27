#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <assert.h>
#include <libgen.h>

#include <gurt/common.h>
#include <gurt/hash.h>
#include <daos.h>
#include <daos_fs.h>
#include "mfu.h"

#define MFU_IO_TRIES  (5)
#define MFU_IO_USLEEP (100)

static int mpi_rank;
dfs_t *dfind_dfs;
struct d_hash_table *dir_hash;

struct mfu_dir_hdl {
        d_list_t	entry;
        dfs_obj_t	*oh;
        char		name[PATH_MAX];
};

#define NUM_DIRENTS 112

struct dfs_mfu_t {
	dfs_obj_t *dir;
	int num_ents;
	int num_splits;
	struct dirent ents[NUM_DIRENTS];
	daos_anchor_t anchor;
};

static inline struct mfu_dir_hdl *
hdl_obj(d_list_t *rlink)
{
        return container_of(rlink, struct mfu_dir_hdl, entry);
}

static bool
key_cmp(struct d_hash_table *htable, d_list_t *rlink,
	const void *key, unsigned int ksize)
{
        struct mfu_dir_hdl *hdl = hdl_obj(rlink);

        return (strcmp(hdl->name, (const char *)key) == 0);
}

static void
rec_free(struct d_hash_table *htable, d_list_t *rlink)
{
        struct mfu_dir_hdl *hdl = hdl_obj(rlink);

        assert(d_hash_rec_unlinked(&hdl->entry));
        dfs_release(hdl->oh);
        free(hdl);
}

static d_hash_table_ops_t hdl_hash_ops = {
        .hop_key_cmp	= key_cmp,
        .hop_rec_free	= rec_free
};

static dfs_obj_t *
lookup_insert_dir(const char *name)
{
        struct mfu_dir_hdl *hdl;
        d_list_t *rlink;
        int rc;

	if (dir_hash == NULL) {
	    rc = d_hash_table_create(0, 16, NULL, &hdl_hash_ops, &dir_hash);
	    if (rc) {
		    fprintf(stderr, "Failed to initialize dir hashtable");
		    return NULL;
	    }
	}

        rlink = d_hash_rec_find(dir_hash, name, strlen(name));
        if (rlink != NULL) {
                hdl = hdl_obj(rlink);
                return hdl->oh;
        }

        hdl = calloc(1, sizeof(struct mfu_dir_hdl));
        if (hdl == NULL)
		return NULL;

        strncpy(hdl->name, name, PATH_MAX-1);
        hdl->name[PATH_MAX-1] = '\0';

        rc = dfs_lookup(dfind_dfs, name, O_RDWR, &hdl->oh, NULL, NULL);
	if (rc) {
		fprintf(stderr, "dfs_lookup() of %s Failed (%d)\n", name, rc);
		return NULL;
	}

        rc = d_hash_rec_insert(dir_hash, hdl->name, strlen(hdl->name),
                               &hdl->entry, true);
	if (rc)
		return NULL;

        return hdl->oh;
}

static int
parse_filename(const char *path, char **_obj_name, char **_cont_name)
{
	char *f1 = NULL;
	char *f2 = NULL;
	char *fname = NULL;
	char *cont_name = NULL;
	int rc = 0;

	if (path == NULL || _obj_name == NULL || _cont_name == NULL)
		return -EINVAL;

	if (strcmp(path, "/") == 0) {
		*_cont_name = strdup("/");
		if (*_cont_name == NULL)
			return -ENOMEM;
		*_obj_name = NULL;
		return 0;
	}

	f1 = strdup(path);
	if (f1 == NULL) {
                rc = -ENOMEM;
                goto out;
        }

	f2 = strdup(path);
	if (f2 == NULL) {
                rc = -ENOMEM;
                goto out;
        }

	fname = basename(f1);
	cont_name = dirname(f2);

	if (cont_name[0] == '.' || cont_name[0] != '/') {
		char cwd[1024];

		if (getcwd(cwd, 1024) == NULL) {
                        rc = -ENOMEM;
                        goto out;
                }

		if (strcmp(cont_name, ".") == 0) {
			cont_name = strdup(cwd);
			if (cont_name == NULL) {
                                rc = -ENOMEM;
                                goto out;
                        }
		} else {
			char *new_dir = calloc(strlen(cwd) + strlen(cont_name)
					       + 1, sizeof(char));
			if (new_dir == NULL) {
                                rc = -ENOMEM;
                                goto out;
                        }

			strcpy(new_dir, cwd);
			if (cont_name[0] == '.') {
				strcat(new_dir, &cont_name[1]);
			} else {
				strcat(new_dir, "/");
				strcat(new_dir, cont_name);
			}
			cont_name = new_dir;
		}
		*_cont_name = cont_name;
	} else {
		*_cont_name = strdup(cont_name);
		if (*_cont_name == NULL) {
                        rc = -ENOMEM;
                        goto out;
                }
	}

	*_obj_name = strdup(fname);
	if (*_obj_name == NULL) {
		free(*_cont_name);
		*_cont_name = NULL;
                rc = -ENOMEM;
                goto out;
	}

out:
	if (f1)
		free(f1);
	if (f2)
		free(f2);
	return rc;
}

/* calls access, and retries a few times if we get EIO or EINTR */
int mfu_access(const char* path, int amode)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = access(path, amode);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* calls lchown, and retries a few times if we get EIO or EINTR */
int mfu_lchown(const char* path, uid_t owner, gid_t group)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = lchown(path, owner, group);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* calls chmod, and retries a few times if we get EIO or EINTR */
int mfu_chmod(const char* path, mode_t mode)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = chmod(path, mode);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* calls utimensat, and retries a few times if we get EIO or EINTR */
int mfu_utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = utimensat(dirfd, pathname, times, flags);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* calls lstat, and retries a few times if we get EIO or EINTR */
int mfu_lstat(const char* path, struct stat* buf)
{
    int rc;
    dfs_obj_t *parent = NULL;
    char *name = NULL, *dir_name = NULL;

    parse_filename(path, &name, &dir_name);

    assert(dir_name);

    parent = lookup_insert_dir(dir_name);
    if (parent == NULL) {
	    fprintf(stderr, "dfs_lookup %s failed \n", dir_name);
	    return ENOENT;
    }

    rc = dfs_stat(dfind_dfs, parent, name, buf);
    if (rc) {
	    fprintf(stderr, "dfs_stat %s failed (%d)\n", name);
	    return rc;
    }

    return 0;
}

/* calls lstat64, and retries a few times if we get EIO or EINTR */
int mfu_lstat64(const char* path, struct stat64* buf)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = lstat64(path, buf);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* call mknod, retry a few times on EINTR or EIO */
int mfu_mknod(const char* path, mode_t mode, dev_t dev)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = mknod(path, mode, dev);
    if (rc < 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* call remove, retry a few times on EINTR or EIO */
int mfu_remove(const char* path)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = remove(path);
    if (rc < 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/*****************************
 * Links
 ****************************/

/* call readlink, retry a few times on EINTR or EIO */
ssize_t mfu_readlink(const char* path, char* buf, size_t bufsize)
{
    ssize_t rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = readlink(path, buf, bufsize);
    if (rc < 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* call symlink, retry a few times on EINTR or EIO */
int mfu_symlink(const char* oldpath, const char* newpath)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = symlink(oldpath, newpath);
    if (rc < 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/*****************************
 * Files
 ****************************/

/* open file with specified flags and mode, retry open a few times on failure */
int mfu_open(const char* file, int flags, ...)
{
    /* extract the mode (see man 2 open) */
    int mode_set = 0;
    mode_t mode = 0;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        mode_set = 1;
    }

    /* attempt to open file */
    int fd = -1;
    errno = 0;
    if (mode_set) {
        fd = open(file, flags, mode);
    }
    else {
        fd = open(file, flags);
    }

    /* if open failed, try a few more times */
    if (fd < 0) {
        /* try again */
        int tries = MFU_IO_TRIES;
        while (tries && fd < 0) {
            /* sleep a bit before consecutive tries */
            usleep(MFU_IO_USLEEP);

            /* open again */
            errno = 0;
            if (mode_set) {
                fd = open(file, flags, mode);
            }
            else {
                fd = open(file, flags);
            }
            tries--;
        }

        /* if we still don't have a valid file, consider it an error */
        if (fd < 0) {
            /* we could abort, but probably don't want to here */
        }
    }

    return fd;
}

/* close file */
int mfu_close(const char* file, int fd)
{
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    int rc = close(fd);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* seek file descriptor to specified position */
off_t mfu_lseek(const char* file, int fd, off_t pos, int whence)
{
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    off_t rc = lseek(fd, pos, whence);
    if (rc == (off_t)-1) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* reliable read from file descriptor (retries, if necessary, until hard error) */
ssize_t mfu_read(const char* file, int fd, void* buf, size_t size)
{
    int tries = MFU_IO_TRIES;
    ssize_t n = 0;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
    while ((size_t)n < size) {
        errno = 0;
        ssize_t rc = read(fd, (char*) buf + n, size - (size_t)n);
        if (rc > 0) {
            /* read some data */
            n += rc;
            tries = MFU_IO_TRIES;
        }
        else if (rc == 0) {
            /* EOF */
            return n;
        }
        else {   /* (rc < 0) */
            /* something worth printing an error about */
            tries--;
            if (tries <= 0) {
                /* too many failed retries, give up */
                MFU_ABORT(-1, "Failed to read file %s errno=%d (%s)",
                            file, errno, strerror(errno)
                           );
            }

            /* sleep a bit before consecutive tries */
            usleep(MFU_IO_USLEEP);
        }
    }
    return n;
}

/* reliable write to file descriptor (retries, if necessary, until hard error) */
ssize_t mfu_write(const char* file, int fd, const void* buf, size_t size)
{
    int tries = 10;
    ssize_t n = 0;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
    while ((size_t)n < size) {
        errno = 0;
        ssize_t rc = write(fd, (const char*) buf + n, size - (size_t)n);
        if (rc > 0) {
            /* wrote some data */
            n += rc;
            tries = MFU_IO_TRIES;
        }
        else if (rc == 0) {
            /* something bad happened, print an error and abort */
            MFU_ABORT(-1, "Failed to write file %s errno=%d (%s)",
                        file, errno, strerror(errno)
                       );
        }
        else {   /* (rc < 0) */
            /* something worth printing an error about */
            tries--;
            if (tries <= 0) {
                /* too many failed retries, give up */
                MFU_ABORT(-1, "Failed to write file %s errno=%d (%s)",
                            file, errno, strerror(errno)
                           );
            }

            /* sleep a bit before consecutive tries */
            usleep(MFU_IO_USLEEP);
        }
    }
    return n;
}

/* truncate a file */
int mfu_truncate(const char* file, off_t length)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = truncate(file, length);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* ftruncate a file */
int mfu_ftruncate(int fd, off_t length)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = ftruncate(fd, length);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* delete a file */
int mfu_unlink(const char* file)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = unlink(file);
    if (rc != 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* force flush of written data */
int mfu_fsync(const char* file, int fd)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = fsync(fd);
    if (rc < 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/*****************************
 * Directories
 ****************************/

/* get current working directory, abort if fail or buffer too small */
void mfu_getcwd(char* buf, size_t size)
{
    errno = 0;
    char* p = getcwd(buf, size);
    if (p == NULL) {
        MFU_ABORT(-1, "Failed to get current working directory errno=%d (%s)",
                    errno, strerror(errno)
                   );
    }
}

/* create directory, retry a few times on EINTR or EIO */
int mfu_mkdir(const char* dir, mode_t mode)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = mkdir(dir, mode);
    if (rc < 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* remove directory, retry a few times on EINTR or EIO */
int mfu_rmdir(const char* dir)
{
    int rc;
    int tries = MFU_IO_TRIES;
    printf ("%d: __FUNCTION__ = %s\n", mpi_rank, __FUNCTION__);
retry:
    errno = 0;
    rc = rmdir(dir);
    if (rc < 0) {
        if (errno == EINTR || errno == EIO) {
            tries--;
            if (tries > 0) {
                /* sleep a bit before consecutive tries */
                usleep(MFU_IO_USLEEP);
                goto retry;
            }
        }
    }
    return rc;
}

/* open directory, retry a few times on EINTR or EIO */
DIR* mfu_opendir(const char* dir, int *nr)
{
    struct dfs_mfu_t *dirp = NULL;
    int rc;

    dirp = calloc(1, sizeof(*dirp));
    if (dirp == NULL)
	    return NULL;

    rc = dfs_lookup(dfind_dfs, dir, O_RDWR, &dirp->dir, NULL, NULL);
    if (rc) {
	    free(dirp);
	    fprintf(stderr, "dfs_lookup %s failed (%d)\n", dir, rc);
	    return NULL;
    }

    if (*nr == -1) {
	    dirp->num_splits = 0;
	    rc = dfs_obj_anchor_split(dirp->dir, &dirp->num_splits, NULL);
	    if (rc) {
		    fprintf(stderr, "dfs_obj_anchor_split failed (%d)", rc);
		    dfs_release(dirp->dir);
		    free(dirp);
		    return NULL;
	    }
	    *nr = dirp->num_splits;
	    goto done;
    }

    assert(*nr >= 0);
    rc = dfs_obj_anchor_set(dirp->dir, *nr, &dirp->anchor);
    if (rc) {
	    fprintf(stderr, "dfs_anchor_split failed (%d)", rc);
	    dfs_release(dirp->dir);
	    free(dirp);
	    return NULL;
    }

done:
    return (DIR *)dirp;
}

/* close directory, retry a few times on EINTR or EIO */
int mfu_closedir(DIR* _dirp)
{
    int rc;
    struct dfs_mfu_t *dirp = (struct dfs_mfu_t *)_dirp;

    rc = dfs_release(dirp->dir);
    if (rc)
	    return rc;
    free(dirp);
    return rc;
}

/* read directory entry, retry a few times on ENOENT, EIO, or EINTR */
struct dirent* mfu_readdir(DIR* _dirp)
{
    int rc;
    struct dfs_mfu_t *dirp = (struct dfs_mfu_t *)_dirp;

    if (dirp->num_ents)
	    goto ret;

    dirp->num_ents = NUM_DIRENTS;

    while (!daos_anchor_is_eof(&dirp->anchor)) {
	    rc = dfs_readdir(dfind_dfs, dirp->dir, &dirp->anchor, &dirp->num_ents,
			     dirp->ents);
	    if (rc)
		    return NULL;

	    if (dirp->num_ents == 0)
		    continue;
	    goto ret;
    }

    assert(daos_anchor_is_eof(&dirp->anchor));
    return NULL;

ret:
    dirp->num_ents--;
    return &dirp->ents[dirp->num_ents];
}
