// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/marking-barrier.h"

#include "src/base/logging.h"
#include "src/heap/heap-inl.h"
#include "src/heap/heap-write-barrier.h"
#include "src/heap/heap.h"
#include "src/heap/incremental-marking-inl.h"
#include "src/heap/incremental-marking.h"
#include "src/heap/mark-compact-inl.h"
#include "src/heap/mark-compact.h"
#include "src/heap/marking-barrier-inl.h"
#include "src/heap/marking-worklist-inl.h"
#include "src/heap/marking-worklist.h"
#include "src/heap/safepoint.h"
#include "src/objects/heap-object.h"
#include "src/objects/js-array-buffer.h"

namespace v8 {
namespace internal {

MarkingBarrier::MarkingBarrier(LocalHeap* local_heap)
    : heap_(local_heap->heap()),
      major_collector_(heap_->mark_compact_collector()),
      minor_collector_(heap_->minor_mark_compact_collector()),
      incremental_marking_(heap_->incremental_marking()),
      major_worklist_(*major_collector_->marking_worklists()->shared()),
      minor_worklist_(*minor_collector_->marking_worklists()->shared()),
      marking_state_(isolate()),
      is_main_thread_barrier_(local_heap->is_main_thread()),
      uses_shared_heap_(isolate()->has_shared_heap()),
      is_shared_space_isolate_(isolate()->is_shared_space_isolate()) {}

MarkingBarrier::~MarkingBarrier() { DCHECK(typed_slots_map_.empty()); }

void MarkingBarrier::Write(HeapObject host, HeapObjectSlot slot,
                           HeapObject value) {
  DCHECK(IsCurrentMarkingBarrier(host));
  DCHECK(is_activated_ || shared_heap_worklist_.has_value());
  MarkValue(host, value);

  if (slot.address()) {
    if (is_compacting_ ||
        (shared_heap_worklist_.has_value() && host.InSharedWritableHeap())) {
      DCHECK_IMPLIES(is_compacting_, is_major());
      MarkCompactCollector::RecordSlot(host, slot, value);
    }
  }
}

void MarkingBarrier::WriteWithoutHost(HeapObject value) {
  DCHECK(is_main_thread_barrier_);
  DCHECK(is_activated_);

  // Without a shared heap and on the shared space isolate (= main isolate) all
  // objects are considered local.
  if (V8_UNLIKELY(uses_shared_heap_) && !is_shared_space_isolate_) {
    // On client isolates (= worker isolates) shared values can be ignored.
    if (value.InSharedWritableHeap()) {
      return;
    }
  }

  MarkValueLocal(value);
}

void MarkingBarrier::Write(Code host, RelocInfo* reloc_info, HeapObject value) {
  DCHECK(IsCurrentMarkingBarrier(host));
  DCHECK(!host.InSharedWritableHeap());
  DCHECK(is_activated_ || shared_heap_worklist_.has_value());
  MarkValue(host, value);
  if (is_compacting_) {
    DCHECK(is_major());
    if (is_main_thread_barrier_) {
      // An optimization to avoid allocating additional typed slots for the
      // main thread.
      major_collector_->RecordRelocSlot(host, reloc_info, value);
    } else {
      RecordRelocSlot(host, reloc_info, value);
    }
  }
}

void MarkingBarrier::Write(JSArrayBuffer host,
                           ArrayBufferExtension* extension) {
  DCHECK(IsCurrentMarkingBarrier(host));
  if (is_minor()) {
    if (Heap::InYoungGeneration(host)) {
      extension->YoungMark();
    }
  } else {
    extension->Mark();
  }
}

void MarkingBarrier::Write(DescriptorArray descriptor_array,
                           int number_of_own_descriptors) {
  DCHECK(IsCurrentMarkingBarrier(descriptor_array));
  DCHECK(IsReadOnlyHeapObject(descriptor_array.map()));

  if (is_minor() && !heap_->InYoungGeneration(descriptor_array)) return;

  // The DescriptorArray needs to be marked black here to ensure that slots are
  // recorded by the Scavenger in case the DescriptorArray is promoted while
  // incremental marking is running. This is needed as the regular marking
  // visitor does not re-process any already marked descriptors. If we don't
  // mark it black here, the Scavenger may promote a DescriptorArray and any
  // already marked descriptors will not have any slots recorded.
  if (!marking_state_.IsBlack(descriptor_array)) {
    marking_state_.WhiteToGrey(descriptor_array);
    marking_state_.GreyToBlack(descriptor_array);
    MarkRange(descriptor_array, descriptor_array.GetFirstPointerSlot(),
              descriptor_array.GetDescriptorSlot(0));
  }

  // Concurrent MinorMC always marks the full young generation DescriptorArray.
  // We cannot use epoch like MajorMC does because only the lower 2 bits are
  // used, and with many MinorMC cycles this could lead to correctness issues.
  const int16_t old_marked =
      is_minor() ? 0
                 : descriptor_array.UpdateNumberOfMarkedDescriptors(
                       major_collector_->epoch(), number_of_own_descriptors);
  if (old_marked < number_of_own_descriptors) {
    // This marks the range from [old_marked, number_of_own_descriptors) instead
    // of registering weak slots which may temporarily hold alive more objects
    // for the current GC cycle. Weakness is not needed for actual trimming, see
    // `MarkCompactCollector::TrimDescriptorArray()`.
    MarkRange(descriptor_array,
              MaybeObjectSlot(descriptor_array.GetDescriptorSlot(old_marked)),
              MaybeObjectSlot(descriptor_array.GetDescriptorSlot(
                  number_of_own_descriptors)));
  }
}

void MarkingBarrier::RecordRelocSlot(Code host, RelocInfo* rinfo,
                                     HeapObject target) {
  DCHECK(IsCurrentMarkingBarrier(host));
  if (!MarkCompactCollector::ShouldRecordRelocSlot(host, rinfo, target)) return;

  MarkCompactCollector::RecordRelocSlotInfo info =
      MarkCompactCollector::ProcessRelocInfo(host, rinfo, target);

  auto& typed_slots = typed_slots_map_[info.memory_chunk];
  if (!typed_slots) {
    typed_slots.reset(new TypedSlots());
  }
  typed_slots->Insert(info.slot_type, info.offset);
}

namespace {
void ActivateSpace(PagedSpace* space) {
  for (Page* p : *space) {
    p->SetOldGenerationPageFlags(true);
  }
}

void ActivateSpace(NewSpace* space) {
  for (Page* p : *space) {
    p->SetYoungGenerationPageFlags(true);
  }
}

void ActivateSpaces(Heap* heap) {
  ActivateSpace(heap->old_space());
  {
    CodePageHeaderModificationScope rwx_write_scope(
        "Modification of Code page header flags requires write access");
    ActivateSpace(heap->code_space());
  }
  ActivateSpace(heap->new_space());
  if (heap->shared_space()) {
    ActivateSpace(heap->shared_space());
  }

  for (LargePage* p : *heap->new_lo_space()) {
    p->SetYoungGenerationPageFlags(true);
    DCHECK(p->IsLargePage());
  }

  for (LargePage* p : *heap->lo_space()) {
    p->SetOldGenerationPageFlags(true);
  }

  {
    CodePageHeaderModificationScope rwx_write_scope(
        "Modification of Code page header flags requires write access");
    for (LargePage* p : *heap->code_lo_space()) {
      p->SetOldGenerationPageFlags(true);
    }
  }

  if (heap->shared_lo_space()) {
    for (LargePage* p : *heap->shared_lo_space()) {
      p->SetOldGenerationPageFlags(true);
    }
  }
}

void DeactivateSpace(PagedSpace* space) {
  for (Page* p : *space) {
    p->SetOldGenerationPageFlags(false);
  }
}

void DeactivateSpace(NewSpace* space) {
  for (Page* p : *space) {
    p->SetYoungGenerationPageFlags(false);
  }
}

void DeactivateSpaces(Heap* heap) {
  DeactivateSpace(heap->old_space());
  DeactivateSpace(heap->code_space());
  DeactivateSpace(heap->new_space());
  if (heap->shared_space()) {
    DeactivateSpace(heap->shared_space());
  }
  for (LargePage* p : *heap->new_lo_space()) {
    p->SetYoungGenerationPageFlags(false);
    DCHECK(p->IsLargePage());
  }
  for (LargePage* p : *heap->lo_space()) {
    p->SetOldGenerationPageFlags(false);
  }
  for (LargePage* p : *heap->code_lo_space()) {
    p->SetOldGenerationPageFlags(false);
  }
  if (heap->shared_lo_space()) {
    for (LargePage* p : *heap->shared_lo_space()) {
      p->SetOldGenerationPageFlags(false);
    }
  }
}
}  // namespace

// static
void MarkingBarrier::ActivateAll(Heap* heap, bool is_compacting,
                                 MarkingBarrierType marking_barrier_type) {
  ActivateSpaces(heap);

  heap->safepoint()->IterateLocalHeaps(
      [is_compacting, marking_barrier_type](LocalHeap* local_heap) {
        local_heap->marking_barrier()->Activate(is_compacting,
                                                marking_barrier_type);
      });

  if (heap->isolate()->is_shared_heap_isolate()) {
    heap->isolate()
        ->shared_heap_isolate()
        ->global_safepoint()
        ->IterateClientIsolates([](Isolate* client) {
          if (client->is_shared_heap_isolate()) return;
          // Force the RecordWrite builtin into the incremental marking code
          // path.
          client->heap()->SetIsMarkingFlag(true);
          client->heap()->safepoint()->IterateLocalHeaps(
              [](LocalHeap* local_heap) {
                local_heap->marking_barrier()->ActivateShared();
              });
        });
  }
}

void MarkingBarrier::Activate(bool is_compacting,
                              MarkingBarrierType marking_barrier_type) {
  DCHECK(!is_activated_);
  DCHECK(major_worklist_.IsLocalEmpty());
  DCHECK(minor_worklist_.IsLocalEmpty());
  is_compacting_ = is_compacting;
  marking_barrier_type_ = marking_barrier_type;
  current_worklist_ = is_minor() ? &minor_worklist_ : &major_worklist_;
  is_activated_ = true;
}

void MarkingBarrier::ActivateShared() {
  DCHECK(!shared_heap_worklist_.has_value());
  Isolate* shared_isolate = isolate()->shared_heap_isolate();
  shared_heap_worklist_.emplace(*shared_isolate->heap()
                                     ->mark_compact_collector()
                                     ->marking_worklists()
                                     ->shared());
}

// static
void MarkingBarrier::DeactivateAll(Heap* heap) {
  DeactivateSpaces(heap);

  heap->safepoint()->IterateLocalHeaps([](LocalHeap* local_heap) {
    local_heap->marking_barrier()->Deactivate();
  });

  if (heap->isolate()->is_shared_heap_isolate()) {
    heap->isolate()
        ->shared_heap_isolate()
        ->global_safepoint()
        ->IterateClientIsolates([](Isolate* client) {
          if (client->is_shared_heap_isolate()) return;
          // We can't just simply disable the marking barrier for all clients. A
          // client may still need it to be set for incremental marking in the
          // local heap.
          const bool is_marking =
              client->heap()->incremental_marking()->IsMarking();
          client->heap()->SetIsMarkingFlag(is_marking);
          client->heap()->safepoint()->IterateLocalHeaps(
              [](LocalHeap* local_heap) {
                local_heap->marking_barrier()->DeactivateShared();
              });
        });
  }
}

void MarkingBarrier::Deactivate() {
  is_activated_ = false;
  is_compacting_ = false;
  DCHECK(typed_slots_map_.empty());
  DCHECK(current_worklist_->IsLocalEmpty());
}

void MarkingBarrier::DeactivateShared() {
  DCHECK(shared_heap_worklist_->IsLocalAndGlobalEmpty());
  shared_heap_worklist_.reset();
}

// static
void MarkingBarrier::PublishAll(Heap* heap) {
  heap->safepoint()->IterateLocalHeaps([](LocalHeap* local_heap) {
    local_heap->marking_barrier()->PublishIfNeeded();
  });

  if (heap->isolate()->is_shared_heap_isolate()) {
    heap->isolate()
        ->shared_heap_isolate()
        ->global_safepoint()
        ->IterateClientIsolates([](Isolate* client) {
          if (client->is_shared_heap_isolate()) return;
          client->heap()->safepoint()->IterateLocalHeaps(
              [](LocalHeap* local_heap) {
                local_heap->marking_barrier()->PublishSharedIfNeeded();
              });
        });
  }
}

void MarkingBarrier::PublishIfNeeded() {
  if (is_activated_) {
    current_worklist_->Publish();
    base::Optional<CodePageHeaderModificationScope> optional_rwx_write_scope;
    if (!typed_slots_map_.empty()) {
      optional_rwx_write_scope.emplace(
          "Merging typed slots may require allocating a new typed slot set.");
    }
    for (auto& it : typed_slots_map_) {
      MemoryChunk* memory_chunk = it.first;
      // Access to TypeSlots need to be protected, since LocalHeaps might
      // publish code in the background thread.
      base::Optional<base::MutexGuard> opt_guard;
      if (v8_flags.concurrent_sparkplug) {
        opt_guard.emplace(memory_chunk->mutex());
      }
      std::unique_ptr<TypedSlots>& typed_slots = it.second;
      RememberedSet<OLD_TO_OLD>::MergeTyped(memory_chunk,
                                            std::move(typed_slots));
    }
    typed_slots_map_.clear();
  }
}

void MarkingBarrier::PublishSharedIfNeeded() {
  if (shared_heap_worklist_) {
    shared_heap_worklist_->Publish();
  }
}

bool MarkingBarrier::IsCurrentMarkingBarrier(
    HeapObject verification_candidate) {
  return WriteBarrier::CurrentMarkingBarrier(verification_candidate) == this;
}

Isolate* MarkingBarrier::isolate() const { return heap_->isolate(); }

#if DEBUG
void MarkingBarrier::AssertMarkingIsActivated() const { DCHECK(is_activated_); }

void MarkingBarrier::AssertSharedMarkingIsActivated() const {
  DCHECK(v8_flags.shared_space);
  DCHECK(shared_heap_worklist_.has_value());
}
#endif  // DEBUG

}  // namespace internal
}  // namespace v8
