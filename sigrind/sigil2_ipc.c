#include "sigil2_ipc.h"
#include "coregrind/pub_core_libcfile.h"
#include "coregrind/pub_core_aspacemgr.h"
#include "coregrind/pub_core_syscall.h"
#include "pub_tool_basics.h"
#include "pub_tool_vki.h"       // errnum, vki_timespec
#include "pub_tool_vkiscnums.h" // __NR_nanosleep

/* IPC channels */
static Bool initialized = False;
static Int emptyfd;
static Int fullfd;
static Sigil2DBISharedData* shmem;

/* cached IPC state */
static UInt           curr_idx;  //buffer index
static EventBuffer*   curr_buf;  //buffer
static BufferedSglEv* curr_slot; //Sigil2 event
static char*          pool_slot; //function name
static Bool           is_full[SIGIL2_DBI_BUFFERS]; //track available buffers


static inline void set_and_init_buffer(UInt buf_idx)
{
    curr_buf = shmem->buf + buf_idx;

    curr_buf->events_used = 0;
    curr_slot = curr_buf->events + curr_buf->events_used;

    curr_buf->pool_used = 0;
    pool_slot = curr_buf->pool + curr_buf->pool_used;
}


static inline void flush_to_sigil2(void)
{
    /* Mark that the buffer is being flushed,
     * and tell Sigil2 the buffer is ready to consume */
    is_full[curr_idx] = True;
    Int res = VG_(write)(fullfd, &curr_idx, sizeof(curr_idx));
    if (res != sizeof(curr_idx))
    {
        VG_(umsg)("error VG_(write)\n");
        VG_(umsg)("error writing to Sigrind fifo\n");
        VG_(umsg)("Cannot recover from previous error. Good-bye.\n");
        VG_(exit)(1);
    }
}


static inline void set_next_buffer(void)
{
    /* try the next buffer, circular */
    ++curr_idx;
    if (curr_idx == SIGIL2_DBI_BUFFERS)
        curr_idx = 0;

    /* if the next buffer is full,
     * wait until Sigil2 communicates that it's free */
    if (is_full[curr_idx])
    {
        UInt buf_idx;
        Int res = VG_(read)(emptyfd, &buf_idx, sizeof(buf_idx));
        if (res != sizeof(buf_idx))
        {
            VG_(umsg)("error VG_(read)\n");
            VG_(umsg)("error reading from Sigrind fifo\n");
            VG_(umsg)("Cannot recover from previous error. Good-bye.\n");
            VG_(exit)(1);
        }

        tl_assert(buf_idx < SIGIL2_DBI_BUFFERS);
        tl_assert(buf_idx == curr_idx);
        curr_idx = buf_idx;
        is_full[curr_idx] = False;
    }

    set_and_init_buffer(curr_idx);
}


static inline Bool is_events_full(void)
{
    return curr_buf->events_used == SIGIL2_MAX_EVENTS;
}


static inline Bool is_pool_full(UInt size)
{
    return (curr_buf->pool_used + size) > SIGIL2_POOL_BYTES;
}


BufferedSglEv* SGL_(acq_event_slot)()
{
    tl_assert(initialized == True);

    if (is_events_full())
    {
        flush_to_sigil2();
        set_next_buffer();
    }

    curr_buf->events_used++;
    return curr_slot++;
}


EventPoolSlotTuple SGL_(acq_event_pool_slot)(UInt size)
{
    tl_assert(initialized == True);

    if (is_events_full() || is_pool_full(size))
    {
        flush_to_sigil2();
        set_next_buffer();
    }

    EventPoolSlotTuple tuple = {curr_slot, pool_slot, curr_buf->pool_used};
    curr_buf->events_used += 1;
    curr_buf->pool_used   += size;
    curr_slot             += 1;
    pool_slot             += size;

    return tuple;
}


/******************************
 * Initialization/Termination
 ******************************/
static int open_fifo(const HChar *fifo_path, int flags)
{
    tl_assert(initialized == False);

    int fd;
    SysRes res = VG_(open) (fifo_path, flags, 0600);
    if (sr_isError (res))
    {
        VG_(umsg) ("error %lu %s\n", sr_Err(res), VG_(strerror)(sr_Err(res)));
        VG_(umsg)("cannot open fifo file %s\n", fifo_path);
        VG_(umsg)("Cannot recover from previous error. Good-bye.\n");
        VG_(exit) (1);
    }

    fd = sr_Res(res);
    fd = VG_(safe_fd)(fd);
    if (fd == -1)
    {
        VG_(umsg)("FIFO for Sigrind failed\n");
        VG_(umsg)("Cannot recover from previous error. Good-bye.\n");
        VG_(exit) (1);
    }

    return fd;
}


