#pragma once

#include <cstdint>

#include <spdlog/spdlog.h>

#include "sample_comm.h"

namespace aipc::rkmpi {

    class MbPool {
    public:
        MbPool() = default;

        explicit MbPool(MB_POOL pool) : pool_(pool) {}

        ~MbPool() { reset(); }

        MbPool(const MbPool &) = delete;
        MbPool &operator=(const MbPool &) = delete;

        MbPool(MbPool &&other) noexcept : pool_(other.pool_) { other.pool_ = MB_INVALID_POOLID; }

        MbPool &operator=(MbPool &&other) noexcept {
            if (this != &other) {
                reset();
                pool_ = other.pool_;
                other.pool_ = MB_INVALID_POOLID;
            }
            return *this;
        }

        static MbPool create(uint64_t bytes_per_block, uint32_t count, MB_ALLOC_TYPE_E alloc_type = MB_ALLOC_TYPE_DMA) {
            MB_POOL_CONFIG_S cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.u64MBSize = bytes_per_block;
            cfg.u32MBCnt = count;
            cfg.enAllocType = alloc_type;

            MB_POOL pool = RK_MPI_MB_CreatePool(&cfg);
            if (pool == MB_INVALID_POOLID) {
                SPDLOG_ERROR("RK_MPI_MB_CreatePool failed");
                return MbPool{};
            }

            return MbPool(pool);
        }

        bool ok() const { return pool_ != MB_INVALID_POOLID; }

        MB_POOL get() const { return pool_; }

        void reset() {
            if (pool_ != MB_INVALID_POOLID) {
                RK_MPI_MB_DestroyPool(pool_);
                pool_ = MB_INVALID_POOLID;
            }
        }

    private:
        MB_POOL pool_{MB_INVALID_POOLID};
    };

} // namespace aipc::rkmpi
