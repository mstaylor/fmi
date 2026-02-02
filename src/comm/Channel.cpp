#include "../../include/comm/Channel.h"
#include "../../include/comm/S3.h"
#include "../../include/comm/Redis.h"
#include "../../include/comm/Direct.h"

std::shared_ptr<FMI::Comm::Channel> FMI::Comm::Channel::get_channel(std::string name, std::map<std::string, std::string> params,
                                                                    std::map<std::string, std::string> model_params) {
    if (name == "S3") {
        return std::make_shared<S3>(params, model_params);
    } else if (name == "Redis") {
        return std::make_shared<Redis>(params, model_params);
    } else if (name == "Direct") {
        return std::make_shared<Direct>(params, model_params);
    } else {
        throw std::runtime_error("Unknown channel name passed");
    }
}

void FMI::Comm::Channel::gather(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root) {
    if (peer_id != root) {
        send(sendbuf, root);
    } else {
        auto buffer_length = sendbuf->len;
        for (unsigned int i = 0; i < num_peers; i++) {
            if (i == root) {
                std::memcpy(recvbuf->get() + root * buffer_length, sendbuf->get(), buffer_length);
            } else {
                auto peer_data = std::make_shared<channel_data>(recvbuf->get() + i * buffer_length, buffer_length, noop_deleter);
                recv(peer_data, i);
            }
        }
    }
}

void FMI::Comm::Channel::gatherv(std::shared_ptr<channel_data> sendbuf,
                                  std::shared_ptr<channel_data> recvbuf,
                                  FMI::Utils::peer_num root,
                                  const std::vector<int32_t>& recvcounts,
                                  const std::vector<int32_t>& displs) {
    // Default implementation - subclasses can provide optimized versions
    if (peer_id != root) {
        send(sendbuf, root);
    } else {
        // Copy own data
        std::memcpy(recvbuf->get() + displs[root], sendbuf->get(), recvcounts[root]);
        // Receive from others
        for (unsigned int i = 0; i < num_peers; i++) {
            if (i != root) {
                auto peer_data = std::make_shared<channel_data>(recvbuf->get() + displs[i], recvcounts[i], noop_deleter);
                recv(peer_data, i);
            }
        }
    }
}

void FMI::Comm::Channel::gatherv(std::shared_ptr<channel_data> sendbuf,
                                  std::shared_ptr<channel_data> recvbuf,
                                  FMI::Utils::peer_num root,
                                  const std::vector<int32_t>& recvcounts,
                                  const std::vector<int32_t>& displs,
                                  FMI::Utils::Mode mode,
                                  std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                    FMI::Utils::fmiContext*)> callback) {
    // Default: just call blocking version
    gatherv(sendbuf, recvbuf, root, recvcounts, displs);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}

void FMI::Comm::Channel::allgather(std::shared_ptr<channel_data> sendbuf,
                                    std::shared_ptr<channel_data> recvbuf,
                                    FMI::Utils::peer_num root) {
    // Default: gather + bcast
    gather(sendbuf, recvbuf, root);
    bcast(recvbuf, root);
}

void FMI::Comm::Channel::allgather(std::shared_ptr<channel_data> sendbuf,
                                    std::shared_ptr<channel_data> recvbuf,
                                    FMI::Utils::peer_num root, FMI::Utils::Mode mode,
                                    std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                      FMI::Utils::fmiContext*)> callback) {
    // Default: just call blocking version
    allgather(sendbuf, recvbuf, root);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}

void FMI::Comm::Channel::allgatherv(std::shared_ptr<channel_data> sendbuf,
                                     std::shared_ptr<channel_data> recvbuf,
                                     FMI::Utils::peer_num root,
                                     const std::vector<int32_t>& recvcounts,
                                     const std::vector<int32_t>& displs) {
    // Default: gatherv + bcast
    gatherv(sendbuf, recvbuf, root, recvcounts, displs);
    bcast(recvbuf, root);
}

void FMI::Comm::Channel::allgatherv(std::shared_ptr<channel_data> sendbuf,
                                     std::shared_ptr<channel_data> recvbuf,
                                     FMI::Utils::peer_num root,
                                     const std::vector<int32_t>& recvcounts,
                                     const std::vector<int32_t>& displs,
                                     FMI::Utils::Mode mode,
                                     std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                       FMI::Utils::fmiContext*)> callback) {
    // Default: just call blocking version
    allgatherv(sendbuf, recvbuf, root, recvcounts, displs);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}

void FMI::Comm::Channel::scatter(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root) {
    if (peer_id == root) {
        auto buffer_length = recvbuf->len;
        for (unsigned int i = 0; i < num_peers; i++) {
            if (i == root) {
                std::memcpy(recvbuf->get(), sendbuf->get() + root * buffer_length, buffer_length);
            } else {
                auto peer_data = std::make_shared<channel_data>(sendbuf->get() + i * buffer_length, buffer_length, noop_deleter);
                send(peer_data, i);
            }
        }
    } else {
        recv(recvbuf, root);
    }
}

void FMI::Comm::Channel::allreduce(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, raw_function f) {
    reduce(sendbuf, recvbuf, 0, f);
    bcast(recvbuf, 0);
}

void FMI::Comm::Channel::bcast(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num root,
                                FMI::Utils::Mode mode,
                                std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                  FMI::Utils::fmiContext*)> callback) {
    // Default: just call blocking version
    bcast(buf, root);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}
