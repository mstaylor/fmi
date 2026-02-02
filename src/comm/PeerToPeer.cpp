#include "../../include/comm/PeerToPeer.h"
#include <cmath>
#include <iostream>
#include <cstring>

void FMI::Comm::PeerToPeer::send(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num dest) {
    send_object(buf, dest);
}

void FMI::Comm::PeerToPeer::recv(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num src) {
    recv_object(buf, src);
}

void FMI::Comm::PeerToPeer::send(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num dest,
                                  FMI::Utils::fmiContext* context, FMI::Utils::Mode mode,
                                  std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                    FMI::Utils::fmiContext*)> callback) {
    auto state = std::make_shared<IOState>();
    state->setRequest(buf);
    state->context = context;
    state->callbackResult = callback;
    state->operation = Utils::send;
    send_object(state, dest, mode);
}

void FMI::Comm::PeerToPeer::recv(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num src,
                                  FMI::Utils::fmiContext* context, FMI::Utils::Mode mode,
                                  std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                    FMI::Utils::fmiContext*)> callback) {
    auto state = std::make_shared<IOState>();
    state->setRequest(buf);
    state->context = context;
    state->callbackResult = callback;
    state->operation = Utils::send;  // recv uses send operation for tracking
    recv_object(state, src, mode);
}

void FMI::Comm::PeerToPeer::bcast(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num root) {
    int rounds = ceil(log2(num_peers));
    Utils::peer_num trans_peer_id = transform_peer_id(peer_id, root, true);
    for (int i = rounds - 1; i >= 0; i--) {
        Utils::peer_num rcpt = trans_peer_id + (Utils::peer_num) std::pow(2, i);
        if (trans_peer_id % (int) std::pow(2, i + 1) == 0 && rcpt < num_peers) {
            Utils::peer_num real_rcpt = transform_peer_id(rcpt, root, false);
            send(buf, real_rcpt);
        } else if (trans_peer_id % (int) std::pow(2, i) == 0 && trans_peer_id % (int) std::pow(2, i + 1) != 0){
            Utils::peer_num real_src = transform_peer_id(trans_peer_id - (int) std::pow(2, i), root, false);
            recv(buf, real_src);
        }
    }
}

void FMI::Comm::PeerToPeer::bcast(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num root,
                                   FMI::Utils::Mode mode,
                                   std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                     FMI::Utils::fmiContext*)> callback) {
    // For now, just call blocking version
    bcast(buf, root);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}

void FMI::Comm::PeerToPeer::barrier() {
    auto nop = [] (char* a, char* b) {};
    char send_val = 1;
    auto sendbuf = std::make_shared<channel_data>(&send_val, sizeof(char), noop_deleter);
    auto recvbuf = std::make_shared<channel_data>(&send_val, sizeof(char), noop_deleter);
    allreduce(sendbuf, recvbuf, {nop, true, true});
}

void FMI::Comm::PeerToPeer::reduce(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root, raw_function f) {
    bool left_to_right = !(f.commutative && f.associative);
    if (left_to_right) {
        reduce_ltr(sendbuf, recvbuf, root, f);
    } else {
        reduce_no_order(sendbuf, recvbuf, root, f);
    }
}

void FMI::Comm::PeerToPeer::reduce_ltr(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root, const raw_function& f) {
    if (peer_id == root) {
        std::size_t tmpbuf_len = sendbuf->len * num_peers;
        auto tmpbuf = std::make_shared<channel_data>(tmpbuf_len);
        gather(sendbuf, tmpbuf, root);
        std::memcpy(reinterpret_cast<void*>(recvbuf->get()), tmpbuf->get(), sendbuf->len);
        for (std::size_t i = sendbuf->len; i < tmpbuf_len; i += sendbuf->len) {
            f.f(recvbuf->get(), tmpbuf->get() + i);
        }
    } else {
        gather(sendbuf, std::make_shared<channel_data>(), root);
    }
}

