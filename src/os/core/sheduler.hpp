// scheduler.hpp — SMP-aware preemptive priority-based task scheduler
//
// Architecture:
//   - SmpSchedulerCore<Backend, MaxTasks, MaxCores, MaxPriorities>:
//     per-core scheduling state, work stealing, inter-processor interrupts.
//   - Task<Scheduler, StackSize>: a task object with a fixed-size stack.
//   - ContextBackend (single-core) or SmpContextBackend (multi-core):
//     hardware glue for interrupts, context switch, ticks, and IPI.
//
// Scheduling policy: fixed-priority preemptive, O(1) ready-bitmap select,
// O(1) round-robin within the same priority level. Across cores: load
// balancing on create, manual affinity, opportunistic work stealing.
//
// Header-only. Requires C++17.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <thread>

namespace embedded::rtos {

    // ---------------------------------------------------------------------------
    //  Task states
    // ---------------------------------------------------------------------------
    enum class TaskState : std::uint8_t {
        Ready     = 0,
        Running   = 1,
        Blocked   = 2,
        Suspended = 3,
        Deleted   = 4
    };

    // ---------------------------------------------------------------------------
    //  Priority — 0 is lowest, (MaxPriorities - 1) is highest.
    // ---------------------------------------------------------------------------
    template <std::size_t MaxPriorities = 16>
    struct TaskPriority {
        std::uint8_t level;
        static constexpr std::size_t max_levels = MaxPriorities;
    };

    // ---------------------------------------------------------------------------
    //  Single-core backend (unchanged from previous revision).
    // ---------------------------------------------------------------------------
    struct ContextBackend {
        using IrqKey   = std::uint32_t;
        using StackPtr = void*;
        using TaskEntry = void(*)(void*) noexcept;

        static IrqKey   disable_interrupts() noexcept;
        static void     restore_interrupts(IrqKey key) noexcept;
        static StackPtr init_task_stack(StackPtr stack_top,
                                        TaskEntry entry,
                                        void* arg) noexcept;
                                        static void     request_context_switch() noexcept;
                                        [[noreturn]] static void start_first_task(StackPtr first_sp) noexcept;
                                        static void     configure_tick(std::uint32_t frequency_hz) noexcept;
                                        static void     acknowledge_tick() noexcept;
    };

    // ---------------------------------------------------------------------------
    //  SMP backend — extends ContextBackend with multi-core primitives.
    //
    //  On Cortex-Mx SMP / RISC-V SMP / x86_64 SMP, "interrupts" mean CPU-local
    //  interrupt masking. The user must implement these on top of the platform's
    //  interrupt controller (GICv3, PLIC, IO-APIC, etc.).
    // ---------------------------------------------------------------------------
    struct SmpContextBackend : ContextBackend {
        // ---- per-core identification -----------------------------------------
        // Returns the calling core's logical id (0..MaxCores-1). Must be
        // available before any other backend function is called.
        static std::size_t current_core_id() noexcept;

        // ---- spinlocks ------------------------------------------------------
        // A simple test-and-set spinlock. Used to protect scheduler state that
        // is read/written across cores. Implementations should be short-held
        // (microseconds at most) — the kernel never holds them across blocking
        // operations.
        struct Spinlock {
            std::atomic<std::uint32_t> state{0};
        };
        static void spinlock_acquire(Spinlock& l) noexcept;
        static void spinlock_release(Spinlock& l) noexcept;

        // ---- memory barriers ------------------------------------------------
        // SMP memory barrier: orders all loads/stores across cores. Use after
        // releasing a lock and before reading data the lock was protecting.
        static void memory_barrier() noexcept;

        // ---- inter-processor interrupt --------------------------------------
        // Send a "reschedule" IPI to another core. The target core's IPI
        // handler must call SchedulerCore::on_ipi(core_id).
        static void send_ipi(std::size_t target_core_id) noexcept;

        // ---- per-core tick enable ------------------------------------------
        // Enable the periodic tick on the calling core. configure_tick is
        // called once on core 0; each secondary calls enable_per_core_tick
        // after it has been brought up.
        static void enable_per_core_tick() noexcept;
    };

    // ---------------------------------------------------------------------------
    //  Task control block (SMP-aware)
    // ---------------------------------------------------------------------------
    struct TaskControlBlock {
        void*        sp           = nullptr;
        void*        stack_base   = nullptr;
        std::size_t  stack_size   = 0;
        void*        arg          = nullptr;
        void       (*entry)(void*) noexcept = nullptr;
        const char*  name         = "?";

