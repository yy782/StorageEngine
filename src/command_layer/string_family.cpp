// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include <absl/container/inlined_vector.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <variant>

#include "base/flags.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "util/fibers/future.h"

#include "db_slice.hpp"
#include "engine_shard.hpp"

namespace dfly {

namespace {

using CI = CommandId;

enum class ExpT { EX, PX, EXAT, PXAT };

constexpr uint32_t kMaxStrLen = 1 << 28;

template <typename T> 
using StringResult = TResultOrT<std::string>;

StringResult ReadString(DbIndex dbid, string_view key, const PrimeValue& pv, EngineShard* es) {
    return pv.IsExternal() ? StringResult{ReadTieredString(dbid, key, pv, es->tiered_storage())}
                          : StringResult{pv.ToString()};
}

// Helper for performing SET operations with various options
class SetCmd { // SET 命令处理器
public:
    explicit SetCmd(OpArgs op_args, bool explicit_journal)
        : op_args_(op_args), explicit_journal_{explicit_journal} {
    }

    enum SetFlags {
        SET_ALWAYS = 0,
        SET_IF_NOTEXIST = 1 << 0,     /* NX: Set if key not exists. */
        SET_IF_EXISTS = 1 << 1,       /* XX: Set if key exists. */
        SET_KEEP_EXPIRE = 1 << 2,     /* KEEPTTL: Set and keep the ttl */
        SET_GET = 1 << 3,             /* GET: Set if want to get key before set */
        SET_EXPIRE_AFTER_MS = 1 << 4, /* EX,PX,EXAT,PXAT: Expire after ms. */
        SET_STICK = 1 << 5,           /* Set STICK flag */
    };

    struct SetParams {
        uint16_t flags = SET_ALWAYS;
        uint64_t expire_after_ms = 0;  // Relative value based on now. 0 means no expiration.
        optional<StringResult>* prev_val = nullptr;  // if set, previous value will be stored if found

        constexpr bool IsConditionalSet() const {
            return flags & SET_IF_NOTEXIST || flags & SET_IF_EXISTS;
        }
    };

  OpStatus Set(const SetParams& params, std::string_view key, std::string_view value);

private:
    OpStatus SetExisting(const SetParams& params, std::string_view value,
                        DbSlice::ItAndUpdater* it_upd);

    void AddNew(const SetParams& params, const DbSlice::Iterator& it, std::string_view key,
                std::string_view value);

    // Called at the end of AddNew of SetExisting
    void PostEdit(const SetParams& params, std::string_view key, std::string_view value,
                  PrimeValue* pv);

    void RecordJournal(const SetParams& params, std::string_view key, std::string_view value);

    OpStatus CachePrevIfNeeded(const SetParams& params, DbSlice::Iterator it);

    const OpArgs op_args_;

};


struct GetReplies {
  GetReplies(SinkReplyBuilder* rb) : rb{static_cast<RedisReplyBuilder*>(rb)} {
    DCHECK(dynamic_cast<RedisReplyBuilder*>(rb));
  }

  template <typename T> void Send(OpResult<T>&& res) const {
    switch (res.status()) {
      case OpStatus::OK:
        return Send(std::move(res.value()));
      case OpStatus::WRONG_TYPE:
        return rb->SendError(kWrongTypeErr);
      case OpStatus::IO_ERROR:
        return rb->SendError(kTieredIoError);
      default:
        rb->SendNull();
    }
  }

  template <typename T> void Send(optional<T>&& res) const {
    if (res.has_value())
      return Send(std::move(*res));
    return rb->SendNull();
  }

  template <typename T> void Send(TResultOrT<T>&& res) const {
    if (holds_alternative<T>(res))
      return Send(get<T>(res));

    io::Result<T> iores = get<1>(std::move(res)).Get();
    if (iores.has_value())
      Send(*iores);
    else
      Send(iores.error().message());
  }

  void Send(size_t val) const {
    rb->SendLong(val);
  }

  void Send(string_view str) const {
    rb->SendBulkString(str);
  }