void FMI::Comm::PeerToPeer::reduce_no_order(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root, const raw_function& f) {
    int rounds = ceil(log2(num_peers));
    Utils::peer_num trans_peer_id = transform_peer_id(peer_id, root, true);

    std::shared_ptr<channel_data> local_recvbuf;
    if (peer_id != root) {
        local_recvbuf = std::make_shared<channel_data>(sendbuf->len);
    } else {
        local_recvbuf = recvbuf;
    }

    for (int i = 0; i < rounds; i++) {
        Utils::peer_num src = trans_peer_id + (Utils::peer_num) std::pow(2, i);

        if (trans_peer_id % (int) std::pow(2, i + 1) == 0 && src < num_peers) {
            Utils::peer_num real_src = transform_peer_id(src, root, false);
            recv(local_recvbuf, real_src);
            f.f(sendbuf->get(), local_recvbuf->get());

        } else if (trans_peer_id % (int) std::pow(2, i) == 0 && trans_peer_id % (int) std::pow(2, i + 1) != 0){
            Utils::peer_num real_dst = transform_peer_id(trans_peer_id - (int) std::pow(2, i), root, false);
            send(sendbuf, real_dst);
        }
    }
    if (peer_id == root) {
        std::memcpy(recvbuf->get(), sendbuf->get(), sendbuf->len);
    }
}

void FMI::Comm::PeerToPeer::allreduce(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, raw_function f) {
    bool left_to_right = !(f.commutative && f.associative);
    if (left_to_right) {
        reduce(sendbuf, recvbuf, 0, f);
        bcast(recvbuf, 0);
    } else {
        allreduce_no_order(sendbuf, recvbuf, f);
    }
}

void FMI::Comm::PeerToPeer::allreduce_no_order(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, const raw_function &f) {
    // Non power of two N: First receive from processes with ID >= 2^ceil(log2(N)), send result after reduction
    int rounds = floor(log2(num_peers));
    int nearest_power_two = (int) std::pow(2, rounds);
    if (num_peers > (unsigned int)nearest_power_two) {
        if (peer_id < (unsigned int)nearest_power_two && peer_id + nearest_power_two < num_peers) {
            recv(recvbuf, peer_id + nearest_power_two);
            f.f(sendbuf->get(), recvbuf->get());
        } else if (peer_id >= (unsigned int)nearest_power_two) {
            send(sendbuf, peer_id - nearest_power_two);
        }
    }
    if (peer_id < (unsigned int)nearest_power_two) {
        // Actual recursive doubling
        for (int i = 0; i < rounds; i++) {
            int peer = peer_id ^ (int) std::pow(2, i);
            if (peer < (int)peer_id) {
                send(sendbuf, peer);
                recv(recvbuf, peer);
            } else {
                recv(recvbuf, peer);
                send(sendbuf, peer);
            }
            f.f(sendbuf->get(), recvbuf->get());
        }
    }
    if (num_peers > (unsigned int)nearest_power_two) {
        if (peer_id < (unsigned int)nearest_power_two && peer_id + nearest_power_two < num_peers) {
            send(sendbuf, peer_id + nearest_power_two);
        } else if (peer_id >= (unsigned int)nearest_power_two) {
            recv(sendbuf, peer_id - nearest_power_two);
        }
    }
    std::memcpy(recvbuf->get(), sendbuf->get(), sendbuf->len);
}

void FMI::Comm::PeerToPeer::scan(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, raw_function f) {
    bool left_to_right = !(f.commutative && f.associative);
    if (left_to_right) {
        scan_ltr(sendbuf, recvbuf, f);
    } else {
        scan_no_order(sendbuf, recvbuf, f);
    }
}

void FMI::Comm::PeerToPeer::scan_ltr(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, const raw_function& f) {
    if (peer_id == 0) {
        send(sendbuf, 1);
        std::memcpy(recvbuf->get(), sendbuf->get(), sendbuf->len);
    } else {
        recv(recvbuf, peer_id - 1);
        f.f(recvbuf->get(), sendbuf->get());
        if (peer_id < num_peers - 1) {
            send(recvbuf, peer_id + 1);
        }
    }
}

