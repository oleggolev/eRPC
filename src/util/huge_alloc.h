#ifndef ERPC_HUGE_ALLOC_H
#define ERPC_HUGE_ALLOC_H

#include <errno.h>
#include <malloc.h>
#include <numaif.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "common.h"
#include "util/rand.h"

namespace ERpc {
/**
 * @brief An allocator that allows:
 * (a) Allocating and deallocating hugepage-backed individual 4K pages.
 * (b) Allocating, but *NOT* deallocating chunks of size >= 2MB.
 *
 * The allocator uses randomly generated positive SHM keys, and deallocates the
 * SHM regions created when it is deleted.
 */
class HugeAllocator {
 private:
  static const size_t kMaxAllocSize = (256 * 1024 * 1024 * 1024ull);

  /* Information about an SHM region. */
  struct shm_region_t {
    int key;        /* The key used to create the SHM region */
    void *base_buf; /* The start address of the allocated SHM buffer */
    size_t size;    /* The size in bytes of the allocated buffer */

    void *cur_buf;         /* Pointer to the currently free hugepage */
    size_t free_hugepages; /* The number of hugepages left in this region */

    shm_region_t(int key, void *buf, size_t size)
        : key(key), base_buf(buf), size(size), cur_buf(buf) {
      assert(size % kHugepageSize == 0);
      free_hugepages = size / kHugepageSize;
    }
  };

  SlowRand slow_rand; /* RNG to generate SHM keys */
  size_t numa_node;   /* NUMA node on which all memory is allocated */

  /*
   * SHM regions used by this allocator, in order of increasing allocation-time
   * size.
   */
  std::vector<shm_region_t> shm_list;
  std::vector<void *> page_freelist; /* Currently free 4k pages */

  size_t tot_free_hugepages; /* Number of free hugepages over all SHM regions */

  /*
   * The size of the previous hugepage allocation made internally by this
   * allocator.
   */
  size_t prev_allocation_size;
  size_t tot_memory_reserved;  /* Total hugepage memory reserved by allocator */
  size_t tot_memory_allocated; /* Total memory allocated to users */

 public:
  HugeAllocator(size_t initial_size, size_t numa_node)
      : numa_node(numa_node),
        tot_free_hugepages(0),
        prev_allocation_size(initial_size),
        tot_memory_reserved(0),
        tot_memory_allocated(0) {
    assert(initial_size > 0 && initial_size <= kMaxAllocSize);
    assert(numa_node <= kMaxNumaNodes);

    /*
     * Reserve initial hugepages. \p reserve_hugepages will throw a runtime
     * exception if reservation fails.
     */
    reserve_hugepages(initial_size, numa_node);
  }

  ~HugeAllocator() {
    /* Delete the created SHM regions */
    for (shm_region_t &shm_region : shm_list) {
      delete_shm(shm_region.key, shm_region.base_buf);
    }
  }

  size_t get_numa_node() { return numa_node; }

  /// Allocate a 4K page.
  forceinline void *alloc_page() {
    if (page_freelist.size() != 0) {
      void *free_page = page_freelist.back();
      page_freelist.pop_back();

      tot_memory_allocated += kPageSize;
      return free_page;
    } else {
      /* There is no free 4K page. */
      if (tot_free_hugepages == 0) {
        prev_allocation_size *= 2;
        bool success = reserve_hugepages(prev_allocation_size, numa_node);
        if (!success) {
          return nullptr; /* We're out of hugepages */
        }
      }

      /*
       * If we are here, there is at least one SHM region with a free hugepage.
       * Pick the smallest SHM region with a free hugepage and carve it into
       * 4K pages. Note that multiple SHM regions can have free hugepages.
       */
      for (shm_region_t &shm_region : shm_list) {
        if (shm_region.free_hugepages > 0) {
          for (size_t i = 0; i < kHugepageSize; i += kPageSize) {
            void *page_addr = (void *)((char *)shm_region.cur_buf + i);
            page_freelist.push_back(page_addr);
          }

          pop_hugepages(shm_region, 1);

          /* If we are here, we have a free 4K page */
          assert(page_freelist.size() > 0);
          void *free_page = page_freelist.back();
          page_freelist.pop_back();

          tot_memory_allocated += kPageSize;
          return free_page;
        }
      }
    }

    exit(-1); /* We should never get here */
    return nullptr;
  }

  forceinline void free_page(void *page) {
    assert((uintptr_t)page % KB(4) == 0);
    page_freelist.push_back(page);

    tot_memory_allocated -= kPageSize;
  }

  inline void *alloc_huge(size_t size) {
    assert(size >= ERpc::kHugepageSize && size <= kMaxAllocSize);

    size = round_up<kHugepageSize>(size);
    size_t reqd_hugepages = size / kHugepageSize;

    for (shm_region_t &shm_region : shm_list) {
      if (shm_region.free_hugepages >= reqd_hugepages) {
        void *hugebuf_addr = shm_region.cur_buf; /* Copy before popping */
        pop_hugepages(shm_region, reqd_hugepages);

        tot_memory_allocated += size;
        return hugebuf_addr;
      }
    }

    /*
     * If here, no existing SHM region has sufficient hugepages. Increase the
     * allocation size, and ensure that we can allocate at least \p size.
     */
    prev_allocation_size *= 2;
    while (prev_allocation_size < size) {
      prev_allocation_size *= 2;
    }

    bool success = reserve_hugepages(prev_allocation_size, numa_node);
    if (!success) {
      /* We're out of hugepages */
      return nullptr;
    }

    /*
     * Use the last SHM region in the list to allocate. Other regions don't
     * have enough space.
     */
    shm_region_t &shm_region = shm_list.back();
    void *hugebuf_addr = shm_region.cur_buf; /* Copy before popping */

    pop_hugepages(shm_region, reqd_hugepages);

    tot_memory_allocated += size;
    return hugebuf_addr;
  }