        TaskState    state        = TaskState::Suspended;
        std::uint8_t priority     = 0;
        std::uint8_t base_priority = 0;

        // Per-task affinity mask. Bit i = allowed to run on core i.
        // 0 means "any core" (default).
        std::uint32_t affinity     = 0;

        // Currently assigned home core (chosen at create time, may migrate).
        std::uint32_t home_core    = 0;

        std::uint32_t time_slice_ticks = 0;
        std::uint32_t ticks_consumed   = 0;
        std::uint32_t wake_at_tick     = 0;

        void*         block_object     = nullptr;
        TaskControlBlock* next_in_block_list = nullptr;

        // Per-priority circular ready list
        TaskControlBlock* ready_prev   = nullptr;
        TaskControlBlock* ready_next   = nullptr;

        // Pinning: if != UINT32_MAX, the task may ONLY run on this core.
        std::uint32_t pinned_core = UINT32_MAX;

        std::uint16_t id          = 0;
        std::uint16_t reserved    = 0;
    };

    // ---------------------------------------------------------------------------
    //  Per-core ready list — O(1) bitmap + per-priority circular linked lists
    // ---------------------------------------------------------------------------
    namespace detail {

        class ReadyList {
        public:
            static constexpr std::size_t kMaxPrio = 32;

            void init() noexcept {
                bitmap_.store(0, std::memory_order_relaxed);
                for (std::size_t i = 0; i < kMaxPrio; ++i) head_[i] = nullptr;
            }

            // Insertion does NOT lock — caller must hold the per-core scheduler lock.
            void insert_locked(TaskControlBlock* tcb) noexcept {
                const std::size_t p = tcb->priority;
                TaskControlBlock* head = head_[p];
                if (head == nullptr) {
                    tcb->ready_prev = tcb;
                    tcb->ready_next = tcb;
                    head_[p] = tcb;
                    bitmap_.fetch_or(bit(p), std::memory_order_relaxed);
                } else {
                    TaskControlBlock* tail = head->ready_prev;
                    tcb->ready_prev = tail;
                    tcb->ready_next = head;
                    tail->ready_next = tcb;
                    head->ready_prev = tcb;
                }
            }

            void remove_locked(TaskControlBlock* tcb) noexcept {
                const std::size_t p = tcb->priority;
                TaskControlBlock* prev = tcb->ready_prev;
                TaskControlBlock* next = tcb->ready_next;
                if (prev == tcb) {
                    head_[p] = nullptr;
                    bitmap_.fetch_and(~bit(p), std::memory_order_relaxed);
                } else {
                    prev->ready_next = next;
                    next->ready_prev = prev;
                    if (head_[p] == tcb) head_[p] = next;
                }
                tcb->ready_prev = nullptr;
                tcb->ready_next = nullptr;
            }

            TaskControlBlock* highest_locked() const noexcept {
                std::uint32_t bmp = bitmap_.load(std::memory_order_relaxed);
                if (bmp == 0) return nullptr;
                const int lz = __builtin_clz(bmp);
                return head_[31 - lz];
            }

            void rotate_locked(std::size_t p) noexcept {
                TaskControlBlock* head = head_[p];
                if (head == nullptr || head->ready_next == head) return;
                head_[p] = head->ready_next;
            }

            bool contains(TaskControlBlock* tcb) const noexcept {
                return tcb->ready_next != nullptr;
            }