void FMI::Comm::PeerToPeer::scan_no_order(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, const raw_function& f) {
    int rounds = floor(log2(num_peers));
    for (int i = 0; i < rounds; i ++) {
        if ((peer_id & ((int) std::pow(2, i + 1) - 1)) == (unsigned int)((int) std::pow(2, i + 1) - 1)) {
            Utils::peer_num src = peer_id - (int) std::pow(2, i);
            recv(recvbuf, src);
            f.f(sendbuf->get(), recvbuf->get());
        } else if ((peer_id & ((int) std::pow(2, i) - 1)) == (unsigned int)((int) std::pow(2, i) - 1)) {
            Utils::peer_num dst = peer_id + (int) std::pow(2, i);
            if (dst < num_peers) {
                send(sendbuf, dst);
                break;
            }
        }
    }
    for (int i = rounds; i > 0; i--) {
        if ((peer_id & ((int) std::pow(2, i) - 1)) == (unsigned int)((int) std::pow(2, i) - 1)) {
            Utils::peer_num dst = peer_id + (int) std::pow(2, i - 1);
            if (dst < num_peers) {
                send(sendbuf, dst);
            }
        } else if ((peer_id & ((int) std::pow(2, i - 1) - 1)) == (unsigned int)((int) std::pow(2, i - 1) - 1)) {
            int src = peer_id - (int) std::pow(2, i - 1);
            if (src > 0) {
                recv(recvbuf, src);
                f.f(sendbuf->get(), recvbuf->get());
            }
        }
    }
    std::memcpy(recvbuf->get(), sendbuf->get(), sendbuf->len);
}

void FMI::Comm::PeerToPeer::gather(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root) {
    int rounds = ceil(log2(num_peers));
    Utils::peer_num trans_peer_id = transform_peer_id(peer_id, root, true);
    std::size_t single_buffer_size = sendbuf->len;

    // Find needed buffer size and allocate it
    std::shared_ptr<channel_data> local_recvbuf;
    if (peer_id != root) {
        unsigned int peers_in_buffer = 1;
        for (int i = rounds - 1; i >= 0; i--) {
            Utils::peer_num src = trans_peer_id + (Utils::peer_num) std::pow(2, i);
            if (trans_peer_id % (int) std::pow(2, i + 1) == 0 && src < num_peers) {
                peers_in_buffer += std::min((Utils::peer_num) std::pow(2, i), num_peers - src);
            }
        }
        local_recvbuf = std::make_shared<channel_data>(peers_in_buffer * single_buffer_size);
        std::memcpy(local_recvbuf->get(), sendbuf->get(), single_buffer_size);
    } else {
        local_recvbuf = recvbuf;
        std::memcpy(recvbuf->get() + single_buffer_size * root, sendbuf->get(), single_buffer_size);
    }

    for (int i = 0; i < rounds; i++) {
        Utils::peer_num src = trans_peer_id + (Utils::peer_num) std::pow(2, i);

        if (trans_peer_id % (int) std::pow(2, i + 1) == 0 && src < num_peers) {
            unsigned int responsible_peers = std::min((Utils::peer_num) std::pow(2, i), num_peers - src);
            std::size_t buf_len = responsible_peers * single_buffer_size;
            Utils::peer_num real_src = transform_peer_id(src, root, false);

            if (peer_id == root) {
                if (real_src * single_buffer_size + buf_len > recvbuf->len) {
                    // Need to wraparound with temporary buffer
                    auto tmp = std::make_shared<channel_data>(buf_len);
                    recv(tmp, real_src);
                    unsigned int length_end = recvbuf->len - real_src * single_buffer_size; // How many bytes to copy at end of buffer
                    std::memcpy(recvbuf->get() + real_src * single_buffer_size, tmp->get(), length_end);
                    std::memcpy(recvbuf->get(), tmp->get() + length_end, buf_len - length_end);
                } else {
                    auto peer_buf = std::make_shared<channel_data>(recvbuf->get() + real_src * single_buffer_size, buf_len, noop_deleter);
                    recv(peer_buf, real_src);
                }
            } else {
                auto peer_buf = std::make_shared<channel_data>(local_recvbuf->get() + (src - trans_peer_id) * single_buffer_size, buf_len, noop_deleter);
                recv(peer_buf, real_src);
            }
        } else if (trans_peer_id % (int) std::pow(2, i) == 0 && trans_peer_id % (int) std::pow(2, i + 1) != 0){
            unsigned int responsible_peers = std::min((Utils::peer_num) std::pow(2, i), num_peers - trans_peer_id);
            std::size_t buf_len = responsible_peers * single_buffer_size;
            Utils::peer_num real_dst = transform_peer_id(trans_peer_id - (int) std::pow(2, i), root, false);
            auto send_buf = std::make_shared<channel_data>(local_recvbuf->get(), buf_len, noop_deleter);
            send(send_buf, real_dst);
        }
    }
}

