// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioInputWindow.hpp"
#include <optional>
#include <utility>
#include <type_traits>

namespace Pro40Audio {
struct EmptyAudioMetadata {};
enum class PublicationResult {ok, closed, noSlot, wrongEpoch, backward};
// Process-local model. Exactly one publisher (including close); many readers.
// Source windows must be stable while copied. No allocation or wait here;
// acquisition permits at most three fresh pin attempts during publication.
// Lifetime: close admission, externally quiesce/join ALL callers, release all
// leases, THEN destroy. A zero reader count alone cannot prove caller quiescence.
template<size_t Capacity,size_t Channels=1,class Metadata=EmptyAudioMetadata> class AudioWindowPublication {
    static_assert(std::is_trivially_copyable_v<Metadata>);
    using Window=AudioInputWindow<Capacity,Channels>;
    static constexpr uint64_t writing=uint64_t(1)<<63;
    static_assert(sizeof(void*)==8 && std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    struct Slot {
        // Writer bit excludes admitted readers. Rejected attempts transiently
        // increment the count even while writing, then undo their own pin.
        // 2^63 live lease/caller pins cannot fit in a 64-bit address space.
        std::atomic<uint64_t> state{0};
        std::optional<Window> window;
        [[no_unique_address]] Metadata metadata{};
    };
    std::array<Slot,3> slots_;
    std::atomic<uint32_t> current_{0};
    std::atomic<bool> closed_{false};
    const uint64_t epoch_;
    uint64_t lastNext_; // Publisher-only.
public:
    class Lease {
        friend class AudioWindowPublication;
        Slot *slot_;
        explicit Lease(Slot *slot):slot_(slot) {}
        void release() noexcept {if(slot_){slot_->state.fetch_sub(1,std::memory_order_release);slot_=nullptr;}}
    public:
        static constexpr size_t channels=Channels;
        Lease(const Lease &)=delete;
        Lease &operator=(const Lease &)=delete;
        Lease(Lease &&other) noexcept:slot_(std::exchange(other.slot_,nullptr)) {}
        Lease &operator=(Lease &&other) noexcept {
            if(this!=&other){release();slot_=std::exchange(other.slot_,nullptr);}return *this;
        }
        ~Lease(){release();}
        // Only call on a live (not moved-from) lease. No raw view escapes.
        uint64_t epoch() const noexcept {return slot_->window->epoch();}
        uint64_t oldest() const noexcept {return slot_->window->oldest();}
        uint64_t next() const noexcept {return slot_->window->next();}
        Metadata metadata() const noexcept {return slot_->metadata;}
        // Owned copy for non-realtime bridge composition; no shared view escapes.
        Window copyWindow() const noexcept {return *slot_->window;}
        // Copy into the owner's preallocated storage. Large windows must not
        // create another full-window temporary on a connection thread's stack.
        void copyWindowInto(std::optional<Window> &destination)const noexcept {destination.emplace(*slot_->window);}
        WindowResult read(uint64_t epoch,uint64_t first,std::span<int32_t> output) const noexcept {
            return slot_->window->read(epoch,first,output);
        }
        WindowResult readNormalized(uint64_t epoch,uint64_t first,std::span<float> output) const noexcept {
            return slot_->window->readNormalized(epoch,first,output);
        }
    };
    explicit AudioWindowPublication(const Window &initial,const Metadata &metadata={}):epoch_(initial.epoch()),lastNext_(initial.next()) {
        for(auto &slot:slots_){slot.window.emplace(initial);slot.metadata=metadata;}
    }
private:
    template<class AfterPin> std::optional<Lease> acquire(AfterPin afterPin) noexcept {
        for(unsigned attempt=0;attempt<3;++attempt) {
            if(closed_.load(std::memory_order_acquire))return std::nullopt;
            const auto index=current_.load(std::memory_order_acquire);
            auto &slot=slots_[index];
            const auto before=slot.state.fetch_add(1,std::memory_order_acquire);
            if(before&writing){slot.state.fetch_sub(1,std::memory_order_release);continue;}
            afterPin(attempt);
            // Pin before examining contents, then recheck publication/admission.
            // Index reuse is safe: a successful pin owns whichever complete window
            // currently occupies the slot; it cannot change until this pin releases.
            if(closed_.load(std::memory_order_acquire) || current_.load(std::memory_order_acquire)!=index) {
                slot.state.fetch_sub(1,std::memory_order_release);continue;
            }
            return Lease(&slot);
        }
        return std::nullopt; // Persistent contention/closure stays explicit.
    }
public:
    std::optional<Lease> tryAcquire() noexcept {return acquire([](unsigned)noexcept{});}
#ifdef PRO40_PUBLICATION_TEST_HOOKS
    // Deterministic interleavings only; production code has an inlined no-op.
    template<class Hook> std::optional<Lease> tryAcquireWithHook(Hook hook)noexcept{return acquire(hook);}
#endif
    PublicationResult tryPublish(const Window &window,const Metadata &metadata={}) noexcept {
        if(closed_.load(std::memory_order_relaxed))return PublicationResult::closed;
        if(window.epoch()!=epoch_)return PublicationResult::wrongEpoch;
        if(window.next()<lastNext_)return PublicationResult::backward;
        const auto current=current_.load(std::memory_order_relaxed);
        for(uint32_t index=0;index<slots_.size();++index) {
            if(index==current)continue;
            auto &slot=slots_[index];uint64_t expected=0;
            if(!slot.state.compare_exchange_strong(expected,writing,std::memory_order_acquire,
                                                    std::memory_order_relaxed))continue;
            slot.window.emplace(window);
            slot.metadata=metadata;
            // Do not overwrite transient rejected-reader pins: their release
            // can follow this publication and must not subtract from zero.
            slot.state.fetch_and(~writing,std::memory_order_release);
            current_.store(index,std::memory_order_release);
            lastNext_=window.next();return PublicationResult::ok;
        }
        return PublicationResult::noSlot; // Preserve pinned snapshots, never overwrite.
    }
    void close() noexcept {closed_.store(true,std::memory_order_release);}
};
}