            // ---- work stealing --------------------------------------------------
            // Try to steal the highest-priority task from this list. Returns
            // nullptr if empty. Lock-free path: use CAS on the bitmap to claim the
            // highest-priority level, then pop its head.
            TaskControlBlock* try_steal_highest() noexcept {
                // The first attempt: grab the bitmap. Subsequent attempts handle
                // races where another core cleared a bit between our load and CAS.
                while (true) {
                    std::uint32_t bmp = bitmap_.load(std::memory_order_acquire);
                    if (bmp == 0) return nullptr;
                    const int lz = __builtin_clz(bmp);
                    const std::uint32_t mask = std::uint32_t(1u) << (31 - lz);
                    // Try to clear our chosen bit. If we win, we own that level.
                    std::uint32_t expected = bmp;
                    if (!bitmap_.compare_exchange_strong(
                        expected, expected & ~mask,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                        continue;  // bitmap changed, retry
                        }
                        // We cleared the bit. Pop the head of that level.
                        TaskControlBlock* head = head_[31 - lz];
                    if (head == nullptr) {
                        // Should not happen, but be safe.
                        bitmap_.fetch_or(mask, std::memory_order_release);
                        continue;
                    }
                    remove_locked(head);
                    return head;
                }
            }

            // ---- diagnostics ----------------------------------------------------
            std::uint32_t bitmap() const noexcept {
                return bitmap_.load(std::memory_order_relaxed);
            }
            std::size_t size() const noexcept {
                std::uint32_t bmp = bitmap_.load(std::memory_order_relaxed);
                std::size_t n = 0;
                while (bmp) { n += bmp & 1u; bmp >>= 1; }
                return n;
            }

        private:
            static constexpr std::uint32_t bit(std::size_t p) noexcept {
                return std::uint32_t(1u) << p;
            }

            std::atomic<std::uint32_t> bitmap_{0};
            TaskControlBlock* head_[kMaxPrio] = {};
        };

    } // namespace detail

    // ---------------------------------------------------------------------------
    //  Per-core scheduler state — one instance per logical core, isolated to
    //  its own cache line(s) to avoid false sharing.
    // ---------------------------------------------------------------------------
    template <std::size_t MaxPriorities>
    struct alignas(128) PerCoreState {
        TaskControlBlock*       current         = nullptr;
        detail::ReadyList       ready;
        // Per-core scheduler lock. Held briefly during enqueue/dequeue and
        // load-balance decisions. Not held across blocking operations.
        SmpContextBackend::Spinlock sched_lock{};
        // Per-core time slice accounting
        std::uint32_t           time_slice_ticks_remaining = 0;
        // Per-core counter — when the local CPU is idle, this is non-zero.
        std::uint32_t           idle_loop_count = 0;
        // Per-core tick counter — only core 0 contributes to the global count;
        // other cores still tick their own slice accounting.
        std::uint32_t           local_ticks     = 0;
        // Padding to keep each PerCoreState on its own cache line(s).
        char                    _pad[128 - (sizeof(detail::ReadyList) +
        sizeof(TaskControlBlock*) +
        sizeof(SmpContextBackend::Spinlock) +
        2 * sizeof(std::uint32_t)) % 128];
    };

    // ---------------------------------------------------------------------------
    //  SMP scheduler
    // ---------------------------------------------------------------------------
    template <typename Backend, std::size_t MaxTasks,
    std::size_t MaxCores = 1, std::size_t MaxPriorities = 16>
    class SmpSchedulerCore {
        static_assert(MaxPriorities <= 32, "MaxPriorities must be <= 32");
        static_assert(MaxCores >= 1, "MaxCores must be >= 1");
        static_assert(MaxTasks > 0, "Need at least one task slot");

    public:
        static constexpr std::size_t kMaxTasks      = MaxTasks;
        static constexpr std::size_t kMaxCores      = MaxCores;
        static constexpr std::size_t kMaxPriorities = MaxPriorities;

        // ---- singleton ------------------------------------------------------
        // Backends that need to call into the kernel from ISR/IPI context
        // (where no `this` is available) use this accessor. The instance is
        // constructed at first use; core 0's `start()` runs the constructor
        // implicitly via `instance()`.
        static SmpSchedulerCore& instance() noexcept {
            static SmpSchedulerCore inst;
            return inst;
        }

