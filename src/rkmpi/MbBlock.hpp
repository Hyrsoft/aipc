#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <spdlog/spdlog.h>

#include "sample_comm.h"

namespace aipc::rkmpi {

    class MbBlock {
    public:
        MbBlock() = default;

        MbBlock(MB_BLK blk, size_t bytes) : blk_(blk), bytes_(bytes) {}

        ~MbBlock() { reset(); }

        MbBlock(const MbBlock &) = delete;
        MbBlock &operator=(const MbBlock &) = delete;

        MbBlock(MbBlock &&other) noexcept : blk_(other.blk_), bytes_(other.bytes_), cached_vir_addr_(other.cached_vir_addr_) {
            other.blk_ = MB_INVALID_HANDLE;
            other.bytes_ = 0;
            other.cached_vir_addr_ = nullptr;
        }

        MbBlock &operator=(MbBlock &&other) noexcept {
            if (this != &other) {
                reset();
                blk_ = other.blk_;
                bytes_ = other.bytes_;
                cached_vir_addr_ = other.cached_vir_addr_;
                other.blk_ = MB_INVALID_HANDLE;
                other.bytes_ = 0;
                other.cached_vir_addr_ = nullptr;
            }
            return *this;
        }

        static std::shared_ptr<MbBlock> Get(MB_POOL pool, size_t bytes, bool block = true) {
            if (pool == MB_INVALID_POOLID) {
                return {};
            }

            MB_BLK blk = RK_MPI_MB_GetMB(pool, bytes, block ? RK_TRUE : RK_FALSE);
            if (blk == MB_INVALID_HANDLE) {
                SPDLOG_ERROR("RK_MPI_MB_GetMB failed (bytes={})", bytes);
                return {};
            }

            return std::make_shared<MbBlock>(blk, bytes);
        }

        bool ok() const { return blk_ != MB_INVALID_HANDLE; }

        MB_BLK handle() const { return blk_; }

        size_t size() const { return bytes_; }

        void *virAddr() const {
            if (!ok()) {
                return nullptr;
            }
            if (cached_vir_addr_ != nullptr) {
                return cached_vir_addr_;
            }
            cached_vir_addr_ = RK_MPI_MB_Handle2VirAddr(blk_);
            return cached_vir_addr_;
        }

        void reset() {
            if (blk_ != MB_INVALID_HANDLE) {
                RK_MPI_MB_ReleaseMB(blk_);
                blk_ = MB_INVALID_HANDLE;
                bytes_ = 0;
                cached_vir_addr_ = nullptr;
            }
        }

    private:
        MB_BLK blk_{MB_INVALID_HANDLE};
        size_t bytes_{0};
        mutable void *cached_vir_addr_{nullptr};
    };

} // namespace aipc::rkmpi