void FMI::Comm::PeerToPeer::gatherv(std::shared_ptr<channel_data> sendbuf,
                                     std::shared_ptr<channel_data> recvbuf,
                                     FMI::Utils::peer_num root,
                                     const std::vector<int32_t>& recvcounts,
                                     const std::vector<int32_t>& displs) {
    // Simple implementation: each non-root peer sends to root
    if (peer_id != root) {
        send(sendbuf, root);
    } else {
        // Copy own data
        std::memcpy(recvbuf->get() + displs[root], sendbuf->get(), recvcounts[root]);
        // Receive from others
        for (unsigned int i = 0; i < num_peers; i++) {
            if (i != root) {
                auto peer_buf = std::make_shared<channel_data>(recvbuf->get() + displs[i], recvcounts[i], noop_deleter);
                recv(peer_buf, i);
            }
        }
    }
}

void FMI::Comm::PeerToPeer::gatherv(std::shared_ptr<channel_data> sendbuf,
                                     std::shared_ptr<channel_data> recvbuf,
                                     FMI::Utils::peer_num root,
                                     const std::vector<int32_t>& recvcounts,
                                     const std::vector<int32_t>& displs,
                                     FMI::Utils::Mode mode,
                                     std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                       FMI::Utils::fmiContext*)> callback) {
    // For now, just call blocking version
    gatherv(sendbuf, recvbuf, root, recvcounts, displs);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}

void FMI::Comm::PeerToPeer::allgather(std::shared_ptr<channel_data> sendbuf,
                                       std::shared_ptr<channel_data> recvbuf,
                                       FMI::Utils::peer_num root) {
    // Two-phase: gather + bcast
    gather(sendbuf, recvbuf, root);
    bcast(recvbuf, root);
}

void FMI::Comm::PeerToPeer::allgather(std::shared_ptr<channel_data> sendbuf,
                                       std::shared_ptr<channel_data> recvbuf,
                                       FMI::Utils::peer_num root, FMI::Utils::Mode mode,
                                       std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                         FMI::Utils::fmiContext*)> callback) {
    // For now, just call blocking version
    allgather(sendbuf, recvbuf, root);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}

void FMI::Comm::PeerToPeer::allgatherv(std::shared_ptr<channel_data> sendbuf,
                                        std::shared_ptr<channel_data> recvbuf,
                                        FMI::Utils::peer_num root,
                                        const std::vector<int32_t>& recvcounts,
                                        const std::vector<int32_t>& displs) {
    // Two-phase: gatherv + bcast
    gatherv(sendbuf, recvbuf, root, recvcounts, displs);
    bcast(recvbuf, root);
}

void FMI::Comm::PeerToPeer::allgatherv(std::shared_ptr<channel_data> sendbuf,
                                        std::shared_ptr<channel_data> recvbuf,
                                        FMI::Utils::peer_num root,
                                        const std::vector<int32_t>& recvcounts,
                                        const std::vector<int32_t>& displs,
                                        FMI::Utils::Mode mode,
                                        std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                                          FMI::Utils::fmiContext*)> callback) {
    // For now, just call blocking version
    allgatherv(sendbuf, recvbuf, root, recvcounts, displs);
    if (callback) {
        callback(Utils::SUCCESS, "", nullptr);
    }
}

