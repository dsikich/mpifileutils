/*
 * Copyright (c) 2013-2015, Lawrence Livermore National Security, LLC.
 *   Produced at the Lawrence Livermore National Laboratory
 *   CODE-673838
 *
 * Copyright (c) 2006-2007,2011-2015, Los Alamos National Security, LLC.
 *   (LA-CC-06-077, LA-CC-10-066, LA-CC-14-046)
 *
 * Copyright (2013-2015) UT-Battelle, LLC under Contract No.
 *   DE-AC05-00OR22725 with the Department of Energy.
 *
 * Copyright (c) 2015, DataDirect Networks, Inc.
 *
 * All rights reserved.
 *
 * This file is part of mpiFileUtils.
 * For details, see https://github.com/hpc/fileutils.
 * Please also read the LICENSE file.
*/

#define _GNU_SOURCE
#ifndef __G_MACROS_H__
#define __G_MACROS_H__
#endif
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h> /* asctime / localtime */
#include <regex.h>
/* These headers are needed to query the Lustre MDS for stat
 * information.  This information may be incomplete, but it
 * is faster than a normal stat, which requires communication
 * with the MDS plus every OST a file is striped across. */
//#define LUSTRE_STAT
#ifdef LUSTRE_STAT
#include <lustre/lustre_user.h>
#endif /* LUSTRE_STAT */

#include <pwd.h> /* for getpwent */
#include <grp.h> /* for getgrent */
#include <errno.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/param.h>

#include <linux/fs.h>
#include <linux/fiemap.h>

#include <libgen.h> /* dirname */
#include "libcircle.h"
#include "dtcmp.h"
#include "mfu.h"
#include "mfu_flist_internal.h"
#include "strmap.h"
#include "handle_args.h"
#ifdef LUSTRE_SUPPORT
#include <lustre/lustre_user.h>
#include <sys/ioctl.h>
#endif

/** Where we should keep statistics related to this file copy. */
DCOPY_statistics_t DCOPY_statistics;

/** Options specified by the user. */
DCOPY_options_t DCOPY_user_opts;

mfu_copy_file_cache_t mfu_copy_src_cache;
mfu_copy_file_cache_t mfu_copy_dst_cache;

static int DCOPY_open_file(const char* file, int read_flag, mfu_copy_file_cache_t* cache)
{
    int newfd = -1;

    /* see if we have a cached file descriptor */
    char* name = cache->name;
    if (name != NULL) {
        /* we have a cached file descriptor */
        int fd = cache->fd;
        if (strcmp(name, file) == 0 && cache->read == read_flag) {
            /* the file we're trying to open matches name and read/write mode,
             * so just return the cached descriptor */
            return fd;
        } else {
            /* the file we're trying to open is different,
             * close the old file and delete the name */
            mfu_close(name, fd);
            mfu_free(&cache->name);
        }
    }

    /* open the new file */
    if (read_flag) {
        int flags = O_RDONLY;
        if (DCOPY_user_opts.synchronous) {
            flags |= O_DIRECT;
        }
        newfd = mfu_open(file, flags);
    } else {
        int flags = O_WRONLY | O_CREAT;
        if (DCOPY_user_opts.synchronous) {
            flags |= O_DIRECT;
        }
        newfd = mfu_open(file, flags, DCOPY_DEF_PERMS_FILE);
    }

    /* cache the file descriptor */
    if (newfd != -1) {
        cache->name = MFU_STRDUP(file);
        cache->fd   = newfd;
        cache->read = read_flag;
#ifdef LUSTRE_SUPPORT
        /* Zero is an invalid ID for grouplock. */
        if (DCOPY_user_opts.grouplock_id != 0) {
            int rc;

            rc = ioctl(newfd, LL_IOC_GROUP_LOCK, DCOPY_user_opts.grouplock_id);
            if (rc) {
                MFU_LOG(MFU_LOG_ERR, "Failed to obtain grouplock with ID %d "
                    "on file `%s', ignoring this error: %s",
                    DCOPY_user_opts.grouplock_id,
                    file, strerror(errno));
            } else {
                MFU_LOG(MFU_LOG_INFO, "Obtained grouplock with ID %d "
                    "on file `%s', fd %d", DCOPY_user_opts.grouplock_id,
                    file, newfd);
            }
        }
#endif
    }

    return newfd;
}

