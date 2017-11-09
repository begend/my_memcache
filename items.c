/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>

/* Forward Declarations */
static void item_link_q(item *it);
static void item_unlink_q(item *it);

/*
 * We only reposition items in the LRU queue if they haven't been repositioned
 * in this many seconds. That saves us from churning on frequently-accessed
 * items.
 */
#define ITEM_UPDATE_INTERVAL 60

#define LARGEST_ID POWER_LARGEST
typedef struct {
    uint64_t evicted;
    uint64_t evicted_nonzero;
    rel_time_t evicted_time;
    uint64_t reclaimed;
    uint64_t outofmemory;
    uint64_t tailrepairs;
    uint64_t expired_unfetched;
    uint64_t evicted_unfetched;
} itemstats_t;

static item *heads[LARGEST_ID];
static item *tails[LARGEST_ID];
static itemstats_t itemstats[LARGEST_ID];
static unsigned int sizes[LARGEST_ID];

void item_stats_reset(void) {
    mutex_lock(&cache_lock);
    memset(itemstats, 0, sizeof(itemstats));
    mutex_unlock(&cache_lock);
}


/* Get the next CAS id for a new item. */
//为新的item生成cas值
uint64_t get_cas_id(void) {
    static uint64_t cas_id = 0;
    return ++cas_id;
}

/* Enable this for reference-count debugging. */
#if 0
# define DEBUG_REFCNT(it,op) \
                fprintf(stderr, "item %x refcnt(%c) %d %c%c%c\n", \
                        it, op, it->refcount, \
                        (it->it_flags & ITEM_LINKED) ? 'L' : ' ', \
                        (it->it_flags & ITEM_SLABBED) ? 'S' : ' ')
#else
# define DEBUG_REFCNT(it,op) while(0)
#endif

/**
 * Generates the variable-sized part of the header for an object.
 *
 * key     - The key
 * nkey    - The length of the key
 * flags   - key flags
 * nbytes  - Number of bytes to hold value and addition CRLF terminator
 * suffix  - Buffer for the "VALUE" line suffix (flags, size).
 * nsuffix - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
//计算item的大小
static size_t item_make_header(const uint8_t nkey, const int flags, const int nbytes,
                     char *suffix, uint8_t *nsuffix) {
    /* suffix is defined at 40 chars elsewhere.. */ //suffix限定了40个字节
    *nsuffix = (uint8_t) snprintf(suffix, 40, " %d %d\r\n", flags, nbytes - 2);
    return sizeof(item) + nkey + *nsuffix + nbytes; //返回item的长度
}