void FMI::Comm::PeerToPeer::scatter(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root) {
    int rounds = ceil(log2(num_peers));
    Utils::peer_num trans_peer_id = transform_peer_id(peer_id, root, true);
    std::size_t single_buffer_size = recvbuf->len;

    std::shared_ptr<channel_data> local_sendbuf = sendbuf;

    for (int i = rounds - 1; i >= 0; i--) {
        Utils::peer_num rcpt = trans_peer_id + (Utils::peer_num) std::pow(2, i);

        if (trans_peer_id % (int) std::pow(2, i + 1) == 0 && rcpt < num_peers) {
            unsigned int responsible_peers = std::min((Utils::peer_num) std::pow(2, i), num_peers - rcpt);
            std::size_t buf_len = responsible_peers * single_buffer_size;
            Utils::peer_num real_rcpt = transform_peer_id(rcpt, root, false);

            if (peer_id == root) {
                if (real_rcpt * single_buffer_size + buf_len > sendbuf->len) {
                    // Wrapping around, need to allocate a temporary buffer
                    auto tmp = std::make_shared<channel_data>(buf_len);
                    unsigned int length_end = sendbuf->len - real_rcpt * single_buffer_size; // How many bytes we need to send at end of buffer
                    std::memcpy(tmp->get(), sendbuf->get() + real_rcpt * single_buffer_size, length_end);
                    // Copy rest from beginning
                    std::memcpy(tmp->get() + length_end, sendbuf->get(), buf_len - length_end);
                    send(tmp, real_rcpt);
                } else {
                    auto send_buf = std::make_shared<channel_data>(sendbuf->get() + real_rcpt * single_buffer_size, buf_len, noop_deleter);
                    send(send_buf, real_rcpt);
                }
            } else {
                auto send_buf = std::make_shared<channel_data>(local_sendbuf->get() + (rcpt - trans_peer_id) * single_buffer_size, buf_len, noop_deleter);
                send(send_buf, real_rcpt);
            }
        } else if (trans_peer_id % (int) std::pow(2, i) == 0 && trans_peer_id % (int) std::pow(2, i + 1) != 0){
            unsigned int responsible_peers = std::min((Utils::peer_num) std::pow(2, i), num_peers - trans_peer_id);
            std::size_t buf_len = responsible_peers * single_buffer_size;
            Utils::peer_num real_src = transform_peer_id(trans_peer_id - (int) std::pow(2, i), root, false);
            local_sendbuf = std::make_shared<channel_data>(buf_len);
            recv(local_sendbuf, real_src);
        }
    }
    if (peer_id == root) {
        std::memcpy(recvbuf->get(), sendbuf->get() + peer_id * single_buffer_size, single_buffer_size);
    } else {
        std::memcpy(recvbuf->get(), local_sendbuf->get(), single_buffer_size);
    }
}

FMI::Utils::EventProcessStatus FMI::Comm::PeerToPeer::channel_event_progress(FMI::Utils::Operation op) {
    // Default implementation - subclasses override for actual non-blocking support
    return Utils::NOOP;
}

FMI::Utils::peer_num FMI::Comm::PeerToPeer::transform_peer_id(FMI::Utils::peer_num id, FMI::Utils::peer_num root, bool forward) {
    if (forward) {
        return (id + num_peers - root) % num_peers; // Transform s.t. root has id 0
    } else {
        return (id + root) % num_peers;
    }
}