        // ---- task creation --------------------------------------------------
        // Creates a task and assigns it to a core. If `affinity_mask == 0` the
        // task may run on any core; the home core is chosen by load balance.
        // Returns task id or SIZE_MAX on failure.
        std::size_t create(const char* name,
                           void(*entry)(void*) noexcept,
                           void* arg,
                           void* stack_base,
                           std::size_t stack_size,
                           std::uint8_t priority,
                           std::uint32_t time_slice_ticks = 1,
                           std::uint32_t affinity_mask   = 0,
                           std::uint32_t pin_to_core     = UINT32_MAX) noexcept {
                               if (priority == 0 || priority >= kMaxPriorities) return SIZE_MAX;
                               if (entry == nullptr || stack_base == nullptr) return SIZE_MAX;

                               const auto key = Backend::disable_interrupts();
                               if (task_count_ >= kMaxTasks) {
                                   Backend::restore_interrupts(key);
                                   return SIZE_MAX;
                               }

                               TaskControlBlock& t = tcbs_[task_count_];
                               t = TaskControlBlock{};
                               t.id              = static_cast<std::uint16_t>(task_count_);
                               t.name            = name;
                               t.entry           = entry;
                               t.arg             = arg;
                               t.stack_base      = stack_base;
                               t.stack_size      = stack_size;
                               t.priority        = priority;
                               t.base_priority   = priority;
                               t.time_slice_ticks = time_slice_ticks ? time_slice_ticks : 1;
                               t.affinity        = affinity_mask ? affinity_mask : all_cores_mask();
                               t.pinned_core     = pin_to_core;
                               t.state           = TaskState::Ready;

                               // Compute top-of-stack aligned to 8 bytes.
                               std::uintptr_t top = reinterpret_cast<std::uintptr_t>(stack_base) + stack_size;
                               top &= ~std::uintptr_t(0x7);
                               t.sp = Backend::init_task_stack(reinterpret_cast<void*>(top), entry, arg);

                               // Pick a home core.
                               const std::size_t home = pin_to_core != UINT32_MAX
                               ? pin_to_core
                               : pick_least_loaded_core(t.affinity);
                               t.home_core = static_cast<std::uint32_t>(home);

                               enqueue_locked(home, &t);
                               ++task_count_;
                               Backend::restore_interrupts(key);

                               // If home core is currently running something lower-priority, kick it.
                               if (per_core_[home].current != nullptr &&
                                   priority > per_core_[home].current->priority) {
                                   if (home == Backend::current_core_id()) {
                                       Backend::request_context_switch();
                                   } else {
                                       Backend::send_ipi(home);
                                   }
                                   }
                                   return t.id;
                           }

                           // ---- per-core start -------------------------------------------------
                           // Core 0 calls this once with secondary_core_count = MaxCores-1 after
                           // constructing the scheduler. Each secondary core calls this with its
                           // own id when it comes up.
                           [[noreturn]] void start_core0(std::uint32_t tick_hz) noexcept {
                               Backend::configure_tick(tick_hz);
                               bring_up_core_self(/*is_core0=*/true);
                               start_running_core0();
                               __builtin_unreachable();
                           }

                           // Secondary core entry point. `core_id` is the calling core's logical
                           // id. The function does not return.
                           [[noreturn]] void start_secondary(std::size_t core_id) noexcept {
                               bring_up_core_self(/*is_core0=*/false);
                               Backend::enable_per_core_tick();
                               start_running_secondary(core_id);
                               __builtin_unreachable();
                           }

                           // ---- affinity -------------------------------------------------------
                           // Restrict a task to the cores in `mask` (bit i = core i). The task is
                           // migrated immediately to a core inside the new mask.
                           void set_affinity(std::size_t task_id, std::uint32_t mask) noexcept {
                               if (task_id >= task_count_) return;
                               TaskControlBlock& t = tcbs_[task_id];
                               const auto key = Backend::disable_interrupts();

                               const bool was_ready = (t.state == TaskState::Ready);
                               if (was_ready) dequeue_locked(t.home_core, &t);

                               t.affinity = mask ? mask : all_cores_mask();
                               // Recompute home core
                               const std::size_t new_home = t.pinned_core != UINT32_MAX
                               ? t.pinned_core
                               : pick_least_loaded_core(t.affinity);
                               t.home_core = static_cast<std::uint32_t>(new_home);

                               if (was_ready) {
                                   t.state = TaskState::Ready;
                                   enqueue_locked(new_home, &t);
                               }
                               Backend::restore_interrupts(key);

                               if (new_home == Backend::current_core_id()) {
                                   Backend::request_context_switch();
                               } else {
                                   Backend::send_ipi(new_home);
                               }
                           }

                           // Pin a task to a specific core. UINT32_MAX un-pins.
                           void pin(std::size_t task_id, std::uint32_t core_id) noexcept {
                               if (task_id >= task_count_) return;
                               TaskControlBlock& t = tcbs_[task_id];
                               set_affinity(task_id, core_id == UINT32_MAX ? all_cores_mask() : (1u << core_id));
                               t.pinned_core = core_id;
                           }