/* copy all extended attributes from op->operand to dest_path */
static void DCOPY_copy_xattrs(
    mfu_flist flist,
    uint64_t idx,
    const char* dest_path)
{
#if DCOPY_USE_XATTRS
    /* get source file name */
    const char* src_path = mfu_flist_file_get_name(flist, idx);

    /* start with a reasonable buffer, we'll allocate more as needed */
    size_t list_bufsize = 1204;
    char* list = (char*) MFU_MALLOC(list_bufsize);

    /* get list, if list_size == ERANGE, try again */
    ssize_t list_size;
    int got_list = 0;

    /* get current estimate for list size */
    while(! got_list) {
        list_size = llistxattr(src_path, list, list_bufsize);

        if(list_size < 0) {
            if(errno == ERANGE) {
                /* buffer is too small, free our current buffer
                 * and call it again with size==0 to get new size */
                mfu_free(&list);
                list_bufsize = 0;
            }
            else if(errno == ENOTSUP) {
                /* this is common enough that we silently ignore it */
                break;
            }
            else {
                /* this is a real error */
                MFU_LOG(MFU_LOG_ERR, "Failed to get list of extended attributes on %s llistxattr() errno=%d %s",
                    src_path, errno, strerror(errno)
                   );
                break;
            }
        }
        else {
            if(list_size > 0 && list_bufsize == 0) {
                /* called llistxattr with size==0 and got back positive
                 * number indicating size of buffer we need to allocate */
                list_bufsize = (size_t) list_size;
                list = (char*) MFU_MALLOC(list_bufsize);
            }
            else {
                /* got our list, it's size is in list_size, which may be 0 */
                got_list = 1;
            }
        }
    }

    /* iterate over list and copy values to new object lgetxattr/lsetxattr */
    if(got_list) {
        char* name = list;

        while(name < list + list_size) {
            /* start with a reasonable buffer,
             * allocate something bigger as needed */
            size_t val_bufsize = 1024;
            void* val = (void*) MFU_MALLOC(val_bufsize);

            /* lookup value for name */
            ssize_t val_size;
            int got_val = 0;

            while(! got_val) {
                val_size = lgetxattr(src_path, name, val, val_bufsize);

                if(val_size < 0) {
                    if(errno == ERANGE) {
                        /* buffer is too small, free our current buffer
                         * and call it again with size==0 to get new size */
                        mfu_free(&val);
                        val_bufsize = 0;
                    }
                    else if(errno == ENOATTR) {
                        /* source object no longer has this attribute,
                         * maybe deleted out from under us */
                        break;
                    }
                    else {
                        /* this is a real error */
                        MFU_LOG(MFU_LOG_ERR, "Failed to get value for name=%s on %s llistxattr() errno=%d %s",
                            name, src_path, errno, strerror(errno)
                           );
                        break;
                    }
                }
                else {
                    if(val_size > 0 && val_bufsize == 0) {
                        /* called lgetxattr with size==0 and got back positive
                         * number indicating size of buffer we need to allocate */
                        val_bufsize = (size_t) val_size;
                        val = (void*) MFU_MALLOC(val_bufsize);
                    }
                    else {
                        /* got our value, it's size is in val_size, which may be 0 */
                        got_val = 1;
                    }
                }
            }

            /* set attribute on destination object */
            if(got_val) {
                int setrc = lsetxattr(dest_path, name, val, (size_t) val_size, 0);

                if(setrc != 0) {
                    MFU_LOG(MFU_LOG_ERR, "Failed to set value for name=%s on %s llistxattr() errno=%d %s",
                        name, dest_path, errno, strerror(errno)
                       );
                }
            }

            /* free value string */
            mfu_free(&val);
            val_bufsize = 0;

            /* jump to next name */
            size_t namelen = strlen(name) + 1;
            name += namelen;
        }
    }

    /* free space allocated for list */
    mfu_free(&list);
    list_bufsize = 0;

    return;
#endif /* DCOPY_USE_XATTR */
}

static void DCOPY_copy_ownership(
    mfu_flist flist,
    uint64_t idx,
    const char* dest_path)
{
    /* get user id and group id of file */
    uid_t uid = (uid_t) mfu_flist_file_get_uid(flist, idx);
    gid_t gid = (gid_t) mfu_flist_file_get_gid(flist, idx);

    /* note that we use lchown to change ownership of link itself, it path happens to be a link */
    if(mfu_lchown(dest_path, uid, gid) != 0) {
        /* TODO: are there other EPERM conditions we do want to report? */

        /* since the user running dcp may not be the owner of the
         * file, we could hit an EPERM error here, and the file
         * will be left with the effective uid and gid of the dcp
         * process, don't bother reporting an error for that case */
        if (errno != EPERM) {
            MFU_LOG(MFU_LOG_ERR, "Failed to change ownership on %s lchown() errno=%d %s",
                dest_path, errno, strerror(errno)
               );
        }
    }

    return;
}

/* TODO: condionally set setuid and setgid bits? */
static void DCOPY_copy_permissions(
    mfu_flist flist,
    uint64_t idx,
    const char* dest_path)
{
    /* get mode and type */
    mfu_filetype type = mfu_flist_file_get_type(flist, idx);
    mode_t mode = (mode_t) mfu_flist_file_get_mode(flist, idx);

    /* change mode */
    if(type != MFU_TYPE_LINK) {
        if(mfu_chmod(dest_path, mode) != 0) {
            MFU_LOG(MFU_LOG_ERR, "Failed to change permissions on %s chmod() errno=%d %s",
                dest_path, errno, strerror(errno)
               );
        }
    }

    return;
}

