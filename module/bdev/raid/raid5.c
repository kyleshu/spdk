#include "bdev_raid.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/log.h"

#include <rte_hash.h>
#include <rte_memory.h>

struct stripe_request {
    /* The associated raid_bdev_io */
    struct raid_bdev_io *raid_io;

    /* The target stripe */
    struct stripe *stripe;

    /* Counter for remaining chunk requests */
    int remaining;

    /* Status of the request */
    enum spdk_bdev_io_status status;

    /* Function to call when all remaining chunk requests have completed */
    void (*chunk_requests_complete_cb)(struct stripe_request *);

    /* Offset into the parent bdev_io iovecs */
    uint64_t iov_offset;

    /* Initial iov_offset */
    uint64_t init_iov_offset;

    /* First data chunk applicable to this request */
    struct chunk *first_data_chunk;

    /* Last data chunk applicable to this request */
    struct chunk *last_data_chunk;

    /* The stripe's parity chunk */
    struct chunk *parity_chunk;

    /* Degraded chunk */
    struct chunk *degraded_chunk;

    /* Link for the stripe's requests list */
    TAILQ_ENTRY(stripe_request) link;

    /* Array of chunks corresponding to base_bdevs */
    struct chunk {
        /* Corresponds to base_bdev index */
        uint8_t index;

        /* Request offset from chunk start */
        uint64_t req_offset;

        /* Request blocks count */
        uint64_t req_blocks;

        /* Preread offset from chunk start */
        uint64_t preread_offset;

        /* Preread blocks count */
        uint64_t preread_blocks;

        /* The iovecs associated with the chunk request */
        struct iovec *iovs;

        /* The number of iovecs */
        int iovcnt;

        /* A single iovec for non-SG buffer request cases */
        struct iovec iov;

        /* The type of chunk request */
        enum chunk_request_type {
            CHUNK_READ,
            CHUNK_WRITE,
            CHUNK_PREREAD,
        } request_type;

        /* For retrying base bdev IOs in case submit fails with -ENOMEM */
        struct spdk_bdev_io_wait_entry waitq_entry;
    } chunks[0];
};

struct stripe {
    /* The stripe's index in the raid array. Also a key for the hash table. */
    uint64_t index;

    /* Hashed key value */
    hash_sig_t hash;

    /* List of requests queued for this stripe */
    TAILQ_HEAD(requests_head, stripe_request) requests;

    /* Protects the requests list */
    pthread_spinlock_t requests_lock; // Note: acquire lock when accessing request queue

    /* Stripe can be reclaimed if this reaches 0 */
    unsigned int refs;

    /* Link for the active/free stripes lists */
    TAILQ_ENTRY(stripe) link;

    /* Array of buffers for chunk parity/preread data */
    void **chunk_buffers;
};

struct raid5_info {
    /* The parent raid bdev */
    struct raid_bdev *raid_bdev;

    /* Number of data blocks in a stripe (without parity) */
    uint64_t stripe_blocks;

    /* Number of stripes on this array */
    uint64_t total_stripes;

    /* Mempool for stripe_requests */
    struct spdk_mempool *stripe_request_mempool;

    /* Pointer to an array of all available stripes */
    struct stripe *stripes;

    /* Hash table containing currently active stripes */
    struct rte_hash *active_stripes_hash; // Note: DPDK's implementation of hash table

    /* List of active stripes (in hash table) */
    TAILQ_HEAD(active_stripes_head, stripe) active_stripes;

    /* List of free stripes (not in hash table) */
    TAILQ_HEAD(, stripe) free_stripes;

    /* Lock protecting the stripes hash and lists */
    pthread_spinlock_t active_stripes_lock;
};

struct raid5_io_channel {
    TAILQ_HEAD(, spdk_bdev_io_wait_entry) retry_queue;
    TAILQ_HEAD(, iov_wrapper) iov_w_queue;
};

#define FOR_EACH_CHUNK(req, c) \
	for (c = req->chunks; \
	     c < req->chunks + req->raid_io->raid_bdev->num_base_bdevs; c++)

#define __NEXT_DATA_CHUNK(req, c) \
	c+1 == req->parity_chunk ? c+2 : c+1 // Note: only works for raid5

#define FOR_EACH_DATA_CHUNK(req, c) \
	for (c = __NEXT_DATA_CHUNK(req, req->chunks-1); \
	     c < req->chunks + req->raid_io->raid_bdev->num_base_bdevs; \
	     c = __NEXT_DATA_CHUNK(req, c))

static inline struct stripe_request *
raid5_chunk_stripe_req(struct chunk *chunk)
{
    return SPDK_CONTAINEROF((chunk - chunk->index), struct stripe_request, chunks);
}

static inline uint8_t
raid5_chunk_data_index(struct chunk *chunk)
{
    // Note: only works for raid5
    return chunk < raid5_chunk_stripe_req(chunk)->parity_chunk ? chunk->index : chunk->index - 1;
}

static inline struct chunk *
raid5_get_data_chunk(struct stripe_request *stripe_req, uint8_t chunk_data_idx)
{
    // Note: only works for raid5
    uint8_t p_chunk_idx = stripe_req->parity_chunk - stripe_req->chunks;

    return &stripe_req->chunks[chunk_data_idx < p_chunk_idx ? chunk_data_idx : chunk_data_idx + 1];
}

static inline uint8_t
raid5_stripe_data_chunks_num(const struct raid_bdev *raid_bdev)
{
    // Note: this also works for raid6
    return raid_bdev->num_base_bdevs - raid_bdev->module->base_bdevs_max_degraded;
}

#ifdef SPDK_CONFIG_ISAL
#include "isa-l/include/raid.h"
// Note: also need q for raid6, also this might not be the most efficient implementation
static void
raid5_xor_buf(void *restrict to, void *restrict from, size_t size)
{
	int ret;
	void *vects[3] = { from, to, to };

	ret = xor_gen(3, size, vects);
	if (ret) {
		SPDK_ERRLOG("xor_gen failed\n");
	}
}
#else
// Note: should not use this one
static void
raid5_xor_buf(void *restrict to, void *restrict from, size_t size)
{
    long *_to = to;
    long *_from = from;
    size_t i;

    assert(size % sizeof(*_to) == 0);

    size /= sizeof(*_to);

    for (i = 0; i < size; i++) {
        _to[i] ^= _from[i];
    }
}
#endif

// Note: need q for raid6
static void
raid5_xor_iovs(struct iovec *iovs_dest, int iovs_dest_cnt, size_t iovs_dest_offset,
               const struct iovec *iovs_src, int iovs_src_cnt, size_t iovs_src_offset,
               size_t size)
{
    struct iovec *v1;
    const struct iovec *v2;
    size_t off1, off2;
    size_t n;

    v1 = iovs_dest;
    v2 = iovs_src;

    n = 0;
    off1 = 0;
    while (v1 < iovs_dest + iovs_dest_cnt) {
        n += v1->iov_len;
        if (n > iovs_dest_offset) {
            off1 = v1->iov_len - (n - iovs_dest_offset);
            break;
        }
        v1++;
    }

    n = 0;
    off2 = 0;
    while (v2 < iovs_src + iovs_src_cnt) {
        n += v2->iov_len;
        if (n > iovs_src_offset) {
            off2 = v2->iov_len - (n - iovs_src_offset);
            break;
        }
        v2++;
    }

    while (v1 < iovs_dest + iovs_dest_cnt &&
           v2 < iovs_src + iovs_src_cnt &&
           size > 0) {
        n = spdk_min(v1->iov_len - off1, v2->iov_len - off2);

        if (n > size) {
            n = size;
        }

        size -= n;

        raid5_xor_buf(v1->iov_base + off1, v2->iov_base + off2, n);

        off1 += n;
        off2 += n;

        if (off1 == v1->iov_len) {
            off1 = 0;
            v1++;
        }

        if (off2 == v2->iov_len) {
            off2 = 0;
            v2++;
        }
    }
}