static Sigil2DBISharedData* open_shmem(const HChar *shmem_path, int flags)
{
    tl_assert(initialized == False);

    SysRes res = VG_(open)(shmem_path, flags, 0600);
    if (sr_isError (res))
    {
        VG_(umsg)("error %lu %s\n", sr_Err(res), VG_(strerror)(sr_Err(res)));
        VG_(umsg)("Cannot open shared_mem file %s\n", shmem_path);
        VG_(umsg)("Cannot recover from previous error. Good-bye.\n");
        VG_(exit)(1);
    }

    int shared_mem_fd = sr_Res(res);
    res = VG_(am_shared_mmap_file_float_valgrind)(sizeof(Sigil2DBISharedData),
                                                  VKI_PROT_READ|VKI_PROT_WRITE,
                                                  shared_mem_fd, (Off64T)0);
    if (sr_isError(res))
    {
        VG_(umsg)("error %lu %s\n", sr_Err(res), VG_(strerror)(sr_Err(res)));
        VG_(umsg)("error VG_(am_shared_mmap_file_float_valgrind) %s\n", shmem_path);
        VG_(umsg)("Cannot recover from previous error. Good-bye.\n");
        VG_(exit)(1);
    }

    Addr addr_shared = sr_Res (res);
    VG_(close)(shared_mem_fd);

    return (Sigil2DBISharedData*) addr_shared;
}


void SGL_(init_IPC)()
{
    tl_assert(initialized == False);

    if (SGL_(clo).ipc_dir == NULL)
    {
       VG_(fmsg)("No --ipc-dir argument found, shutting down...\n");
       VG_(exit)(1);
    }

    Int ipc_dir_len = VG_(strlen)(SGL_(clo).ipc_dir);
    Int filename_len;

    //len is strlen + null + other chars (/ and -0)
    filename_len = ipc_dir_len + VG_(strlen)(SIGIL2_DBI_SHMEM_NAME) + 4;
    HChar shmem_path[filename_len];
    VG_(snprintf)(shmem_path, filename_len, "%s/%s-0", SGL_(clo).ipc_dir, SIGIL2_DBI_SHMEM_NAME);

    filename_len = ipc_dir_len + VG_(strlen)(SIGIL2_DBI_EMPTYFIFO_NAME) + 4;
    HChar emptyfifo_path[filename_len];
    VG_(snprintf)(emptyfifo_path, filename_len, "%s/%s-0", SGL_(clo).ipc_dir, SIGIL2_DBI_EMPTYFIFO_NAME);

    filename_len = ipc_dir_len + VG_(strlen)(SIGIL2_DBI_FULLFIFO_NAME) + 4;
    HChar fullfifo_path[filename_len];
    VG_(snprintf)(fullfifo_path, filename_len, "%s/%s-0", SGL_(clo).ipc_dir, SIGIL2_DBI_FULLFIFO_NAME);

#if defined(VGO_linux) && defined(VGA_amd64)
    /* TODO any serious implications in Valgrind of calling syscalls directly?
     * MDL20170220 The "VG_(syscall)" wrappers don't look like they do much
     * else besides doing platform specific setup.
     * In our case, we only accommodate x86_64 or aarch64. */
    struct vki_timespec req;
    req.tv_sec = 0;
    req.tv_nsec = 500000000;
    /* wait some time before trying to connect,
     * giving Sigil2 time to bring up IPC */
    VG_(do_syscall2)(__NR_nanosleep, (UWord)&req, 0);
#else
#error "Only linux is supported"
#endif

    /* XXX Valgrind might get stuck waiting for Sigil
     * if Sigil unexpectedly exits before trying
     * to connect; unlikely, but could implement timeout */
    emptyfd = open_fifo(emptyfifo_path, VKI_O_RDONLY);
    fullfd  = open_fifo(fullfifo_path, VKI_O_WRONLY);
    shmem   = open_shmem(shmem_path, VKI_O_RDWR);

    /* initialize cached IPC state */
    curr_idx = 0;
    set_and_init_buffer(curr_idx);
    for (UInt i=0; i<SIGIL2_DBI_BUFFERS; ++i)
        is_full[i] = False;

    initialized = True;
}


void SGL_(term_IPC)(void)
{
    tl_assert(initialized == True);

    /* send finish sequence */
    UInt finished = SIGIL2_DBI_FINISHED;
    if (VG_(write)(fullfd, &curr_idx, sizeof(curr_idx)) != sizeof(curr_idx) ||
        VG_(write)(fullfd, &finished, sizeof(finished)) != sizeof(finished))
    {
        VG_(umsg)("error VG_(write)\n");
        VG_(umsg)("error writing to Sigrind fifo\n");
        VG_(umsg)("Cannot recover from previous error. Good-bye.\n");
        VG_(exit)(1);
    }

    /* wait until Sigrind disconnects */
    while (VG_(read)(emptyfd, &finished, sizeof(finished)) > 0);

    VG_(close)(emptyfd);
    VG_(close)(fullfd);
}