static void DCOPY_copy_timestamps(
    mfu_flist flist,
    uint64_t idx,
    const char* dest_path)
{
    /* get atime seconds and nsecs */
    uint64_t atime      = mfu_flist_file_get_atime(flist, idx);
    uint64_t atime_nsec = mfu_flist_file_get_atime_nsec(flist, idx);

    /* get mtime seconds and nsecs */
    uint64_t mtime      = mfu_flist_file_get_mtime(flist, idx);
    uint64_t mtime_nsec = mfu_flist_file_get_mtime_nsec(flist, idx);

    /* fill in time structures */
    struct timespec times[2];
    times[0].tv_sec  = (time_t) atime;
    times[0].tv_nsec = (long)   atime_nsec;
    times[1].tv_sec  = (time_t) mtime;
    times[1].tv_nsec = (long)   mtime_nsec;

    /* set times with nanosecond precision using utimensat,
     * assume path is relative to current working directory,
     * if it's not absolute, and set times on link (not target file)
     * if dest_path refers to a link */
    if(utimensat(AT_FDCWD, dest_path, times, AT_SYMLINK_NOFOLLOW) != 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to change timestamps on %s utime() errno=%d %s",
            dest_path, errno, strerror(errno)
           );
    }

#if 0
    /* TODO: see stat-time.h and get_stat_atime/mtime/ctime to read sub-second times,
     * and use utimensat to set sub-second times */
    /* as last step, change timestamps */
    if(! S_ISLNK(statbuf->st_mode)) {
        struct utimbuf times;
        times.actime  = statbuf->st_atime;
        times.modtime = statbuf->st_mtime;
        if(utime(dest_path, &times) != 0) {
            MFU_LOG(MFU_LOG_ERR, "Failed to change timestamps on %s utime() errno=%d %s",
                dest_path, errno, strerror(errno)
               );
        }
    }
    else {
        struct timeval tv[2];
        tv[0].tv_sec  = statbuf->st_atime;
        tv[0].tv_usec = 0;
        tv[1].tv_sec  = statbuf->st_mtime;
        tv[1].tv_usec = 0;
        if(lutimes(dest_path, tv) != 0) {
            MFU_LOG(MFU_LOG_ERR, "Failed to change timestamps on %s utime() errno=%d %s",
                dest_path, errno, strerror(errno)
               );
        }
    }
#endif

    return;
}

/* called by single process upon detection of a problem */
void DCOPY_abort(int code)
{
    MPI_Abort(MPI_COMM_WORLD, code);
    exit(code);
}

/* called globally by all procs to exit */
void DCOPY_exit(int code)
{
    /* CIRCLE_finalize or will this hang? */
    mfu_finalize();
    MPI_Finalize();
    exit(code);
}

int mfu_copy_close_file(mfu_copy_file_cache_t* cache)
{
    int rc = 0;

    /* close file if we have one */
    char* name = cache->name;
    if (name != NULL) {
        int fd = cache->fd;

        /* if open for write, fsync */
        int read_flag = cache->read;
        if (! read_flag) {
            rc = mfu_fsync(name, fd);
        }

        /* close the file and delete the name string */
        rc = mfu_close(name, fd);
        mfu_free(&cache->name);
    }

    return rc;
}

/* iterate through list of files and set ownership, timestamps,
 * and permissions starting from deepest level and working upwards,
 * we go in this direction in case updating a file updates its
 * parent directory */
void mfu_copy_set_metadata(int levels, int minlevel, mfu_flist* lists)
{
    /* get current rank */
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0) {
        if(DCOPY_user_opts.preserve) {
            MFU_LOG(MFU_LOG_INFO, "Setting ownership, permissions, and timestamps.");
        }
        else {
            MFU_LOG(MFU_LOG_INFO, "Fixing permissions.");
        }
    }

    /* now set timestamps on files starting from deepest level */
    int level;
    for (level = levels-1; level >= 0; level--) {
        /* get list at this level */
        mfu_flist list = lists[level];

        /* cycle through our list of items and set timestamps
         * for each one at this level */
        uint64_t idx;
        uint64_t size = mfu_flist_size(list);
        for (idx = 0; idx < size; idx++) {
            /* get type of item */
            mfu_filetype type = mfu_flist_file_get_type(list, idx);

            /* we've already set these properties for links,
             * so we can skip those here */
            if (type == MFU_TYPE_LINK) {
                continue;
            }

            /* TODO: skip file if it's not readable */

            /* get destination name of item */
            const char* name = mfu_flist_file_get_name(list, idx);
            char* dest = DCOPY_build_dest(name);

            /* No need to copy it */
            if (dest == NULL) {
                continue;
            }

            if(DCOPY_user_opts.preserve) {
                DCOPY_copy_ownership(list, idx, dest);
                DCOPY_copy_permissions(list, idx, dest);
                DCOPY_copy_timestamps(list, idx, dest);
            }
            else {
                /* TODO: set permissions based on source permissons
                 * masked by umask */
                DCOPY_copy_permissions(list, idx, dest);
            }

            /* free destination item */
            mfu_free(&dest);
        }
        
        /* wait for all procs to finish before we start
         * with files at next level */
        MPI_Barrier(MPI_COMM_WORLD);
    }

    return;
}