static void
raid5_memset_iovs(struct iovec *iovs, int iovcnt, char c)
{
    struct iovec *iov;

    for (iov = iovs; iov < iovs + iovcnt; iov++) {
        memset(iov->iov_base, c, iov->iov_len);
    }
}

static void
raid5_memcpy_iovs(struct iovec *iovs_dest, int iovs_dest_cnt, size_t iovs_dest_offset,
               const struct iovec *iovs_src, int iovs_src_cnt, size_t iovs_src_offset,
               size_t size)
{
    struct iovec *v1;
    const struct iovec *v2;
    size_t off1, off2;
    size_t n;

    v1 = iovs_dest;
    v2 = iovs_src;

    n = 0;
    off1 = 0;
    while (v1 < iovs_dest + iovs_dest_cnt) {
        n += v1->iov_len;
        if (n > iovs_dest_offset) {
            off1 = v1->iov_len - (n - iovs_dest_offset);
            break;
        }
        v1++;
    }

    n = 0;
    off2 = 0;
    while (v2 < iovs_src + iovs_src_cnt) {
        n += v2->iov_len;
        if (n > iovs_src_offset) {
            off2 = v2->iov_len - (n - iovs_src_offset);
            break;
        }
        v2++;
    }

    while (v1 < iovs_dest + iovs_dest_cnt &&
           v2 < iovs_src + iovs_src_cnt &&
           size > 0) {
        n = spdk_min(v1->iov_len - off1, v2->iov_len - off2);

        if (n > size) {
            n = size;
        }

        size -= n;

        memcpy(v1->iov_base + off1, v2->iov_base + off2, n);

        off1 += n;
        off2 += n;

        if (off1 == v1->iov_len) {
            off1 = 0;
            v1++;
        }

        if (off2 == v2->iov_len) {
            off2 = 0;
            v2++;
        }
    }
}

static int
raid5_chunk_map_iov(struct chunk *chunk, const struct iovec *iov, int iovcnt,
                    uint64_t offset, uint64_t len)
{
    int i;
    size_t off = 0;
    int start_v = -1;
    size_t start_v_off;
    int new_iovcnt = 0;

    // Note: find the start iov
    for (i = 0; i < iovcnt; i++) {
        if (off + iov[i].iov_len > offset) {
            start_v = i;
            break;
        }
        off += iov[i].iov_len;
    }

    if (start_v == -1) {
        return -EINVAL;
    }

    start_v_off = off;

    // Note: find the end iov
    for (i = start_v; i < iovcnt; i++) {
        new_iovcnt++;

        if (off + iov[i].iov_len >= offset + len) {
            break;
        }
        off += iov[i].iov_len;
    }

    assert(start_v + new_iovcnt <= iovcnt);

    // Note: dynamically change size of iovs
    if (new_iovcnt > chunk->iovcnt) {
        void *tmp;

        if (chunk->iovs == &chunk->iov) {
            chunk->iovs = NULL;
        }
        tmp = realloc(chunk->iovs, new_iovcnt * sizeof(struct iovec));
        if (!tmp) {
            return -ENOMEM;
        }
        chunk->iovs = tmp;
    }
    chunk->iovcnt = new_iovcnt;

    off = start_v_off;
    iov += start_v;

    // Note: set up iovs from iovs from bdev_io
    for (i = 0; i < new_iovcnt; i++) {
        chunk->iovs[i].iov_base = iov->iov_base + (offset - off);
        chunk->iovs[i].iov_len = spdk_min(len, iov->iov_len - (offset - off));

        off += iov->iov_len;
        iov++;
        offset += chunk->iovs[i].iov_len;
        len -= chunk->iovs[i].iov_len;
    }

    if (len > 0) {
        return -EINVAL;
    }

    return 0;
}

static int
raid5_chunk_map_req_data(struct chunk *chunk)
{
    struct stripe_request *stripe_req = raid5_chunk_stripe_req(chunk);
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(stripe_req->raid_io);
    uint64_t len = chunk->req_blocks * bdev_io->bdev->blocklen;
    int ret;

    ret = raid5_chunk_map_iov(chunk, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
                              stripe_req->iov_offset, len);
    if (ret == 0) {
        stripe_req->iov_offset += len;
    }

    return ret;
}

static void
raid5_io_channel_retry_request(struct raid5_io_channel *r5ch)
{
    struct spdk_bdev_io_wait_entry *waitq_entry;

    waitq_entry = TAILQ_FIRST(&r5ch->retry_queue);
    assert(waitq_entry != NULL);
    TAILQ_REMOVE(&r5ch->retry_queue, waitq_entry, link);
    waitq_entry->cb_fn(waitq_entry->cb_arg);
}

static void
raid5_submit_stripe_request(struct stripe_request *stripe_req);

static void
_raid5_submit_stripe_request(void *_stripe_req)
{
    struct stripe_request *stripe_req = _stripe_req;

    raid5_submit_stripe_request(stripe_req);
}

static void
raid5_stripe_request_put(struct stripe_request *stripe_req)
{
    struct raid5_info *r5info = stripe_req->raid_io->raid_bdev->module_private;
    struct chunk *chunk;

    FOR_EACH_CHUNK(stripe_req, chunk) {
        if (chunk->iovs != &chunk->iov) {
            free(chunk->iovs);
        }
    }

    spdk_mempool_put(r5info->stripe_request_mempool, stripe_req);
}

static void
raid5_complete_stripe_request(struct stripe_request *stripe_req)
{
    struct stripe *stripe = stripe_req->stripe;
    struct raid_bdev_io *raid_io = stripe_req->raid_io;
    enum spdk_bdev_io_status status = stripe_req->status;
    struct raid5_io_channel *r5ch = raid_bdev_io_channel_get_resource(raid_io->raid_ch);
    struct stripe_request *next_req;
    struct chunk *chunk;
    uint64_t req_blocks;

    // Note: if next request available, submit it
    pthread_spin_lock(&stripe->requests_lock);
    next_req = TAILQ_NEXT(stripe_req, link);
    TAILQ_REMOVE(&stripe->requests, stripe_req, link);
    pthread_spin_unlock(&stripe->requests_lock);
    if (next_req) {
        spdk_thread_send_msg(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(
                                     next_req->raid_io->raid_ch)),
                             _raid5_submit_stripe_request, next_req);
    }

    req_blocks = 0;
    FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
        req_blocks += chunk->req_blocks;
    }

    raid5_stripe_request_put(stripe_req);

    if (raid_bdev_io_complete_part(raid_io, req_blocks, status)) {
        __atomic_fetch_sub(&stripe->refs, 1, __ATOMIC_SEQ_CST);

        if (!TAILQ_EMPTY(&r5ch->retry_queue)) {
            raid5_io_channel_retry_request(r5ch);
        }
    }
}

static inline enum spdk_bdev_io_status errno_to_status(int err)
{
    err = abs(err);
    switch (err) {
        case 0:
            return SPDK_BDEV_IO_STATUS_SUCCESS;
        case ENOMEM:
            return SPDK_BDEV_IO_STATUS_NOMEM;
        default:
            return SPDK_BDEV_IO_STATUS_FAILED;
    }
}