                           // ---- yield / sleep --------------------------------------------------
                           void yield() noexcept {
                               const std::size_t me = Backend::current_core_id();
                               const auto key = Backend::disable_interrupts();
                               TaskControlBlock* cur = per_core_[me].current;
                               if (cur == nullptr || cur == &idle_tcb_) {
                                   Backend::restore_interrupts(key);
                                   return;
                               }
                               cur->time_slice_ticks = 0;
                               Backend::request_context_switch();
                               Backend::restore_interrupts(key);
                           }

                           void sleep(std::uint32_t ticks) noexcept {
                               if (ticks == 0) { yield(); return; }
                               const std::size_t me = Backend::current_core_id();
                               const auto key = Backend::disable_interrupts();
                               TaskControlBlock* cur = per_core_[me].current;
                               if (cur == nullptr) {
                                   Backend::restore_interrupts(key);
                                   return;
                               }
                               const std::uint32_t now = tick_count_.load(std::memory_order_acquire);
                               cur->wake_at_tick = now + ticks;
                               cur->state = TaskState::Blocked;
                               dequeue_locked(me, cur);
                               Backend::request_context_switch();
                               Backend::restore_interrupts(key);
                           }

                           // ---- unblock / wake -------------------------------------------------
                           void wake_blocked(TaskControlBlock* t) noexcept {
                               if (t == nullptr) return;
                               const auto key = Backend::disable_interrupts();
                               if (t->state == TaskState::Blocked) {
                                   t->state = TaskState::Ready;
                                   t->wake_at_tick = 0;
                                   t->block_object = nullptr;
                                   enqueue_locked(t->home_core, t);
                               }
                               Backend::restore_interrupts(key);

                               const std::size_t home = t->home_core;
                               if (home == Backend::current_core_id()) {
                                   // Re-check whether the just-woken task outranks the current
                                   // one. If so, request a switch.
                                   if (t->priority > per_core_[home].current->priority) {
                                       Backend::request_context_switch();
                                   }
                               } else {
                                   Backend::send_ipi(home);
                               }
                           }

                           // ---- priority -------------------------------------------------------
                           void set_priority(std::size_t task_id, std::uint8_t priority) noexcept {
                               if (task_id >= task_count_) return;
                               TaskControlBlock& t = tcbs_[task_id];
                               const auto key = Backend::disable_interrupts();
                               const bool was_ready = (t.state == TaskState::Ready);
                               if (was_ready) dequeue_locked(t.home_core, &t);
                               t.priority = priority;
                               t.base_priority = priority;
                               if (was_ready) {
                                   t.state = TaskState::Ready;
                                   enqueue_locked(t.home_core, &t);
                               }
                               Backend::restore_interrupts(key);

                               // If this task now outranks its core's current, preempt.
                               const std::size_t home = t.home_core;
                               if (home == Backend::current_core_id()) {
                                   if (per_core_[home].current != nullptr &&
                                       priority > per_core_[home].current->priority) {
                                       Backend::request_context_switch();
                                       }
                               } else {
                                   Backend::send_ipi(home);
                               }
                           }

                           void suspend(std::size_t task_id) noexcept {
                               if (task_id >= task_count_) return;
                               TaskControlBlock& t = tcbs_[task_id];
                               const std::size_t home = t.home_core;
                               const auto key = Backend::disable_interrupts();
                               if (t.state == TaskState::Ready) dequeue_locked(home, &t);
                               t.state = TaskState::Suspended;
                               Backend::restore_interrupts(key);

                               if (home == Backend::current_core_id() && per_core_[home].current == &t) {
                                   Backend::request_context_switch();
                               } else {
                                   Backend::send_ipi(home);
                               }
                           }

                           void resume(std::size_t task_id) noexcept {
                               if (task_id >= task_count_) return;
                               TaskControlBlock& t = tcbs_[task_id];
                               const std::size_t home = t.home_core;
                               const auto key = Backend::disable_interrupts();
                               if (t.state == TaskState::Suspended) {
                                   t.state = TaskState::Ready;
                                   enqueue_locked(home, &t);
                               }
                               Backend::restore_interrupts(key);

                               if (home == Backend::current_core_id()) {
                                   if (per_core_[home].current != nullptr &&
                                       t.priority > per_core_[home].current->priority) {
                                       Backend::request_context_switch();
                                       }
                               } else {
                                   Backend::send_ipi(home);
                               }
                           }