static int mfu_create_directory(mfu_flist list, uint64_t idx)
{
    /* get name of directory */
    const char* name = mfu_flist_file_get_name(list, idx);

    /* get destination name */
    char* dest_path = DCOPY_build_dest(name);

    /* No need to copy it */
    if (dest_path == NULL) {
        return 0;
    }

   /* create the destination directory */
    MFU_LOG(MFU_LOG_DBG, "Creating directory `%s'", dest_path);
    int rc = mfu_mkdir(dest_path, DCOPY_DEF_PERMS_DIR);
    if(rc != 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to create directory `%s' (errno=%d %s)", \
            dest_path, errno, strerror(errno));
        mfu_free(&dest_path);
        return -1;
    }

    /* we do this now in case there are Lustre attributes for
     * creating / striping files in the directory */

    /* copy extended attributes on directory */
    if (DCOPY_user_opts.preserve) {
        DCOPY_copy_xattrs(list, idx, dest_path);
    }

    /* increment our directory count by one */
    DCOPY_statistics.total_dirs++;

    /* free the directory name */
    mfu_free(&dest_path);

    return 0;
}

/* create directories, we work from shallowest level to the deepest
 * with a barrier in between levels, so that we don't try to create
 * a child directory until the parent exists */
int mfu_create_directories(int levels, int minlevel, mfu_flist* lists)
{
    /* get current rank */
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int rc = 0;

    int verbose = (mfu_debug_level <= MFU_LOG_INFO);

    /* indicate to user what phase we're in */
    if (rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Creating directories.");
    }

    /* get our rank and number of ranks in job */
    int ranks;
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* work from shallowest level to deepest level */
    int level;
    for (level = 0; level < levels; level++) {
        /* time how long this takes */
        double start = MPI_Wtime();

        /* get list of items for this level */
        mfu_flist list = lists[level];

        /* create each directory we have at this level */
        uint64_t idx;
        uint64_t size = mfu_flist_size(list);
        uint64_t count = 0;
        for (idx = 0; idx < size; idx++) {
            /* check whether we have a directory */
            mfu_filetype type = mfu_flist_file_get_type(list, idx);
            if (type == MFU_TYPE_DIR) {
                /* create the directory */
                int tmp_rc = mfu_create_directory(list, idx);
                if (tmp_rc != 0) {
                    rc = tmp_rc;
                }

                count++;
            }
        }

        /* wait for all procs to finish before we start
         * creating directories at next level */
        MPI_Barrier(MPI_COMM_WORLD);

        /* stop our timer */
        double end = MPI_Wtime();

         /* print statistics */
        if (verbose) {
            uint64_t min, max, sum;
            MPI_Allreduce(&count, &min, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);
            MPI_Allreduce(&count, &max, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
            MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
            double rate = 0.0;
            double secs = end - start;
            if (secs > 0.0) {
              rate = (double)sum / secs;
            }
            if (rank == 0) {
                printf("  level=%d min=%lu max=%lu sum=%lu rate=%f/sec secs=%f\n",
                  (minlevel + level), (unsigned long)min, (unsigned long)max, (unsigned long)sum, rate, secs
                );
                fflush(stdout);
            }
        }
    }

    return rc;
}

static int mfu_create_link(mfu_flist list, uint64_t idx)
{
    /* get source name */
    const char* src_path = mfu_flist_file_get_name(list, idx);

    /* get destination name */
    const char* dest_path = DCOPY_build_dest(src_path);

    /* No need to copy it */
    if (dest_path == NULL) {
        return 0;
    }

    /* read link target */
    char path[PATH_MAX + 1];
    ssize_t rc = mfu_readlink(src_path, path, sizeof(path) - 1);

    if(rc < 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to read link `%s' readlink() errno=%d %s",
            src_path, errno, strerror(errno)
        );
        mfu_free(&dest_path);
        return -1;
    }

    /* ensure that string ends with NUL */
    path[rc] = '\0';

    /* create new link */
    int symrc = mfu_symlink(path, dest_path);

    if(symrc < 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to create link `%s' symlink() errno=%d %s",
            dest_path, errno, strerror(errno)
        );
        mfu_free(&dest_path);
        return -1;
    }

    /* TODO: why not do this later? */

    /* set permissions on link */
    if (DCOPY_user_opts.preserve) {
        DCOPY_copy_xattrs(list, idx, dest_path);
        DCOPY_copy_ownership(list, idx, dest_path);
        DCOPY_copy_permissions(list, idx, dest_path);
    }

    /* free destination path */
    mfu_free(&dest_path);

    /* increment our directory count by one */
    DCOPY_statistics.total_links++;

    return 0;
}

static int mfu_create_file(mfu_flist list, uint64_t idx)
{
    /* get source name */
    const char* src_path = mfu_flist_file_get_name(list, idx);

    /* get destination name */
    const char* dest_path = DCOPY_build_dest(src_path);

    /* No need to copy it */
    if (dest_path == NULL) {
        return 0;
    }

    /* since file systems like Lustre require xattrs to be set before file is opened,
     * we first create it with mknod and then set xattrs */

    /* create file with mknod
    * for regular files, dev argument is supposed to be ignored,
    * see makedev() to create valid dev */
    dev_t dev;
    memset(&dev, 0, sizeof(dev_t));
    int mknod_rc = mfu_mknod(dest_path, DCOPY_DEF_PERMS_FILE | S_IFREG, dev);

    if(mknod_rc < 0) {
        if(errno == EEXIST) {
            /* TODO: should we unlink and mknod again in this case? */
        }

        MFU_LOG(MFU_LOG_ERR, "File `%s' mknod() errno=%d %s",
            dest_path, errno, strerror(errno)
        );
    }

    /* copy extended attributes, important to do this first before
     * writing data because some attributes tell file system how to
     * stripe data, e.g., Lustre */
    if (DCOPY_user_opts.preserve) {
        DCOPY_copy_xattrs(list, idx, dest_path);
    }

    /* Truncate destination files to 0 bytes when sparse file is enabled,
     * this is because we will not overwrite sections corresponding to holes
     * and we need those to be set to 0 */
    if (DCOPY_user_opts.sparse) {
        /* truncate destination file to 0 bytes */
        struct stat st;
        int status = mfu_lstat(dest_path, &st);
        if (status == 0) {
            /* destination exists, truncate it to 0 bytes */
            status = truncate64(dest_path, 0);
            if (status) {
                MFU_LOG(MFU_LOG_ERR, "Failed to truncate destination file: %s (errno=%d %s)",
                          dest_path, errno, strerror(errno));
            }
        } else if (errno == -ENOENT) {
            /* destination does not exist, which is fine */
            status = 0;
        } else {
            /* had an error stating destination file */
            MFU_LOG(MFU_LOG_ERR, "mfu_lstat() file: %s (errno=%d %s)",
                      dest_path, errno, strerror(errno));
        }

        if (status) {
            /* do we need to abort here? */
            //return;
        }
    }

    /* free destination path */
    mfu_free(&dest_path);

    /* increment our file count by one */
    DCOPY_statistics.total_files++;

    return 0;
}