static void
raid5_abort_stripe_request(struct stripe_request *stripe_req, enum spdk_bdev_io_status status)
{
    stripe_req->remaining = 0;
    stripe_req->status = status;
    raid5_complete_stripe_request(stripe_req);
}

static void
raid5_complete_reconstructed_stripe_request(struct stripe_request *stripe_req)
{
    struct chunk *chunk;
    int ret;
    struct chunk *d_chunk = stripe_req->degraded_chunk;
    uint32_t blocklen = stripe_req->raid_io->raid_bdev->bdev.blocklen;
    size_t src_offset;
    uint64_t len;
    struct iovec *preread_iovs;
    int preread_iovcnt;

    // Note: construct d_chunk
    raid5_memset_iovs(d_chunk->iovs, d_chunk->iovcnt, 0);
    FOR_EACH_CHUNK(stripe_req, chunk) {
        if (chunk != d_chunk) {
            if (chunk->request_type == CHUNK_PREREAD) {
                src_offset = (d_chunk->req_offset - chunk->preread_offset) * blocklen;
            } else {
                src_offset = (d_chunk->req_offset - chunk->req_offset) * blocklen;
            }
            raid5_xor_iovs(d_chunk->iovs, d_chunk->iovcnt, 0,
                           chunk->iovs, chunk->iovcnt, src_offset,
                           d_chunk->req_blocks * blocklen);
        }
    }

    // Note: copy data chunk if necessary
    stripe_req->iov_offset = stripe_req->init_iov_offset;
    FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
        len = chunk->req_blocks * blocklen;
        if (chunk->req_blocks > 0 && chunk != d_chunk && chunk->request_type == CHUNK_PREREAD) {
            preread_iovs = chunk->iovs;
            preread_iovcnt = chunk->iovcnt;
            ret = raid5_chunk_map_req_data(chunk);
            if (ret) {
                raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
            }
            src_offset = (chunk->req_offset - chunk->preread_offset) * blocklen;
            raid5_memcpy_iovs(chunk->iovs, chunk->iovcnt, 0,
                              preread_iovs, preread_iovcnt, src_offset,
                              chunk->req_blocks * blocklen);
        } else {
            stripe_req->iov_offset += len;
        }
    }

    // Note: all chunks are ready
    raid5_complete_stripe_request(stripe_req);
}

static void
raid5_complete_chunk_request(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct chunk *chunk = cb_arg;
    struct stripe_request *stripe_req = raid5_chunk_stripe_req(chunk);

    spdk_bdev_free_io(bdev_io);

    if (!success) {
        stripe_req->status = SPDK_BDEV_IO_STATUS_FAILED;
    }

    if (--stripe_req->remaining == 0) {
        if (stripe_req->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
            stripe_req->chunk_requests_complete_cb(stripe_req);
        } else {
            raid5_complete_stripe_request(stripe_req);
        }
    }
}

static void
_raid5_submit_chunk_request(void *_chunk)
{
    struct chunk *chunk = _chunk;
    struct stripe_request *stripe_req = raid5_chunk_stripe_req(chunk);
    struct raid_bdev_io *raid_io = stripe_req->raid_io;
    struct raid_bdev *raid_bdev = raid_io->raid_bdev;
    struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk->index];
    struct spdk_io_channel *base_ch = raid_io->raid_ch->base_channel[chunk->index];
    uint64_t offset_blocks;
    uint64_t num_blocks;
    enum spdk_bdev_io_type io_type;
    uint64_t base_offset_blocks;
    int ret;

    if (chunk->request_type == CHUNK_PREREAD) {
        offset_blocks = chunk->preread_offset;
        num_blocks = chunk->preread_blocks;
        io_type = SPDK_BDEV_IO_TYPE_READ;
    } else {
        offset_blocks = chunk->req_offset;
        num_blocks = chunk->req_blocks;
        if (chunk->request_type == CHUNK_READ) {
            io_type = SPDK_BDEV_IO_TYPE_READ;
        } else if (chunk->request_type == CHUNK_WRITE) {
            io_type = SPDK_BDEV_IO_TYPE_WRITE;
        } else {
            assert(false);
        }
    }

    base_offset_blocks = (stripe_req->stripe->index << raid_bdev->strip_size_shift) + offset_blocks;

    if (io_type == SPDK_BDEV_IO_TYPE_READ) {
        ret = spdk_bdev_readv_blocks(base_info->desc, base_ch,
                                     chunk->iovs, chunk->iovcnt,
                                     base_offset_blocks, num_blocks,
                                     raid5_complete_chunk_request,
                                     chunk);
    } else {
        ret = spdk_bdev_writev_blocks(base_info->desc, base_ch,
                                      chunk->iovs, chunk->iovcnt,
                                      base_offset_blocks, num_blocks,
                                      raid5_complete_chunk_request,
                                      chunk);
    }

    if (spdk_unlikely(ret != 0)) {
        if (ret == -ENOMEM) {
            struct spdk_bdev_io_wait_entry *wqe = &chunk->waitq_entry;

            wqe->bdev = base_info->bdev;
            wqe->cb_fn = _raid5_submit_chunk_request;
            wqe->cb_arg = chunk;
            spdk_bdev_queue_io_wait(base_info->bdev, base_ch, wqe);
        } else {
            SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
            assert(false);
        }
    }
}

static void
raid5_submit_chunk_request(struct chunk *chunk, enum chunk_request_type type)
{
    struct stripe_request *stripe_req = raid5_chunk_stripe_req(chunk);

    stripe_req->remaining++;

    chunk->request_type = type;

    _raid5_submit_chunk_request(chunk);
}

static void
raid5_stripe_write_submit(struct stripe_request *stripe_req)
{
    struct chunk *chunk;
    struct chunk *d_chunk = stripe_req->degraded_chunk;

    stripe_req->chunk_requests_complete_cb = raid5_complete_stripe_request;

    FOR_EACH_CHUNK(stripe_req, chunk) {
        if (chunk->req_blocks > 0 && chunk != d_chunk) {
            raid5_submit_chunk_request(chunk, CHUNK_WRITE);
        }
    }
}

// Note: Read-Modify-Write
static void
raid5_stripe_write_preread_complete_rmw(struct stripe_request *stripe_req)
{
    struct chunk *chunk;
    struct chunk *p_chunk = stripe_req->parity_chunk;
    uint32_t blocklen = stripe_req->raid_io->raid_bdev->bdev.blocklen;
    int ret;

    FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
        size_t dest_offset;

        if (chunk->req_blocks == 0) {
            continue;
        }

        dest_offset = (chunk->req_offset - p_chunk->req_offset) * blocklen;

        /* xor old parity with old data... */
        raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt, dest_offset,
                       chunk->iovs, chunk->iovcnt, 0,
                       chunk->req_blocks * blocklen);

        ret = raid5_chunk_map_req_data(chunk);
        if (ret) {
            raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
            return;
        }

        /* ...and with new data */
        raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt, dest_offset,
                       chunk->iovs, chunk->iovcnt, 0,
                       chunk->req_blocks * blocklen);
    }

    raid5_stripe_write_submit(stripe_req);
}

