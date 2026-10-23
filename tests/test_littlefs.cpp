// tests/test_littlefs.cpp — littlefs на RAM-блочном устройстве.
#include "os/fs/littlefs.hpp"
#include "platforms/stm32/sd_spi.hpp"
#include "hal/mock/mock_hal.hpp"

#include <cstdio>
#include <cstring>

using namespace embed;

namespace {
class RamDisk final : public fs::BlockDevice {
public:
    RamDisk() { std::memset(data_, 0xFF, sizeof(data_)); }
    uint32_t block_count() const noexcept override { return kBlocks; }
    uint32_t block_size() const noexcept override { return kBlock; }
    Status read(uint32_t lba, void* out, uint32_t count) noexcept override {
        if (lba >= kBlocks || count > kBlocks - lba) return Status::InvalidArg;
        std::memcpy(out, data_ + lba * kBlock, count * kBlock);
        return Status::Ok;
    }
    Status write(uint32_t lba, const void* in, uint32_t count) noexcept override {
        if (lba >= kBlocks || count > kBlocks - lba) return Status::InvalidArg;
        std::memcpy(data_ + lba * kBlock, in, count * kBlock);
        return Status::Ok;
    }
    Status erase(uint32_t lba, uint32_t count) noexcept override {
        if (lba >= kBlocks || count > kBlocks - lba) return Status::InvalidArg;
        std::memset(data_ + lba * kBlock, 0xFF, count * kBlock);
        return Status::Ok;
    }
    static constexpr uint32_t kBlock = 512u;
    static constexpr uint32_t kBlocks = 64u;
private:
    uint8_t data_[kBlock * kBlocks]{};
};

int total = 0, failed = 0;
void check(const char* what, bool ok) {
    ++total;
    if (!ok) ++failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}
}

int main() {
    std::puts("littlefs:");
    embed::hal::mock::MockGpioPin cd;
    embed::hal::mock::MockGpioPin cs;
    embed::hal::mock::MockSpi sd_bus;
    embed::stm32::sd::Card sd(sd_bus, cs, &cd, true);
    cd.set(false);   // active-low card detect: карта вставлена
    check("SD present (inserted)", sd.present());
    cd.set(true);
    check("SD present (removed)", !sd.present());
    check("SD poll removed", sd.poll() == Status::Unavailable && !sd.initialized());
    RamDisk disk;
    fs::LittleFs<512> lfs;
    check("format", lfs.format(disk) == Status::Ok);
    check("mount", lfs.mount(disk) == Status::Ok && lfs.mounted());
    const char text[] = "hello littlefs";
    check("write", lfs.write_file("/hello.txt", text, sizeof(text) - 1u) == Status::Ok);
    char out[32]{};
    Size got = 0u;
    check("read", lfs.read_file("/hello.txt", out, sizeof(out), got) == Status::Ok &&
          got == sizeof(text) - 1u && std::memcmp(out, text, got) == 0);
    check("mkdir", lfs.mkdir("/dir") == Status::Ok);
    check("rename", lfs.rename("/hello.txt", "/dir/hello.txt") == Status::Ok);
    check("remove", lfs.remove("/dir/hello.txt") == Status::Ok);
    lfs.unmount();
    check("remount", lfs.mount(disk) == Status::Ok && lfs.mounted());
    check("persist after remount", lfs.read_file("/dir/hello.txt", out, sizeof(out), got) ==
          Status::NotFound);
    lfs.unmount();
    std::printf("%d/%d проверок пройдено%s\n", total - failed, total,
                failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return failed == 0 ? 0 : 1;
}
