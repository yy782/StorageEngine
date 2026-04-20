#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "dash_internal.hpp"

namespace dfly{

// 实现缓存淘汰，分层存储暂缓

template<typename _Key, typename _Value, typename Policy>
class DashTable : public detail::DashTableBase{
public:
    DashTable(const DashTable&) = delete;
    DashTable& operator=(const DashTable&) = delete;
    using Base = detail::DashTableBase;
    using SegmentType = detail::Segment<_Key, _Value, Policy>;
    using SegmentIterator = typename SegmentType::Iterator;

    using Key_t = _Key; // not same
    using Value_t = _Value; 
    using Segment_t = SegmentType;


    template <bool IsConst, bool IsSingleBucket = false> 
    class Iterator;
    using const_iterator = Iterator<true>;
    using iterator = Iterator<false>;

    struct BucketSet;
    using const_bucket_iterator = Iterator<true, true>;
    using bucket_iterator = Iterator<false, true>;
    using Cursor = detail::DashCursor;


    struct HotBuckets;
    struct DefaultEvictionPolicy;

    DashTable(size_t capacity_log = 1, const Policy& policy = Policy{},
            PMR_NS::memory_resource* mr = PMR_NS::get_default_resource());
    ~DashTable();
    template <typename U, typename V> 
    std::pair<iterator, bool> 
    Insert(U&& key, V&& value) {
        DefaultEvictionPolicy policy;
        return InsertInternal(std::forward<U>(key), std::forward<V>(value), policy,
                            InsertMode::kInsertIfNotFound);
    }    
    template <typename U> 
    const_iterator Find(U&& key) const;
    template <typename U> 
    iterator Find(U&& key);
    
    void Erase(iterator it);
    size_t Erase(const Key_t& k);
    iterator begin() {
        iterator it{this, 0, 0, 0};
        it.Seek2Occupied(); // 将迭代器向前移动到下一个“被占用”的槽位。
        return it;
    }

    const_iterator cbegin() const {
        const_iterator it{this, 0, 0, 0};
        it.Seek2Occupied();
        return it;
    }

    iterator end() const {
        return iterator{};
    }
    const_iterator cend() const {
        return const_iterator{};
    }    

    using Base::depth;
    using Base::Empty;
    using Base::size;
    using Base::unique_segments;

    void Clear();
    
    
private:
    enum class InsertMode {
        kInsertIfNotFound,
        kForceInsert,
    };    


    template <typename U, typename V, typename EvictionPolicy>
    std::pair<iterator, bool> 
    InsertInternal(U&& key, V&& value, EvictionPolicy& policy,
                                            InsertMode mode);

    SegmentType* ConstructSegment(uint8_t depth, uint32_t id);  
    template <typename Cb> 
    void IterateDistinct(Cb&& cb);

    size_t NextSeg(size_t sid) const {
        size_t delta = (1u << (global_depth_ - segment_[sid]->local_depth()));
        return sid + delta;
    }   

    Policy policy_;
    std::vector<SegmentType*, PMR_NS::polymorphic_allocator<SegmentType*>> segment_;

};


template <typename _Key, typename _Value, typename Policy>
template <bool IsConst, bool IsSingleBucket>
class DashTable<_Key, _Value, Policy>::Iterator {
    using Owner = std::conditional_t<IsConst, const DashTable, DashTable>;
public:

    using iterator_category = std::forward_iterator_tag; // 前向迭代器
    using difference_type = std::ptrdiff_t;
    using IteratorPairType =
        std::conditional_t<IsConst, 
                            detail::IteratorPair<const Key_t, const Value_t>,
                            detail::IteratorPair<Key_t, Value_t>>;
    template <bool TIsConst = IsConst, bool TIsSingleB>
    requires TIsConst Iterator(const Iterator<!TIsConst, TIsSingleB>& other)
    noexcept : 
        owner_(other.owner_),
        seg_id_(other.seg_id_),
        bucket_id_(other.bucket_id_),
        slot_id_(other.slot_id_) {}
    template <bool TIsSingle>
    Iterator(const Iterator<IsConst, TIsSingle>& other) 
    noexcept : 
        owner_(other.owner_),
        seg_id_(other.seg_id_),
        bucket_id_(other.bucket_id_),
        slot_id_(IsSingleBucket ? 0 : other.slot_id_)
        {

            if constexpr (IsSingleBucket) {
                Seek2Occupied();
            }
        }
    Iterator()=default; // not same
    Iterator(const Iterator& other) = default;
    Iterator(Iterator&& other) = default;
    Iterator& operator=(const Iterator& other) = default;
    Iterator& operator=(Iterator&& other) = default;   
    
    Iterator& operator++() {
        ++slot_id_;
        Seek2Occupied();
        return *this;
    }

    Iterator& operator+=(int delta) {
        slot_id_ += delta;
        Seek2Occupied();
        return *this;
    }    
    IteratorPairType operator->() const {
        auto* seg = owner_->segment_[seg_id_];
        return {seg->Key(bucket_id_, slot_id_), seg->Value(bucket_id_, slot_id_)};
    }
    bool is_done() const {
        return owner_ == nullptr;
    }

    bool IsOccupied() const {
        return (seg_id_ < owner_->segment_.size()) &&
            ((owner_->segment_[seg_id_]->IsBusy(bucket_id_, slot_id_)));
    }

    Owner& owner() const {
        return *owner_;
    }

    friend bool operator==(const Iterator& lhs, const Iterator& rhs) {
        if (lhs.owner_ == nullptr && rhs.owner_ == nullptr)
            return true;
        return lhs.owner_ == rhs.owner_ && lhs.seg_id_ == rhs.seg_id_ &&
            lhs.bucket_id_ == rhs.bucket_id_ && lhs.slot_id_ == rhs.slot_id_;
    }

    friend bool operator!=(const Iterator& lhs, const Iterator& rhs) {
        return !(lhs == rhs);
    }
private:
    friend class DashTable;
    Iterator(Owner* me, uint32_t seg_id, detail::PhysicalBid bid, uint8_t sid) : 
    owner_(me), 
    seg_id_(seg_id), 
    bucket_id_(bid), 
    slot_id_(sid) { }    
    // 迭代器的位置信息
    Owner* owner_;      // 所属的 DashTable
    uint32_t seg_id_;   // 当前 segment 索引
    detail::PhysicalBid bucket_id_;     // 当前 bucket 索引（0-67）
    uint8_t slot_id_;       // 当前槽位索引（0-13）
    
    // 对于单桶迭代器，bucket_id_ 是固定的
    // 对于全表迭代器，bucket_id_ 会递增
};

template <typename _Key, typename _Value, typename Policy>
struct DashTable<_Key, _Value, Policy>::BucketSet { // ？？？？
    auto buckets() const {
        bool is_all = limit_ > ids_.size(); // 判断是连续范围还是离散列表
        return std::views::iota(0u, limit_) | //   生成 0..limit_-1 的整数序列 
                std::views::transform([*this, is_all](uint8_t i) {
                uint8_t index = is_all ? i : ids_[i];// 根据模式选择桶 ID
                return bucket_iterator{owner_, seg_id_, index}; // 生成指向该桶的迭代器
            });
    }

    bool operator==(const BucketSet& other) const {
        return owner_ == other.owner_ && seg_id_ == other.seg_id_ && limit_ == other.limit_ &&
            ids_[0] == other.ids_[0] && ids_[1] == other.ids_[1];
    }

private:
    friend class DashTable;

    BucketSet(DashTable* owner, uint32_t seg_id, uint8_t limit, uint8_t ids[2])
        : owner_{owner}, seg_id_{seg_id}, limit_{limit}, ids_{ids[0], ids[1]} {
    }