// Note: Reconstruction-Writes + full stripe write
static void
raid5_stripe_write_preread_complete(struct stripe_request *stripe_req)
{
    struct chunk *chunk;
    struct chunk *p_chunk = stripe_req->parity_chunk;
    uint32_t blocklen = stripe_req->raid_io->raid_bdev->bdev.blocklen;
    int ret = 0;

    raid5_memset_iovs(p_chunk->iovs, p_chunk->iovcnt, 0);

    FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
        if (chunk->preread_blocks > 0) {
            raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt,
                           (chunk->preread_offset - p_chunk->req_offset) * blocklen,
                           chunk->iovs, chunk->iovcnt, 0,
                           chunk->preread_blocks * blocklen);
        }

        if (chunk->req_blocks > 0) {
            ret = raid5_chunk_map_req_data(chunk);
            if (ret) {
                raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
                return;
            }

            raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt,
                           (chunk->req_offset - p_chunk->req_offset) * blocklen,
                           chunk->iovs, chunk->iovcnt, 0,
                           chunk->req_blocks * blocklen);
        }
    }

    raid5_stripe_write_submit(stripe_req);
}

static void
raid5_stripe_write_preread_complete_degraded(struct stripe_request *stripe_req)
{
    struct chunk *chunk;
    struct chunk *d_chunk = stripe_req->degraded_chunk;
    struct chunk *p_chunk = stripe_req->parity_chunk;
    struct raid_bdev *raid_bdev = stripe_req->raid_io->raid_bdev;
    uint32_t blocklen = raid_bdev->bdev.blocklen;
    int ret = 0;
    uint64_t real_preread_offset, real_preread_blocks, src_offset;

    if (p_chunk->preread_blocks) {
        d_chunk->preread_offset = p_chunk->preread_offset;
        d_chunk->preread_blocks = p_chunk->preread_blocks;
        d_chunk->iov.iov_base = stripe_req->stripe->chunk_buffers[d_chunk->index];
        d_chunk->iov.iov_len = p_chunk->preread_blocks * blocklen;
        raid5_memset_iovs(d_chunk->iovs, d_chunk->iovcnt, 0);
        FOR_EACH_CHUNK(stripe_req, chunk) {
            if (chunk != d_chunk) {
                src_offset = (d_chunk->preread_offset - chunk->preread_offset) * blocklen;
                raid5_xor_iovs(d_chunk->iovs, d_chunk->iovcnt, 0,
                            chunk->iovs, chunk->iovcnt, src_offset,
                            d_chunk->preread_blocks * blocklen);
            }
        }

        raid5_memset_iovs(p_chunk->iovs, p_chunk->iovcnt, 0);

        FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
            if (chunk->req_offset) {
                real_preread_offset = 0;
                real_preread_blocks = chunk->req_offset;
            } else {
                real_preread_offset = chunk->req_blocks;
                real_preread_blocks = raid_bdev->strip_size - chunk->req_blocks;
            }
            if (real_preread_blocks > 0) {
                raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt,
                            (real_preread_offset - p_chunk->req_offset) * blocklen,
                            chunk->iovs, chunk->iovcnt, 
                            (real_preread_offset - p_chunk->preread_offset) * blocklen,
                            real_preread_blocks * blocklen);
            }

            if (chunk->req_blocks > 0) {
                ret = raid5_chunk_map_req_data(chunk);
                if (ret) {
                    raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
                    return;
                }

                raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt,
                            (chunk->req_offset - p_chunk->req_offset) * blocklen,
                            chunk->iovs, chunk->iovcnt, 0,
                            chunk->req_blocks * blocklen);
            }
        }
    } else {
        raid5_memset_iovs(p_chunk->iovs, p_chunk->iovcnt, 0);

        FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
            if (chunk->preread_blocks > 0) {
                raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt,
                            (chunk->preread_offset - p_chunk->req_offset) * blocklen,
                            chunk->iovs, chunk->iovcnt, 0,
                            chunk->preread_blocks * blocklen);
            }

            if (chunk->req_blocks > 0) {
                ret = raid5_chunk_map_req_data(chunk);
                if (ret) {
                    raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
                    return;
                }

                raid5_xor_iovs(p_chunk->iovs, p_chunk->iovcnt,
                            (chunk->req_offset - p_chunk->req_offset) * blocklen,
                            chunk->iovs, chunk->iovcnt, 0,
                            chunk->req_blocks * blocklen);
            }
        }
    }

    raid5_stripe_write_submit(stripe_req);
}

static void
raid5_degraded_write(struct stripe_request *stripe_req)
{
    struct chunk *d_chunk = stripe_req->degraded_chunk;
    struct chunk *p_chunk = stripe_req->parity_chunk;
    struct chunk *chunk;
    struct raid_bdev *raid_bdev = stripe_req->raid_io->raid_bdev;
    int ret;

    if (d_chunk == p_chunk) {
        FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
            if (chunk->req_blocks > 0) {
                ret = raid5_chunk_map_req_data(chunk);
                if (ret) {
                    raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
                    return;
                }
            }
        }
        raid5_stripe_write_submit(stripe_req);
        return;
    }

    if (stripe_req->first_data_chunk == stripe_req->last_data_chunk) {
        p_chunk->req_offset = stripe_req->first_data_chunk->req_offset;
        p_chunk->req_blocks = stripe_req->first_data_chunk->req_blocks;
    } else {
        p_chunk->req_offset = 0;
        p_chunk->req_blocks = raid_bdev->strip_size;
    }  

    if (d_chunk->req_blocks) {
        stripe_req->chunk_requests_complete_cb = raid5_stripe_write_preread_complete_degraded;
    } else {
        stripe_req->chunk_requests_complete_cb = raid5_stripe_write_preread_complete_rmw;
    }

    FOR_EACH_CHUNK(stripe_req, chunk) {
        if (chunk == d_chunk) {
            chunk->preread_offset = 0;
            chunk->preread_blocks = 0;
            continue;            
        }
        if (!d_chunk->req_blocks) {
            chunk->preread_offset = chunk->req_offset;
            chunk->preread_blocks = chunk->req_blocks;
        } else {
            if (stripe_req->first_data_chunk == stripe_req->last_data_chunk) {
                if (chunk == p_chunk) {
                    chunk->preread_offset = 0;
                    chunk->preread_blocks = 0;
                } else {
                    chunk->preread_offset = p_chunk->req_offset;
                    chunk->preread_blocks = p_chunk->req_blocks;
                }
            } else {
                if (d_chunk->req_offset == 0 && d_chunk->req_blocks == raid_bdev->strip_size) {
                    if (chunk == p_chunk) {
                        chunk->preread_offset = 0;
                        chunk->preread_blocks = 0;
                    } else if (chunk->req_offset) {
                        chunk->preread_offset = 0;
                        chunk->preread_blocks = chunk->req_offset;
                    } else {
                        chunk->preread_offset = chunk->req_blocks;
                        chunk->preread_blocks = raid_bdev->strip_size - chunk->req_blocks;
                    }
                } else {
                    if (chunk == p_chunk) {
                        if (d_chunk->req_offset) {
                            chunk->preread_offset = 0;
                            chunk->preread_blocks = d_chunk->req_offset;
                        } else {
                            chunk->preread_offset = d_chunk->req_blocks;
                            chunk->preread_blocks = raid_bdev->strip_size - d_chunk->req_blocks;
                        }
                    } else if (chunk == stripe_req->first_data_chunk || chunk == stripe_req->last_data_chunk || !chunk->req_blocks) {
                        chunk->preread_offset = 0;
                        chunk->preread_blocks = raid_bdev->strip_size;
                    } else {
                        if (d_chunk->req_offset) {
                            chunk->preread_offset = 0;
                            chunk->preread_blocks = d_chunk->req_offset;
                        } else {
                            chunk->preread_offset = d_chunk->req_blocks;
                            chunk->preread_blocks = raid_bdev->strip_size - d_chunk->req_blocks;
                        }
                    }
                }

            }
        }

        if (chunk->preread_blocks || chunk == p_chunk) {
            size_t len;

            if (chunk == p_chunk) {
                len = chunk->req_blocks * raid_bdev->bdev.blocklen;
            } else {
                len = chunk->preread_blocks * raid_bdev->bdev.blocklen;
            }

            chunk->iov.iov_base = stripe_req->stripe->chunk_buffers[chunk->index];
            chunk->iov.iov_len = len;
        }

        if (chunk->preread_blocks) {
            raid5_submit_chunk_request(chunk, CHUNK_PREREAD);
        }
    }

    // Note: If no preread needs to be done (full stripe), complete immediately
    if (stripe_req->remaining == 0) {
        stripe_req->chunk_requests_complete_cb(stripe_req);
    }

}