                           // ---- on_tick: per-core ISR entry point -----------------------------
                           // Called by the local tick interrupt on every core.
                           void on_tick() noexcept {
                               const std::size_t me = Backend::current_core_id();
                               per_core_[me].local_ticks++;
                               if (me == 0) tick_count_.fetch_add(1, std::memory_order_relaxed);
                               if (per_core_[me].time_slice_ticks_remaining > 0) {
                                   --per_core_[me].time_slice_ticks_remaining;
                               }
                               Backend::acknowledge_tick();
                           }

                           // ---- on_ipi: invoked from the IPI ISR --------------------------------
                           // The IPI handler on the target core calls this. The IPI itself just
                           // means "check for higher-priority work"; the handler triggers a
                           // context switch if necessary.
                           void on_ipi() noexcept {
                               const std::size_t me = Backend::current_core_id();
                               TaskControlBlock* cur = per_core_[me].current;
                               TaskControlBlock* top = per_core_[me].ready.highest_locked();
                               if (top == nullptr) {
                                   // No work locally; try to steal.
                                   top = try_steal_from_others(me);
                               }
                               if (top != nullptr &&
                                   (cur == nullptr || top->priority > cur->priority ||
                                   (cur->time_slice_ticks_remaining == 0))) {
                                   Backend::request_context_switch();
                                   }
                           }

                           // ---- on_context_switch ---------------------------------------------
                           // Called by the PendSV / context-switch handler on the current core.
                           // The backend passes the just-saved SP via set_outgoing_sp().
                           void on_context_switch() noexcept {
                               const std::size_t me = Backend::current_core_id();
                               PerCoreState<kMaxPriorities>& pc = per_core_[me];

                               // Save the just-saved SP of the outgoing task.
                               if (outgoing_sp_) {
                                   pc.current->sp = outgoing_sp_;
                                   outgoing_sp_ = nullptr;
                               }
                               if (pc.current != nullptr) {
                                   pc.current->ticks_consumed++;
                                   // Round-robin within priority: rotate when slice expires.
                                   if (pc.time_slice_ticks_remaining == 0 && pc.current != &idle_tcb_) {
                                       pc.time_slice_ticks_remaining =
                                       current_time_slice_for(pc.current->priority);
                                       // Re-insert at back of same priority.
                                       if (pc.current->state == TaskState::Running) {
                                           pc.current->state = TaskState::Ready;
                                           pc.ready.insert_locked(pc.current);
                                       }
                                   }
                               }

                               // Wake sleepers whose deadline has passed (only core 0 to avoid
                               // multi-wake races; the global tick_count_ is the timebase).
                               if (me == 0) {
                                   const std::uint32_t now = tick_count_.load(std::memory_order_acquire);
                                   for (std::size_t i = 0; i < task_count_; ++i) {
                                       TaskControlBlock& t = tcbs_[i];
                                       if (t.state == TaskState::Blocked &&
                                           t.wake_at_tick != 0 &&
                                           static_cast<std::int32_t>(now - t.wake_at_tick) >= 0) {
                                           t.wake_at_tick = 0;
                                       t.state = TaskState::Ready;
                                       enqueue_locked(t.home_core, &t);
                                           }
                                   }
                               }

                               // Pick next: highest-priority ready task on this core. If empty,
                               // try to steal.
                               TaskControlBlock* next = pc.ready.highest_locked();
                               if (next == nullptr) {
                                   next = try_steal_from_others(me);
                               }
                               if (next == nullptr) {
                                   next = &idle_tcb_;
                               } else {
                                   // We stole or popped the head. Lock-free remove:
                                   pc.ready.remove_locked(next);
                               }
                               pc.current = next;
                               next->state = TaskState::Running;
                               pc.time_slice_ticks_remaining =
                               current_time_slice_for(next->priority);
                               incoming_sp_ = next->sp;
                           }

                           void set_outgoing_sp(void* sp) noexcept { outgoing_sp_ = sp; }
                           void* incoming_sp() const noexcept { return incoming_sp_; }

                           // ---- introspection --------------------------------------------------
                           TaskControlBlock* current() noexcept {
                               return per_core_[Backend::current_core_id()].current;
                           }
                           TaskControlBlock& tcb(std::size_t id) noexcept { return tcbs_[id]; }
                           std::uint32_t tick() const noexcept { return tick_count_.load(std::memory_order_acquire); }
                           std::size_t task_count() const noexcept { return task_count_; }
                           std::size_t core_count() const noexcept { return core_count_; }
                           PerCoreState<kMaxPriorities>& per_core(std::size_t i) noexcept { return per_core_[i]; }

