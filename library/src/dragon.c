#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include "dragon.h"

#define PSF_DIR "/proc/self/fd"
#define NVIDIA_UVM_PATH "/dev/nvidia-uvm"
#define DRAGON_IOCTL_INIT 1000
#define DRAGON_IOCTL_MAP 1001
#define DRAGON_IOCTL_TRASH_NRBLOCKS 1002
#define DRAGON_IOCTL_TRASH_RESERVED_NRPAGES 1003
#define DRAGON_IOCTL_REMAP 1004

#define MIN_SIZE ((size_t)1 << 21)
#define DEFAULT_TRASH_NR_BLOCKS 16
#define DEFAULT_TRASH_NR_RESERVED_PAGES ((unsigned long)1 << 21)

#define DRAGON_ENVNAME_ENABLE_READ_CACHE    "DRAGON_ENABLE_READ_CACHE"
#define DRAGON_ENVNAME_ENABLE_LAZY_WRITE    "DRAGON_ENABLE_LAZY_WRITE"
#define DRAGON_ENVNAME_ENABLE_AIO_READ      "DRAGON_ENABLE_AIO_READ"
#define DRAGON_ENVNAME_ENABLE_AIO_WRITE     "DRAGON_ENABLE_AIO_WRITE"
#define DRAGON_ENVNAME_READAHEAD_TYPE       "DRAGON_READAHEAD_TYPE"
#define DRAGON_ENVNAME_NR_RESERVED_PAGES    "DRAGON_NR_RESERVED_PAGES"

#define DRAGON_INIT_FLAG_ENABLE_READ_CACHE      0x01
#define DRAGON_INIT_FLAG_ENABLE_LAZY_WRITE      0x02
#define DRAGON_INIT_FLAG_ENABLE_AIO_READ        0x04
#define DRAGON_INIT_FLAG_ENABLE_AIO_WRITE       0x08

static int nvidia_uvm_fd = -1;
static int fadvice = -1;

static int minsize = MIN_SIZE;
static int disabled_dragon = 0;

static GHashTable *addr_map;

typedef struct
{
    unsigned long trash_nr_blocks;
    unsigned long trash_reserved_nr_pages;
    unsigned short flags;
    unsigned int status;
} dragon_ioctl_init_t;

typedef struct
{
    int backing_fd;
    void *uvm_addr;
    size_t size;
    unsigned short flags;
    unsigned int status;
} dragon_ioctl_map_t;