static void
raid5_stripe_write(struct stripe_request *stripe_req)
{
    struct raid_bdev *raid_bdev = stripe_req->raid_io->raid_bdev;
    struct chunk *p_chunk = stripe_req->parity_chunk;
    struct chunk *d_chunk = stripe_req->degraded_chunk;
    struct chunk *chunk;
    int preread_balance = 0;


    if (d_chunk) {
        raid5_degraded_write(stripe_req);
        return;
    }

    // Note: single chunk -> partial parity update; > 1 chunk -> full parity update
    if (stripe_req->first_data_chunk == stripe_req->last_data_chunk) {
        p_chunk->req_offset = stripe_req->first_data_chunk->req_offset;
        p_chunk->req_blocks = stripe_req->first_data_chunk->req_blocks;
    } else {
        p_chunk->req_offset = 0;
        p_chunk->req_blocks = raid_bdev->strip_size;
    }

    // Note: This is the vote process
    // 1. req_blocks == 0: no update, favors rmw
    // 2. 0 < req_blocks < stripe_size: partial update, no preference
    // 3. req_blocks == stripe_size: full update, favors reconstruction write
    FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
        if (chunk->req_blocks < p_chunk->req_blocks) {
            preread_balance++;
        }

        if (chunk->req_blocks > 0) {
            preread_balance--;
        }
    }

    if (preread_balance > 0) {
        stripe_req->chunk_requests_complete_cb = raid5_stripe_write_preread_complete_rmw;
    } else {
        stripe_req->chunk_requests_complete_cb = raid5_stripe_write_preread_complete;
    }

    FOR_EACH_CHUNK(stripe_req, chunk) {
        if (preread_balance > 0) { // Note: for RMW, request chunks are the same as the preread chunks
            chunk->preread_offset = chunk->req_offset;
            chunk->preread_blocks = chunk->req_blocks;
        } else {
            if (chunk == p_chunk) { // Note: for reconstruction write, no need to read parity
                chunk->preread_offset = 0;
                chunk->preread_blocks = 0;
            } else if (stripe_req->first_data_chunk == stripe_req->last_data_chunk) {
                if (chunk->req_blocks) { // Note: don't read request chunks
                    chunk->preread_offset = 0;
                    chunk->preread_blocks = 0;
                } else { // Note: read non-request chunks
                    chunk->preread_offset = p_chunk->req_offset;
                    chunk->preread_blocks = p_chunk->req_blocks;
                }
            } else {
                if (chunk->req_offset) { // Note: if it's a request chunk and has offset, we need to make it complete
                    chunk->preread_offset = 0;
                    chunk->preread_blocks = chunk->req_offset;
                } else { // Note: if it's a request chunk without offset, we need to make it complete
                    // Note: for non-request chunks, read the full chunk
                    chunk->preread_offset = chunk->req_blocks;
                    chunk->preread_blocks = raid_bdev->strip_size - chunk->req_blocks;
                }
            }
        }

        if (chunk->preread_blocks || chunk == p_chunk) {
            size_t len;

            if (chunk == p_chunk) {
                len = chunk->req_blocks * raid_bdev->bdev.blocklen;
            } else {
                len = chunk->preread_blocks * raid_bdev->bdev.blocklen;
            }

            chunk->iov.iov_base = stripe_req->stripe->chunk_buffers[chunk->index];
            chunk->iov.iov_len = len;
        }

        if (chunk->preread_blocks) {
            raid5_submit_chunk_request(chunk, CHUNK_PREREAD);
        }
    }

    // Note: If no preread needs to be done (full stripe), complete immediately
    if (stripe_req->remaining == 0) {
        stripe_req->chunk_requests_complete_cb(stripe_req);
    }
}

static int
raid5_check_degraded(struct stripe_request *stripe_req)
{
    struct raid_bdev_io *raid_io = stripe_req->raid_io;
    struct raid_bdev *raid_bdev = raid_io->raid_bdev;
    struct chunk *chunk;
    struct chunk *d_chunk = stripe_req->degraded_chunk = NULL;
    uint8_t total_degraded = 0;
    struct raid_base_bdev_info *base_info;

    // Note: check if any degraded device is involved
    FOR_EACH_CHUNK(stripe_req, chunk) {
        base_info = &raid_bdev->base_bdev_info[chunk->index];
        if (base_info->degraded) {
            total_degraded++;
            d_chunk = stripe_req->degraded_chunk = chunk;
        }
    }

    if (total_degraded > raid_bdev->module->base_bdevs_max_degraded) {
        return -1;
    }

    return 0;
}

static void
raid5_stripe_read(struct stripe_request *stripe_req)
{
    struct chunk *chunk;
    int ret;
    struct raid_bdev_io *raid_io = stripe_req->raid_io;
    struct raid_bdev *raid_bdev = raid_io->raid_bdev;
    struct chunk *d_chunk = stripe_req->degraded_chunk;
    uint64_t len;

    if (d_chunk && d_chunk->req_blocks > 0) { // Note: read necessary blocks for reconstruction
        stripe_req->chunk_requests_complete_cb = raid5_complete_reconstructed_stripe_request;
        FOR_EACH_CHUNK(stripe_req, chunk) {
            if (chunk->req_blocks == 0) { // Note: parity chunk or chunks which are not requested
                chunk->preread_offset = d_chunk->req_offset;
                chunk->preread_blocks = d_chunk->req_blocks;
                chunk->iov.iov_base = stripe_req->stripe->chunk_buffers[chunk->index];
                chunk->iov.iov_len = chunk->preread_blocks * raid_bdev->bdev.blocklen;
            } else if (chunk == d_chunk) {
                chunk->preread_offset = 0;
                chunk->preread_blocks = 0;
                ret = raid5_chunk_map_req_data(chunk);
                if (ret) {
                    raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
                    return;
                }
            } else if ((chunk->req_offset > d_chunk->req_offset ||
                        chunk->req_offset + chunk->req_blocks < d_chunk->req_offset + d_chunk->req_blocks)) {
                chunk->preread_offset = spdk_min(chunk->req_offset, d_chunk->req_offset);
                chunk->preread_blocks = spdk_max(chunk->req_offset + chunk->req_blocks, d_chunk->req_offset + d_chunk->req_blocks) - chunk->preread_offset;
                chunk->iov.iov_base = stripe_req->stripe->chunk_buffers[chunk->index];
                chunk->iov.iov_len = chunk->preread_blocks * raid_bdev->bdev.blocklen;
                len = chunk->req_blocks * raid_bdev->bdev.blocklen;
                stripe_req->iov_offset += len;
            } else {
                chunk->preread_offset = 0;
                chunk->preread_blocks = 0;
                ret = raid5_chunk_map_req_data(chunk);
                if (ret) {
                    raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
                    return;
                }
            }
            if (chunk->preread_blocks) {
                raid5_submit_chunk_request(chunk, CHUNK_PREREAD);
            } else if (chunk->req_blocks && chunk != d_chunk) {
                raid5_submit_chunk_request(chunk, CHUNK_READ);
            }
        }        
    } else {
        stripe_req->chunk_requests_complete_cb = raid5_complete_stripe_request;
        FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
            if (chunk->req_blocks > 0) {
                ret = raid5_chunk_map_req_data(chunk);
                if (ret) {
                    raid5_abort_stripe_request(stripe_req, errno_to_status(ret));
                    return;
                }
                raid5_submit_chunk_request(chunk, CHUNK_READ);
            }
        }       
    }
}