/*@null@*/
//执行item的存储操作,该操作会将item挂载到LRU表和slabcalss中 
item *do_item_alloc(char *key, const size_t nkey, const int flags,
                    const rel_time_t exptime, const int nbytes,
                    const uint32_t cur_hv) {
    uint8_t nsuffix;
    item *it = NULL;
    char suffix[40];
    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix); //计算item的总大小(空间) 
    if (settings.use_cas) { //如果使用了cas
        ntotal += sizeof(uint64_t); //增加cas的空间
    }

    unsigned int id = slabs_clsid(ntotal); //那大小选择合适的slab
    if (id == 0)
        return 0;

    mutex_lock(&cache_lock); //执行LRU锁
    /* do a quick check if we have any expired items in the tail.. */
    int tries = 5; //如果LRU中尝试5次还没合适的空间，则执行申请空间的操作 
    int tried_alloc = 0;
    item *search;
    void *hold_lock = NULL;
    rel_time_t oldest_live = settings.oldest_live; //初始化时选择的过期时间

    search = tails[id]; //第id个LRU表的尾部
    /* We walk up *only* for locked items. Never searching for expired.
     * Waste of CPU for almost all deployments */
    for (; tries > 0 && search != NULL; tries--, search=search->prev) {
        uint32_t hv = hash(ITEM_key(search), search->nkey, 0); //获取分段锁
        /* Attempt to hash item lock the "search" item. If locked, no
         * other callers can incr the refcount
         */
        /* FIXME: I think we need to mask the hv here for comparison? */
        if (hv != cur_hv && (hold_lock = item_trylock(hv)) == NULL) //尝试执行锁操作，这里执行的乐观锁  
            continue;
        /* Now see if the item is refcount locked */
        if (refcount_incr(&search->refcount) != 2) { //判断item是否被锁住，item的引用次数其实充当的也是一种锁  
            refcount_decr(&search->refcount); //更新it的引用次数
            /* Old rare bug could cause a refcount leak. We haven't seen
             * it in years, but we leave this code in to prevent failures
             * just in case */
            if (search->time + TAIL_REPAIR_TIME < current_time) { //如果it的添加时间比当前时间小于3*3600 
                itemstats[id].tailrepairs++; //更新统计信息
                search->refcount = 1;
                do_item_unlink_nolock(search, hv); //执行分段解锁操作
            }
            if (hold_lock)
                item_trylock_unlock(hold_lock); //执行分段解锁操作
            continue;
        }

        /* Expired or flushed */  //过期时间判断
        if ((search->exptime != 0 && search->exptime < current_time)
            || (search->time <= oldest_live && oldest_live <= current_time)) {
            itemstats[id].reclaimed++;
            if ((search->it_flags & ITEM_FETCHED) == 0) {
                itemstats[id].expired_unfetched++; //更新统计信息
            }
            it = search;
            slabs_adjust_mem_requested(it->slabs_clsid, ITEM_ntotal(it), ntotal);//slabclass申请合适的空间
            do_item_unlink_nolock(it, hv); //执行的Hash表的分段解锁操作
            /* Initialize the item block: */
            it->slabs_clsid = 0;
        } else if ((it = slabs_alloc(ntotal, id)) == NULL) { //申请合适的slabclass  
            tried_alloc = 1; //申请失败一次 
            if (settings.evict_to_free == 0) { //关闭了LRU的
                itemstats[id].outofmemory++; //统计信息更新
            } else { //打开了LRU的操作
                itemstats[id].evicted++; //更新统计信息
                itemstats[id].evicted_time = current_time - search->time;
                if (search->exptime != 0)
                    itemstats[id].evicted_nonzero++;
                if ((search->it_flags & ITEM_FETCHED) == 0) {
                    itemstats[id].evicted_unfetched++;
                }
                it = search;
                slabs_adjust_mem_requested(it->slabs_clsid, ITEM_ntotal(it), ntotal); //选择合适的slabclass空间
                do_item_unlink_nolock(it, hv); //执行it的分段解锁操作 
                /* Initialize the item block: */
                it->slabs_clsid = 0;

                /* If we've just evicted an item, and the automover is set to
                 * angry bird mode, attempt to rip memory into this slab class.
                 * TODO: Move valid object detection into a function, and on a
                 * "successful" memory pull, look behind and see if the next alloc
                 * would be an eviction. Then kick off the slab mover before the
                 * eviction happens.
                 */
                if (settings.slab_automove == 2) //如果打开了slab调整 
                    slabs_reassign(-1, id); //唤醒调整线程
            }
        }

        refcount_decr(&search->refcount); //更新引用次数  
        /* If hash values were equal, we don't grab a second lock */
        if (hold_lock)
            item_trylock_unlock(hold_lock); //解分段锁
        break;
    }

    if (!tried_alloc && (tries == 0 || search == NULL)) //5次循环查找，未找到合适的空间  
        it = slabs_alloc(ntotal, id); //则从内存池申请新的空间

    if (it == NULL) { //内存池申请失败
        itemstats[id].outofmemory++;  //更新统计信息
        mutex_unlock(&cache_lock); //释放LRU锁
        return NULL;
    }

    assert(it->slabs_clsid == 0);
    assert(it != heads[id]);

    /* Item initialization can happen outside of the lock; the item's already
     * been removed from the slab LRU.
     */
    it->refcount = 1;     /* the caller will have a reference */ //更新it的引用次数  
    mutex_unlock(&cache_lock);
    it->next = it->prev = it->h_next = 0; //执行初始化操作
    it->slabs_clsid = id; //it所属的slabclass为第id个

    DEBUG_REFCNT(it, '*');
    it->it_flags = settings.use_cas ? ITEM_CAS : 0;
    it->nkey = nkey; //it的key 
    it->nbytes = nbytes; //it的缓冲区的数据
    memcpy(ITEM_key(it), key, nkey); //it的数据信息
    it->exptime = exptime; //it的过期时间 
    memcpy(ITEM_suffix(it), suffix, (size_t)nsuffix); //it的前缀信息  
    it->nsuffix = nsuffix;
    return it;
}
//释放item
void item_free(item *it) {
    size_t ntotal = ITEM_ntotal(it); //获得item的大小
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0); //判断item的状态是否正确
    assert(it != heads[it->slabs_clsid]); //item不能为LRU的头指针
    assert(it != tails[it->slabs_clsid]); //item不能为LRU的尾指针
    assert(it->refcount == 0); //释放时，需保证引用次数为0

    /* so slab size changer can tell later if item is already free or not */
    clsid = it->slabs_clsid;
    it->slabs_clsid = 0; //断开slabclass的链接
    DEBUG_REFCNT(it, 'F');

    // 内存释放. memcached 采用了 slab 的内存管理方法.
    slabs_free(it, ntotal, clsid); //slabclass结构执行释放
}