static dragonError_t init_module()
{
    DIR *d;
    struct dirent *dir;
    char psf_path[512];
    char *psf_realpath;
    dragon_ioctl_init_t request;
    int status;

    char *env_val;
    char *endptr;
    long nr_pages = -1;

    d = opendir(PSF_DIR);
    if (d)
    {
        while ((dir = readdir(d)) != NULL)
        {
            if (dir->d_type == DT_LNK)
            {
                sprintf(psf_path, "%s/%s", PSF_DIR, dir->d_name);
                psf_realpath = realpath(psf_path, NULL);
                if (strcmp(psf_realpath, NVIDIA_UVM_PATH) == 0)
                    nvidia_uvm_fd = atoi(dir->d_name);
                free(psf_realpath);
                if (nvidia_uvm_fd >= 0)
                    break;
            }
        }
        closedir(d);
    }
    if (nvidia_uvm_fd < 0)
    {
        fprintf(stderr, "Cannot open %s\n", PSF_DIR);
        return D_ERR_UVM;
    }

    env_val = secure_getenv("DRAGON_MINSIZE");
    if (env_val) {
       sscanf(env_val, "%u", &minsize);
    }

    env_val = secure_getenv("NO_DRAGON");
    if (env_val) {
        disabled_dragon = 1;
	goto out;
    }

    request.trash_nr_blocks = DEFAULT_TRASH_NR_BLOCKS;
    request.trash_reserved_nr_pages = DEFAULT_TRASH_NR_RESERVED_PAGES;
    request.flags = 0;

    env_val = secure_getenv(DRAGON_ENVNAME_NR_RESERVED_PAGES);
    if (env_val && (nr_pages = strtol(env_val, &endptr, 10)) >= 0)
    {
        if (*env_val != '\0' && *endptr == '\0')
            request.trash_reserved_nr_pages = (unsigned long)nr_pages;
    }
    fprintf(stderr, "reserved_nr_pages: %lu\n", request.trash_reserved_nr_pages);

    env_val = secure_getenv(DRAGON_ENVNAME_ENABLE_READ_CACHE);
    if (!(env_val && strncasecmp(env_val, "no", 2) == 0))
        request.flags |= DRAGON_INIT_FLAG_ENABLE_READ_CACHE;

    env_val = secure_getenv(DRAGON_ENVNAME_ENABLE_LAZY_WRITE);
    if (!(env_val && strncasecmp(env_val, "no", 2) == 0))
        request.flags |= DRAGON_INIT_FLAG_ENABLE_LAZY_WRITE;

    env_val = secure_getenv(DRAGON_ENVNAME_ENABLE_AIO_READ);
    if (!(env_val && strncasecmp(env_val, "no", 2) == 0))
        request.flags |= DRAGON_INIT_FLAG_ENABLE_AIO_READ;

    env_val = secure_getenv(DRAGON_ENVNAME_ENABLE_AIO_WRITE);
    if (!(env_val && strncasecmp(env_val, "no", 2) == 0))
        request.flags |= DRAGON_INIT_FLAG_ENABLE_AIO_WRITE;

    env_val = secure_getenv(DRAGON_ENVNAME_READAHEAD_TYPE);
    if (env_val && strncasecmp(env_val, "agg", 3) == 0)
    {
        fadvice = POSIX_FADV_SEQUENTIAL;
        fprintf(stderr, "Aggressive read ahead is enabled.\n");
    }
    else if (env_val && strncasecmp(env_val, "dis", 3) == 0)
    {
        fadvice = POSIX_FADV_RANDOM;
        fprintf(stderr, "Read ahead is disabled.\n");
    }
    else
        fadvice = POSIX_FADV_NORMAL;

    if ((status = ioctl(nvidia_uvm_fd, DRAGON_IOCTL_INIT, &request)) != 0)
    {
        fprintf(stderr, "ioctl init error: %d\n", status);
        return D_ERR_IOCTL;
    }

out:
    addr_map = g_hash_table_new(NULL, NULL);

    return D_OK;
}

#define BUFSIZE	(1024 * 4)

static dragonError_t
fillup_from_file(dragon_ioctl_map_t *request)
{
    size_t size;
    unsigned char *addr;

    if (!(request->flags & D_F_READ))
        return D_OK;

    addr = (unsigned char *)request->uvm_addr;
    size = request->size;
    while (size > 0)
    {
        char buf[BUFSIZE];
        int nread = BUFSIZE;

        if (nread > size)
            nread = size;
	nread = read(request->backing_fd, buf, nread);
	if (nread == 0)
            break;
	memcpy(addr, buf, nread);
	addr += nread;
        size -= nread;
    }

    return D_OK;
}

static void
flush_to_file(dragon_ioctl_map_t *request)
{
    size_t size;
    unsigned char *addr;

    if (!(request->flags & D_F_WRITE))
        return;

    addr = (unsigned char *)request->uvm_addr;
    size = request->size;
    while (size > 0)
    {
        char buf[BUFSIZE];
        int nwrite = BUFSIZE;

        if (nwrite > size)
            nwrite = size;

	memcpy(buf, addr, nwrite);
	nwrite = write(request->backing_fd, buf, nwrite);
	if (nwrite == 0)
            break;
	addr += nwrite;
        size -= nwrite;
    }
}

static dragonError_t
do_dragon_map(dragon_ioctl_map_t *request)
{
    int status;

    if ((request->flags & D_F_READ) && !(request->flags & D_F_VOLATILE))
    {
        if ((status = posix_fadvise(request->backing_fd, 0, 0, fadvice)) != 0)
            fprintf(stderr, "fadvise error: %d\n", status);
        if ((fadvice == POSIX_FADV_SEQUENTIAL) && readahead(request->backing_fd, 0, request->size) != 0)
            fprintf(stderr, "readahead error.\n");
    }

    if ((status = ioctl(nvidia_uvm_fd, DRAGON_IOCTL_MAP, request)) != 0)
    {
        fprintf(stderr, "ioctl error: %d\n", status);
	return D_ERR_IOCTL;
    }

    return D_OK;
}