static void
raid5_submit_stripe_request(struct stripe_request *stripe_req)
{
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(stripe_req->raid_io);
    int ret;

    ret = raid5_check_degraded(stripe_req);
    if (ret) {
        raid5_abort_stripe_request(stripe_req, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
        raid5_stripe_read(stripe_req);
    } else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
        raid5_stripe_write(stripe_req);
    } else {
        assert(false);
    }
}

static void
raid5_handle_stripe(struct raid_bdev_io *raid_io, struct stripe *stripe,
                    uint64_t stripe_offset, uint64_t blocks, uint64_t iov_offset)
{
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
    struct raid_bdev *raid_bdev = raid_io->raid_bdev;
    struct raid5_info *r5info = raid_bdev->module_private;
    struct stripe_request *stripe_req;
    struct chunk *chunk;
    uint64_t stripe_offset_from, stripe_offset_to;
    uint8_t first_chunk_data_idx, last_chunk_data_idx;
    bool do_submit;

    if (raid_io->base_bdev_io_remaining == blocks &&
        bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE &&
        blocks < raid_bdev->strip_size) {
        /*
         * Split in 2 smaller requests if this request would require
         * a non-contiguous parity chunk update
         */
        uint64_t blocks_limit = raid_bdev->strip_size -
                                (stripe_offset % raid_bdev->strip_size);
        if (blocks > blocks_limit) {
            raid5_handle_stripe(raid_io, stripe, stripe_offset,
                                blocks_limit, iov_offset);
            blocks -= blocks_limit;
            stripe_offset += blocks_limit;
            iov_offset += blocks_limit * raid_bdev->bdev.blocklen;
        }
    }

    stripe_req = spdk_mempool_get(r5info->stripe_request_mempool);
    if (spdk_unlikely(!stripe_req)) {
        raid_bdev_io_complete_part(raid_io, blocks, SPDK_BDEV_IO_STATUS_NOMEM);
        return;
    }

    stripe_req->raid_io = raid_io;
    stripe_req->iov_offset = iov_offset;
    stripe_req->init_iov_offset = iov_offset;
    stripe_req->status = SPDK_BDEV_IO_STATUS_SUCCESS;
    stripe_req->remaining = 0;

    stripe_req->stripe = stripe;
    stripe_req->parity_chunk = &stripe_req->chunks[raid5_stripe_data_chunks_num(
            raid_bdev) - stripe->index % raid_bdev->num_base_bdevs];

    stripe_offset_from = stripe_offset;
    stripe_offset_to = stripe_offset_from + blocks;
    first_chunk_data_idx = stripe_offset_from >> raid_bdev->strip_size_shift;
    last_chunk_data_idx = (stripe_offset_to - 1) >> raid_bdev->strip_size_shift;

    stripe_req->first_data_chunk = raid5_get_data_chunk(stripe_req, first_chunk_data_idx);
    stripe_req->last_data_chunk = raid5_get_data_chunk(stripe_req, last_chunk_data_idx);

    FOR_EACH_CHUNK(stripe_req, chunk) {
        chunk->index = chunk - stripe_req->chunks;
        chunk->iovs = &chunk->iov;
        chunk->iovcnt = 1;

        if (chunk == stripe_req->parity_chunk ||
            chunk < stripe_req->first_data_chunk ||
            chunk > stripe_req->last_data_chunk) {
            chunk->req_offset = 0;
            chunk->req_blocks = 0;
        } else {
            uint64_t chunk_offset_from = raid5_chunk_data_index(chunk) << raid_bdev->strip_size_shift;
            uint64_t chunk_offset_to = chunk_offset_from + raid_bdev->strip_size;

            if (stripe_offset_from > chunk_offset_from) {
                chunk->req_offset = stripe_offset_from - chunk_offset_from;
            } else {
                chunk->req_offset = 0;
            }

            if (stripe_offset_to < chunk_offset_to) {
                chunk->req_blocks = stripe_offset_to - chunk_offset_from;
            } else {
                chunk->req_blocks = raid_bdev->strip_size;
            }

            chunk->req_blocks -= chunk->req_offset;
        }
    }

    pthread_spin_lock(&stripe->requests_lock);
    do_submit = TAILQ_EMPTY(&stripe->requests);
    TAILQ_INSERT_TAIL(&stripe->requests, stripe_req, link);
    pthread_spin_unlock(&stripe->requests_lock);

    if (do_submit) {
        raid5_submit_stripe_request(stripe_req);
    }
}

static int
raid5_reclaim_stripes(struct raid5_info *r5info)
{
    struct stripe *stripe, *tmp;
    int reclaimed = 0;
    int ret;
    int to_reclaim = (RAID_MAX_STRIPES / 8) - RAID_MAX_STRIPES +
                     rte_hash_count(r5info->active_stripes_hash);

    TAILQ_FOREACH_REVERSE_SAFE(stripe, &r5info->active_stripes, active_stripes_head, link, tmp) {
        if (__atomic_load_n(&stripe->refs, __ATOMIC_SEQ_CST) > 0) {
            continue;
        }

        TAILQ_REMOVE(&r5info->active_stripes, stripe, link);
        TAILQ_INSERT_TAIL(&r5info->free_stripes, stripe, link);

        ret = rte_hash_del_key_with_hash(r5info->active_stripes_hash,
                                         &stripe->index, stripe->hash);
        if (spdk_unlikely(ret < 0)) {
            assert(false);
        }

        if (++reclaimed > to_reclaim) {
            break;
        }
    }

    return reclaimed;
}

static struct stripe *
raid5_get_stripe(struct raid5_info *r5info, uint64_t stripe_index)
{
    struct stripe *stripe;
    hash_sig_t hash;
    int ret;

    hash = rte_hash_hash(r5info->active_stripes_hash, &stripe_index);

    pthread_spin_lock(&r5info->active_stripes_lock);
    ret = rte_hash_lookup_with_hash_data(r5info->active_stripes_hash,
                                         &stripe_index, hash, (void **)&stripe);
    if (ret == -ENOENT) {
        stripe = TAILQ_FIRST(&r5info->free_stripes);
        if (!stripe) {
            if (raid5_reclaim_stripes(r5info) > 0) {
                stripe = TAILQ_FIRST(&r5info->free_stripes);
                assert(stripe != NULL);
            } else {
                pthread_spin_unlock(&r5info->active_stripes_lock);
                return NULL;
            }
        }
        TAILQ_REMOVE(&r5info->free_stripes, stripe, link);

        stripe->index = stripe_index;
        stripe->hash = hash;

        ret = rte_hash_add_key_with_hash_data(r5info->active_stripes_hash,
                                              &stripe_index, hash, stripe);
        if (spdk_unlikely(ret < 0)) {
            assert(false);
        }
    } else {
        TAILQ_REMOVE(&r5info->active_stripes, stripe, link);
    }
    TAILQ_INSERT_HEAD(&r5info->active_stripes, stripe, link);

    __atomic_fetch_add(&stripe->refs, 1, __ATOMIC_SEQ_CST);

    pthread_spin_unlock(&r5info->active_stripes_lock);

    return stripe;
}