/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
    char prefix[40];
    uint8_t nsuffix;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes,
                                     prefix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    return slabs_clsid(ntotal) != 0;
}
//将item加入到对应classid的LRU链的head，这里是item加入到LRU链表中
static void item_link_q(item *it) { /* item is the new head */
    item **head, **tail;
    assert(it->slabs_clsid < LARGEST_ID);
    assert((it->it_flags & ITEM_SLABBED) == 0);

    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];
    assert(it != *head);
    assert((*head && *tail) || (*head == 0 && *tail == 0));
    it->prev = 0;
    it->next = *head;
    if (it->next) it->next->prev = it; //执行插入数据操作
    *head = it;
    if (*tail == 0) *tail = it;
    sizes[it->slabs_clsid]++;
    return;
}
//将item从对应classid的LRU链上移除，这里是item从LRU链表中删除 
static void item_unlink_q(item *it) {
    item **head, **tail;
    assert(it->slabs_clsid < LARGEST_ID);
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    if (*head == it) {
        assert(it->prev == 0);
        *head = it->next;
    }
    if (*tail == it) {
        assert(it->next == 0);
        *tail = it->prev;
    }
    assert(it->next != it);
    assert(it->prev != it);

    if (it->next) it->next->prev = it->prev; //断开连接
    if (it->prev) it->prev->next = it->next;
    sizes[it->slabs_clsid]--;
    return;
}

int do_item_link(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    mutex_lock(&cache_lock);
    it->it_flags |= ITEM_LINKED;
    it->time = current_time;

    STATS_LOCK();
    stats.curr_bytes += ITEM_ntotal(it);
    stats.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();

    /* Allocate a new CAS ID on link. */
    ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
    assoc_insert(it, hv);
    item_link_q(it);
    refcount_incr(&it->refcount);
    mutex_unlock(&cache_lock);

    return 1;
}
//将item从hashtable和LRU链中移除。是do_item_link的逆操作
void do_item_unlink(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    mutex_lock(&cache_lock);
    if ((it->it_flags & ITEM_LINKED) != 0) { //判断状态值，保证item还在LRU队列中  
        it->it_flags &= ~ITEM_LINKED; //修改状态值
        STATS_LOCK(); //更新统计信息
        stats.curr_bytes -= ITEM_ntotal(it);
        stats.curr_items -= 1;
        STATS_UNLOCK();
        assoc_delete(ITEM_key(it), it->nkey, hv); //从Hash表中删除  
        item_unlink_q(it); //将item从slabclass对应的LRU队列摘除  
        do_item_remove(it); //移除item
    }
    mutex_unlock(&cache_lock);
}

/* FIXME: Is it necessary to keep this copy/pasted code? */
void do_item_unlink_nolock(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats.curr_bytes -= ITEM_ntotal(it);
        stats.curr_items -= 1;
        STATS_UNLOCK();
        assoc_delete(ITEM_key(it), it->nkey, hv);
        item_unlink_q(it);
        do_item_remove(it);
    }
}
//移除item
void do_item_remove(item *it) {
    MEMCACHED_ITEM_REMOVE(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0); //判断item的状态是否正确  

    if (refcount_decr(&it->refcount) == 0) { //修改item的引用次数
        item_free(it); //释放item
    }
}
//更新item，这个只更新时间
void do_item_update(item *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) { //更新有时间限制
        assert((it->it_flags & ITEM_SLABBED) == 0);

        mutex_lock(&cache_lock);
        if ((it->it_flags & ITEM_LINKED) != 0) { //更新LRU队列的Item
            item_unlink_q(it);  //断开连接
            it->time = current_time; //更新item的时间
            item_link_q(it); //重新添加
        }
        mutex_unlock(&cache_lock);
    }
}
//用新的item替换老的item
int do_item_replace(item *it, item *new_it, const uint32_t hv) {
    MEMCACHED_ITEM_REPLACE(ITEM_key(it), it->nkey, it->nbytes,
                           ITEM_key(new_it), new_it->nkey, new_it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0); //判断it是已经分配过的，如果未分配，则断言失败  

    do_item_unlink(it, hv); //断开连接
    return do_item_link(new_it, hv); //重新添加
}