                           void set_time_slice(std::uint8_t priority, std::uint32_t ticks) noexcept {
                               if (priority >= kMaxPriorities) return;
                               slices_[priority] = ticks;
                           }

                           // ---- second-core bring-up API (called by user-supplied trampoline) --
                           // When a secondary core finishes platform init, it calls this on the
                           // shared scheduler instance to register itself.
                           void register_core(std::size_t core_id) noexcept {
                               if (core_id >= kMaxCores) return;
                               const auto key = Backend::disable_interrupts();
                               if (core_id + 1 > core_count_) core_count_ = core_id + 1;
                               // Initialize that core's per-core state.
                               per_core_[core_id] = PerCoreState<kMaxPriorities>{};
                               per_core_[core_id].ready.init();
                               Backend::restore_interrupts(key);
                           }

    private:
        // ---- internals -----------------------------------------------------
        std::uint32_t all_cores_mask() const noexcept {
            return (kMaxCores >= 32) ? 0xFFFFFFFFu : ((1u << kMaxCores) - 1u);
        }

        void enqueue_locked(std::size_t core, TaskControlBlock* t) noexcept {
            if (t->pinned_core != UINT32_MAX) core = t->pinned_core;
            Backend::spinlock_acquire(per_core_[core].sched_lock);
            per_core_[core].ready.insert_locked(t);
            t->state = TaskState::Ready;
            Backend::spinlock_release(per_core_[core].sched_lock);
        }

        void dequeue_locked(std::size_t core, TaskControlBlock* t) noexcept {
            if (t->pinned_core != UINT32_MAX) core = t->pinned_core;
            Backend::spinlock_acquire(per_core_[core].sched_lock);
            if (per_core_[core].ready.contains(t)) {
                per_core_[core].ready.remove_locked(t);
            }
            Backend::spinlock_release(per_core_[core].sched_lock);
        }

        // Pick the core with the fewest ready tasks among those in the mask.
        std::size_t pick_least_loaded_core(std::uint32_t mask) const noexcept {
            std::size_t best = 0;
            std::size_t best_load = SIZE_MAX;
            for (std::size_t i = 0; i < kMaxCores; ++i) {
                if ((mask & (1u << i)) == 0) continue;
                const std::size_t load = per_core_[i].ready.size() +
                (per_core_[i].current ? 1 : 0);
                if (load < best_load) { best_load = load; best = i; }
            }
            return best;
        }

        // Work stealing: scan other cores for a stealable task. Returns the
        // stolen task (removed from the victim's ready list and not yet
        // inserted into ours — caller handles re-insertion).
        TaskControlBlock* try_steal_from_others(std::size_t me) noexcept {
            // Round-robin victim selection based on a static offset to avoid
            // thundering herd.
            static std::atomic<std::size_t> rr_victim{0};
            const std::size_t start = rr_victim.fetch_add(1, std::memory_order_relaxed) % kMaxCores;
            for (std::size_t step = 0; step < kMaxCores; ++step) {
                std::size_t victim = (start + step) % kMaxCores;
                if (victim == me) continue;
                // Don't steal pinned tasks or tasks that don't allow this core.
                TaskControlBlock* stolen = per_core_[victim].ready.try_steal_highest();
                if (stolen == nullptr) continue;
                if (stolen->pinned_core != UINT32_MAX && stolen->pinned_core != me) {
                    // Can't run on this core; put it back.
                    per_core_[victim].ready.insert_locked(stolen);
                    continue;
                }
                if ((stolen->affinity & (1u << me)) == 0) {
                    per_core_[victim].ready.insert_locked(stolen);
                    continue;
                }
                // Migrate: update home_core and return to caller.
                stolen->home_core = static_cast<std::uint32_t>(me);
                return stolen;
            }
            return nullptr;
        }

        void bring_up_core_self(bool is_core0) noexcept {
            // Each core initializes its own per-core state lazily on first
            // access, but we do it eagerly here so the structure is ready
            // before the first context switch.
            const std::size_t me = is_core0 ? 0 : Backend::current_core_id();
            if (is_core0) {
                for (std::size_t i = 0; i < kMaxCores; ++i) {
                    per_core_[i] = PerCoreState<kMaxPriorities>{};
                    per_core_[i].ready.init();
                }
                core_count_ = kMaxCores;
            } else {
                register_core(me);
            }
        }

