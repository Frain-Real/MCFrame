    #include "../../os/core/memory/memory_manager.hpp"
    
    uint32_t embed::platform_max_heap_size() noexcept {
        return 2048;
    }
    uint32_t embed::platform_max_stack_pool_size() noexcept{
        return 2048;
    }
    bool embed::platform_has_ext_ram() noexcept {
        return false;
    }
    uint32_t* embed::platform_ext_ram_base() noexcept{
        return 0;
    }
    uint32_t embed::platform_max_ext_ram_size() noexcept{
        return 0;
    }
    bool embed::platform_has_ext_rom() noexcept{
        return false;
    }
    uint32_t embed::platform_max_ext_rom_size() noexcept{
        return 0;
    }
    bool embed::platform_ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept {
        return false;
    }
    bool embed::platform_ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept {
        return false;
    }
    bool embed::platform_ext_rom_erase(uint32_t offset, uint32_t len) noexcept {
        return false;
    }
    void embed::platform_stack_paint(uint32_t* stack, uint32_t words) noexcept {

    }
    uint32_t* embed::platform_stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) noexcept {
        return 0;
    }
