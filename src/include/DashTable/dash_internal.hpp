#pragma once 
namespace dfly {
namespace detail {

// SlotBitmap
// 模式1：单整数模式
// ┌─────────────────────────────────────────────────────────────────┐
// │                    单个 uint32_t (32 位)                        │
// ├───────────────┬───────────────┬───────────────┬───────────────┤
// │   高 14 位     │   中 14 位     │   低 4 位     │               │
// │   (busy 位图)  │   (probe 位图) │(槽位计数size_)│               │
// │   位 18-31     │   位 4-17      │   位 0-3      │               │
// └───────────────┴───────────────┴───────────────┴───────────────┘
// 模式2：双整数模式
// ┌─────────────────────────────────────────────────────────────────┐
// │                    第一个 uint32_t                              │
// ├─────────────────────────────────────────────────────────────────┤
// │                      busy 位图 (低 28 位)                        │
// │                      高 4 位未使用                               │
// └─────────────────────────────────────────────────────────────────┘

// ┌─────────────────────────────────────────────────────────────────┐
// │                    第二个 uint32_t                              │
// ├─────────────────────────────────────────────────────────────────┤
// │                      probe 位图 (低 28 位)                       │
// │                      高 4 位未使用                               │
// └─────────────────────────────────────────────────────────────────┘

template <unsigned NUM_SLOTS> 
class SlotBitmap {
    static_assert(NUM_SLOTS > 0 && NUM_SLOTS <= 28); // 超过 28 个槽位，单个 uint32_t（32 位）存不下所有状态
    static constexpr bool SINGLE = NUM_SLOTS <= 14; // 是否可以使用单 32 位整数存储位图 , 每个槽位需要 2 位信息（busy + probe）
    static constexpr unsigned kLen = SINGLE ? 1 : 2;
    static constexpr unsigned kAllocMask = (1u << NUM_SLOTS) - 1; // 槽位掩码 ， 用于操作高 14 位的 busy 位图
    static constexpr unsigned kBitmapLenMask = (1 << 4) - 1; // 长度掩码 , 对应已用槽位数量

public:
    uint32_t GetProbe(bool probe) const {
        if constexpr (SINGLE)
            return ((val_[0].d >> 4) & kAllocMask) ^ ((!probe) * kAllocMask);
        else
            return (val_[1].d & kAllocMask) ^ ((!probe) * kAllocMask);
    }
    uint32_t GetBusy() const {
        return SINGLE ? val_[0].d >> 18 : val_[0].d;
    }

    bool IsFull() const {
        return Size() == NUM_SLOTS;
    }

    unsigned Size() const {
        return SINGLE ? (val_[0].d & kBitmapLenMask) : __builtin_popcount(val_[0].d);
    }
    int FindEmptySlot() const {
        uint32_t mask = ~(GetBusy());
        int slot = __builtin_ctz(mask);
        assert(slot < int(NUM_SLOTS));
        return slot;
    }
    void ClearSlots(uint32_t mask){
        if (SINGLE) {
            uint32_t count = __builtin_popcount(mask);
            assert(count <= (val_[0].d & 0xFF));
            mask = (mask << 4) | (mask << 18);
            val_[0].d &= ~mask;
            val_[0].d -= count;
        } else {
            val_[0].d &= ~mask;
            val_[1].d &= ~mask;
        }
    }


    void Clear() {
        if (SINGLE) {
            val_[0].d = 0;
        } else {
            val_[0].d = val_[1].d = 0;
        }
    }

    void ClearSlot(unsigned index);
    void SetSlot(unsigned index, bool probe);

    bool ShiftLeft();

    void Swap(unsigned slot_a, unsigned slot_b);

private:
    struct Unaligned {
        // 强制非对齐，性能换内存? ，可能不会牺牲性能，对CPU缓存友好?
        uint32_t d __attribute__((packed, aligned(1)));

        Unaligned() : d(0) {
        }
    };

    Unaligned val_[kLen];
};  // SlotBitmap



template <unsigned NUM_SLOTS> 
class BucketBase {
    static constexpr unsigned kStashFpLen = 4;
    static constexpr unsigned kStashPresentBit = 1 << 4;

    using FpArray = std::array<uint8_t, NUM_SLOTS>;
    using StashFpArray = std::array<uint8_t, kStashFpLen>;

public:
    using SlotId = uint8_t;
    static constexpr SlotId kNanSlot = 255;

    bool IsFull() const {
        return Size() == NUM_SLOTS;
    }

    bool IsEmpty() const {
        return GetBusy() == 0;
    }

    unsigned Size() const {
        return slotb_.Size();
    }

