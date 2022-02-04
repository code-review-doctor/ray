#pragma once
#include <grpcpp/server.h>

#include "ray/common/asio/instrumented_io_context.h"
#include "src/ray/protobuf/syncer.grpc.pb.h"

namespace ray {
namespace syncing {

struct Reporter {
  virtual ray::rpc::syncer::RaySyncMessage Snapshot() const = 0;
};

struct Receiver {
  virtual void Update(ray::rpc::syncer::RaySyncMessage &) = 0;
};

class RaySyncer {
 public:
  using RayComponentId = ray::rpc::syncer::RayComponentId;
  using RaySyncMessage = ray::rpc::syncer::RaySyncMessage;
  using RaySyncMessages = ray::rpc::syncer::RaySyncMessages;
  using RaySyncMessageType = ray::rpc::syncer::RaySyncMessageType;
  using ServerReactor = grpc::ServerBidiReactor<ray::rpc::syncer::RaySyncMessages,
                                                ray::rpc::syncer::RaySyncMessages>;
  using ClientReactor = grpc::ClientBidiReactor<ray::rpc::syncer::RaySyncMessages,
                                                ray::rpc::syncer::RaySyncMessages>;
  static constexpr size_t kComponentArraySize =
      static_cast<size_t>(ray::rpc::syncer::RayComponentId_ARRAYSIZE);

  RaySyncer(std::string node_id, instrumented_io_context &io_context);

  // Follower will send its message to leader
  // Leader will broadcast what it received to followers
  void Follow(std::shared_ptr<grpc::Channel> channel);

  // Register a component
  void Register(RayComponentId component_id, const Reporter *reporter,
                Receiver *receiver) {
    reporters_[component_id] = reporter;
    receivers_[component_id] = receiver;
  }

  // Update the message for component
  void Update(const std::string &from_node_id, RaySyncMessage message);

  void Update(const std::string &from_node_id, RaySyncMessages messages);

  std::vector<std::shared_ptr<RaySyncMessage>> SyncMessages(
      const std::string &node_id) const;

  const std::string &GetNodeId() const { return node_id_; }

  ServerReactor *Accept(const std::string &node_id) {
    auto reactor = std::make_unique<SyncerServerReactor>(*this, node_id);
    followers_.emplace(node_id, std::move(reactor));
    AddNode(node_id);
    return followers_[node_id].get();
  }

 private:
  void AddNode(const std::string &node_id) {
    cluster_messages_[node_id] = NodeIndexedMessages();
  }
  void DumpClusterMessages() const {
    RAY_LOG(INFO) << "---- DumpClusterMessages ----";
    for (auto &iter : cluster_messages_) {
      RAY_LOG(INFO) << "FromNodeId: " << iter.first << " - " << iter.second.size();
      for (auto &iterr : iter.second) {
        RAY_LOG(INFO) << "\tNodeIndexedMessages: " << iterr.first.first << ":"
                      << iterr.first.second << " - " << iterr.second.get();
      }
    }
  }
  template <typename T>
  struct Protocol : public T {
    using T::StartRead;
    using T::StartWrite;
    Protocol() {}
    void OnReadDone(bool ok) override {
      if (ok) {
        instance->io_context_.dispatch(
            [this] {
              instance->Update(node_id, std::move(in_message));
              in_message.Clear();
              StartRead(&in_message);
            },
            "ReadDone");
      } else {
        StartRead(&in_message);
      }
    }

    void OnWriteDone(bool ok) override {
      if (ok) {
        timer->expires_from_now(boost::posix_time::milliseconds(100));
        timer->async_wait([this](const boost::system::error_code &error) {
          if (error == boost::asio::error::operation_aborted) {
            return;
          }
          RAY_CHECK(!error) << error.message();
          SendMessage();
        });
      } else {
        instance->io_context_.dispatch([this] { SendMessage(); }, "RaySyncWrite");
      }
    }

    void ResetOutMessage() {
      arena.Reset();
      out_message =
          google::protobuf::Arena::CreateMessage<ray::rpc::syncer::RaySyncMessages>(
              &arena);
    }

    void SendMessage() {
      for (size_t i = 0; i < kComponentArraySize; ++i) {
        if (instance->reporters_[i] != nullptr) {
          instance->Update(instance->GetNodeId(), instance->reporters_[i]->Snapshot());
        }
      }
      buffer = instance->SyncMessages(node_id);
      if (buffer.empty()) {
        OnWriteDone(true);
        return;
      }
      ResetOutMessage();
      for (auto &message : buffer) {
        out_message->mutable_sync_messages()->UnsafeArenaAddAllocated(message.get());
      }
      StartWrite(out_message);
    }

