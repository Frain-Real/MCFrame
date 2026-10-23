// os/fs/littlefs.hpp
// ---------------------------------------------------------------------------
// littlefs 2.x adapter. lfs.c/lfs.h/lfs_util.h входят в src/os/fs и
// собираются с LFS_NO_MALLOC: все буферы статические.
//
// BlockSize — размер erase-блока (SD: 512; NOR/Flash: 4096). LittleFs<4096>
// создаёт три буфера по 4 КБ; по умолчанию 512.
// ---------------------------------------------------------------------------
#pragma once

#include "block_device.hpp"

#if __has_include("lfs.h")
#include "lfs.h"

namespace embed::fs {

template <lfs_size_t BlockSize = 512u, lfs_size_t Lookahead = 32u>
class LittleFs {
    static_assert(BlockSize > 0u, "littlefs block size");
    static_assert(Lookahead > 0u, "littlefs lookahead");

public:
    LittleFs() noexcept = default;
    ~LittleFs() noexcept { unmount(); }
    LittleFs(const LittleFs&) = delete;
    LittleFs& operator=(const LittleFs&) = delete;

    Status mount(BlockDevice& dev) noexcept {
        if (mounted_) return Status::AlreadyExists;
        const Status st = prepare(dev);
        if (st != Status::Ok) return st;
        const int rc = lfs_mount(&lfs_, &cfg_);
        if (rc != 0) return from_err(rc);
        mounted_ = true;
        return Status::Ok;
    }

    /// Форматирование разрушает данные; вызывать только явно.
    Status format(BlockDevice& dev) noexcept {
        if (mounted_) unmount();
        const Status st = prepare(dev);
        if (st != Status::Ok) return st;
        const int rc = lfs_format(&lfs_, &cfg_);
        if (rc != 0) return from_err(rc);
        return Status::Ok;
    }

    void unmount() noexcept {
        if (mounted_) {
            (void)lfs_unmount(&lfs_);
            mounted_ = false;
        }
    }

    bool mounted() const noexcept { return mounted_; }

    Status write_file(const char* path, const void* data, Size len) noexcept {
        if (!mounted_ || path == nullptr) return Status::Unavailable;
        if (len > 0x7FFFFFFFu || (data == nullptr && len != 0u)) return Status::InvalidArg;
        lfs_file_t f{};
        lfs_file_config fc{};
        fc.buffer = file_buf_;
        int rc = lfs_file_opencfg(&lfs_, &f, path,
                                  LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fc);
        if (rc != 0) return from_err(rc);
        const lfs_ssize_t n = lfs_file_write(&lfs_, &f, data, static_cast<lfs_size_t>(len));
        const int c = lfs_file_close(&lfs_, &f);
        if (n < 0) return from_err(static_cast<int>(n));
        if (c != 0) return from_err(c);
        return (static_cast<Size>(n) == len) ? Status::Ok : Status::NoMemory;
    }

    Status read_file(const char* path, void* data, Size cap, Size& got) noexcept {
        got = 0u;
        if (!mounted_ || path == nullptr || data == nullptr) return Status::Unavailable;
        lfs_file_t f{};
        lfs_file_config fc{};
        fc.buffer = file_buf_;
        int rc = lfs_file_opencfg(&lfs_, &f, path, LFS_O_RDONLY, &fc);
        if (rc != 0) return from_err(rc);
        const lfs_ssize_t n = lfs_file_read(&lfs_, &f, data, static_cast<lfs_size_t>(cap));
        const int c = lfs_file_close(&lfs_, &f);
        if (n < 0) return from_err(static_cast<int>(n));
        if (c != 0) return from_err(c);
        got = static_cast<Size>(n);
        return Status::Ok;
    }

    Status remove(const char* path) noexcept {
        if (!mounted_ || path == nullptr) return Status::Unavailable;
        const int rc = lfs_remove(&lfs_, path);
        return rc == 0 ? Status::Ok : from_err(rc);
    }

    Status rename(const char* old_path, const char* new_path) noexcept {
        if (!mounted_ || old_path == nullptr || new_path == nullptr) return Status::Unavailable;
        const int rc = lfs_rename(&lfs_, old_path, new_path);
        return rc == 0 ? Status::Ok : from_err(rc);
    }

    Status mkdir(const char* path) noexcept {
        if (!mounted_ || path == nullptr) return Status::Unavailable;
        const int rc = lfs_mkdir(&lfs_, path);
        return rc == 0 ? Status::Ok : from_err(rc);
    }

    lfs_t& raw() noexcept { return lfs_; }

private:
    static Status from_err(int rc) noexcept {
        switch (rc) {
            case LFS_ERR_OK:      return Status::Ok;
            case LFS_ERR_NOENT:   return Status::NotFound;
            case LFS_ERR_EXIST:   return Status::AlreadyExists;
            case LFS_ERR_NOSPC:   return Status::NoMemory;
            case LFS_ERR_NOMEM:   return Status::NoMemory;
            case LFS_ERR_INVAL:   return Status::InvalidArg;
            case LFS_ERR_CORRUPT: return Status::Corruption;
            case LFS_ERR_IO:      return Status::HardwareError;
            default:              return Status::HardwareError;
        }
    }