static void
raid5_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid5_submit_rw_request(void *_raid_io)
{
    struct raid_bdev_io *raid_io = _raid_io;

    raid5_submit_rw_request(raid_io);
}

static void
raid5_complete_chunk_request_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void
raid5_complete_chunk_request_read(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct iov_wrapper *iov_w = cb_arg;
    struct raid_bdev_io *raid_io = iov_w->raid_io;
    struct raid5_io_channel *r5ch = (struct raid5_io_channel*) raid_bdev_io_channel_get_resource(raid_io->raid_ch);

    spdk_bdev_free_io(bdev_io);

    raid_bdev_io_complete_part(raid_io, iov_w->num_blocks, success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
    TAILQ_INSERT_TAIL(&r5ch->iov_w_queue, iov_w, link);
}

static int
raid5_map_iov(struct iovec *iovs, const struct iovec *iov, int iovcnt,
              uint64_t offset, uint64_t len)
{
    int i;
    size_t off = 0;
    int start_v = -1;
    size_t start_v_off;
    int new_iovcnt = 0;
    int ret;

    // Note: find the start iov
    for (i = 0; i < iovcnt; i++) {
        if (off + iov[i].iov_len > offset) {
            start_v = i;
            break;
        }
        off += iov[i].iov_len;
    }

    if (start_v == -1) {
        return -1;
    }

    start_v_off = off;

    // Note: find the end iov
    for (i = start_v; i < iovcnt; i++) {
        new_iovcnt++;

        if (off + iov[i].iov_len >= offset + len) {
            break;
        }
        off += iov[i].iov_len;
    }

    assert(start_v + new_iovcnt <= iovcnt);

    ret = new_iovcnt;

    off = start_v_off;
    iov += start_v;

    // Note: set up iovs from iovs from bdev_io
    for (i = 0; i < new_iovcnt; i++) {
        iovs[i].iov_base = (char*)iov->iov_base + (offset - off);
        iovs[i].iov_len = spdk_min(len, iov->iov_len - (offset - off));

        off += iov->iov_len;
        iov++;
        offset += iovs[i].iov_len;
        len -= iovs[i].iov_len;
    }

    if (len > 0) {
        return -1;
    }

    return ret;
}

static void
raid5_handle_read(struct raid_bdev_io *raid_io, uint64_t stripe_index, uint64_t stripe_offset, uint64_t blocks)
{
    struct raid_bdev *raid_bdev = raid_io->raid_bdev;
    struct raid5_io_channel *r5ch = (struct raid5_io_channel*) raid_bdev_io_channel_get_resource(raid_io->raid_ch);
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
    struct iovec *iov = bdev_io->u.bdev.iovs;
    int iovcnt = bdev_io->u.bdev.iovcnt;
    uint64_t iov_offset = 0;

    uint64_t chunk_offset_from, chunk_offset_to, chunk_req_offset, chunk_req_blocks, chunk_len;
    struct iov_wrapper *iov_w;
    struct raid_base_bdev_info *base_info;
    struct spdk_io_channel *base_ch;
    int chunk_iovcnt;
    uint64_t base_offset_blocks;

    uint64_t stripe_offset_from = stripe_offset;
    uint64_t stripe_offset_to = stripe_offset_from + blocks;
    uint8_t first_chunk_idx = stripe_offset_from >> raid_bdev->strip_size_shift;
    uint8_t last_chunk_idx = (stripe_offset_to - 1) >> raid_bdev->strip_size_shift;

    uint8_t p_chunk_idx = raid5_stripe_data_chunks_num(raid_bdev) - stripe_index % raid_bdev->num_base_bdevs; // different for raid6
    if (first_chunk_idx >= p_chunk_idx) {
        first_chunk_idx++;
    }
    if (last_chunk_idx >= p_chunk_idx) {
        last_chunk_idx++;
    }
    for (uint8_t i = 0; i < raid_bdev->num_base_bdevs; i++) {
        if (i == p_chunk_idx || i < first_chunk_idx || i > last_chunk_idx) { // add q for raid6
            continue;
        } else {
            chunk_offset_from = (i < p_chunk_idx ? i : i - 1) << raid_bdev->strip_size_shift; // different for raid6
            chunk_offset_to = chunk_offset_from + raid_bdev->strip_size;

            if (stripe_offset_from > chunk_offset_from) {
                chunk_req_offset = stripe_offset_from - chunk_offset_from;
            } else {
                chunk_req_offset = 0;
            }

            if (stripe_offset_to < chunk_offset_to) {
                chunk_req_blocks = stripe_offset_to - chunk_offset_from;
            } else {
                chunk_req_blocks = raid_bdev->strip_size;
            }

            chunk_req_blocks -= chunk_req_offset;

            chunk_len = chunk_req_blocks << raid_bdev->blocklen_shift;

            iov_w = TAILQ_FIRST(&r5ch->iov_w_queue);
            TAILQ_REMOVE(&r5ch->iov_w_queue, iov_w, link);
            iov_w->num_blocks = chunk_req_blocks;
            iov_w->raid_io = raid_io;

            chunk_iovcnt = raid5_map_iov(iov_w->iovs, iov, iovcnt, iov_offset, chunk_len);
            iov_offset += chunk_len;

            base_info = &raid_bdev->base_bdev_info[i];
            base_ch = raid_io->raid_ch->base_channel[i];
            base_offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + chunk_req_offset;

            spdk_bdev_readv_blocks(base_info->desc, base_ch, iov_w->iovs, chunk_iovcnt, base_offset_blocks,
                                   chunk_req_blocks, raid5_complete_chunk_request_read, iov_w);
        }
    }

}

static void
raid5_submit_rw_request(struct raid_bdev_io *raid_io)
{
    struct raid_bdev *raid_bdev = raid_io->raid_bdev;
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
    struct raid5_info *r5info = raid_io->raid_bdev->module_private;
    uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;
    uint64_t num_blocks = bdev_io->u.bdev.num_blocks;
    uint64_t stripe_index = offset_blocks / r5info->stripe_blocks;
    uint64_t stripe_offset = offset_blocks % r5info->stripe_blocks;
    struct stripe *stripe;

    // TODO: for testing
//    if (raid5_stripe_data_chunks_num(raid_bdev) - stripe_index % raid_bdev->num_base_bdevs == 7) {
//        stripe_index += (stripe_index + 1) % r5info->total_stripes;
//    }
//    stripe_offset = (stripe_offset % raid_bdev->strip_size) + (raid_bdev->strip_size * 6);

    //comment out this block
    // if (!raid_bdev->degraded && bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
    //     raid_io->base_bdev_io_remaining = num_blocks;
    //     raid5_handle_read(raid_io, stripe_index, stripe_offset, num_blocks);
    //     return;
    // }

    stripe = raid5_get_stripe(r5info, stripe_index);
    if (spdk_unlikely(stripe == NULL)) {
        struct raid5_io_channel *r5ch = raid_bdev_io_channel_get_resource(raid_io->raid_ch);
        struct spdk_bdev_io_wait_entry *wqe = &raid_io->waitq_entry;

        wqe->cb_fn = _raid5_submit_rw_request;
        wqe->cb_arg = raid_io;
        TAILQ_INSERT_TAIL(&r5ch->retry_queue, wqe, link);
        return;
    }

    raid_io->base_bdev_io_remaining = num_blocks;

    raid5_handle_stripe(raid_io, stripe, stripe_offset, num_blocks, 0);
}

static int
raid5_stripe_init(struct stripe *stripe, struct raid_bdev *raid_bdev)
{
    uint8_t i;

    stripe->chunk_buffers = calloc(raid_bdev->num_base_bdevs, sizeof(void *));
    if (!stripe->chunk_buffers) {
        SPDK_ERRLOG("Failed to allocate chunk buffers array\n");
        return -ENOMEM;
    }

    for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
        void *buf;

        buf = spdk_dma_malloc(raid_bdev->strip_size * raid_bdev->bdev.blocklen,
                              spdk_max(spdk_bdev_get_buf_align(raid_bdev->base_bdev_info[i].bdev), 32),
                              NULL);
        if (!buf) {
            SPDK_ERRLOG("Failed to allocate chunk buffer\n");
            for (; i > 0; --i) {
                spdk_dma_free(stripe->chunk_buffers[i]);
            }
            free(stripe->chunk_buffers);
            return -ENOMEM;
        }

        stripe->chunk_buffers[i] = buf;
    }

    TAILQ_INIT(&stripe->requests);
    pthread_spin_init(&stripe->requests_lock, PTHREAD_PROCESS_PRIVATE);

    return 0;
}

