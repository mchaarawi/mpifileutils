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

#include <dirent.h>
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
#include <ctype.h>
#include <regex.h>

#include <pwd.h> /* for getpwent */
#include <grp.h> /* for getgrent */
#include <errno.h>
#include <string.h>

#include <libgen.h> /* dirname */

#include "libcircle.h"
#include "dtcmp.h"
#include "bayer.h"


/* TODO: change globals to struct */
static int walk_stat = 1;

struct perms {
    int octal;
    long mode_octal;
    int usr;
    int group;
    int all;
    int plus;
    int read;
    int write;
    int execute;
    int capital_execute;
    struct perms* next;
    int assignment;
    char source;
};

/*****************************
 * Driver functions
 ****************************/
static int lookup_gid(const char* name, gid_t* gid)
{
    uint64_t values[2];

    /* get our rank */
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    /* have rank 0 lookup the group id */
    if (rank == 0) {
        /* lookup specified group name */
        errno = 0;
        struct group* gr = getgrnam(name);
        if (gr != NULL) {
            /* lookup succeeded */
            values[0] = 1;
            values[1] = (uint64_t) gr->gr_gid;
        }
        else {
            /* indicate that lookup failed */
            values[0] = 0;

            /* print error message if we can */
            if (errno != 0) {
                BAYER_LOG(BAYER_LOG_ERR, "Failed to find entry for group %s errno=%d %s",
                          name, errno, strerror(errno)
                         );
            }
        }
    }

    /* broadcast result from lookup */
    MPI_Bcast(values, 2, MPI_UINT64_T, 0, MPI_COMM_WORLD);

    /* set the group id */
    int rc = (int) values[0];
    if (values[0] == 1) {
        *gid = (gid_t) values[1];
    }

    return rc;
}

static int parse_source(const char* str, struct perms* p)
{
    int rc = 1;
    p->source = '\0';
    if (strlen(str) == 1) {

        /* source can be only one of (u, g, or a) find it and keep a copy in p->source */
        if (str[0] == 'u') {
            p->source = 'u';
        }
        else if (str[0] == 'g') {
            p->source = 'g';
        }
        else if (str[0] == 'a') {
            p->source = 'a';
        }
        else {
            rc = 0;
        }
    }
    else {
        rc = 0;
    }
    return rc;
}

static int parse_rwx(const char* str, struct perms* p)
{
    int rc = 1;
    p->read = 0;
    p->write = 0;
    p->execute = 0;
    p->capital_execute = 0;

    /* set all of the r,w, and x flags */
    while (str[0] == 'r' || str[0] == 'w' || str[0] == 'x' || str[0] == 'X') {
        if (str[0] == 'r') {
            p->read = 1;
        }
        else if (str[0] == 'w') {
            p->write = 1;
        }
        else if (str[0] == 'x') {
            p->execute = 1;
        }
        else if (str[0] == 'X') {
            p->capital_execute = 1;
        }
        else if (rc != 1) {
            rc = 0;
        }
        str++;
    }
    return rc;
}

static int parse_plusminus(const char* str, struct perms* p)
{
    int rc = 1;
    p->plus = 0;
    p->assignment = 0;

    /* set the plus, minus, or equal flags */
    if (str[0] == '+') {
        p->plus = 1;
        str++;
        rc = parse_rwx(str, p);
    }
    else if (str[0] == '-') {
        p->plus = 0;
        str++;
        rc = parse_rwx(str, p);
    }
    else if (str[0] == '=') {
        p->assignment = 1;
        str++;
        rc = parse_source(str, p);
    }
    else if (rc != 1) {
        rc = 0;
    }
    return rc;
}

static int parse_uga(const char* str, struct perms* p)
{
    int rc = 1;
    p->usr = 0;
    p->group = 0;
    p->all = 0;

    /* set the user, group, and all flags */
    while (str[0] == 'u' || str[0] == 'g' || str[0] == 'a') {
        if (str[0] == 'u') {
            p->usr = 1;
        }
        else if (str[0] == 'g') {
            p->group = 1;
        }
        else if (str[0] == 'a') {
            p->all = 1;
        }
        else if (rc != 1) {
            rc = 0;
        }
        str++;
    }
    rc = parse_plusminus(str, p);
    return rc;
}