    RaySyncer *instance;
    std::unique_ptr<boost::asio::deadline_timer> timer;

    std::string node_id;
    google::protobuf::Arena arena;
    ray::rpc::syncer::RaySyncMessages in_message;
    ray::rpc::syncer::RaySyncMessages *out_message;
    std::vector<std::shared_ptr<RaySyncMessage>> buffer;
  };

  class SyncerClientReactor : public Protocol<ClientReactor> {
   public:
    SyncerClientReactor(RaySyncer &instance, const std::string &node_id,
                        ray::rpc::syncer::RaySyncer::Stub &stub) {
      this->instance = &instance;
      this->timer =
          std::make_unique<boost::asio::deadline_timer>(this->instance->io_context_);
      this->node_id = node_id;
      rpc_context_.AddMetadata("node_id", node_id);
      stub.async()->StartSync(&rpc_context_, this);
      ResetOutMessage();
      StartCall();
    }

    void OnReadInitialMetadataDone(bool ok) override {
      RAY_CHECK(ok) << "Fail to read initial data";
      const auto &metadata = rpc_context_.GetServerInitialMetadata();
      auto iter = metadata.find("node_id");
      RAY_CHECK(iter != metadata.end());
      RAY_LOG(INFO) << "Start to follow " << iter->second;
      instance->AddNode(std::string(iter->second.begin(), iter->second.end()));
      node_id = std::string(iter->second.begin(), iter->second.end());
      StartRead(&in_message);
      instance->io_context_.dispatch([this] { SendMessage(); }, "RaySyncWrite");
    }

    void OnDone(const grpc::Status &status) override {
      instance->io_context_.dispatch([this] { instance->followers_.erase(node_id); },
                                     "RaySyncDone");
    }

   private:
    grpc::ClientContext rpc_context_;
  };

  class SyncerServerReactor : public Protocol<ServerReactor> {
   public:
    SyncerServerReactor(RaySyncer &instance, const std::string &node_id) {
      this->instance = &instance;
      this->node_id = node_id;
      this->timer =
          std::make_unique<boost::asio::deadline_timer>(this->instance->io_context_);
      StartSendInitialMetadata();
    }

    const std::string &GetNodeId() const { return node_id; }

    void OnSendInitialMetadataDone(bool ok) {
      if (ok) {
        StartRead(&in_message);
        instance->io_context_.dispatch([this] { SendMessage(); }, "RaySyncWrite");
      } else {
        Finish(grpc::Status::OK);
      }
    }

    void OnDone() override {
      instance->io_context_.dispatch([this] { instance->followers_.erase(node_id); },
                                     "RaySyncDone");
    }
  };

 private:
  const std::string node_id_;
  std::unique_ptr<ray::rpc::syncer::RaySyncer::Stub> leader_stub_;
  std::unique_ptr<ClientReactor> leader_;
  // Manage messages
  //   {from_node_id -> {(node_id, ResourceManager|Cluster) -> SyncMessage} }
  //   A <- B <- [C1,..,C100]
  //    \-- D
  //   When send message to the other node, don't send the message coming from that node
  //   because it has already got that.
  // TODO: Spilit it to make it easier to understand.
  using NodeIndexedMessages = absl::flat_hash_map<std::pair<std::string, RayComponentId>,
                                                  std::shared_ptr<RaySyncMessage>>;
  absl::flat_hash_map<std::string, NodeIndexedMessages> cluster_messages_;

  // Manage connections
  absl::flat_hash_map<std::string, std::unique_ptr<ServerReactor>> followers_;

  // For local nodes
  std::array<const Reporter *, kComponentArraySize> reporters_;
  std::array<Receiver *, kComponentArraySize> receivers_;
  instrumented_io_context &io_context_;
};

class RaySyncerService : public ray::rpc::syncer::RaySyncer::CallbackService {
 public:
  RaySyncerService(RaySyncer &syncer) : syncer_(syncer) {}

  grpc::ServerBidiReactor<ray::rpc::syncer::RaySyncMessages,
                          ray::rpc::syncer::RaySyncMessages>
      *StartSync(grpc::CallbackServerContext *context) override {
    const auto &metadata = context->client_metadata();
    auto iter = metadata.find("node_id");
    RAY_CHECK(iter != metadata.end());
    auto node_id = std::string(iter->second.begin(), iter->second.end());
    context->AddInitialMetadata("node_id", syncer_.GetNodeId());
    return syncer_.Accept(node_id);
  }

 private:
  RaySyncer &syncer_;
};

}  // namespace syncing
}  // namespace ray