        [[noreturn]] void start_running_core0() noexcept {
            // Pick highest-priority ready task on core 0.
            TaskControlBlock* first = per_core_[0].ready.highest_locked();
            if (first == nullptr) {
                first = &idle_tcb_;
            } else {
                per_core_[0].ready.remove_locked(first);
            }
            per_core_[0].current = first;
            first->state = TaskState::Running;
            per_core_[0].time_slice_ticks_remaining =
            current_time_slice_for(first->priority);
            Backend::start_first_task(first->sp);
            __builtin_unreachable();
        }

        [[noreturn]] void start_running_secondary(std::size_t core_id) noexcept {
            // Secondary core enters the scheduler loop.
            TaskControlBlock* first = per_core_[core_id].ready.highest_locked();
            if (first == nullptr) first = &idle_tcb_;
            else per_core_[core_id].ready.remove_locked(first);
            per_core_[core_id].current = first;
            first->state = TaskState::Running;
            per_core_[core_id].time_slice_ticks_remaining =
            current_time_slice_for(first->priority);
            Backend::start_first_task(first->sp);
            __builtin_unreachable();
        }

        std::uint32_t current_time_slice_for(std::uint8_t p) const noexcept {
            return slices_[p] ? slices_[p] : 1;
        }

        // ---- storage -------------------------------------------------------
        TaskControlBlock tcbs_[kMaxTasks];
        std::size_t      task_count_ = 0;
        std::size_t      core_count_ = 1;

        // Per-core state. Each PerCoreState is aligned to 128 bytes to land on
        // its own cache line(s) — critical for SMP performance.
        PerCoreState<kMaxPriorities> per_core_[kMaxCores];

        // Global tick counter — incremented by core 0 only.
        alignas(64) std::atomic<std::uint32_t> tick_count_{0};

        // Scratch space used during context switch.
        void* outgoing_sp_ = nullptr;
        void* incoming_sp_ = nullptr;

        std::uint32_t slices_[kMaxPriorities] = {};

        // Idle task — never destroyed. Lives on whatever stack the backend
        // chooses (typically the boot stack on each core).
        TaskControlBlock idle_tcb_{};
    };

    // ---------------------------------------------------------------------------
    //  Task<Scheduler, StackSize> — RAII-friendly task object
    // ---------------------------------------------------------------------------
    template <typename Scheduler, std::size_t StackSize>
    class Task {
    public:
        Task(Scheduler& sched,
             const char* name,
             void(*entry)(void*) noexcept,
             void* arg = nullptr,
             std::uint8_t priority = 1,
             std::uint32_t time_slice = 1,
             std::uint32_t affinity_mask = 0,
             std::uint32_t pin_to_core   = UINT32_MAX) noexcept
             : sched_(sched) {
                 for (std::size_t i = 0; i < StackSize; ++i) stack_[i] = 0xCC;
                 id_ = sched_.create(name, entry, arg, stack_, StackSize,
                                     priority, time_slice, affinity_mask, pin_to_core);
             }

             bool valid() const noexcept { return id_ != SIZE_MAX; }
             std::size_t id() const noexcept { return id_; }

             void suspend() noexcept { sched_.suspend(id_); }
             void resume()  noexcept { sched_.resume(id_); }
             void set_affinity(std::uint32_t mask) noexcept { sched_.set_affinity(id_, mask); }
             void pin(std::uint32_t core) noexcept { sched_.pin(id_, core); }
             void set_priority(std::uint8_t p) noexcept { sched_.set_priority(id_, p); }

             std::size_t stack_high_watermark() const noexcept {
                 std::size_t free_bytes = 0;
                 for (std::size_t i = 0; i < StackSize; ++i) {
                     if (stack_[i] != 0xCC) break;
                     ++free_bytes;
                 }
                 return free_bytes;
             }

    private:
        Scheduler&      sched_;
        std::size_t     id_ = SIZE_MAX;
        alignas(8) std::uint8_t stack_[StackSize];
    };

    // ---------------------------------------------------------------------------
    //  Backward-compat: when MaxCores == 1, SmpSchedulerCore degenerates to the
    //  previous single-core API. The user can use either:
    //      using Sched = SmpSchedulerCore<Backend, 16, 1>;     // single core
    //      using Sched = SmpSchedulerCore<Backend, 16, 4>;     // quad core
    // ---------------------------------------------------------------------------

} // namespace embedded::rtos
