// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <utils/function_ref.hpp>

#include <boost/intrusive/list.hpp>
#include <functional>
#include <variant>

namespace util {
namespace fb2 {
namespace detail {

class FiberInterface;

class Waiter {
  friend class FiberInterface;
  explicit Waiter(FiberInterface* cntx) : cntx_(cntx) {
  }

 public:
  using Callback = absl::FunctionRef<void()>;

  // Create from reference to functor. Functor must outlive waiter
  explicit Waiter(const Callback& cb) : cntx_{cb} {
  }

  // For some boost versions/distributions, the default move c'tor does not work well,
  // so we implement it explicitly.
  Waiter(Waiter&& o) : cntx_{std::move(o.cntx_)}, persistent_{o.persistent_} {
    // it does not work well for slist because its reference is used by slist members
    // (probably when caching last).
    o.wait_hook.swap_nodes(wait_hook);
    o.cntx_ = static_cast<FiberInterface*>(nullptr);
    o.persistent_ = false;
  }

  // safe_link is used in assertions via IsLinked() method.
  using ListHookType =
      boost::intrusive::list_member_hook<boost::intrusive::link_mode<boost::intrusive::safe_link>>;

  bool IsLinked() const {
    return wait_hook.is_linked();
  }

  // Persistent waiters are NOT unlinked by WaitQueue::NotifyOne() or
  // WaitQueue::NotifyAll(). Their callback is fired but they remain in the queue.
  bool is_persistent() const {
    return persistent_;
  }

  void set_persistent(bool v) {
    persistent_ = v;
  }

  void Notify(FiberInterface* active) const;

  ListHookType wait_hook;

 private:
  void NotifyImpl(FiberInterface*, FiberInterface* active) const;
  void NotifyImpl(const Callback&, FiberInterface* active) const;

  std::variant<FiberInterface*, Callback> cntx_;
  bool persistent_ = false;
};

// Intrusive list of non-owned waiter objects.
// Not thread safe, must be protected with lock.
class WaitQueue {
 public:
  bool empty() const {
    return wait_list_.empty();
  }

  void Link(Waiter* waiter);

  void Unlink(Waiter* waiter) {
    auto it = WaitList::s_iterator_to(*waiter);
    wait_list_.erase(it);
  }

  // Return true if a waiter exitsted and was notified
  bool NotifyOne(FiberInterface* active);

  // Return true if any waiter was notified
  bool NotifyAll(FiberInterface* active);

 private:
  using WaitList = boost::intrusive::list<
      Waiter, boost::intrusive::member_hook<Waiter, Waiter::ListHookType, &Waiter::wait_hook>,
      boost::intrusive::constant_time_size<false>>;

  WaitList wait_list_;
};

}  // namespace detail
}  // namespace fb2
}  // namespace util
