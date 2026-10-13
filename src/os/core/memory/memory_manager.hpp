// memory_manager.hpp
#pragma once
#include "../../common.hpp"
#include "../task/task.hpp"
#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace embed {

    // ────────────────────────────────────────────────────────────────────────
    // Ёмкости менеджера памяти.
    //
    // ЗАЧЕМ ЭТО ЗДЕСЬ: прежние константы (32 задачи, две отдельные таблицы)
    // занимали ~1 КБ BSS — больше, чем все задачи примера вместе. Теперь
    // ёмкость задаётся конфигурацией сборки, а таблицы объединены.
    // ────────────────────────────────────────────────────────────────────────
    #ifndef MCFRAME_MAX_TASKS
    #define MCFRAME_MAX_TASKS 8u
    #endif

    /// Максимальное количество задач/стеков, отслеживаемых менеджером.
    static constexpr uint32_t kMaxTrackedTasks = MCFRAME_MAX_TASKS;

    static_assert(kMaxTrackedTasks >= 1u, "нужна хотя бы одна задача");
    static_assert(kMaxTrackedTasks <= kMaxTasks,
                  "MCFRAME_MAX_TASKS не может превышать kMaxTasks планировщика");

    class memory_manager {
    public:
        // ──── Heap ────
        // Заголовок блока. size ВСЕГДА содержит реальный размер блока
        // (включая заголовок); признак «свободен» — флаг в kFreeBit.
        // Прежде size == 0 означал одновременно «свободен» и «до конца кучи»,
        // из-за чего статистика и выделение расходились, а free() терял размер.
        static constexpr Size kFreeBit = static_cast<Size>(1) << (sizeof(Size) * 8u - 1u);

        struct heap_block {
            Size        size;    // реальный размер | kFreeBit
            Size        magic;    // kHeapMagic — детекция порчи/двойного free
            heap_block* next;    // только для свободных блоков (в порядке адресов)

            bool is_free() const noexcept { return (size & kFreeBit) != 0u; }
            Size real_size() const noexcept { return size & ~kFreeBit; }
            void set_size(Size n, bool free) noexcept {
                size = (n & ~kFreeBit) | (free ? kFreeBit : 0u);
            }
        };

        static constexpr Size kHeapMagic    = 0x48454150u;   // 'HEAP'
        /// Выравнивание покрывает max_align_t (на 64-битном хосте это 16),
        /// иначе heap_alloc вернул бы неверно выровненный указатель.
        static constexpr Size kHeapAlign =
            (alignof(std::max_align_t) < 8u) ? 8u
                                             : static_cast<Size>(alignof(std::max_align_t));
        static constexpr Size kHeapMinBlock = 16u;

        /// Учёт задачи и её стека.
        ///
        /// ОДНА таблица вместо прежних двух (stack_slot + task_mem_info),
        /// которые дублировали base/words и занимали 1024 Б вместо 512.
        struct task_slot {
            TaskHandler task       = nullptr;   // nullptr = слот свободен
            uint32_t*   stack_base = nullptr;   // младший адрес
            Size        stack_words = 0u;       // слов выдано
            Size        high_water  = 0u;       // минимум свободных слов за всё время
        };

    private:
        // ──── Heap ────
        uint8_t*    heap_start_     = nullptr;
        Size        heap_size_      = 0;
        heap_block* heap_first_     = nullptr;   // первый блок (по адресу)
        heap_block* heap_free_list_ = nullptr;   // свободные, в порядке адресов
        Size        heap_free_      = 0;         // ВЕДЁТСЯ, а не вычисляется обходом
        Size        heap_faults_    = 0;         // испорченные / двойные free
        bool        initialized_    = false;

        // ──── Stack pool ────
        uint32_t*   stack_pool_base_ = nullptr;
        Size        stack_pool_size_ = 0;   // всего слов
        Size        stack_pool_used_ = 0;   // занято слов

        // ──── Учёт задач ────
        task_slot   slots_[kMaxTrackedTasks]{};

        // ──── External RAM/ROM ────
        uint32_t*   ext_ram_base_   = nullptr;
        Size        ext_ram_size_   = 0;   // слов
        bool        ext_ram_active_ = false;
        Size        ext_rom_size_   = 0;   // байт
        bool        ext_rom_active_ = false;

        task_slot* find_slot(TaskHandler t) noexcept;
        task_slot* find_slot_by_stack(uint32_t* base) noexcept;
        void       heap_coalesce(heap_block* b) noexcept;

        static Size align_up(Size n, Size align) noexcept {
            return (n + align - 1u) & ~(align - 1u);
        }

    public:
        memory_manager() noexcept = default;
        memory_manager(const memory_manager&)            = delete;
        memory_manager& operator=(const memory_manager&) = delete;

        // ──────────── Init ────────────
        // Платформа даёт максимумы и АДРЕСА, приложение — сколько реально
        // использовать. Значения обрезаются до максимумов, фактические
        // размеры доступны через heap_total()/stack_pool_total().
        //
        // heap_bytes == 0 ИЛИ capacity == 0 => куча не поднимается
        // (heap_alloc всегда вернёт nullptr), и регион в линкере можно не
        // резервировать вовсе.
        Status init(Size heap_bytes, Size stack_pool_words,
                    Size ext_ram_words = 0, Size ext_rom_bytes = 0) noexcept;
        bool   is_initialized() const noexcept { return initialized_; }

        // ──────────── Stack pool ────────────
        uint32_t* stack_alloc(Size words) noexcept;
        void      stack_free(uint32_t* stack) noexcept;

        /// Зарегистрировать задачу, покрасить стек и собрать стартовый фрейм.
        /// Возвращает SP для первой загрузки либо nullptr.
        uint32_t* task_stack_init(TaskHandler t, uint32_t* stack, Size words) noexcept;
        /// Снять задачу с учёта (вызывается из scheduler::remove_task).
        void task_unregister(TaskHandler t) noexcept;

        // ──────────── Heap ────────────
        void* heap_alloc(Size bytes) noexcept;
        void  heap_free(void* ptr) noexcept;
        /// Куча сконфигурирована и готова к работе.
        bool  heap_available() const noexcept { return heap_size_ != 0u; }

        // ──────────── Monitoring ────────────
        Size task_stack_free(TaskHandler t) noexcept;   // свободных слов
        Size task_stack_high_water(TaskHandler t) noexcept;
        bool task_stack_overflow(TaskHandler t) noexcept;
        void task_update_high_water(TaskHandler t) noexcept;
        void update_all_high_water() noexcept;
        /// Сбросить водяной знак в «стек полностью свободен».
        /// Нужно, чтобы измерять пик потребления конкретного участка кода:
        /// сбросить → дать задаче поработать → прочитать high_water().
        void task_reset_high_water(TaskHandler t) noexcept;
        void reset_all_high_water() noexcept;

        // ──────────── Diagnostics ────────────
        /// Полная проверка целостности кучи (для тестов и watchdog).
        bool heap_check() const noexcept;

        // ──────────── External ROM (файловое хранилище) ────────────
        bool ext_rom_active() const noexcept { return ext_rom_active_; }
        bool ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) noexcept;
        bool ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) noexcept;
        bool ext_rom_erase(uint32_t offset, uint32_t len) noexcept;

        // ──────────── Stats ────────────
        Size heap_total()       const noexcept { return heap_size_; }
        Size heap_free_bytes()  const noexcept { return heap_free_; }  // O(1)
        Size heap_used()        const noexcept { return heap_size_ - heap_free_; }
        Size heap_faults()      const noexcept { return heap_faults_; }
        Size heap_largest_free() const noexcept;

        Size stack_pool_total() const noexcept { return stack_pool_size_; }
        Size stack_pool_used()  const noexcept { return stack_pool_used_; }
        Size stack_pool_free()  const noexcept { return stack_pool_size_ - stack_pool_used_; }

        Size ext_ram_total()    const noexcept { return ext_ram_size_; }
        bool ext_ram_active()   const noexcept { return ext_ram_active_; }
        Size ext_rom_total()    const noexcept { return ext_rom_size_; }
        Size tracked_count()    const noexcept;
        /// Ёмкость учёта задач (для отчётов о потреблении RAM).
        static constexpr Size tracked_capacity() noexcept { return kMaxTrackedTasks; }
        static constexpr Size slot_size() noexcept { return sizeof(task_slot); }

        /// Статическое потребление RAM менеджером (для отчётов о памяти).
        static constexpr Size static_bytes() noexcept { return sizeof(memory_manager); }
    };

    inline memory_manager g_memory_manager{};
}