static void free_list(struct perms** p_head)
{
    struct perms* tmp;
    struct perms* head = *p_head;
    struct perms* current = head;

    /* free the memory for the linked list of structs */
    while (current != NULL) {
        tmp = current;
        bayer_free(&current);
        current = tmp->next;
    }
    *p_head = NULL;
}

static int parse_modebits(char* modestr, struct perms** p_head)
{
    int rc = 0;
    if (modestr != NULL) {
        rc = 1;
        int octal = 0;

        /* if it is octal then assume it will start with a digit */
        if (strlen(modestr) <= 4) {
            octal = 1;

            /* make sure you only have digits and is in the range 0 - 7 */
            for (int i = 0; i <= strlen(modestr) - 1; i++) {
                if (modestr[i] < '0' || modestr[i] > '7') {
                    octal = 0;
                    break;
                }
            }
        }

        /* if in octal mode then just create one node that head points to */
        if (octal) {
            rc = 1;
            struct perms* p = BAYER_MALLOC(sizeof(struct perms));
            p->next = NULL;
            p->octal = 1;
            p->mode_octal = strtol(modestr, NULL, 8);
            *p_head = p;

            /* if it is not in octal mode assume you are in symbolic mode */
        }
        else {
            struct perms* tail = NULL;

            /* make a copy of the input string in case there is an error with the input */
            char* tmpstr = BAYER_STRDUP(modestr);

            /* create a linked list of structs that gets broken up based on the comma syntax
             * i.e. u+r,g+x */
            for (char* token = strtok(tmpstr, ","); token != NULL; token = strtok(NULL, ",")) {

                /* allocate memory for a new struct and set the next pointer to null also
                 * turn octal mode off */
                struct perms* p = malloc(sizeof(struct perms));
                p->next = NULL;
                p->octal = 0;

                /* start parsing this 'token' of the input string */
                rc = parse_uga(token, p);

                /* if the tail is not null then point the tail at the latest struct/token */
                if (tail != NULL) {
                    tail->next = p;
                }

                /* if head is not pointing at anything then this token is the head of the list */
                if (*p_head == NULL) {
                    *p_head = p;
                }

                /* have the tail point at the current/last struct */
                tail = p;

                /* if there was an error parsing the string then free the memory of the list */
                if (rc != 1) {
                    free_list(p_head);
                    break;
                }
            }

            /* free the duplicated string */
            bayer_free(&tmpstr);
        }
    }
    return rc;
}

static void check_usr_input_perms(struct perms* head, int* dir_perms)
{
    /* extra flags to check if usr read and execute are being turned on */
    int usr_r = 0;
    int usr_x = 0;

    /* flag to check if the usr read & execute bits are being turned on */
    *dir_perms = 0;

    if (head->octal) {
        usr_r = 0;
        usr_x = 0;

        /* use a mask to check if the usr_read and usr_execute bits are being turned on */
        long usr_r_mask = 1 << 8;
        long usr_x_mask = 1 << 6;
        if (usr_r_mask & head->mode_octal) {
            usr_r = 1;
        }
        if (usr_x_mask & head->mode_octal) {
            usr_x = 1;
        }

        /* only set the dir_perms flag if both the user execute and user read flags are on */
        if (usr_r && usr_x) {
            *dir_perms = 1;
        }
    }
    else {
        struct perms* p = head;
        /* if in synbolic mode then loop through the linked list of structs to check for u+rx in input */
        while (p != NULL) {
            /* check if the execute and read are being turned on for each element of linked linked so
             * that if say someone does something like u+r,u+x (so dir_perms=1) or u+rwx,u-rx (dir_perms=0)
             * it will still give the correct result */
            if ((p->usr && p->plus) && (p->read)) {
                usr_r = 1;
            }
            if ((p->usr && (!p->plus)) && (p->read)) {
                usr_r = 0;
            }
            if ((p->usr && p->plus) && (p->execute || p->capital_execute)) {
                usr_x = 1;
            }
            if ((p->usr && (!p->plus)) && (p->execute || p->capital_execute)) {
                usr_x = 0;
            }

            /* update pointer to next element of linked list */
            p = p->next;
        }

        /* only set the dir_perms flag if both the user execute and user read flags are on */
        if (usr_r && usr_x) {
            *dir_perms = 1;
        }
    }
}