static void free_request(dragon_ioctl_map_t *request)
{
    close(request->backing_fd);
    cudaFree(request->uvm_addr);
    free(request);
}

dragonError_t dragon_map(const char *filename, size_t size, unsigned short flags, void **paddr)
{
    int f_flags = 0;
    int f_fd;
    dragon_ioctl_map_t *request;
    cudaError_t error;
    int ret = D_OK;

    if ((request = (dragon_ioctl_map_t *)calloc(1, sizeof(dragon_ioctl_map_t))) == NULL)
    {
        fprintf(stderr, "Cannot calloc dragon_ioctl_map_t\n");
	return D_ERR_MEM;
    }

    error = cudaMallocManaged(paddr, size >= minsize ? size : minsize, cudaMemAttachGlobal);
    if (error != cudaSuccess)
    {
        fprintf(stderr, "cudaMallocManaged error: %s %s\n", cudaGetErrorName(error), cudaGetErrorString(error));
        return D_ERR_UVM;
    }

    if (nvidia_uvm_fd < 0)
    {
        ret = init_module();
        if (ret != D_OK) {
            cudaFree(*paddr);
	    return ret;
	}
    }

    f_flags = O_RDWR | O_LARGEFILE;
    if (flags & D_F_CREATE) {
        f_fd = creat(filename, S_IRUSR | S_IWUSR);
        if (f_fd >= 0)
            close(f_fd);
    }

    if ((f_fd = open(filename, f_flags)) < 0)
    {
        fprintf(stderr, "Cannot open the file %s\n", filename);
        ret = D_ERR_FILE;
	cudaFree(*paddr);
	return ret;
    }

    if ((flags & D_F_CREATE) && ftruncate(f_fd, size) != 0)
    {
        fprintf(stderr, "Cannot truncate the file %s\n", filename);
        ret = D_ERR_FILE;
	close(f_fd);
	cudaFree(*paddr);
	return ret;
    }

    request->backing_fd = f_fd;
    request->uvm_addr = *paddr;
    request->size = size;
    request->flags = flags;

    if (disabled_dragon)
        ret = fillup_from_file(request);
    else
        ret = do_dragon_map(request);

    if (ret == D_OK)
        g_hash_table_insert(addr_map, request->uvm_addr, request);
    else
	free_request(request);

    return ret;
}

dragonError_t dragon_remap(void *addr, unsigned short flags)
{
    int status;
    dragon_ioctl_map_t *request = g_hash_table_lookup(addr_map, addr);

    if (request == NULL)
    {
        fprintf(stderr, "%p is not mapped via dragon_map\n", addr);
        return D_ERR_INTVAL;
    }

    cudaDeviceSynchronize();

    request->flags = flags;

    if ((status = ioctl(nvidia_uvm_fd, DRAGON_IOCTL_REMAP, request)) != 0)
    {
        fprintf(stderr, "ioctl error: %d\n", status);
        return D_ERR_IOCTL;
    }

    return D_OK;
}

dragonError_t dragon_trash_set_num_blocks(unsigned long nrblocks)
{
    return D_ERR_NOT_IMPLEMENTED;
}

dragonError_t dragon_trash_set_num_reserved_sys_cache_pages(unsigned long nrpages)
{
    return D_ERR_NOT_IMPLEMENTED;
}

dragonError_t dragon_flush(void *addr)
{
    return D_ERR_NOT_IMPLEMENTED;
}

dragonError_t dragon_unmap(void *addr)
{
    dragon_ioctl_map_t *request = g_hash_table_lookup(addr_map, addr);

    if (request == NULL)
    {
        fprintf(stderr, "%p is not mapped via dragon_map\n", addr);
        return D_ERR_INTVAL;
    }

    if (disabled_dragon) {
        flush_to_file(request);
    }
    else {
        if ((request->flags & D_F_WRITE) && !(request->flags & D_F_VOLATILE))
             fsync(request->backing_fd);
    }

    g_hash_table_remove(addr_map, addr);
    free_request(request);

    return D_OK;
}