double FMI::Comm::PeerToPeer::get_operation_latency(FMI::Utils::OperationInfo op_info) {
    std::size_t size_in_bytes = op_info.data_size;
    switch (op_info.op) {
        case Utils::send:
            return get_latency(1, 1, size_in_bytes);
        case Utils::bcast:
            return ceil(log2(num_peers)) * get_latency(1, 1, size_in_bytes);
        case Utils::reduce:
            if (!op_info.left_to_right) {
                return ceil(log2(num_peers)) * get_latency(1, 1, size_in_bytes);
            } // else, gather used
        case Utils::gather:
        case Utils::gatherv:
        case Utils::scatter:
        {
            // ceil(log2(num_peers)) rounds, doubling buffer size in each round
            double latency = 0.;
            for (int i = 1; i <= floor(log2(num_peers)); i++) {
                latency += get_latency(1, 1, i * size_in_bytes);
            }
            // Different buffer sizes for non power of two
            int rem_nodes = num_peers - (int) std::pow(2, floor(log2(num_peers)));
            latency += get_latency(1, 1, rem_nodes * size_in_bytes);
            return latency;
        }
        case Utils::allgather:
        case Utils::allgatherv:
        {
            // gather + bcast
            double latency = 0.;
            for (int i = 1; i <= floor(log2(num_peers)); i++) {
                latency += get_latency(1, 1, i * size_in_bytes);
            }
            int rem_nodes = num_peers - (int) std::pow(2, floor(log2(num_peers)));
            latency += get_latency(1, 1, rem_nodes * size_in_bytes);
            latency += ceil(log2(num_peers)) * get_latency(1, 1, num_peers * size_in_bytes);
            return latency;
        }
        case Utils::barrier:
            size_in_bytes = 1;
        case Utils::allreduce:
            if (op_info.left_to_right) {
                // Reduce with gather, followed by bcast
                double latency = 0.;
                for (int i = 1; i <= floor(log2(num_peers)); i++) {
                    latency += get_latency(1, 1, i * size_in_bytes);
                }
                int rem_nodes = num_peers - (int) std::pow(2, floor(log2(num_peers)));
                latency += get_latency(1, 1, rem_nodes * size_in_bytes);
                latency += ceil(log2(num_peers)) * get_latency(1, 1, size_in_bytes); // bcast
                return latency;
            } else {
                // Send and recv in every round
                double latency = 2 * floor(log2(num_peers)) * get_latency(1, 1, size_in_bytes);
                if (floor(log2(num_peers)) != num_peers) {
                    // 2 additional rounds in beginning / end with only send / receive
                    latency += 2 * get_latency(1, 1, size_in_bytes);
                }
                return latency;
            }
        case Utils::scan:
            if (op_info.left_to_right) {
                return (num_peers - 1) * get_latency(1, 1, size_in_bytes);
            } else {
                return 2 * floor(log2(num_peers)) * get_latency(1, 1, size_in_bytes);
            }

    }
    throw std::runtime_error("Operation not implemented");

}

double FMI::Comm::PeerToPeer::get_operation_price(FMI::Utils::OperationInfo op_info) {
    std::size_t size_in_bytes = op_info.data_size;
    switch (op_info.op) {
        case Utils::send:
            return get_price(1, 1, size_in_bytes);
        case Utils::bcast:
        {
            double comm_rounds = num_peers - 1;
            return comm_rounds * get_price(1, 1, size_in_bytes);
        }
        case Utils::reduce:
            if (!op_info.left_to_right) {
                double comm_rounds = num_peers - 1;
                return comm_rounds * get_price(1, 1, size_in_bytes);
            } // else, gather used
        case Utils::gather:
        case Utils::gatherv:
        case Utils::scatter:
        {
            double costs = 0.;
            for (int i = 1; i <= ceil(log2(num_peers)); i++) {
                // Fast overapproximation for non power of two
                costs += std::pow(2, floor(log2(num_peers)) - i) * get_price(1, 1, i * size_in_bytes);
            }
            return costs;
        }
        case Utils::allgather:
        case Utils::allgatherv:
        {
            // gather + bcast
            double costs = 0.;
            for (int i = 1; i <= ceil(log2(num_peers)); i++) {
                costs += std::pow(2, floor(log2(num_peers)) - i) * get_price(1, 1, i * size_in_bytes);
            }
            double comm_rounds = num_peers - 1;
            costs += comm_rounds * get_price(1, 1, num_peers * size_in_bytes);
            return costs;
        }
        case Utils::barrier:
        size_in_bytes = 1;
        case Utils::allreduce:
        if (op_info.left_to_right) {
            // Reduce with gather, followed by bcast
            double costs = 0.;
            for (int i = 1; i <= ceil(log2(num_peers)); i++) {
                // Overapproximation for non power of two
                costs += std::pow(2, floor(log2(num_peers)) - i) * get_price(1, 1, i * size_in_bytes);
            }
            double comm_rounds = num_peers - 1;
            costs += comm_rounds * get_price(1, 1, size_in_bytes);
            return costs;
        } else {
            // Send and recv in every round
            double comm_rounds = 2 * (num_peers - 1);
            return comm_rounds * get_price(1, 1, size_in_bytes);
        }
        case Utils::scan:
            if (op_info.left_to_right) {
                return (num_peers - 1) * get_price(1, 1, size_in_bytes);
            } else {
                // Binomial tree, bounded by 2 * num_peers: https://link.springer.com/content/pdf/10.1007%2F11846802.pdf
                return 2 * num_peers * get_price(1, 1, size_in_bytes);
            }

    }
    throw std::runtime_error("Operation not implemented");
}