static void read_source_bits(struct perms* p, mode_t* bits, int* read, int* write, int* execute)
{
    *read = 0;
    *write = 0;
    *execute = 0;
    /* based on the source (u,g, or a) then check which bits were on for each one (r,w, or x) */
    if (p->source == 'u') {
        if (*bits & S_IRUSR) {
            *read = 1;
        }
        if (*bits & S_IWUSR) {
            *write = 1;
        }
        if (*bits & S_IXUSR) {
            *execute = 1;
        }
    }
    if (p->source == 'g') {
        if (*bits & S_IRGRP) {
            *read = 1;
        }
        if (*bits & S_IWGRP) {
            *write = 1;
        }
        if (*bits & S_IXGRP) {
            *execute = 1;
        }
    }
    if (p->source == 'a') {
        if (*bits & S_IROTH) {
            *read = 1;
        }
        if (*bits & S_IWOTH) {
            *write = 1;
        }
        if (*bits & S_IXOTH) {
            *execute = 1;
        }
    }
}

static void set_assign_bits(struct perms* p, mode_t* mode, int* read, int* write, int* execute)
{
    /* set the r, w, x bits on usr, group, and execute based on if the flags were set with parsing
     * the input string */
    if (p->usr) {
        if (*read) {
            *mode |= S_IRUSR;
        }
        else {
            *mode &= ~S_IRUSR;
        }
        if (*write) {
            *mode |= S_IWUSR;
        }
        else {
            *mode &= ~S_IWUSR;
        }
        if (*execute) {
            *mode |= S_IXUSR;
        }
        else {
            *mode &= ~S_IXUSR;
        }
    }
    if (p->group) {
        if (*read) {
            *mode |= S_IRGRP;
        }
        else {
            *mode &= ~S_IRGRP;
        }
        if (*write) {
            *mode |= S_IWGRP;
        }
        else {
            *mode &= ~S_IWGRP;
        }
        if (*execute) {
            *mode |= S_IXGRP;
        }
        else {
            *mode &= ~S_IXGRP;
        }
    }
    if (p->all) {
        if (*read) {
            *mode |= S_IROTH;
        }
        else {
            *mode &= ~S_IROTH;
        }
        if (*write) {
            *mode |= S_IWOTH;
        }
        else {
            *mode &= ~S_IWOTH;
        }
        if (*execute) {
            *mode |= S_IXOTH;
        }
        else {
            *mode &= ~S_IXOTH;
        }
    }
}

static void set_symbolic_bits(struct perms* p, mode_t* mode, bayer_filetype* type)
{
    /* set the bits based on flags set when parsing input string */
    if (p->usr) {
        if (p->plus) {
            if (p->read) {
                *mode |= S_IRUSR;
            }
            if (p->write) {
                *mode |= S_IWUSR;
            }
            if (p->execute) {
                *mode |= S_IXUSR;
            }
            if (p->capital_execute) {
                if (*type == BAYER_TYPE_DIR) {
                    *mode |= S_IXUSR;
                }
            }
        }
        else {
            if (p->read) {
                *mode &= ~S_IRUSR;
            }
            if (p->write) {
                *mode &= ~S_IWUSR;
            }
            if (p->execute) {
                *mode &= ~S_IXUSR;
            }
            if (p->capital_execute) {
                if (*type == BAYER_TYPE_DIR) {
                    *mode &= ~S_IXUSR;
                }
            }
        }
    }
    /* all & group check the capital_execute flag, so if there is a
    * capital X in the input string i.e g+X then if the usr execute
    * bit is set the group execute bit will be set to on. If the usr
    * execute bit is not on, then it will be left alone. If the usr
    * says something like ug+X then the usr bit in the input string
    * will be ignored unless it is a directory. In the case of something
    * like g-X, then it will ALWAYS turn off the group or all execute bit.
    * This is slightly different behavior then the +X, but it is intentional
    * and how chmod also works. */
    if (p->group) {
        if (p->plus) {
            if (p->read) {
                *mode |= S_IRGRP;
            }
            if (p->write) {
                *mode |= S_IWGRP;
            }
            if (p->execute) {
                *mode |= S_IXGRP;
            }
            if (p->capital_execute) {
                if (*mode & S_IXUSR || *type == BAYER_TYPE_DIR) {
                    *mode |= S_IXGRP;
                }
            }
        }
        else {
            if (p->read) {
                *mode &= ~S_IRGRP;
            }
            if (p->write) {
                *mode &= ~S_IWGRP;
            }
            if (p->execute) {
                *mode &= ~S_IXGRP;
            }
            if (p->capital_execute) {
                *mode &= ~S_IXGRP;
            }
        }
    }
    if (p->all) {
        if (p->plus) {
            if (p->read) {
                *mode |= S_IROTH;
            }
            if (p->write) {
                *mode |= S_IWOTH;
            }
            if (p->execute) {
                *mode |= S_IXOTH;
            }
            if (p->capital_execute) {
                if (*mode & S_IXUSR || *type == BAYER_TYPE_DIR) {
                    *mode |= S_IXOTH;
                }
            }
        }
        else {
            if (p->read) {
                *mode &= ~S_IROTH;
            }
            if (p->write) {
                *mode &= ~S_IWOTH;
            }
            if (p->execute) {
                *mode &= ~S_IXOTH;
            }
            if (p->capital_execute) {
                *mode &= ~S_IXOTH;
            }
        }
    }
}