    DashTable* owner_;
    uint32_t seg_id_;
    uint8_t limit_; // 桶数量限制
    std::array<uint8_t, 2> ids_;
};


template <typename _Key, typename _Value, typename Policy>
DashTable<_Key, _Value, Policy>::DashTable(size_t capacity_log, const Policy& policy,
                                           PMR_NS::memory_resource* mr)
    : Base(capacity_log), policy_(policy), segment_(mr) {
    segment_.resize(unique_segments_);
    for (uint32_t i = 0; i < segment_.size(); ++i) {
        segment_[i] = ConstructSegment(global_depth_, i);  
    }
}
template <typename _Key, typename _Value, typename Policy>
DashTable<_Key, _Value, Policy>::~DashTable() {
    Clear();
    auto* resource = segment_.get_allocator().resource();
    PMR_NS::polymorphic_allocator<SegmentType> pa(resource);
    using alloc_traits = std::allocator_traits<decltype(pa)>;

    IterateDistinct([&](SegmentType* seg) {
        alloc_traits::destroy(pa, seg);
        alloc_traits::deallocate(pa, seg, 1);
        return false;
    });
}
template <typename _Key, typename _Value, typename Policy>
typename DashTable<_Key, _Value, Policy>::SegmentType* 
DashTable<_Key, _Value, Policy>::ConstructSegment(uint8_t depth, uint32_t id) {
    auto* mr = segment_.get_allocator().resource();
    PMR_NS::polymorphic_allocator<SegmentType> pa(mr);
    SegmentType* res = pa.allocate(1);
    pa.construct(res, depth, id, mr);  //   new SegmentType(depth);
    bucket_count_ += res->num_buckets();
    return res;
}



template <typename _Key, typename _Value, typename Policy>
template <typename Cb>
void DashTable<_Key, _Value, Policy>::IterateDistinct(Cb&& cb) { // ????
    size_t i = 0;
    while (i < segment_.size()) {
        auto* seg = segment_[i];
        size_t next_id = NextSeg(i);
        if (cb(seg))
            break;
        i = next_id;
    }
}

template <typename _Key, typename _Value, typename Policy>
void DashTable<_Key, _Value, Policy>::Clear() {
    auto cb = [this](SegmentType* seg) {
        seg->TraverseAll([this, seg](const SegmentIterator& it) {
            policy_.DestroyKey(seg->Key(it.index, it.slot));
            policy_.DestroyValue(seg->Value(it.index, it.slot));
        });
        seg->Clear();
        return false;
    };

    IterateDistinct(cb);
    size_ = 0;

    // Consider the following case: table with 8 segments overall, 4 distinct.
    // S1, S1, S1, S1, S2, S3, S4, S4
    /* This corresponds to the tree:
                R
            /  \
            S1   /\
                /\ S4
            S2 S3
        We want to collapse this tree into, say, 2 segment directory.
        That means we need to keep S1, S2 but delete S3, S4.
        That means, we need to move representative segments until we reached the desired size
        and then erase all other distinct segments.
    **********/
    if (global_depth_ > initial_depth_) {
        PMR_NS::polymorphic_allocator<SegmentType> pa(segment_.get_allocator());
        using alloc_traits = std::allocator_traits<decltype(pa)>;

        size_t dest = 0, src = 0;
        size_t new_size = (1 << initial_depth_);
        bucket_count_ = 0;
        while (src < segment_.size()) {
        auto* seg = segment_[src];
        size_t next_src = NextSeg(src);  // must do before because NextSeg is dependent on seg.
        if (dest < new_size) {
            seg->set_local_depth(initial_depth_);
            bucket_count_ += seg->num_buckets();
            segment_[dest++] = seg;
        } else {
            alloc_traits::destroy(pa, seg);
            alloc_traits::deallocate(pa, seg, 1);
        }

        src = next_src;
        }

        global_depth_ = initial_depth_;
        unique_segments_ = new_size;
        segment_.resize(new_size);
    }
}


}




















