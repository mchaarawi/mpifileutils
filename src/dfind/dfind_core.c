#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "mpi.h"
#include <uuid/uuid.h>
#include <gurt/common.h>
#include <gurt/hash.h>
#include <daos.h>
#include <daos_fs.h>

#include "../common/mfu.h"
#include "common.h"

static uuid_t pool_uuid;
static uuid_t cont_uuid;
static daos_handle_t poh;
static daos_handle_t coh;
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
static char *svc;
#endif
static char *dfs_prefix;
static int rank, ranks;

extern dfs_t *dfind_dfs;
extern struct d_hash_table *dir_hash;
extern double start_time;
extern int stonewall;

enum handleType {
        POOL_HANDLE,
        CONT_HANDLE,
	DFS_HANDLE
};

/* For DAOS methods. */
#define DCHECK(rc, format, ...)                                         \
do {                                                                    \
        int _rc = (rc);                                                 \
                                                                        \
        if (_rc != 0) {                                                  \
                fprintf(stderr, "ERROR (%s:%d): %d: %d: "               \
                        format"\n", __FILE__, __LINE__, rank, _rc,	\
                        ##__VA_ARGS__);                                 \
                fflush(stderr);                                         \
                exit(-1);                                       	\
        }                                                               \
} while (0)

/* Distribute process 0's pool or container handle to others. */
static void
HandleDistribute(enum handleType type)
{
        d_iov_t global;
        int        rc;

        global.iov_buf = NULL;
        global.iov_buf_len = 0;
        global.iov_len = 0;

        assert(type == POOL_HANDLE || type == CONT_HANDLE || type == DFS_HANDLE);
        if (rank == 0) {
                /* Get the global handle size. */
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(poh, &global);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_local2global(coh, &global);
                else
                        rc = dfs_local2global(dfind_dfs, &global);
                DCHECK(rc, "Failed to get global handle size");
        }

        MPI_Bcast(&global.iov_buf_len, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);

	global.iov_len = global.iov_buf_len;
        global.iov_buf = malloc(global.iov_buf_len);
        if (global.iov_buf == NULL)
		MPI_Abort(MPI_COMM_WORLD, -1);

        if (rank == 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(poh, &global);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_local2global(coh, &global);
                else
                        rc = dfs_local2global(dfind_dfs, &global);
                DCHECK(rc, "Failed to create global handle");
        }

        MPI_Bcast(global.iov_buf, global.iov_buf_len, MPI_BYTE, 0, MPI_COMM_WORLD);

        if (rank != 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_global2local(global, &poh);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_global2local(poh, global, &coh);
                else
                        rc = dfs_global2local(poh, coh, 0, global, &dfind_dfs);
                DCHECK(rc, "Failed to get local handle");
        }

        free(global.iov_buf);
}

int MFU_PRED_EXEC  (mfu_flist flist, uint64_t idx, void* arg);
int MFU_PRED_PRINT (mfu_flist flist, uint64_t idx, void* arg);

int MFU_PRED_EXEC (mfu_flist flist, uint64_t idx, void* arg)
{
    int argmax = 1024*1024;
    int written = 0;
    int ret;
    char* command = MFU_STRDUP((char*) arg);
    char* cmdline = (char*) MFU_MALLOC(argmax);
    char* subst = strstr(command, "{}");
    
    if (subst) {
        subst[0] = '\0';
        subst += 2; /* Point to the first char after '{}' */
    }

    const char* name = mfu_flist_file_get_name(flist, idx);

    written = snprintf(cmdline, argmax/sizeof(char), "%s%s%s", command, name, subst);
    if (written > argmax/sizeof(char)) {
        fprintf(stderr, "argument %s to exec too long.\n", cmdline);
        mfu_free(&cmdline);
        mfu_free(&command);
        return -1;
    }
    
    ret = system(cmdline);

    mfu_free(&cmdline);
    mfu_free(&command);

    return ret ? 0 : 1;
}

int MFU_PRED_PRINT (mfu_flist flist, uint64_t idx, void* arg)
{
    const char* name = mfu_flist_file_get_name(flist, idx);
    printf("%s\n", name);
    return 1;
}

static void print_usage(void)
{
    printf("\n");
    printf("Usage: dfind [options] <path> EXPRESSIONS...\n");
    printf("\n");
    printf("Options:\n");
    printf("  -i, --input <file>                      - read list from file\n");
    printf("  -o, --output <file>                     - write processed list to file\n");
    printf("  -v, --verbose                           - verbose output\n");
    printf("  -q, --quiet                             - quiet output\n");
    printf("  -h, --help                              - print usage\n");
    printf("\n");
    printf("Tests:\n");
    printf("  --amin N       - last accessed N minutes ago\n");
    printf("  --anewer FILE  - last accessed more recently than FILE modified\n");
    printf("  --atime N      - last accessed N days ago\n");
    printf("  --cmin N       - status last changed N minutes ago\n");
    printf("  --cnewer FILE  - status last changed more recently than FILE modified\n");
    printf("  --ctime N      - status last changed N days ago\n");
    printf("  --mmin N       - data last modified N minutes ago\n");
    printf("  --newer FILE   - modified more recently than FILE\n");
    printf("  --mtime N      - data last modified N days ago\n");
    printf("\n");
    printf("  --gid N        - numeric group ID is N\n");
    printf("  --group NAME   - belongs to group NAME\n");
    printf("  --uid N        - numeric user ID is N\n");
    printf("  --user NAME    - owned by user NAME\n");
    printf("\n");
    printf("  --name PATTERN - base name matches shell pattern PATTERN\n");
    printf("  --path PATTERN - full path matches shell pattern PATTERN\n");
    printf("  --regex REGEX  - full path matches POSIX regex REGEX\n");
    printf("\n");
    printf("  --size N       - size is N bytes.  Supports attached units like KB, MB, GB\n");
    printf("  --type C       - of type C: d=dir, f=file, l=symlink\n");
    printf("\n");
    printf("  Tests with N can use -N (less than N), N (exactly N), +N (more than N)\n");
    printf("\n");
    printf("Actions:\n");
    printf("  --print        - print item name to stdout\n");
    printf("  --exec CMD ;   - execute CMD on item\n");
    printf("\n");
    fflush(stdout);
    return;
}

/* apply predicate tests and actions to matching items in flist */
static void mfu_flist_pred(mfu_flist flist, mfu_pred* p)
{
    uint64_t idx;
    uint64_t size = mfu_flist_size(flist);
    for (idx = 0; idx < size; idx++) {
        mfu_pred_execute(flist, idx, p);
    }
    return;
}

/* look up mtimes for specified file,
 * return secs/nsecs in newly allocated mfu_pred_times struct,
 * return NULL on error */
static mfu_pred_times* get_mtimes(const char* file)
{
#if 1
    mfu_param_path param_path;

    mfu_param_path_set(file, &param_path);
    if (! param_path.path_stat_valid) {
        return NULL;
    }

    mfu_pred_times* t = (mfu_pred_times*) MFU_MALLOC(sizeof(mfu_pred_times));
    mfu_stat_get_mtimes(&param_path.path_stat, &t->secs, &t->nsecs);
    mfu_param_path_free(&param_path);
    return t;
#endif
#if 0
    int rc; 
    struct stat buf;

    rc = lstat(file, &buf);
    if (rc != 0)
	    return NULL;

    mfu_pred_times* t = (mfu_pred_times*) MFU_MALLOC(sizeof(mfu_pred_times));
    mfu_stat_get_mtimes(&buf, &t->secs, &t->nsecs);

    return t;
#endif
}

static int add_type(mfu_pred* p, char t)
{
    mode_t* type = (mode_t*) MFU_MALLOC(sizeof(mode_t));
    switch (t) {
    case 'b':
        *type = S_IFBLK;
        break;
    case 'c':
        *type = S_IFCHR;
        break;
    case 'd':
        *type = S_IFDIR;
        break;
    case 'f':
        *type = S_IFREG;
        break;
    case 'l':
        *type = S_IFLNK;
        break;
    case 'p':
        *type = S_IFIFO;
        break;
    case 's':
        *type = S_IFSOCK;
        break;
    
    default:
        /* unsupported type character */
        mfu_free(&type);
        return -1;
        break;
    }

    /* add check for this type */
    mfu_pred_add(p, MFU_PRED_TYPE, (void *)type);
    return 1;
}

static void pred_commit (mfu_pred* p)
{
    int need_print = 1;

    mfu_pred* cur = p;
    while (cur) {
        if (cur->f == MFU_PRED_PRINT || cur->f == MFU_PRED_EXEC) {
            need_print = 0;
            break;
        }
        cur = cur->next;
    }
    
    if (need_print) {
//        mfu_pred_add(p, MFU_PRED_PRINT, NULL);
    }
}

int dfind_main (int argc, char** argv)
{
    /* initialize MPI */
    MPI_Init(&argc, &argv);
    mfu_init();

    /* get our rank and the size of comm_world */
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* pointer to mfu_walk_opts */
    mfu_walk_opts_t* walk_opts = mfu_walk_opts_new();
    walk_opts->use_stat = 0;

    /* capture current time for any time based queries,
     * to get a consistent value, capture and bcast from rank 0 */
    mfu_pred_times* now_t = mfu_pred_now();

    int ch;

    mfu_pred* pred_head = mfu_pred_new();
    char* inputname  = NULL;
    char* outputname = NULL;
    char *mnewer=NULL, *cnewer=NULL,*anewer=NULL;
    mfu_pred_times* t;
    int walk = 0;
    int text = 1;
    int rc;

    static struct option long_options[] = {
        {"input",     1, 0, 'i'},
        {"output",    1, 0, 'o'},
        {"verbose",   0, 0, 'v'},
        {"quiet",     0, 0, 'q'},
        {"help",      0, 0, 'h'},

        { "maxdepth", required_argument, NULL, 'd' },

        { "gid",      required_argument, NULL, 'g' },
        { "group",    required_argument, NULL, 'G' },
        { "uid",      required_argument, NULL, 'u' },
        { "user",     required_argument, NULL, 'U' },

        { "size",     required_argument, NULL, 's' },

        { "name",     required_argument, NULL, 'n' },
        { "path",     required_argument, NULL, 'P' },
        { "regex",    required_argument, NULL, 'r' },

        { "pool",     required_argument, NULL, 'x' },
        { "cont",     required_argument, NULL, 'y' },
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
        { "svcl",     required_argument, NULL, 'z' },
#endif
        { "prefix",   required_argument, NULL, 'X' },
        { "stonewall", required_argument, NULL, 'W' },

        { "amin",     required_argument, NULL, 'a' },
        { "mmin",     required_argument, NULL, 'm' },
        { "cmin",     required_argument, NULL, 'c' },

        { "atime",    required_argument, NULL, 'A' },
        { "mtime",    required_argument, NULL, 'M' },
        { "ctime",    required_argument, NULL, 'C' },

        { "anewer",   required_argument, NULL, 'B' },
        { "newer",    required_argument, NULL, 'N' },
        { "cnewer",   required_argument, NULL, 'D' },

        { "type",     required_argument, NULL, 't' },

        { "print",    no_argument,       NULL, 'p' },
        { "exec",     required_argument, NULL, 'e' },
        { NULL, 0, NULL, 0 },
    };

    options.maxdepth = INT_MAX;
    
    int usage = 0;
    while (1) {
        int c = getopt_long_only(
                    argc, argv, "i:o:t:vqh",
                    long_options, NULL
                );

        if (c == -1) {
            break;
        }

        int i;
        int space;
        char* buf;
        mfu_pred_times_rel* tr;
        regex_t* r;
        int ret;

        /* verbose by default */
        mfu_debug_level = MFU_LOG_VERBOSE;

    	switch (c) {
    	case 'e':
            space = 1024 * 1024;
    	    buf = (char *)MFU_MALLOC(space);
    	    for (i = optind-1; strcmp(";", argv[i]); i++) {
    	        if (i > argc) {
                    if (rank == 0) {
    	                printf("%s: exec missing terminating ';'\n", argv[0]);
                    }
    	            exit(1);
    	        }
    	        strncat(buf, argv[i], space);
    	        space -= strlen(argv[i]) + 1; /* save room for space or null */
    	        if (space <= 0) {
                    if (rank == 0) {
    	                printf("%s: exec argument list too long.\n", argv[0]);
                    }
    	            mfu_free(&buf);
    	            continue;
    	        }
    	        strcat(buf, " ");
    	        optind++;
    	    }
    	    buf[strlen(buf)] = '\0'; /* clobbers trailing space */
    	    mfu_pred_add(pred_head, MFU_PRED_EXEC, buf);
    	    break;
    
    	case 'd':
    	    options.maxdepth = atoi(optarg);
    	    break;

    	case 'g':
            /* TODO: error check argument */
    	    buf = MFU_STRDUP(optarg);
    	    mfu_pred_add(pred_head, MFU_PRED_GID, (void *)buf);
    	    break;

    	case 'G':
    	    buf = MFU_STRDUP(optarg);
    	    mfu_pred_add(pred_head, MFU_PRED_GROUP, (void *)buf);
    	    break;

    	case 'u':
            /* TODO: error check argument */
    	    buf = MFU_STRDUP(optarg);
    	    mfu_pred_add(pred_head, MFU_PRED_UID, (void *)buf);
    	    break;

    	case 'U':
    	    buf = MFU_STRDUP(optarg);
    	    mfu_pred_add(pred_head, MFU_PRED_USER, (void *)buf);
    	    break;

    	case 's':
    	    buf = MFU_STRDUP(optarg);
    	    mfu_pred_add(pred_head, MFU_PRED_SIZE, (void *)buf);
    	    break;

    	case 'n':
    	    mfu_pred_add(pred_head, MFU_PRED_NAME, MFU_STRDUP(optarg));
    	    break;
    	case 'P':
    	    mfu_pred_add(pred_head, MFU_PRED_PATH, MFU_STRDUP(optarg));
    	    break;
    	case 'r':
            r = (regex_t*) MFU_MALLOC(sizeof(regex_t));
            ret = regcomp(r, optarg, 0);
            if (ret) {
                MFU_ABORT(-1, "Could not compile regex: `%s' rc=%d\n", optarg, ret);
            }
    	    mfu_pred_add(pred_head, MFU_PRED_REGEX, (void*)r);
    	    break;
    
    	case 'a':
            tr = mfu_pred_relative(optarg, now_t);
    	    mfu_pred_add(pred_head, MFU_PRED_AMIN, (void *)tr);
    	    break;
    	case 'm':
            tr = mfu_pred_relative(optarg, now_t);
    	    mfu_pred_add(pred_head, MFU_PRED_MMIN, (void *)tr);
    	    break;
    	case 'c':
            tr = mfu_pred_relative(optarg, now_t);
    	    mfu_pred_add(pred_head, MFU_PRED_CMIN, (void *)tr);
    	    break;

    	case 'A':
            tr = mfu_pred_relative(optarg, now_t);
    	    mfu_pred_add(pred_head, MFU_PRED_ATIME, (void *)tr);
    	    break;
    	case 'M':
            tr = mfu_pred_relative(optarg, now_t);
    	    mfu_pred_add(pred_head, MFU_PRED_MTIME, (void *)tr);
    	    break;
    	case 'C':
            tr = mfu_pred_relative(optarg, now_t);
    	    mfu_pred_add(pred_head, MFU_PRED_CTIME, (void *)tr);
    	    break;

    	case 'B':
	    anewer = MFU_STRDUP(optarg);
    	    break;
    	case 'N':
	    mnewer = MFU_STRDUP(optarg);
    	    break;
    	case 'D':
	    cnewer = MFU_STRDUP(optarg);
    	    break;
    
    	case 'p':
    	    mfu_pred_add(pred_head, MFU_PRED_PRINT, NULL);
    	    break;
    
    	case 't':
            ret = add_type(pred_head, *optarg);
            if (ret != 1) {
                if (rank == 0) {
    	            printf("%s: unsupported file type %s\n", argv[0], optarg);
                }
    	        exit(1);
            }
    	    break;

        case 'x':
	    ret = uuid_parse(optarg, pool_uuid);
	    if (ret) {
		    printf("%s: invalid pool uuid %s\n", argv[0], optarg);
		    exit(1);
	    }
    	    break;
        case 'y':
	    ret = uuid_parse(optarg, cont_uuid);
	    if (ret) {
		    printf("%s: invalid container uuid %s\n", argv[0], optarg);
		    exit(1);
	    }
    	    break;
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
        case 'z':
    	    svc = MFU_STRDUP(optarg);
    	    break;
#endif
        case 'X':
    	    dfs_prefix = MFU_STRDUP(optarg);
    	    break;
	case 'W':
            stonewall = atoi(optarg);
	    break;
        case 'i':
            inputname = MFU_STRDUP(optarg);
            break;
        case 'o':
            outputname = MFU_STRDUP(optarg);
            break;
        case 'v':
            mfu_debug_level = MFU_LOG_VERBOSE;
            break;
        case 'q':
            mfu_debug_level = MFU_LOG_NONE;
            break;
        case 'h':
            usage = 1;
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

    rc = daos_init();
    DCHECK(rc, "Failed to initialize daos");

    if (rank == 0) {
	daos_pool_info_t pool_info;
	daos_cont_info_t co_info;

#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
        d_rank_list_t *svcl = NULL;

	svcl = daos_rank_list_parse(svc, ":");
	if (svcl == NULL)
		MPI_Abort(MPI_COMM_WORLD, -1);

	/** Connect to DAOS pool */
	rc = daos_pool_connect(pool_uuid, NULL, svcl, DAOS_PC_RW,
			       &poh, &pool_info, NULL);
	d_rank_list_free(svcl);
#else
	rc = daos_pool_connect(pool_uuid, NULL, DAOS_PC_RW,
			       &poh, &pool_info, NULL);
#endif
	DCHECK(rc, "Failed to connect to pool");

	rc = daos_cont_open(poh, cont_uuid, DAOS_COO_RW, &coh, &co_info, NULL);
	DCHECK(rc, "Failed to open container");

	rc = dfs_mount(poh, coh, O_RDWR, &dfind_dfs);
	DCHECK(rc, "Failed to mount DFS namespace");
    }

    HandleDistribute(POOL_HANDLE);
    HandleDistribute(CONT_HANDLE);
    HandleDistribute(DFS_HANDLE);

    if (dfs_prefix) {
	    rc = dfs_set_prefix(dfind_dfs, dfs_prefix);
	    DCHECK(rc, "Failed to set DFS Prefix");
    }

    dir_hash = NULL;

    if (anewer) {
        t = get_mtimes(anewer);
	if (t == NULL) {
            if (rank == 0)
		printf("can't find file %s\n", anewer);
	    exit(1);
	}
	mfu_pred_add(pred_head, MFU_PRED_ANEWER, (void *)t);
	free(anewer);
    }
    if (mnewer) {
        t = get_mtimes(mnewer);
	if (t == NULL) {
            if (rank == 0)
		printf("can't find file %s\n", mnewer);
	    exit(1);
	}
	mfu_pred_add(pred_head, MFU_PRED_MNEWER, (void *)t);
	free(mnewer);
    }
    if (cnewer) {
        t = get_mtimes(cnewer);
	if (t == NULL) {
            if (rank == 0)
		printf("can't find file %s\n", cnewer);
	    exit(1);
	}
	mfu_pred_add(pred_head, MFU_PRED_CNEWER, (void *)t);
	free(cnewer);
    }

    pred_commit(pred_head);

    /* paths to walk come after the options */
    int numpaths = 0;
    mfu_param_path* paths = NULL;
    if (optind < argc) {
        /* got a path to walk */
        walk = 1;

        /* determine number of paths specified by user */
        numpaths = argc - optind;

        /* allocate space for each path */
        paths = (mfu_param_path*) MFU_MALLOC((size_t)numpaths * sizeof(mfu_param_path));

        /* process each path */
        char** p = &argv[optind];
        mfu_param_path_set_all((uint64_t)numpaths, (const char**)p, paths);
        optind += numpaths;

        /* don't allow user to specify input file with walk */
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
    
    if (usage) {
        if (rank == 0) {
            print_usage();
        }
        mfu_finalize();
        MPI_Finalize();
        return 0;
    }

    /* create an empty file list */
    mfu_flist flist = mfu_flist_new();

    if (walk) {
        /* walk list of input paths */
        mfu_flist_walk_param_paths(numpaths, paths, walk_opts, flist);
    }
    else {
        /* read data from cache file */
        mfu_flist_read_cache(inputname, flist);
    }

    /* apply predicates to each item in list */
    mfu_flist flist2 = mfu_flist_filter_pred(flist, pred_head);

    /* write data to cache file */
    if (outputname != NULL) {
        if (!text) {
            mfu_flist_write_cache(outputname, flist2);
        } else {
            mfu_flist_write_text(outputname, flist2);
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    fflush(stdout);

    if (rank == 0)
	    MFU_LOG(MFU_LOG_INFO, "Full Scanned List:");
    mfu_flist_print_summary(flist);
    if (rank == 0)
	    MFU_LOG(MFU_LOG_INFO, "Matched List:");
    mfu_flist_print_summary(flist2);

    mfu_flist_summarize(flist);
    mfu_flist_summarize(flist2);

    if (rank == 0) {
	    uint64_t total, matched;

	    total = mfu_flist_global_size(flist);
	    matched = mfu_flist_global_size(flist2);
	    printf("MATCHED %llu/%llu\n", matched, total);
    
    }

    /* free off the filtered list */
    mfu_flist_free(&flist2);

    /* free users, groups, and files objects */
    mfu_flist_free(&flist);

    /* free predicate list */
    mfu_pred_free(&pred_head);

    /* free memory allocated for options */
    mfu_free(&outputname);
    mfu_free(&inputname);

    /* free the path parameters */
    mfu_param_path_free_all(numpaths, paths);

    /* free memory allocated to hold params */
    mfu_free(&paths);

    /* free structure holding current time */
    mfu_free(&now_t);

    /* free the walk options */
    mfu_walk_opts_delete(&walk_opts);

    if (dir_hash)
      d_hash_table_destroy(dir_hash, true /* force */);

    rc = dfs_umount(dfind_dfs);
    DCHECK(rc, "Failed to umount DFS namespace");
    MPI_Barrier(MPI_COMM_WORLD);

    rc = daos_cont_close(coh, NULL);
    DCHECK(rc, "Failed to close container (%d)", rc);
    MPI_Barrier(MPI_COMM_WORLD);

    rc = daos_pool_disconnect(poh, NULL);
    DCHECK(rc, "Failed to disconnect from pool");
    MPI_Barrier(MPI_COMM_WORLD);

    rc = daos_fini();
    DCHECK(rc, "Failed to finalize DAOS");

    /* shut down MPI */
    mfu_finalize();
    MPI_Finalize();

    return 0;
}

pfind_find_results_t * pfind_find(pfind_options_t * opt)
{
    pfind_find_results_t *res;

    res = malloc(sizeof(pfind_find_results_t));
    memset(res, 0, sizeof(*res));

    mfu_init();

    /* get our rank and the size of comm_world */
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* pointer to mfu_walk_opts */
    mfu_walk_opts_t* walk_opts = mfu_walk_opts_new();
    walk_opts->use_stat = 0;

    /* capture current time for any time based queries,
     * to get a consistent value, capture and bcast from rank 0 */
    mfu_pred_times* now_t = mfu_pred_now();

    int ch;

    mfu_pred* pred_head = mfu_pred_new();
    char* inputname  = NULL;
    char* outputname = NULL;
    char *mnewer=NULL, *cnewer=NULL,*anewer=NULL;
    mfu_pred_times* t;
    int walk = 0;
    int text = 1;
    int rc;

    options.maxdepth = INT_MAX;
    
    int i;
    int space;
    char* buf;
    mfu_pred_times_rel* tr;
    regex_t* r;
    int ret;

    /* use the env variables to retrieve the pool and container */
    char *pool_str = getenv("DAOS_POOL");
    uuid_parse(pool_str, pool_uuid);

    char *cont_str = getenv("DAOS_CONT");
    uuid_parse(cont_str, cont_uuid);

#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
    svc = getenv("DAOS_SVCL");
#endif

    dfs_prefix = getenv("DAOS_FUSE");

    rc = daos_init();
    DCHECK(rc, "Failed to initialize daos");

    if (rank == 0) {
	daos_pool_info_t pool_info;
	daos_cont_info_t co_info;

#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
        d_rank_list_t *svcl = NULL;

	svcl = daos_rank_list_parse(svc, ":");
	if (svcl == NULL)
		MPI_Abort(MPI_COMM_WORLD, -1);

	/** Connect to DAOS pool */
	rc = daos_pool_connect(pool_uuid, NULL, svcl, DAOS_PC_RW,
			       &poh, &pool_info, NULL);
	d_rank_list_free(svcl);
#else
	rc = daos_pool_connect(pool_uuid, NULL, DAOS_PC_RW,
			       &poh, &pool_info, NULL);
#endif
	DCHECK(rc, "Failed to connect to pool");

	rc = daos_cont_open(poh, cont_uuid, DAOS_COO_RW, &coh, &co_info, NULL);
	DCHECK(rc, "Failed to open container");

	rc = dfs_mount(poh, coh, O_RDWR, &dfind_dfs);
	DCHECK(rc, "Failed to mount DFS namespace");
    }

    HandleDistribute(POOL_HANDLE);
    HandleDistribute(CONT_HANDLE);
    HandleDistribute(DFS_HANDLE);

    if (dfs_prefix) {
	    rc = dfs_set_prefix(dfind_dfs, dfs_prefix);
	    DCHECK(rc, "Failed to set DFS Prefix");
    }

    dir_hash = NULL;

    /* verbose by default */
    mfu_debug_level = MFU_LOG_VERBOSE;

    char *tmp;
    asprintf(&tmp, "%"PRIu64, opt->size);
    mfu_pred_add(pred_head, MFU_PRED_SIZE, (void *)tmp);

    if (opt->name_pattern) {
	    mfu_pred_add(pred_head, MFU_PRED_NAME, opt->name_pattern);
    }

    if (opt->timestamp_file) {
	    t = get_mtimes(opt->timestamp_file);
	    if (t == NULL) {
		    if (rank == 0)
			    printf("can't find file %s\n", mnewer);
		    exit(1);
	    }
	    mfu_pred_add(pred_head, MFU_PRED_MNEWER, (void *)t);
    }

    if (opt->stonewall)
	    stonewall = opt->stonewall;

    /* paths to walk */
    int numpaths = 0;
    mfu_param_path* paths = NULL;

    MPI_Barrier(MPI_COMM_WORLD);
    start_time = MPI_Wtime();

    if (opt->workdir) {
        /* got a path to walk */
        walk = 1;
        numpaths = 1;

        /* allocate space for each path */
        paths = (mfu_param_path*) MFU_MALLOC((size_t)numpaths * sizeof(mfu_param_path));

        /* process each path */
        char** p = &opt->workdir;
        mfu_param_path_set_all((uint64_t)numpaths, (const char**)p, paths);
    }

    pred_commit(pred_head);

    /* create an empty file list */
    mfu_flist flist = mfu_flist_new();

    if (walk) {
        /* walk list of input paths */
        mfu_flist_walk_param_paths(numpaths, paths, walk_opts, flist);
    }
    else {
        /* read data from cache file */
        mfu_flist_read_cache(inputname, flist);
    }

    /* apply predicates to each item in list */
    mfu_flist flist2 = mfu_flist_filter_pred(flist, pred_head);

    /* write data to cache file */
    if (outputname != NULL) {
        if (!text) {
            mfu_flist_write_cache(outputname, flist2);
        } else {
            mfu_flist_write_text(outputname, flist2);
        }
    }

    double end_time = MPI_Wtime();

    if (rank == 0)
	    MFU_LOG(MFU_LOG_INFO, "Full Scanned List:");
    mfu_flist_print_summary(flist);
    if (rank == 0)
	    MFU_LOG(MFU_LOG_INFO, "Matched List:");
    mfu_flist_print_summary(flist2);

    mfu_flist_summarize(flist);
    mfu_flist_summarize(flist2);

    uint64_t total, matched;

    total = mfu_flist_global_size(flist);
    matched = mfu_flist_global_size(flist2);

    if (rank == 0) {
	    printf("MATCHED %llu/%llu\n", matched, total);
    }

    //res->total_files = total;
    //res->found_files = matched;
    //double runtime = end_time - start_time;
    //MPI_Allreduce(&runtime, &res->runtime, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

    res->total_files = mfu_flist_size(flist);
    res->found_files = mfu_flist_size(flist2);
    res->errors = 0;
    res->unknown_file = 0;
    res->runtime = end_time - start_time;
    res->rate = res->total_files / res->runtime;
    res->com = MPI_COMM_WORLD;

    /* free off the filtered list */
    mfu_flist_free(&flist2);

    /* free users, groups, and files objects */
    mfu_flist_free(&flist);

    /* free predicate list */
    mfu_pred_free(&pred_head);

    /* free memory allocated for options */
    mfu_free(&outputname);
    mfu_free(&inputname);

    /* free the path parameters */
    mfu_param_path_free_all(numpaths, paths);

    /* free memory allocated to hold params */
    mfu_free(&paths);

    /* free structure holding current time */
    mfu_free(&now_t);

    /* free the walk options */
    mfu_walk_opts_delete(&walk_opts);

    if (dir_hash)
      d_hash_table_destroy(dir_hash, true /* force */);

    rc = dfs_umount(dfind_dfs);
    DCHECK(rc, "Failed to umount DFS namespace");
    MPI_Barrier(MPI_COMM_WORLD);

    rc = daos_cont_close(coh, NULL);
    DCHECK(rc, "Failed to close container (%d)", rc);
    MPI_Barrier(MPI_COMM_WORLD);

    rc = daos_pool_disconnect(poh, NULL);
    DCHECK(rc, "Failed to disconnect from pool");
    MPI_Barrier(MPI_COMM_WORLD);

    rc = daos_fini();
    DCHECK(rc, "Failed to finalize DAOS");

    mfu_finalize();

    return res;
}