    void Delete(SlotId sid) {
        slotb_.ClearSlot(sid);
    }

    unsigned Find(uint8_t fp_hash, bool probe) const {
        unsigned mask = CompareFP(fp_hash) & GetBusy();
        return mask & GetProbe(probe);
    }

    uint8_t Fp(unsigned i) const {
        assert(i < finger_arr_.size());
        return finger_arr_[i];
    }
    uint32_t GetProbe(bool probe) const {
        return slotb_.GetProbe(probe);
    }
    uint32_t GetBusy() const {
        return slotb_.GetBusy();
    }

    bool IsBusy(unsigned slot) const {
        return (GetBusy() & (1u << slot)) != 0;
    }
    void ClearSlots(uint32_t mask) {
        slotb_.ClearSlots(mask);
    }
    void Clear() {
        slotb_.Clear();
    }

    void ClearStashPtrs() {
        stash_busy_ = 0;
        stash_pos_ = 0;
        stash_probe_mask_ = 0;
        overflow_count_ = 0;
    }

    bool HasStash() const {
        return stash_busy_ & kStashPresentBit;
    }

    // void SetHash(unsigned slot_id, uint8_t meta_hash, bool probe);

    bool HasStashOverflow() const {
        return overflow_count_ > 0;
    }
    // template <typename F>
    // std::pair<unsigned, SlotId> IterateStash(uint8_t fp, bool is_probe, F&& func) const;

    void Swap(unsigned slot_a, unsigned slot_b) {
        slotb_.Swap(slot_a, slot_b);
        std::swap(finger_arr_[slot_a], finger_arr_[slot_b]);
    }

protected:
    uint32_t CompareFP(uint8_t fp) const;
    bool ShiftRight();
    bool SetStash(uint8_t fp, unsigned stash_pos, bool probe);
    bool ClearStash(uint8_t fp, unsigned stash_pos, bool probe);

    SlotBitmap<NUM_SLOTS> slotb_;  
    FpArray finger_arr_;
    StashFpArray stash_arr_; // 存储 Stash 槽位的指纹

    uint8_t stash_busy_ = 0;  
    uint8_t stash_pos_ = 0;   
    uint8_t stash_probe_mask_ = 0;


    uint8_t overflow_count_ = 0;  // 溢出计数器。记录有多少个 Stash 引用被“溢出”存储到了邻居桶中。
};  // BucketBase

struct DefaultSegmentPolicy {
    static constexpr unsigned kSlotNum = 12;
    static constexpr unsigned kBucketNum = 64;
    // static constexpr unsigned  kStashBucketNum = 4;
    // static constexpr bool kUseVersion = true;
};

using PhysicalBid = uint8_t; // 数据实际存储的桶位置
using LogicalBid = uint8_t; // 键经过哈希后应该归属的桶位置

template <typename KeyType, typename ValueType, typename Policy = DefaultSegmentPolicy>
class Segment {
    static constexpr unsigned kSlotNum = Policy::kSlotNum;
    static constexpr unsigned kBucketNum = Policy::kBucketNum;
    static constexpr unsigned kStashBucketNum = 4;
    // static constexpr bool kUseVersion = Policy::kUseVersion;
    static constexpr unsigned kFingerBits = 8;
    static constexpr unsigned kTotalBuckets = kBucketNum + kStashBucketNum;
    static_assert(kTotalBuckets < 0xFF);
    using BucketType = BucketBase<kSlotNum>;

    struct Bucket : public BucketType{
        using BucketType::kNanSlot;
        using typename BucketType::SlotId;
        KeyType key[kSlotNum];
        ValueType value[kSlotNum];    
    };
    static constexpr PhysicalBid kNanBid = 0xFF;
    using SlotId = typename BucketType::SlotId;    
public:
    struct Iterator {
        PhysicalBid index;  // bucket index
        uint8_t slot;

        Iterator() : index(kNanBid), slot(BucketType::kNanSlot) {
        }

        Iterator(PhysicalBid bi, uint8_t sid) : index(bi), slot(sid) {
        }

        bool found() const {
            return index != kNanBid;
        }
    };
    using Value_t = ValueType;
    using Key_t = KeyType;
    using Hash_t = uint64_t;

    template <typename K, typename V, typename Pred, typename OnMoveCb>
    std::pair<Iterator, bool> Insert(K&& key, V&& value, Hash_t key_hash, Pred&& pred,
                                    OnMoveCb&& on_move_cb);

    template <typename HashFn, typename OnMoveCb>
    void Split(HashFn&& hfunc, Segment* dest, OnMoveCb&& on_move_cb);

    void Delete(const Iterator& it, Hash_t key_hash);