/*@null@*/
char *do_item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes) {
    unsigned int memlimit = 2 * 1024 * 1024;   /* 2MB max response size */
    char *buffer;
    unsigned int bufcurr;
    item *it;
    unsigned int len;
    unsigned int shown = 0;
    char key_temp[KEY_MAX_LENGTH + 1];
    char temp[512];

    it = heads[slabs_clsid];

    buffer = malloc((size_t)memlimit);
    if (buffer == 0) return NULL;
    bufcurr = 0;

    while (it != NULL && (limit == 0 || shown < limit)) {
        assert(it->nkey <= KEY_MAX_LENGTH);
        /* Copy the key since it may not be null-terminated in the struct */
        strncpy(key_temp, ITEM_key(it), it->nkey);
        key_temp[it->nkey] = 0x00; /* terminate */
        len = snprintf(temp, sizeof(temp), "ITEM %s [%d b; %lu s]\r\n",
                       key_temp, it->nbytes - 2,
                       (unsigned long)it->exptime + process_started);
        if (bufcurr + len + 6 > memlimit)  /* 6 is END\r\n\0 */
            break;
        memcpy(buffer + bufcurr, temp, len);
        bufcurr += len;
        shown++;
        it = it->next;
    }

    memcpy(buffer + bufcurr, "END\r\n", 6);
    bufcurr += 5;

    *bytes = bufcurr;
    return buffer;
}

void item_stats_evictions(uint64_t *evicted) {
    int i;
    mutex_lock(&cache_lock);
    for (i = 0; i < LARGEST_ID; i++) {
        evicted[i] = itemstats[i].evicted;
    }
    mutex_unlock(&cache_lock);
}

void do_item_stats_totals(ADD_STAT add_stats, void *c) {
    itemstats_t totals;
    memset(&totals, 0, sizeof(itemstats_t));
    int i;
    for (i = 0; i < LARGEST_ID; i++) {
        totals.expired_unfetched += itemstats[i].expired_unfetched;
        totals.evicted_unfetched += itemstats[i].evicted_unfetched;
        totals.evicted += itemstats[i].evicted;
        totals.reclaimed += itemstats[i].reclaimed;
    }
    APPEND_STAT("expired_unfetched", "%llu",
                (unsigned long long)totals.expired_unfetched);
    APPEND_STAT("evicted_unfetched", "%llu",
                (unsigned long long)totals.evicted_unfetched);
    APPEND_STAT("evictions", "%llu",
                (unsigned long long)totals.evicted);
    APPEND_STAT("reclaimed", "%llu",
                (unsigned long long)totals.reclaimed);
}

void do_item_stats(ADD_STAT add_stats, void *c) {
    int i;
    for (i = 0; i < LARGEST_ID; i++) {
        if (tails[i] != NULL) {
            const char *fmt = "items:%d:%s";
            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;
            if (tails[i] == NULL) {
                /* We removed all of the items in this slab class */
                continue;
            }
            APPEND_NUM_FMT_STAT(fmt, i, "number", "%u", sizes[i]);
            APPEND_NUM_FMT_STAT(fmt, i, "age", "%u", current_time - tails[i]->time);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted",
                                "%llu", (unsigned long long)itemstats[i].evicted);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted_nonzero",
                                "%llu", (unsigned long long)itemstats[i].evicted_nonzero);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted_time",
                                "%u", itemstats[i].evicted_time);
            APPEND_NUM_FMT_STAT(fmt, i, "outofmemory",
                                "%llu", (unsigned long long)itemstats[i].outofmemory);
            APPEND_NUM_FMT_STAT(fmt, i, "tailrepairs",
                                "%llu", (unsigned long long)itemstats[i].tailrepairs);
            APPEND_NUM_FMT_STAT(fmt, i, "reclaimed",
                                "%llu", (unsigned long long)itemstats[i].reclaimed);
            APPEND_NUM_FMT_STAT(fmt, i, "expired_unfetched",
                                "%llu", (unsigned long long)itemstats[i].expired_unfetched);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted_unfetched",
                                "%llu", (unsigned long long)itemstats[i].evicted_unfetched);
        }
    }

    /* getting here means both ascii and binary terminators fit */
    add_stats(NULL, 0, NULL, 0, c);
}