static void set_modebits(struct perms* head, mode_t old_mode, mode_t* mode, bayer_filetype type)
{
    /* if in octal mode then loop through and check which ones are on based on the mask and
     * the current octal mode bits */
    if (head->octal) {
        *mode = (mode_t)0;
        /* array of constants to check which mode bits are on or off */
        mode_t permbits[12] = {S_ISUID, S_ISGID, S_ISVTX,
                               S_IRUSR, S_IWUSR, S_IXUSR,
                               S_IRGRP, S_IWGRP, S_IXGRP,
                               S_IROTH, S_IWOTH, S_IXOTH
                              };

        /* start with the bit all the way to the left (of 12 bits) on */
        long mask = 1 << 11;
        for (int i = 0; i < 12; i++) {
            /* use mask to check which bits are on and loop through
             * each element in the array of constants, and if it is
             * on (the mode bits pass in as input) then update the
             * current mode */
            if (mask & head->mode_octal) {
                *mode |= permbits[i];
            }
            /* move the 'on' bit to the right one each time through the loop */
            mask >>= 1;
        }
    }
    else {
        struct perms* p = head;
        *mode = old_mode;
        /* if in symbolic mode then loop through the linked list of structs */
        while (p != NULL) {
            /* if the assignment flag is set (equal's was found when parsing the input string)
             * then break it up into source & target i.e u=g, then the the taret is u and the source
             * is g. So, all of u's bits will be set the same as g's bits */
            if (p->assignment) {
                int read, write, execute;
                /* find the source bit with read_source_bits, only can be one at a time (u, g, or a), and
                 * set appropriate read, write, execute flags */
                read_source_bits(p, mode, &read, &write, &execute);
                /* if usr, group, or execute were on when parsing the input string then they are considered
                 * a target and the new mode is change accordingly in set_assign_bits */
                set_assign_bits(p, mode, &read, &write, &execute);
            }
            else {
                /* if the assignment flag is not set then just use
                 * regular symbolic notation to check if usr, group, or all is set, then
                 * plus/minus, and change new mode accordingly */
                set_symbolic_bits(p, mode, &type);
            }
            /* update pointer to next element of linked list */
            p = p->next;
        }
    }
}

