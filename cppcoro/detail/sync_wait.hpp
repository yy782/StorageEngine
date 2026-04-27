

#ifndef COROUTINE_SYNC_WAIT_H
#define COROUTINE_SYNC_WAIT_H

#include "lightweight_manual_reset_event.hpp"
#include "sync_wait_task.hpp"
#include "awaiter_impl.hpp"

#include <cstdint>
#include <atomic>

namespace yy
{
template<typename AWAITABLE>
auto sync_wait(AWAITABLE&& awaitable)
-> typename detail::awaitable_traits<AWAITABLE&&>::await_result_t
{
#if CPPCORO_COMPILER_MSVC
  // HACK: Need to explicitly specify template argument to make_sync_wait_task
		// here to work around a bug in MSVC when passing parameters by universal
		// reference to a coroutine which causes the compiler to think it needs to
		// 'move' parameters passed by rvalue reference.
		auto task = detail::make_sync_wait_task<AWAITABLE>(awaitable);
#else
  auto task = detail::make_sync_wait_task(std::forward<AWAITABLE>(awaitable));
#endif
  detail::lightweight_manual_reset_event event;
  task.start(event);
  event.wait();
  return task.result();
}
}

#endif 