  RedisReplyBuilder* rb;
};


OpStatus SetCmd::Set(const SetParams& params, string_view key, string_view value) {
    auto& db_slice = op_args_.GetDbSlice();
    auto op_res = db_slice.AddOrFind(op_args_.db_cntx, key, std::nullopt);


    if (!op_res->is_new) {
        return SetExisting(params, value, &(*op_res));
    } else {
        AddNew(params, op_res->it, key, value);
        return OpStatus::OK;
    }
}

OpStatus SetCmd::SetExisting(const SetParams& params, string_view value,
                             DbSlice::ItAndUpdater* it_upd) {

    PrimeKey& key = it_upd->it->first;
    PrimeValue& prime_value = it_upd->it->second;
    EngineShard* shard = op_args_.shard;

    auto& db_slice = op_args_.GetDbSlice();
    uint64_t at_ms =
        params.expire_after_ms ? params.expire_after_ms + op_args_.db_cntx.time_now_ms : 0;

    if (!(params.flags & SET_KEEP_EXPIRE)) {
        if (at_ms) {
            db_slice.AddExpire(op_args_.db_cntx.db_index, it_upd->it, at_ms);
        } else {
            db_slice.RemoveExpire(op_args_.db_cntx.db_index, it_upd->it);
        }
    }
    prime_value.SetString(value);
    return OpStatus::OK;
}

void SetCmd::AddNew(const SetParams& params, const DbSlice::Iterator& it, std::string_view key,
                    std::string_view value) {
  auto& db_slice = op_args_.GetDbSlice();
  it->second = PrimeValue{value};

  if (params.expire_after_ms) {
      db_slice.AddExpire(op_args_.db_cntx.db_index, it,
                        params.expire_after_ms + op_args_.db_cntx.time_now_ms);
  }
}




struct NegativeExpire {};  // Returned if relative expiry was in the past
struct ErrorReply{};
std::variant<SetCmd::SetParams, ErrorReply, NegativeExpire> ParseSetParams(
    CmdArgParser parser, const CommandContext* cmd_cntx) {
    SetCmd::SetParams sparams;

    while (parser.HasNext()) {
      if (auto exp_type = parser.TryMapNext("EX", ExpT::EX, "PX", ExpT::PX, "EXAT", ExpT::EXAT,
                                            "PXAT", ExpT::PXAT);
          exp_type) {
        auto int_arg = parser.Next<int64_t>();
        if (parser.HasError())
          return ErrorReply{};

        sparams.flags |= SetCmd::SET_EXPIRE_AFTER_MS;

        DbSlice::ExpireParams expiry{
            .value = int_arg,
            .unit = *exp_type == ExpT::PX || *exp_type == ExpT::PXAT ? TimeUnit::MSEC : TimeUnit::SEC,
            .absolute = *exp_type == ExpT::EXAT || *exp_type == ExpT::PXAT,
        };

        int64_t now_ms = GetCurrentTimeMs();
        auto [rel_ms, abs_ms] = expiry.Calculate(now_ms, false);

        // Remove existed key if the key is expired already
        if (rel_ms < 0)
          return NegativeExpire{};

        std::tie(sparams.expire_after_ms, ignore) = expiry.Calculate(now_ms, true);
    }
    return sparams;
}

cmd::CmdR CmdSet(CmdArgList args, CommandContext* cmd_cntx) {
    facade::CmdArgParser parser{args};

    auto [key, value] = parser.Next<string_view, string_view>();
    auto params_result = ParseSetParams(parser, cmd_cntx);

    auto& sparams = util::bf2::get<SetCmd::SetParams>(params_result);

    optional<StringResult> prev;
    if (sparams.flags & SetCmd::SET_GET)
        sparams.prev_val = &prev;

    optional<util::fb2::Future<bool>> backpressure;
    sparams.backpressure = &backpressure;

    auto cb = [&](Transaction* t, EngineShard* shard) {
        return SetCmd(t->GetOpArgs(shard), true).Set(sparams, key, value);
    };

    OpStatus result = co_await cmd::SingleHop(cb);
    auto* rb = cmd_cntx->rb();

    switch (result) {
        case OpStatus::WRONG_TYPE:
            rb->SendError(kWrongTypeErr);  // TODO(vlad): use co_return after await?
            co_return std::nullopt;
        case OpStatus::OUT_OF_MEMORY:
            rb->SendError(kOutOfMemory);
            co_return std::nullopt;
        default:
            break;
    };


    if (sparams.flags & SetCmd::SET_GET) {
        GetReplies{rb}.Send(std::move(prev));
        co_return std::nullopt;
    }

    if (result == OpStatus::OK) {
        rb->SendOk();
    } else {
        static_cast<RedisReplyBuilder*>(rb)->SendNull();
    }

    co_return std::nullopt;
}



cmd::CmdR CmdGet(CmdArgList args, CommandContext* cmd_cntx) {
    auto cb = [key = ArgS(args, 0)](Transaction* tx, EngineShard* es) -> OpResult<StringResult> {
      auto it_res = tx->GetDbSlice(es->shard_id()).FindReadOnly(tx->GetDbContext(), key, OBJ_STRING);
      if (!it_res.ok())
          return it_res.status();

      return ReadString(tx->GetDbIndex(), key, (*it_res)->second, es);
    };

    GetReplies{cmd_cntx->rb()}.Send(co_await cmd::SingleHopT(cb));
    co_return std::nullopt;
}


}  // namespace

}

cmd::CmdR CmdSet(CmdArgList args, CommandContext* cmd_cntx) {
    facade::CmdArgParser parser{args};

    auto [key, value] = parser.Next<string_view, string_view>();
    auto params_result = ParseSetParams(parser, cmd_cntx); // 解析 SET 命令的选项（如 EX 、 PX 、 NX 等）

    if (holds_alternative<ErrorReply>(params_result))
        co_return util::fb2::get<ErrorReply>(params_result);

    auto& sparams = get<SetCmd::SetParams>(params_result); // 获取解析后的 SetParams 结构体

    auto cb = [&](Transaction* t, EngineShard* shard) {
        return SetCmd(t->GetOpArgs(shard), true).Set(sparams, key, value);
    };

    OpStatus result = co_await cmd::SingleHop(cb);
    auto* rb = cmd_cntx->rb();

    switch (result) {
        case OpStatus::WRONG_TYPE:
            rb->SendError(kWrongTypeErr);  // TODO(vlad): use co_return after await?
            co_return std::nullopt;
        case OpStatus::OUT_OF_MEMORY:
            rb->SendError(kOutOfMemory);
            co_return std::nullopt;
        default:
            break;
    };



    if (result == OpStatus::OK) {
        rb->SendOk();
    } else {
        static_cast<RedisReplyBuilder*>(rb)->SendNull();
    }

    co_return std::nullopt;
}

cmd::CmdR CmdGet(CmdArgList args, CommandContext* cmd_cntx) {
    auto cb = [key = ArgS(args, 0)](Transaction* tx, EngineShard* es) -> OpResult<StringResult> {
        auto it_res = tx->GetDbSlice(es->shard_id()).FindReadOnly(tx->GetDbContext(), key, OBJ_STRING);
        if (!it_res.ok())
            return it_res.status();

        return ReadString(tx->GetDbIndex(), key, (*it_res)->second, es);
    };

    GetReplies{cmd_cntx->rb()}.Send(co_await cmd::SingleHopT(cb));
    co_return std::nullopt;
}



void RegisterStringFamily(CommandRegistry* registry) {
  constexpr uint32_t kMSetMask = CO::JOURNALED | CO::DENYOOM | CO::NO_AUTOJOURNAL;

  registry->StartFamily();
  *registry
      << CI{"SET", -3, 1, 1}.SetAsyncHandler(
             CmdSet)
      << CI{"GET", 2, 1, 1}.SetAsyncHandler(CmdGet)
      ;
}

}  // namespace dfly