static void
raid5_stripe_deinit(struct stripe *stripe, struct raid_bdev *raid_bdev)
{
    uint8_t i;

    for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
        spdk_dma_free(stripe->chunk_buffers[i]);
    }
    free(stripe->chunk_buffers);

    pthread_spin_destroy(&stripe->requests_lock);
}

static void
raid5_free(struct raid5_info *r5info)
{
    unsigned int i;

    pthread_spin_destroy(&r5info->active_stripes_lock);

    if (r5info->active_stripes_hash) {
        rte_hash_free(r5info->active_stripes_hash);
    }

    if (r5info->stripe_request_mempool) {
        spdk_mempool_free(r5info->stripe_request_mempool);
    }

    if (r5info->stripes) {
        for (i = 0; i < RAID_MAX_STRIPES; i++) {
            raid5_stripe_deinit(&r5info->stripes[i], r5info->raid_bdev);
        }
        free(r5info->stripes);
    }

    free(r5info);
}

static int
raid5_start(struct raid_bdev *raid_bdev)
{
    uint64_t min_blockcnt = UINT64_MAX;
    struct raid_base_bdev_info *base_info;
    struct raid5_info *r5info;
    char name_buf[32];
    struct rte_hash_parameters hash_params = { 0 };
    unsigned int i;
    int ret = 0;

    r5info = calloc(1, sizeof(*r5info));
    if (!r5info) {
        SPDK_ERRLOG("Failed to allocate r5info\n");
        return -ENOMEM;
    }
    r5info->raid_bdev = raid_bdev;

    pthread_spin_init(&r5info->active_stripes_lock, PTHREAD_PROCESS_PRIVATE);

    RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
        min_blockcnt = spdk_min(min_blockcnt, base_info->bdev->blockcnt);
    }

    r5info->total_stripes = min_blockcnt / raid_bdev->strip_size;
    r5info->stripe_blocks = raid_bdev->strip_size * raid5_stripe_data_chunks_num(raid_bdev);

    raid_bdev->bdev.blockcnt = r5info->stripe_blocks * r5info->total_stripes;
    raid_bdev->bdev.optimal_io_boundary = r5info->stripe_blocks;
    raid_bdev->bdev.split_on_optimal_io_boundary = true;

    r5info->stripes = calloc(RAID_MAX_STRIPES, sizeof(*r5info->stripes));
    if (!r5info->stripes) {
        SPDK_ERRLOG("Failed to allocate stripes array\n");
        ret = -ENOMEM;
        goto out;
    }

    TAILQ_INIT(&r5info->free_stripes);

    for (i = 0; i < RAID_MAX_STRIPES; i++) {
        struct stripe *stripe = &r5info->stripes[i];

        ret = raid5_stripe_init(stripe, raid_bdev);
        if (ret) {
            for (; i > 0; --i) {
                raid5_stripe_deinit(&r5info->stripes[i], raid_bdev);
            }
            free(r5info->stripes);
            r5info->stripes = NULL;
            goto out;
        }

        TAILQ_INSERT_TAIL(&r5info->free_stripes, stripe, link);
    }

    snprintf(name_buf, sizeof(name_buf), "raid5_sreq_%p", raid_bdev);

    r5info->stripe_request_mempool = spdk_mempool_create(name_buf,
                                                         RAID_MAX_STRIPES * 4,
                                                         sizeof(struct stripe_request) + sizeof(struct chunk) * raid_bdev->num_base_bdevs,
                                                         SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
                                                         SPDK_ENV_SOCKET_ID_ANY);
    if (!r5info->stripe_request_mempool) {
        SPDK_ERRLOG("Failed to allocate stripe_request_mempool\n");
        ret = -ENOMEM;
        goto out;
    }

    snprintf(name_buf, sizeof(name_buf), "raid5_hash_%p", raid_bdev);

    hash_params.name = name_buf;
    hash_params.entries = RAID_MAX_STRIPES * 2;
    hash_params.key_len = sizeof(uint64_t);

    r5info->active_stripes_hash = rte_hash_create(&hash_params);
    if (!r5info->active_stripes_hash) {
        SPDK_ERRLOG("Failed to allocate active_stripes_hash\n");
        ret = -ENOMEM;
        goto out;
    }

    TAILQ_INIT(&r5info->active_stripes);

    raid_bdev->module_private = r5info;
    out:
    if (ret) {
        raid5_free(r5info);
    }
    return ret;
}

static void
raid5_stop(struct raid_bdev *raid_bdev)
{
    struct raid5_info *r5info = raid_bdev->module_private;

    raid5_free(r5info);
}

static int
raid5_io_channel_resource_init(struct raid_bdev *raid_bdev, void *resource)
{
    struct raid5_io_channel *r5ch = resource;

    TAILQ_INIT(&r5ch->retry_queue);
    TAILQ_INIT(&r5ch->iov_w_queue);
    uint16_t i;
    struct iov_wrapper *iov_w;
    for (i = 0; i < 512; i++) {
        iov_w = calloc(1, sizeof(struct iov_wrapper));
        TAILQ_INSERT_TAIL(&r5ch->iov_w_queue, iov_w, link);
    }

    return 0;
}

static void
raid5_io_channel_resource_deinit(void *resource)
{
    struct raid5_io_channel *r5ch = resource;

    assert(TAILQ_EMPTY(&r5ch->retry_queue));
    struct iov_wrapper *iov_w;
    while (!TAILQ_EMPTY(&r5ch->iov_w_queue)) {
        iov_w = TAILQ_FIRST(&r5ch->iov_w_queue);
        TAILQ_REMOVE(&r5ch->iov_w_queue, iov_w, link);
        free(iov_w);
    }
}

static struct raid_bdev_module g_raid5_module = {
        .level = RAID5,
        .base_bdevs_min = 3,
        .base_bdevs_max_degraded = 1,
        .io_channel_resource_size = sizeof(struct raid5_io_channel),
        .start = raid5_start,
        .stop = raid5_stop,
        .submit_rw_request = raid5_submit_rw_request,
        .io_channel_resource_init = raid5_io_channel_resource_init,
        .io_channel_resource_deinit = raid5_io_channel_resource_deinit,
};
RAID_MODULE_REGISTER(&g_raid5_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid5)