int mfu_create_files(int levels, int minlevel, mfu_flist* lists)
{
    /* get current rank */
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int rc = 0;

    int verbose = (mfu_debug_level <= MFU_LOG_INFO);

    /* indicate to user what phase we're in */
    if (rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Creating files.");
    }

    /* get our rank and number of ranks in job */
    int ranks;
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* TODO: we don't need to have a barrier between levels */

    int level;
    for (level = 0; level < levels; level++) {
        /* time how long this takes */
        double start = MPI_Wtime();

        /* get list of items for this level */
        mfu_flist list = lists[level];

        /* iterate over items and set write bit on directories if needed */
        uint64_t idx;
        uint64_t size = mfu_flist_size(list);
        uint64_t count = 0;
        for (idx = 0; idx < size; idx++) {
            /* get type of item */
            mfu_filetype type = mfu_flist_file_get_type(list, idx);

            /* process files and links */
            if (type == MFU_TYPE_FILE) {
                /* TODO: skip file if it's not readable */
                mfu_create_file(list, idx);
                count++;
            } else if (type == MFU_TYPE_LINK) {
                mfu_create_link(list, idx);
                count++;
            }
        }

        /* wait for all procs to finish before we start
         * with files at next level */
        MPI_Barrier(MPI_COMM_WORLD);

        /* stop our timer */
        double end = MPI_Wtime();

        /* print timing statistics */
        if (verbose) {
            uint64_t min, max, sum;
            MPI_Allreduce(&count, &min, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);
            MPI_Allreduce(&count, &max, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
            MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
            double rate = 0.0;
            double secs = end - start;
            if (secs > 0.0) {
              rate = (double)sum / secs;
            }
            if (rank == 0) {
                printf("  level=%d min=%lu max=%lu sum=%lu rate=%f secs=%f\n",
                  (minlevel + level), (unsigned long)min, (unsigned long)max, (unsigned long)sum, rate, secs
                );
                fflush(stdout);
            }
        }
    }

    return rc;
}

/* return 1 if entire buffer is 0, return 0 if any byte is not 0,
 * we avoid writing NULL blocks when supporting sparse files */
static int mfu_is_all_null(const char* buf, uint64_t buf_size)
{
    uint64_t i;
    for (i = 0; i < buf_size; i++) {
        if (buf[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* when using sparse files, we need to write the last byte if the
 * hole is adjacent to EOF, so we need to detect whether we're at
 * the end of the file */
static int mfu_is_eof(const char* file, int fd)
{
    /* read one byte from fd to determine whether this is EOF.
     * This is not efficient, but it is the only reliable way */
    char buf[1];
    ssize_t num_of_bytes_read = mfu_read(file, fd, buf, 1);

    /* return if we detect EOF */
    if(! num_of_bytes_read) {
        return 1;
    }

    /* otherwise, we're not at EOF yet, seek back one byte */
    if(mfu_lseek(file, fd, -1, SEEK_CUR) == (off_t)-1) {
        MFU_LOG(MFU_LOG_ERR, "Couldn't seek in path `%s' errno=%d %s",
                  file, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int mfu_copy_file_normal(
    const char* src,
    const char* dest,
    const int in_fd,
    const int out_fd,
    off_t offset,
    off_t length,
    uint64_t file_size)
{
    /* hint that we'll read from file sequentially */
//    posix_fadvise(in_fd, offset, chunk_size, POSIX_FADV_SEQUENTIAL);

    /* seek to offset in source file */
    if(mfu_lseek(src, in_fd, offset, SEEK_SET) == (off_t)-1) {
        MFU_LOG(MFU_LOG_ERR, "Couldn't seek in source path `%s' errno=%d %s", \
            src, errno, strerror(errno));
        return -1;
    }

    /* seek to offset in destination file */
    if(mfu_lseek(dest, out_fd, offset, SEEK_SET) == (off_t)-1) {
        MFU_LOG(MFU_LOG_ERR, "Couldn't seek in destination path `%s' errno=%d %s", \
            dest, errno, strerror(errno));
        return -1;
    }

    /* get buffer */
    size_t buf_size = DCOPY_user_opts.block_size;
    void* buf = DCOPY_user_opts.block_buf1;

    /* write data */
    size_t total_bytes = 0;
    while(total_bytes <= length) {
        /* determine number of bytes that we
         * can read = max(buf size, remaining chunk) */
        size_t left_to_read = length - total_bytes;
        if(left_to_read > buf_size) {
            left_to_read = buf_size;
        }

        /* read data from source file */
        ssize_t num_of_bytes_read = mfu_read(src, in_fd, buf, left_to_read);

        /* check for EOF */
        if(! num_of_bytes_read) {
            break;
        }

        /* compute number of bytes to write */
        size_t bytes_to_write = (size_t) num_of_bytes_read;
        if(DCOPY_user_opts.synchronous) {
            /* O_DIRECT requires particular write sizes,
             * ok to write beyond end of file so long as
             * we truncate in cleanup step */
            size_t remainder = buf_size - (size_t) num_of_bytes_read;
            if(remainder > 0) {
                /* zero out the end of the buffer for security,
                 * don't want to leave data from another file at end of
                 * current file if we fail before truncating */
                char* bufzero = ((char*)buf + num_of_bytes_read);
                memset(bufzero, 0, remainder);
            }

            /* assumes buf_size is magic size for O_DIRECT */
            bytes_to_write = buf_size;
        }

        /* Write data to destination file.
         * Do nothing for a hole in the middle of a file,
         * because write of next chunk will create one for us.
         * Write only the last byte to create the hole,
         * if the hole is next to EOF. */
        ssize_t num_of_bytes_written = bytes_to_write;
        if (DCOPY_user_opts.sparse && mfu_is_all_null(buf, bytes_to_write)) {
            /* TODO: isn't there a better way to know if we're at EOF,
             * e.g., by using file size? */
            /* determine whether we're at the end of the file */
            int end_of_file = mfu_is_eof(src, in_fd);
            if (end_of_file < 0) {
                /* hit an error while looking for EOF */
                return -1;
            }

            /* if we're at the end of the file, write out a byte,
             * otherwise just seek out destination file pointer
             * ahead without writing anything */
            if (end_of_file) {
                /* seek to last byte position in file */
                if(mfu_lseek(dest, out_fd, bytes_to_write - 1, SEEK_CUR) == (off_t)-1) {
                    MFU_LOG(MFU_LOG_ERR, "Couldn't seek in destination path `%s' errno=%d %s", \
                        dest, errno, strerror(errno));
                    return -1;
                }

                /* write out a single byte */
                mfu_write(dest, out_fd, buf, 1);
            } else {
                /* this section of the destination file is all 0,
                 * seek past this section */
                if(mfu_lseek(dest, out_fd, bytes_to_write, SEEK_CUR) == (off_t)-1) {
                    MFU_LOG(MFU_LOG_ERR, "Couldn't seek in destination path `%s' errno=%d %s", \
                        dest, errno, strerror(errno));
                    return -1;
                }
            }
        } else {
            /* write bytes to destination file */
            num_of_bytes_written = mfu_write(dest, out_fd, buf, bytes_to_write);
        }

        /* check for an error */
        if(num_of_bytes_written < 0) {
            MFU_LOG(MFU_LOG_ERR, "Write error when copying from `%s' to `%s' errno=%d %s",
                src, dest, errno, strerror(errno));
            return -1;
        }

        /* check that we wrote the same number of bytes that we read */
        if((size_t)num_of_bytes_written != bytes_to_write) {
            MFU_LOG(MFU_LOG_ERR, "Write error when copying from `%s' to `%s'",
                src, dest);
            return -1;
        }

        /* add bytes to our total (use bytes read,
         * which may be less than number written) */
        total_bytes += (size_t) num_of_bytes_read;
    }

    /* Increment the global counter. */
    DCOPY_statistics.total_size += (int64_t) total_bytes;
    DCOPY_statistics.total_bytes_copied += (int64_t) total_bytes;

#if 0
    /* force data to file system */
    if(total_bytes > 0) {
        mfu_fsync(dest, out_fd);
    }
#endif

    /* no need to truncate if sparse file is enabled,
     * since we truncated files when they were first created */
    if (DCOPY_user_opts.sparse) {
        return 0;
    }

    /* if we wrote the last chunk, truncate the file */
    off_t last_written = offset + length;
    off_t file_size_offt = (off_t) file_size;
    if (last_written >= file_size_offt || file_size == 0) {
       /*
        * Use ftruncate() here rather than truncate(), because grouplock
        * of Lustre would cause block to truncate() since the fd is different
        * from the out_fd.
        */
        if(ftruncate(out_fd, file_size_offt) < 0) {
            MFU_LOG(MFU_LOG_ERR, "Failed to truncate destination file: %s (errno=%d %s)",
                dest, errno, strerror(errno));
            return -1;
       }
    }

    /* we don't bother closing the file because our cache does it for us */

    return 0;
}

static int mfu_copy_file_fiemap(
    const char* src,
    const char* dest,
    const int in_fd,
    const int out_fd,
    uint64_t offset,
    uint64_t length,
    uint64_t file_size,
    bool* normal_copy_required)
{
    *normal_copy_required = true;
    if (DCOPY_user_opts.synchronous) {
        goto fail_normal_copy;
    }

    size_t last_ext_start = offset;
    size_t last_ext_len = 0;

    struct fiemap *fiemap = (struct fiemap*)malloc(sizeof(struct fiemap));
    if (fiemap == NULL) {
        MFU_LOG(MFU_LOG_ERR, "Out of memory allocating fiemap\n");
        goto fail_normal_copy;
    }
    memset(fiemap, 0, sizeof(struct fiemap));

    fiemap->fm_start  = offset;
    fiemap->fm_length = length;
    fiemap->fm_flags  = FIEMAP_FLAG_SYNC;
    fiemap->fm_extent_count   = 0;
    fiemap->fm_mapped_extents = 0;

    struct stat sb;
    if (fstat(in_fd, &sb) < 0) {
        goto fail_normal_copy;
    }

    if (ioctl(in_fd, FS_IOC_FIEMAP, fiemap) < 0) {
        MFU_LOG(MFU_LOG_ERR, "fiemap ioctl() failed for src %s\n", src);
        goto fail_normal_copy;
    }

    size_t extents_size = sizeof(struct fiemap_extent) * (fiemap->fm_mapped_extents);

    if ((fiemap = (struct fiemap*)realloc(fiemap,sizeof(struct fiemap) +
                                  extents_size)) == NULL)
    {
        MFU_LOG(MFU_LOG_ERR, "Out of memory reallocating fiemap\n");
        goto fail_normal_copy;
    }

    memset(fiemap->fm_extents, 0, extents_size);
    fiemap->fm_extent_count   = fiemap->fm_mapped_extents;
    fiemap->fm_mapped_extents = 0;

    if (ioctl(in_fd, FS_IOC_FIEMAP, fiemap) < 0) {
        MFU_LOG(MFU_LOG_ERR, "fiemap ioctl() failed for src %s\n", src);
        goto fail_normal_copy;
    }

    uint64_t last_byte = offset + length;

    if (fiemap->fm_mapped_extents > 0) {
        uint64_t fe_logical = fiemap->fm_extents[0].fe_logical;
        uint64_t fe_length  = fiemap->fm_extents[0].fe_length;
        if (fe_logical < offset) {
            fiemap->fm_extents[0].fe_length -= (offset - fe_logical);
            fiemap->fm_extents[0].fe_logical = offset;
        }

        fe_logical = fiemap->fm_extents[fiemap->fm_mapped_extents - 1].fe_logical;
        fe_length  = fiemap->fm_extents[fiemap->fm_mapped_extents - 1].fe_length;
        if (fe_logical + fe_length > last_byte) {
           fiemap->fm_extents[fiemap->fm_mapped_extents - 1].fe_length -=
           fe_logical + fe_length - last_byte;
        }
    }

    *normal_copy_required = false;

    /* seek to offset in source file */
    if (mfu_lseek(src, in_fd, (off_t)last_ext_start, SEEK_SET) < 0) {
        MFU_LOG(MFU_LOG_ERR, "Couldn't seek in source path `%s' errno=%d %s", \
            src, errno, strerror(errno));
        goto fail;
    }

    /* seek to offset in destination file */
    if (mfu_lseek(dest, out_fd, (off_t)last_ext_start, SEEK_SET) < 0) {
        MFU_LOG(MFU_LOG_ERR, "Couldn't seek in destination path `%s' errno=%d %s", \
            dest, errno, strerror(errno));
        goto fail;
    }

    unsigned int i;
    for (i = 0; i < fiemap->fm_mapped_extents; i++) {
        size_t ext_start;
        size_t ext_len;
        size_t ext_hole_size;

        size_t buf_size = DCOPY_user_opts.block_size;
        void* buf = DCOPY_user_opts.block_buf1;

        ext_start = fiemap->fm_extents[i].fe_logical;
        ext_len = fiemap->fm_extents[i].fe_length;
        ext_hole_size = ext_start - (last_ext_start + last_ext_len);

        if (ext_hole_size) {
            if (mfu_lseek(src, in_fd, (off_t)ext_start, SEEK_SET) < 0) {
                MFU_LOG(MFU_LOG_ERR, "Couldn't seek in source path `%s' errno=%d %s", \
                    src, errno, strerror(errno));
                goto fail;
            }
            if (mfu_lseek(dest, out_fd, (off_t)ext_hole_size, SEEK_CUR) < 0) {
                MFU_LOG(MFU_LOG_ERR, "Couldn't seek in destination path `%s' errno=%d %s", \
                    dest, errno, strerror(errno));
                goto fail;
            }
        }

        last_ext_start = ext_start;
        last_ext_len = ext_len;

        while (ext_len) {
            ssize_t num_read = mfu_read(src, in_fd, buf, MIN(ext_len, buf_size));

            if (!num_read)
                break;

            ssize_t num_written = mfu_write(dest, out_fd, buf, (size_t)num_read);

            if (num_written < 0) {
                MFU_LOG(MFU_LOG_ERR, "Write error when copying from `%s' to `%s' errno=%d %s",
                          src, dest, errno, strerror(errno));
                goto fail;
            }
            if (num_written != num_read) {
                MFU_LOG(MFU_LOG_ERR, "Write error when copying from `%s' to `%s'",
                    src, dest);
                goto fail;
            }

            ext_len -= (size_t)num_written;
            DCOPY_statistics.total_bytes_copied += (int64_t) num_written;
        }
    }

    off_t last_written = (off_t) last_byte;
    off_t file_size_offt = (off_t) file_size;
    if (last_written >= file_size_offt || file_size == 0) {
       /*
        * Use ftruncate() here rather than truncate(), because grouplock
        * of Lustre would cause block to truncate() since the fd is different
        * from the out_fd.
        */
        if (ftruncate(out_fd, file_size_offt) < 0) {
            MFU_LOG(MFU_LOG_ERR, "Failed to truncate destination file: %s (errno=%d %s)",
                dest, errno, strerror(errno));
            goto fail;
       }
    }

    if (last_written >= file_size_offt) {
        DCOPY_statistics.total_size += (int64_t) (file_size_offt - (off_t) offset);
    } else {
        DCOPY_statistics.total_size += (int64_t) last_byte;
    }

    free(fiemap);
    return 0;

fail:
    free(fiemap);

fail_normal_copy:
    return -1;
}

static int mfu_copy_file(
    const char* src,
    const char* dest,
    uint64_t offset,
    uint64_t length,
    uint64_t file_size)
{
    int ret;
    bool normal_copy_required;

    /* open the input file */
    int in_fd = DCOPY_open_file(src, 1, &mfu_copy_src_cache);
    if (in_fd < 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to open input file `%s' errno=%d %s",
            src, errno, strerror(errno));
        return -1;
    }

    /* open the output file */
    int out_fd = DCOPY_open_file(dest, 0, &mfu_copy_dst_cache);
    if (out_fd < 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to open output file `%s' errno=%d %s",
            dest, errno, strerror(errno));
        return -1;
    }

    if (DCOPY_user_opts.sparse) {
        ret = mfu_copy_file_fiemap(src, dest, in_fd, out_fd, offset,
                               length, file_size, &normal_copy_required);
        if (!ret || !normal_copy_required) {
            return ret;
        }
    }

    return mfu_copy_file_normal(src, dest, in_fd, out_fd, offset, length, file_size);
}

/* After receiving all incoming chunks, process open and write their chunks 
 * to the files. The process which writes the last chunk to each file also 
 * truncates the file to correct size.  A 0-byte file still has one chunk. */
void mfu_copy_files(mfu_flist list, uint64_t chunk_size)
{
    /* get current rank */
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    /* indicate which phase we're in to user */
    if (rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Copying data.");
    }
    
    /* split file list into a linked list of file sections,
     * this evenly spreads the file sections across processes */
    mfu_file_chunk* p = mfu_file_chunk_list_alloc(list, chunk_size);
    
    /* loop over and copy data for each file section we're responsible for */
    while (p != NULL) {
        /* get name of destination file */
        char* dest_path = DCOPY_build_dest(p->name);

        /* No need to copy it */
        if (dest_path == NULL) {
            continue;
        }

        /* call copy_file for each element of the copy_elem linked list of structs */
        mfu_copy_file(p->name, dest_path, (off_t)p->offset, (off_t)p->length, p->file_size);

        /* free the dest name */
        mfu_free(&dest_path);

        /* update pointer to next element */
        p = p->next;
    }
    
    /* free the linked list */
    mfu_file_chunk_list_free(&p);
}

void mfu_flist_copy(mfu_flist src_cp_list, int preserve, int do_sync)
{
    /* TODO: consider file system striping params here */
    /* hard code some configurables for now */

    /* Set default block size */
    DCOPY_user_opts.block_size = FD_BLOCK_SIZE;

    /* allocate buffer to read/write files, aligned on 1MB boundaraies */
    size_t alignment = 1024*1024;
    DCOPY_user_opts.block_buf1 = (char*) MFU_MEMALIGN(DCOPY_user_opts.block_size, alignment);
    DCOPY_user_opts.block_buf2 = (char*) MFU_MEMALIGN(DCOPY_user_opts.block_size, alignment);

    /* split items in file list into sublists depending on their
     * directory depth */
    int levels, minlevel;
    mfu_flist* lists;
    mfu_flist_array_by_depth(src_cp_list, &levels, &minlevel, &lists);

    /* TODO: filter out files that are bigger than 0 bytes if we can't read them */

    /* create directories, from top down */
    mfu_create_directories(levels, minlevel, lists);

    /* create files and links */
    mfu_create_files(levels, minlevel, lists);

    /* copy data */
    mfu_copy_files(src_cp_list, DCOPY_user_opts.chunk_size);

    /* close files */
    mfu_copy_close_file(&mfu_copy_src_cache);
    mfu_copy_close_file(&mfu_copy_dst_cache);

    /* set permissions, ownership, and timestamps if needed */
    mfu_copy_set_metadata(levels, minlevel, lists);

    /* free our lists of levels */
    mfu_flist_array_free(levels, &lists);

    /* free buffers */
    mfu_free(&DCOPY_user_opts.block_buf1);
    mfu_free(&DCOPY_user_opts.block_buf2);

    return;
}