    Status prepare(BlockDevice& dev) noexcept {
        if (!dev.present()) return Status::Unavailable;
        if (dev.block_size() != BlockSize) return Status::NotSupported;
        if (dev.block_count() == 0u) return Status::InvalidArg;
        dev_ = &dev;
        cfg_ = lfs_config{};
        cfg_.context = this;
        cfg_.read = &LittleFs::read_cb;
        cfg_.prog = &LittleFs::prog_cb;
        cfg_.erase = &LittleFs::erase_cb;
        cfg_.sync = &LittleFs::sync_cb;
        cfg_.read_size = BlockSize;
        // littlefs должен уметь дописывать CRC/pad в конец metadata-блока;
        // prog_size == block_size не оставляет места для этого.
        cfg_.prog_size = 1u;
        cfg_.block_size = BlockSize;
        cfg_.block_count = dev.block_count();
        cfg_.cache_size = BlockSize;
        cfg_.lookahead_size = Lookahead;
        cfg_.block_cycles = 500;
        cfg_.read_buffer = read_buf_;
        cfg_.prog_buffer = prog_buf_;
        cfg_.lookahead_buffer = lookahead_buf_;
        cfg_.name_max = 64u;
        return Status::Ok;
    }

    static int read_cb(const struct lfs_config* c, lfs_block_t block,
                       lfs_off_t off, void* buffer, lfs_size_t size) noexcept {
        auto* self = static_cast<LittleFs*>(c->context);
        if (self == nullptr || block >= self->cfg_.block_count ||
            off + size > BlockSize) {
            return LFS_ERR_INVAL;
        }
        const Status st = self->device()->read(static_cast<uint32_t>(block), self->tmp_, 1u);
        if (st != Status::Ok) return LFS_ERR_IO;
        auto* out = static_cast<uint8_t*>(buffer);
        for (lfs_size_t i = 0u; i < size; ++i) out[i] = self->tmp_[off + i];
        return 0;
    }

    static int prog_cb(const struct lfs_config* c, lfs_block_t block,
                       lfs_off_t off, const void* buffer, lfs_size_t size) noexcept {
        auto* self = static_cast<LittleFs*>(c->context);
        if (self == nullptr || block >= self->cfg_.block_count ||
            off + size > BlockSize) {
            return LFS_ERR_INVAL;
        }
        const Status rd = self->device()->read(static_cast<uint32_t>(block), self->tmp_, 1u);
        if (rd != Status::Ok) return LFS_ERR_IO;
        const auto* in = static_cast<const uint8_t*>(buffer);
        for (lfs_size_t i = 0u; i < size; ++i) self->tmp_[off + i] = in[i];
        const Status wr = self->device()->write(static_cast<uint32_t>(block), self->tmp_, 1u);
        if (wr != Status::Ok) return LFS_ERR_IO;
        return 0;
    }

    static int erase_cb(const struct lfs_config* c, lfs_block_t block) noexcept {
        auto* self = static_cast<LittleFs*>(c->context);
        if (self == nullptr || block >= self->cfg_.block_count) return LFS_ERR_INVAL;
        const Status er = self->device()->erase(static_cast<uint32_t>(block), 1u);
        if (er == Status::Ok) return 0;
        if (er != Status::NotSupported) return LFS_ERR_IO;
        // Fallback для SD/RAM: перезапись 0xFF. Для NOR нужен реальный
        // erase(), иначе littlefs будет работать некорректно.
        for (lfs_size_t i = 0u; i < BlockSize; ++i) self->tmp_[i] = 0xFFu;
        return self->device()->write(static_cast<uint32_t>(block), self->tmp_, 1u)
            == Status::Ok ? 0 : LFS_ERR_IO;
    }

    static int sync_cb(const struct lfs_config* c) noexcept {
        auto* self = static_cast<LittleFs*>(c->context);
        return (self != nullptr && self->device()->sync() == Status::Ok) ? 0 : LFS_ERR_IO;
    }

    BlockDevice* device() const noexcept { return dev_; }

    BlockDevice* dev_ = nullptr;
    lfs_t lfs_{};
    lfs_config cfg_{};
    bool mounted_ = false;
    uint8_t read_buf_[BlockSize]{};
    uint8_t prog_buf_[BlockSize]{};
    uint8_t file_buf_[BlockSize]{};
    uint8_t lookahead_buf_[Lookahead]{};
    uint8_t tmp_[BlockSize]{};
};

} // namespace embed::fs

#else

namespace embed::fs {
class LittleFs {
public:
    Status mount(BlockDevice&) noexcept { return Status::NotSupported; }
    Status format(BlockDevice&) noexcept { return Status::NotSupported; }
    void unmount() noexcept {}
    bool mounted() const noexcept { return false; }
    Status write_file(const char*, const void*, Size) noexcept { return Status::NotSupported; }
    Status read_file(const char*, void*, Size, Size& got) noexcept {
        got = 0u;
        return Status::NotSupported;
    }
    Status remove(const char*) noexcept { return Status::NotSupported; }
    Status rename(const char*, const char*) noexcept { return Status::NotSupported; }
    Status mkdir(const char*) noexcept { return Status::NotSupported; }
};
} // namespace embed::fs
#endif