    void Clear();  // clears the segment.

    size_t SlowSize() const;
    
    size_t local_depth() const {
        return local_depth_;
    }
    unsigned num_buckets() const {
        return kBucketNum + kStashBucketNum;
    }
    uint32_t segment_id() const {
        return segment_id_;
    }
    void set_segment_id(uint32_t new_id) {
        segment_id_ = new_id;
    }
    const Bucket& GetBucket(PhysicalBid i) const {
        return bucket_[i];
    }

    Bucket& GetBucket(PhysicalBid i) {
        return bucket_[i];
    }

    bool IsBusy(PhysicalBid bid, unsigned slot) const {
        return GetBucket(bid).GetBusy() & (1U << slot);
    }    
    Key_t& Key(PhysicalBid bid, unsigned slot) {
        assert(IsBusy(bid, slot));
        return GetBucket(bid).key[slot];
    }

    const Key_t& Key(PhysicalBid bid, unsigned slot) const {
        assert(IsBusy(bid, slot));
        return GetBucket(bid).key[slot];
    }

    Value_t& Value(PhysicalBid bid, unsigned slot) {
        assert(IsBusy(bid, slot));
        return GetBucket(bid).value[slot];
    }

    const Value_t& Value(PhysicalBid bid, unsigned slot) const {
        assert(IsBusy(bid, slot));
        return GetBucket(bid).value[slot];
    }

    template <typename Cb> 
    void TraverseAll(Cb&& cb) const; // 对当前 Segment 中所有被占用的槽位遍历接口
private:
    Bucket bucket_[kTotalBuckets];
    uint8_t local_depth_; 
    uint32_t segment_id_;  // segment id in the table.
    PMR_NS::memory_resource* mr_ = nullptr;
};

class DashTableBase {
 public:
    explicit DashTableBase(uint32_t gd)
        : unique_segments_(1 << gd), initial_depth_(gd), global_depth_(gd) {
    }

    DashTableBase(const DashTableBase&) = delete;
    DashTableBase& operator=(const DashTableBase&) = delete;

    uint32_t unique_segments() const {
        return unique_segments_;
    }

    uint16_t depth() const {
        return global_depth_;
    }

    size_t size() const {
        return size_;
    }

    size_t Empty() const {
        return size_ == 0;
    }

 protected:
    uint32_t SegmentId(size_t hash) const {
        if (global_depth_) {
            return hash >> (64 - global_depth_);
        }

        return 0;
    }

    size_t size_ = 0;
    uint32_t unique_segments_ = 0; // 实际段数
    uint32_t bucket_count_ = 0;
    uint8_t initial_depth_;
    uint8_t global_depth_;
};  // DashTableBase
template <typename KeyType, typename ValueType> 
struct IteratorPair {
    IteratorPair(KeyType& k, ValueType& v) : 
    first(k), second(v) {
    }

    IteratorPair* operator->() {
        return this;
    }

    const IteratorPair* operator->() const {
        return this;
    }

    KeyType& first;
    ValueType& second;
};
class DashCursor {
public:
    explicit DashCursor(uint64_t token = 0) : val_(token) {
    }

    DashCursor(uint8_t depth, uint32_t seg_id, PhysicalBid bid)
        : val_((uint64_t(seg_id) << (40 - depth)) | bid) {
    }

    static DashCursor end() {
        return DashCursor{};
    }

    PhysicalBid bucket_id() const {
        return val_ & 0xFF;
    }
    uint32_t segment_id(uint8_t depth) const {
        return val_ >> (40 - depth);
    }
    uint64_t token() const {
        return val_;
    }
    explicit operator bool() const {
        return val_ != 0;
    }
private:
    uint64_t val_;
};

template <unsigned NUM_SLOTS> 
uint32_t BucketBase<NUM_SLOTS>::CompareFP(uint8_t fp) const {
    static_assert(FpArray{}.size() <= 16);
    const __m128i key_data = _mm_set1_epi8(fp);
    __m128i seg_data = mm_loadu_si128(reinterpret_cast<const __m128i*>(finger_arr_.data()));
    __m128i rv_mask = _mm_cmpeq_epi8(seg_data, key_data);
    int mask = _mm_movemask_epi8(rv_mask);
    return mask;
}


template <typename Key, typename Value, typename Policy>
template <typename Cb>
void Segment<Key, Value, Policy>::TraverseAll(Cb&& cb) const {
    for (uint8_t i = 0; i < kTotalBuckets; ++i) {
        bucket_[i].ForEachSlot([&](auto*, SlotId slot, bool) 
        { cb(Iterator{i, slot}); });
    }
}

}
}