static void dchmod_level(bayer_flist list, uint64_t* dchmod_count, const char* grname, struct perms* head, gid_t* gid)
{
    /* each process directly changes permissions on its elements for each level */
    uint64_t idx;
    uint64_t size = bayer_flist_size(list);
    for (idx = 0; idx < size; idx++) {
        /* get file name */
        const char* dest_path = bayer_flist_file_get_name(list, idx);

        /* update group if user gave a group name */
        if (grname != NULL) {
            /* get user id and group id of file */
            uid_t uid = (uid_t) bayer_flist_file_get_uid(list, idx);
            gid_t oldgid = (gid_t) bayer_flist_file_get_gid(list, idx);

            /* only bother to change group if it's different */
            if (oldgid != gid) {
                /* note that we use lchown to change ownership of link itself, it path happens to be a link */
                if (bayer_lchown(dest_path, uid, gid) != 0) {
                    /* TODO: are there other EPERM conditions we do want to report? */

                    /* since the user running dcp may not be the owner of the
                    * file, we could hit an EPERM error here, and the file
                    * will be left with the effective uid and gid of the dcp
                    * process, don't bother reporting an error for that case */
                    if (errno != EPERM) {
                        BAYER_LOG(BAYER_LOG_ERR, "Failed to change ownership on %s lchown() errno=%d %s",
                                  dest_path, errno, strerror(errno)
                                 );
                    }
                }
            }
        }
        if (head != NULL) {

            /* get mode and type */
            bayer_filetype type = bayer_flist_file_get_type(list, idx);

            /* if in octal mode skip this call */
            mode_t mode = 0;
            if (!head->octal) {
                mode = (mode_t) bayer_flist_file_get_mode(list, idx);
            }

            /* change mode, unless item is a link */
            if (type != BAYER_TYPE_LINK) {
                mode_t new_mode;
                set_modebits(head, mode, &new_mode, type);

                /* set the mode on the file */
                if (bayer_chmod(dest_path, new_mode) != 0) {
                    BAYER_LOG(BAYER_LOG_ERR, "Failed to change permissions on %s chmod() errno=%d %s",
                              dest_path, errno, strerror(errno)
                             );
                }
            }
        }
    }

    /* report number of permissions changed */
    *dchmod_count = size;

    return;
}

static bayer_flist* bayer_flist_filter_regex(bayer_flist flist, const char* exclude_exp, 
        const char* match_exp, int* match_flag, int* exclude_flag, int* name_flag,
        bayer_flist filtered_flist) {

    /* pointer to correct flist if it gets filtered. if not, just points to flist */
    bayer_flist* flist_ptr = &flist;
    char* regex_exp = NULL;

    /* set flags and reg expression */
    if (*exclude_flag) {
        regex_exp = BAYER_STRDUP(exclude_exp);
    } else if (*match_flag) {
        regex_exp = BAYER_STRDUP(match_exp);
    }

    /* check if user passed in an exclude or match expression, if so then filter the list */
    if (regex_exp != NULL) {
        regex_t regex;
        int regex_return;

        /* compile regular expression & if it fails print error */
        regex_return = regcomp(&regex, regex_exp, 0);
        if (regex_return) {
            printf(stderr, "could not compile regex\n");
        }

        uint64_t idx = 0;
        uint64_t size = bayer_flist_size(flist);

        /* initialize filtered_flist */
        filtered_flist = bayer_flist_subset(flist);
        
        /* copy the things that don't or do (based on input) match the regex into a 
         * filtered list */
        while (idx < size) {
            char* file_name = bayer_flist_file_get_name(flist, idx);

            /* create bayer_path object from a string path */
            bayer_path* pathname = bayer_path_from_str(file_name);
            /* get the last component of that path */
            int rc = bayer_path_basename(pathname);
            /* now get a string from the path */
            char* basename = NULL;
            if (rc == 0) {
                basename = bayer_path_strdup(pathname);
            }

            /* execute regex on each filename if user uses --name option then use 
             * basename (not full path) to match/exclude with */
            if (*name_flag) {
                regex_return = regexec(&regex, basename, 0, NULL, 0);
            } else {
                regex_return = regexec(&regex, file_name, 0, NULL, 0);
            }

            /* if it doesn't match then copy it to the filtered list */
            if (regex_return && *exclude_flag) {
                bayer_flist_file_copy(flist, idx, filtered_flist);
            } else if ((!regex_return) && *match_flag) {
                bayer_flist_file_copy(flist, idx, filtered_flist);
            }
            /* free bayer path object and set pointer to NULL */
            bayer_path_delete(&pathname);
            /* free the basename string */
            bayer_free(&basename);
            idx++;
        }

        /* summarize the filtered list */
        bayer_flist_summarize(filtered_flist);

        /* update pointer to point at filtered list */
        flist_ptr = &filtered_flist;
    }
    
    return flist_ptr;
}

