// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/flat_hash_set.h>

#include "conn_context.h"
#include "facade/parsed_command.h"
#include "facade/reply_mode.h"
#include "server/acl/acl_commands_def.h"
#include "server/common.h"
#include "server/tx_base.h"
#include "server/version.h"

namespace dfly {

class EngineShardSet;
class ChannelStore;
class Interpreter;
struct FlowInfo;

// Stores command id and arguments for delayed invocation.
// Used for storing MULTI/EXEC commands.
class StoredCmd {
 public:
  // Deep copy of args, creates backing storage internally.
  StoredCmd(const CommandId* cid, ArgSlice args, facade::ReplyMode mode = facade::ReplyMode::FULL);

  // Shallow copy of args.
  StoredCmd(const CommandId* cid, facade::ParsedArgs args)
      : cid_{cid}, args_{args}, reply_mode_(facade::ReplyMode::FULL) {
  }

  size_t NumArgs() const {
    return args_.size();
  }

  size_t UsedMemory() const {
    return backed_ ? backed_->HeapMemory() + sizeof(*backed_) : 0;
  }

  facade::ArgSlice Slice(CmdArgVec* scratch) const;
  std::string FirstArg() const;

  const CommandId* Cid() const {
    return cid_;
  }

  facade::ReplyMode ReplyMode() const {
    return reply_mode_;
  }

 private:
  const CommandId* cid_;     // underlying command
  facade::ParsedArgs args_;  // arguments

  // TODO: we could optimize the storage further by introducing StoredCmdCollection and
  // keep the backing storage there. Then this class will only use shallow copies.
  std::unique_ptr<cmn::BackedArguments> backed_;
  facade::ReplyMode reply_mode_;  // reply mode
};

struct ConnectionState {
    DbIndex db_index_ = 0;
};

class ConnectionContext : public facade::ConnectionContext {
public:
    ConnectionContext(facade::Connection* owner, dfly::acl::UserCredentials cred);
    Namespace* ns = nullptr;
    ConnectionState conn_state;
    DbIndex db_index() const {
        return conn_state.db_index_;
    }
};

class CommandContext : public facade::ParsedCommand {
 public:
  CommandContext() = default;
  CommandContext(facade::SinkReplyBuilder* rb, facade::ConnectionContext* conn_cntx) {
    Init(rb, conn_cntx);
  }

  void SetupTx(const CommandId* cid, Transaction* tx) {
    cid_ = cid;
    tx_ = tx;
  }

  void UpdateCid(const CommandId* cid) {
    cid_ = cid;
  }

  virtual size_t GetSize() const override {
    return sizeof(CommandContext);
  }

  ConnectionContext* server_conn_cntx() const {
    return static_cast<ConnectionContext*>(conn_cntx_);
  }

  void RecordLatency(facade::ArgSlice tail_args) const;

  facade::Connection* conn() const {
    return conn_cntx_->conn();
  }

  facade::SinkReplyBuilder* SwapReplier(facade::SinkReplyBuilder* new_rb) {
    return std::exchange(rb_, new_rb);
  }

  Transaction* tx() const {
    return tx_;
  }

  const CommandId* cid() const {
    return cid_;
  }

  uint64_t start_time_ns = 0;

  // Stores backing array for tail args slice
  CmdArgVec arg_slice_backing;

 protected:
  void ReuseInternal() final;

  Transaction* tx_ = nullptr;
  const CommandId* cid_ = nullptr;
};

}  // namespace dfly