  /// Return the total amount of memory reserved as hugepages.
  size_t get_reserved_memory() {
    assert(tot_memory_reserved % kHugepageSize == 0);
    return tot_memory_reserved;
  }

  /// Return the total amount of memory allocated to the user.
  size_t get_allocated_memory() {
    assert(tot_memory_allocated % kPageSize == 0);
    return tot_memory_allocated;
  }

 private:
  /// Remove num_hugepages from the beginning of this SHM region.
  inline void pop_hugepages(shm_region_t &shm_region, size_t num_hugepages) {
    assert(shm_region.free_hugepages >= num_hugepages);
    shm_region.cur_buf =
        (void *)((char *)shm_region.cur_buf + (num_hugepages * kHugepageSize));

    shm_region.free_hugepages -= num_hugepages;
    tot_free_hugepages -= num_hugepages;
  }

  /**
   * @brief Try to reserve \p size (rounded to 2MB) bytes as huge pages on
   * NUMA node \p numa_node.
   *
   * @return True if the allocation succeeds. False if the allocation fails
   * because no more hugepages are available.
   *
   * @throw runtime_error If allocation fails for a reason other than out
   * of memory.
   */
  bool reserve_hugepages(size_t size, size_t numa_node) {
    std::ostringstream xmsg; /* The exception message */
    size = round_up<kHugepageSize>(size);
    int shm_key, shm_id;

    while (true) {
      /*
       * Choose a positive SHM key. Negative is fine but it looks scary in the
       * error message.
       */
      shm_key = static_cast<int>(slow_rand.next_u64());
      shm_key = std::abs(shm_key);

      /* Try to get an SHM region */
      shm_id = shmget(shm_key, size, IPC_CREAT | IPC_EXCL | 0666 | SHM_HUGETLB);

      if (shm_id == -1) {
        switch (errno) {
          case EEXIST:
            /* @shm_key already exists. Try again. */
            break;

          case EACCES:
            xmsg << "eRPC HugeAllocator: SHM allocation error. "
                 << "Insufficient permissions.";
            throw std::runtime_error(xmsg.str());

          case EINVAL:
            xmsg << "eRPC HugeAllocator: SHM malloc error: SHMMAX/SHMIN "
                 << "mismatch. size = " << std::to_string(size) << " ("
                 << std::to_string(size / MB(1)) << " MB)";
            throw std::runtime_error(xmsg.str());

          case ENOMEM:
            erpc_dprintf("eRPC HugeAllocator: SHM malloc error: Insufficient "
                         "memory. SHM key = %d, size = %lu (%lu MB).\n",
                         shm_key, size, size / MB(1));
            return false;

          default:
            xmsg << "eRPC HugeAllocator: Unexpected SHM malloc error "
                 << strerror(errno);
            throw std::runtime_error(xmsg.str());
        }
      } else {
        /* shm_key worked. Break out of the while loop */
        break;
      }
    }

    void *shm_buf = shmat(shm_id, nullptr, 0);
    if (shm_buf == nullptr) {
      fprintf(stderr,
              "eRPC HugeAllocator: SHM malloc error: shmat() failed "
              "for key %d\n",
              shm_key);
      exit(-1);
    }

    /* Bind the buffer to the NUMA node */
    const unsigned long nodemask = (1ul << (unsigned long)numa_node);
    long ret = mbind(shm_buf, size, MPOL_BIND, &nodemask, 32, 0);
    if (ret != 0) {
      fprintf(stderr,
              "eRPC HugeAllocator: SHM malloc error. mbind() failed "
              "for key %d\n",
              shm_key);
      exit(-1);
    }

    /* If we are here, the allocation succeeded. Record for deallocation. */
    memset(shm_buf, 0, size);

    shm_list.push_back(shm_region_t(shm_key, shm_buf, size));
    tot_free_hugepages += (size / kHugepageSize);
    tot_memory_reserved += size;

    return true;
  }

  void delete_shm(int shm_key, void *shm_buf) {
    int shmid = shmget(shm_key, 0, 0);
    if (shmid == -1) {
      switch (errno) {
        case EACCES:
          fprintf(stderr,
                  "eRPC HugeAllocator: SHM free error: "
                  "Insufficient permissions. SHM key = %d.\n",
                  shm_key);
          break;
        case ENOENT:
          fprintf(stderr,
                  "eRPC HugeAllocator: SHM free error: No such SHM key."
                  "SHM key = %d.\n",
                  shm_key);
          break;
        default:
          fprintf(stderr,
                  "eRPC HugeAllocator: SHM free error: A wild SHM error: "
                  "%s\n",
                  strerror(errno));
          break;
      }

      exit(-1);
    }

    int ret = shmctl(shmid, IPC_RMID, nullptr); /* Please don't fail */
    if (ret != 0) {
      fprintf(stderr, "eRPC HugeAllocator: Error freeing SHM ID %d\n", shmid);
      exit(-1);
    }

    ret = shmdt(shm_buf);
    if (ret != 0) {
      fprintf(stderr,
              "HugeAllocator: Error freeing SHM buf %p. "
              "(SHM key = %d)\n",
              shm_buf, shm_key);
      exit(-1);
    }
  }
};
}

#endif  // ERPC_HUGE_ALLOC_H