static void flist_chmod(
    bayer_flist flist, const char* grname, struct perms* head, char* exclude_exp, 
    char* match_exp, int* match_flag, int* exclude_flag, int* name_flag, bayer_flist filtered_flist)
{
    /* use a pointer to flist to point at regular flist or filtered flist
     * if one is used */
    bayer_flist* flist_ptr = bayer_flist_filter_regex(flist, exclude_exp, match_exp, match_flag,
            exclude_flag, name_flag, filtered_flist);

    /* lookup groupid if set, bail out if not */
    gid_t gid;
    if (grname != NULL) {
        int lookup_rc = lookup_gid(grname, &gid);
        if (lookup_rc == 0) {
            /* failed to get group id, bail out */
            return;
        }
    }

    /* set up levels */
    int level;

    /* wait for all tasks and start timer */
    MPI_Barrier(MPI_COMM_WORLD);
    double start_dchmod = MPI_Wtime();

    /* split files into separate lists by directory depth */
    int levels, minlevel;
    bayer_flist* lists;
    bayer_flist_array_by_depth(*flist_ptr, &levels, &minlevel, &lists);

    /* remove files starting at the deepest level */
    for (level = levels - 1; level >= 0; level--) {
        double start = MPI_Wtime();

        /* get list of items for this level */
        bayer_flist list = lists[level];

        uint64_t size = 0;
        /* do a dchmod on each element in the list for this level & pass it the size */
        dchmod_level(list, &size, grname, head, &gid);

        /* wait for all processes to finish before we start with files at next level */
        MPI_Barrier(MPI_COMM_WORLD);

        double end = MPI_Wtime();

        if (bayer_debug_level >= BAYER_LOG_VERBOSE) {
            uint64_t min, max, sum;
            MPI_Allreduce(&size, &min, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);
            MPI_Allreduce(&size, &max, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
            MPI_Allreduce(&size, &sum, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
            double rate = 0.0;
            if (end - start > 0.0) {
                rate = (double)sum / (end - start);
            }
            double time_diff = end - start;
            if (bayer_rank == 0) {
                printf("level=%d min=%lu max=%lu sum=%lu rate=%f secs=%f\n",
                       (minlevel + level), (unsigned long)min, (unsigned long)max, (unsigned long)sum, rate, time_diff
                      );
                fflush(stdout);
            }
        }
    }

    /* free the array of lists */
    bayer_flist_array_free(levels, &lists);

    /* wait for all tasks & end timer */
    MPI_Barrier(MPI_COMM_WORLD);
    double end_dchmod = MPI_Wtime();

    /* report remove count, time, and rate */
    if (bayer_debug_level >= BAYER_LOG_VERBOSE && bayer_rank == 0) {
        uint64_t all_count = bayer_flist_global_size(*flist_ptr);
        double time_diff = end_dchmod - start_dchmod;
        double rate = 0.0;
        if (time_diff > 0.0) {
            rate = ((double)all_count) / time_diff;
        }
        printf("dchmod %lu items in %f seconds (%f items/sec)\n",
               all_count, time_diff, rate
              );
    }

    return;
}

static void print_usage(void)
{
    printf("\n");
    printf("Usage: dchmod [options] <path> ...\n");
    printf("\n");
    printf("Options:\n");
    printf("  -i, --input   <file>               - read list from file\n");
    printf("  -g, --group   <name>               - change group to specified group name\n");
    printf("  -m, --mode    <string>             - change mode\n");
    printf("  -e, --exclude <regular expression> - exclude a list of files from command\n");
    printf("  -a, --match   <regular expression> - match a list of files from command\n");
    printf("  -n, --name                         - exclude a list of files from command\n");
    printf("  -v, --verbose                      - verbose output\n");
    printf("  -h, --help                          - print usage\n");
    printf("\n");
    fflush(stdout);
    return;
}

int main(int argc, char** argv)
{
    int i;

    /* start and end time variables for dchmod */
    double start_write;
    double end_write;

    /* initialize MPI */
    MPI_Init(&argc, &argv);
    bayer_init();

    /* get our rank and the size of comm_world */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* parse command line options */
    char* inputname = NULL;
    char* groupname = NULL;
    char* modestr = NULL;
    char* exclude_pattern = NULL;
    char* match_pattern = NULL;
    struct perms* head = NULL;
    int walk = 0;
    /* flag used to check if permissions need to be 
     * set on the walk */
    int dir_perms = 0;
    /* flags used to filter the list */
    int name_flag = 0;
    int match_flag = 0;
    int exclude_flag = 0;

    int option_index = 0;
    static struct option long_options[] = {
        {"input",    1, 0, 'i'},
        {"group",    1, 0, 'g'},
        {"mode",     1, 0, 'm'},
        {"exclude",  1, 0, 'e'},
        {"match",    1, 0, 'a'},
        {"name",    0, 0, 'n'}, 
        {"help",     0, 0, 'h'},
        {"verbose",  0, 0, 'v'},
        {0, 0, 0, 0}
    };

    int usage = 0;
    while (1) {
        int c = getopt_long(
                    argc, argv, "i:g:m:e:a:nlhv",
                    long_options, &option_index
                );

        if (c == -1) {
            break;
        }

        switch (c) {
            case 'i':
                inputname = BAYER_STRDUP(optarg);
                break;
            case 'g':
                groupname = BAYER_STRDUP(optarg);
                break;
            case 'm':
                modestr = BAYER_STRDUP(optarg);
                break;
            case 'e':
                exclude_pattern = BAYER_STRDUP(optarg);
                exclude_flag = 1;
                break;
            case 'a':
                match_pattern = BAYER_STRDUP(optarg);
                match_flag = 1;
                break;
            case 'n':
                name_flag = 1;
                break;                
            case 'h':
                usage = 1;
                break;
            case 'v':
                bayer_debug_level = BAYER_LOG_VERBOSE;
                break;
            case '?':
                usage = 1;
                break;
            default:
                if (rank == 0) {
                    printf("?? getopt returned character code 0%o ??\n", c);
                }
        }
    }

    /* paths to walk come after the options */
    int numpaths = 0;
    bayer_param_path* paths = NULL;
    if (optind < argc) {
        /* got a path to walk */
        walk = 1;

        /* determine number of paths specified by user */
        numpaths = argc - optind;

        /* allocate space for each path */
        paths = (bayer_param_path*) BAYER_MALLOC((size_t)numpaths * sizeof(bayer_param_path));

        /* process each path */
        char** argpaths = &argv[optind];
        bayer_param_path_set_all(numpaths, argpaths, paths);

        /* advance to next set of options */
        optind += numpaths;

        /* don't allow input file and walk */
        if (inputname != NULL) {
            usage = 1;
        }
    }
    else {
        /* if we're not walking, we must be reading,
         * and for that we need a file */
        if (inputname == NULL) {
            usage = 1;
        }
    }
    if (modestr != NULL) {
        mode_t mode;
        int valid = parse_modebits(modestr, &head);
        if (!valid) {
            usage = 1;
            if (rank == 0) {
                printf("invalid mode string: %s\n", modestr);
            }
        }
    }
    /* print usage if we need to */
    if (usage) {
        if (rank == 0) {
            print_usage();
        }
        bayer_finalize();
        MPI_Finalize();
        return 1;
    }

    /* create an empty file list */
    bayer_flist flist = bayer_flist_new();

    /* for filtered list if exclude pattern is used */
    bayer_flist filtered_flist = NULL;

    check_usr_input_perms(head, &dir_perms);

    /* get our list of files, either by walking or reading an
     * input file */
    if (walk) {
        /* if in octal mode set walk_stat=0 */
        if (head->octal && groupname == NULL) {
            walk_stat = 0;
        }
        /* walk list of input paths */
        bayer_param_path_walk(numpaths, paths, walk_stat, flist, dir_perms);
    }
    else {
        /* read list from file */
        bayer_flist_read_cache(inputname, flist);
    }

    /* change group and permissions */
    flist_chmod(flist, groupname, head, exclude_pattern, match_pattern, &match_flag,
            &exclude_flag, &name_flag, filtered_flist);

    /* free filtered_flist if it was used */
    if (filtered_flist != NULL) {
        bayer_flist_free(&filtered_flist);
    }

    /* free the file list */
    bayer_flist_free(&flist);

    /* free the path parameters */
    bayer_param_path_free_all(numpaths, paths);

    /* free memory allocated to hold params */
    bayer_free(&paths);

    /* free the group name */
    bayer_free(&groupname);

    /* free the modestr */
    bayer_free(&modestr);

    /* free the exclude_pattern if it isn't null */
    if (exclude_pattern != NULL) {
        bayer_free(&exclude_pattern);
    }

    /* free the match_pattern if it isn't null */
    if (match_pattern != NULL) {
        bayer_free(&match_pattern);
    }

    /* free the head of the list */
    free_list(&head);

    /* free the input file name */
    bayer_free(&inputname);

    /* shut down MPI */
    bayer_finalize();
    MPI_Finalize();

    return 0;
}