/** dumps out a list of objects of each size, with granularity of 32 bytes */
/*@null@*/
void do_item_stats_sizes(ADD_STAT add_stats, void *c) {

    /* max 1MB object, divided into 32 bytes size buckets */
    const int num_buckets = 32768;
    unsigned int *histogram = calloc(num_buckets, sizeof(int));

    if (histogram != NULL) {
        int i;

        /* build the histogram */
        for (i = 0; i < LARGEST_ID; i++) {
            item *iter = heads[i];
            while (iter) {
                int ntotal = ITEM_ntotal(iter);
                int bucket = ntotal / 32;
                if ((ntotal % 32) != 0) bucket++;
                if (bucket < num_buckets) histogram[bucket]++;
                iter = iter->next;
            }
        }

        /* write the buffer */
        for (i = 0; i < num_buckets; i++) {
            if (histogram[i] != 0) {
                char key[8];
                snprintf(key, sizeof(key), "%d", i * 32);
                APPEND_STAT(key, "%u", histogram[i]);
            }
        }
        free(histogram);
    }
    add_stats(NULL, 0, NULL, 0, c);
}

/** wrapper around assoc_find which does the lazy expiration logic */
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv) {
    //mutex_lock(&cache_lock);

    // 查找 key 是否存在
    item *it = assoc_find(key, nkey, hv); //从Hash表中获取相应的结构  
    if (it != NULL) {
        refcount_incr(&it->refcount); //item的引用次数+1
        /* Optimization for slab reassignment. prevents popular items from
         * jamming in busy wait. Can only do this here to satisfy lock order
         * of item_lock, cache_lock, slabs_lock. */
        if (slab_rebalance_signal && //如果正在进行slab调整，且该item是调整的对象  
            ((void *)it >= slab_rebal.slab_start && (void *)it < slab_rebal.slab_end)) {
            do_item_unlink_nolock(it, hv); //将item从hashtable和LRU链中移除  
            do_item_remove(it); //删除item
            it = NULL;
        }
    }
    //mutex_unlock(&cache_lock);
    int was_found = 0;

    if (settings.verbose > 2) { //打印调试信息
        if (it == NULL) {
            fprintf(stderr, "> NOT FOUND %s", key);
        } else {
            fprintf(stderr, "> FOUND KEY %s", ITEM_key(it));
            was_found++;
        }
    }

    if (it != NULL) {
        //判断Memcached初始化是否开启过期删除机制，如果开启，则执行删除相关操作 
        if (settings.oldest_live != 0 && settings.oldest_live <= current_time &&
            it->time <= settings.oldest_live) {
            do_item_unlink(it, hv); //将item从hashtable和LRU链中移除
            do_item_remove(it); //删除item
            it = NULL;
            if (was_found) {
                fprintf(stderr, " -nuked by flush");
            }
            //判断item是否过期
        } else if (it->exptime != 0 && it->exptime <= current_time) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
            if (was_found) {
                fprintf(stderr, " -nuked by expire");
            }
        } else { //item的标识修改为已经读取  
            it->it_flags |= ITEM_FETCHED;
            DEBUG_REFCNT(it, '+');
        }
    }

    if (settings.verbose > 2)
        fprintf(stderr, "\n");

    return it;
}

item *do_item_touch(const char *key, size_t nkey, uint32_t exptime,
                    const uint32_t hv) {
    item *it = do_item_get(key, nkey, hv);
    if (it != NULL) {
        it->exptime = exptime;
    }
    return it;
}

/* expires items that are more recent than the oldest_live setting. */
void do_item_flush_expired(void) {
    int i;
    item *iter, *next;
    if (settings.oldest_live == 0)
        return;
    for (i = 0; i < LARGEST_ID; i++) {
        /* The LRU is sorted in decreasing time order, and an item's timestamp
         * is never newer than its last access time, so we only need to walk
         * back until we hit an item older than the oldest_live time.
         * The oldest_live checking will auto-expire the remaining items.
         */
        for (iter = heads[i]; iter != NULL; iter = next) {
            if (iter->time >= settings.oldest_live) {
                next = iter->next;
                if ((iter->it_flags & ITEM_SLABBED) == 0) {
                    do_item_unlink_nolock(iter, hash(ITEM_key(iter), iter->nkey, 0));
                }
            } else {
                /* We've hit the first old item. Continue to the next queue. */
                break;
            }
        }
    }
}
