#include "../../include/comm/Direct.h"
#include <tcpunch.h>
#include <sys/socket.h>
#include <boost/log/trivial.hpp>
#include <thread>
#include <netinet/tcp.h>
#include <cmath>
#include <iostream>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <memory>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

FMI::Comm::Direct::Direct(std::map<std::string, std::string> params, std::map<std::string, std::string> model_params) {
    struct addrinfo hints, *res, *p;
    int status;
    char ipstr[INET6_ADDRSTRLEN];

    hostname = params["host"];
    port = std::stoi(params["port"]);
    if (model_params["resolve_host_dns"] == "true") {

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if ((status = getaddrinfo(hostname.c_str(), nullptr, &hints, &res)) != 0) {
            std::cerr << "getaddrinfo error: " << gai_strerror(status) << std::endl;
        } else {
            // Iterate through the result list and convert each address to a string
            for(p = res; p != nullptr; p = p->ai_next) {
                void *addr;

                // Get the pointer to the address itself,
                struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
                addr = &(ipv4->sin_addr);

                // Convert the IP to a string and print it:
                inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
                std::cout << " resolved dns: " << ipstr << std::endl;
            }

            freeaddrinfo(res); // Free the linked list
            hostname = ipstr;

        }

      resolve_host_dns = true;
    } else {
       resolve_host_dns = false;
    }

    max_timeout = std::stoi(params["max_timeout"]);
    std::cout << "max_timeout set to: " << max_timeout << std::endl;
    bandwidth = std::stod(model_params["bandwidth"]);

    overhead = std::stod(model_params["overhead"]);

    transfer_price = std::stod(model_params["transfer_price"]);

    vm_price = std::stod(model_params["vm_price"]);

    requests_per_hour = std::stoi(model_params["requests_per_hour"]);


    if (model_params["include_infrastructure_costs"] == "true") {
        include_infrastructure_costs = true;
    } else {
        include_infrastructure_costs = false;
    }
}

FMI::Comm::Direct::~Direct() {
    // Close all sockets
    for (auto& [mode_key, socket_vec] : sockets) {
        for (int sock : socket_vec) {
            if (sock != -1) {
                close(sock);
            }
        }
    }
}

void FMI::Comm::Direct::init() {
    // Initialize non-blocking socket vectors if needed
    if (sockets.find(Utils::NONBLOCKING) == sockets.end()) {
        sockets[Utils::NONBLOCKING] = std::vector<int>(num_peers, -1);
    }
}

int FMI::Comm::Direct::getMaxTimeout() {
    return max_timeout;
}

void FMI::Comm::Direct::send_object(std::shared_ptr<channel_data> buf, Utils::peer_num rcpt_id) {
    check_socket(rcpt_id, comm_name + std::to_string(peer_id) + "_" + std::to_string(rcpt_id));
    long sent = ::send(sockets[Utils::BLOCKING][rcpt_id], buf->get(), buf->len, 0);
    if (sent == -1) {
        if (errno == EAGAIN) {
            throw Utils::Timeout();
        }
        BOOST_LOG_TRIVIAL(error) << peer_id << ": Error when sending: " << strerror(errno) ;
    }
}

void FMI::Comm::Direct::recv_object(std::shared_ptr<channel_data> buf, Utils::peer_num sender_id) {
    check_socket(sender_id, comm_name + std::to_string(sender_id) + "_" + std::to_string(peer_id));
    long received = ::recv(sockets[Utils::BLOCKING][sender_id], buf->get(), buf->len, MSG_WAITALL);
    if (received == -1 || received < (long)buf->len) {
        if (errno == EAGAIN) {
            throw Utils::Timeout();
        }
        BOOST_LOG_TRIVIAL(error) << peer_id << ": Error when receiving: " << strerror(errno);
    }
}

void FMI::Comm::Direct::send_object(std::shared_ptr<IOState> state, Utils::peer_num rcpt_id, Utils::Mode mode) {
    if (mode == Utils::BLOCKING) {
        send_object(state->request, rcpt_id);
        if (state->callbackResult) {
            state->callbackResult(Utils::SUCCESS, "", state->context);
        }
        return;
    }

    // Non-blocking mode
    std::string pair_name = get_pairing_name(peer_id, rcpt_id, mode);
    check_socket_nbx(rcpt_id, pair_name);

    int sock = sockets[Utils::NONBLOCKING][rcpt_id];
    state->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_timeout);

    // Try to send what we can
    ssize_t sent = ::send(sock, state->request->get() + state->processed,
                          state->request->len - state->processed, MSG_DONTWAIT);

    if (sent > 0) {
        state->processed += sent;
    }

    if (state->processed >= state->request->len) {
        // Complete
        if (state->callbackResult) {
            state->callbackResult(Utils::SUCCESS, "", state->context);
        }
    } else {
        // Register for progress
        io_states[Utils::send][sock] = state;
    }
}

void FMI::Comm::Direct::recv_object(std::shared_ptr<IOState> state, Utils::peer_num sender_id, Utils::Mode mode) {
    if (mode == Utils::BLOCKING) {
        recv_object(state->request, sender_id);
        if (state->callbackResult) {
            state->callbackResult(Utils::SUCCESS, "", state->context);
        }
        return;
    }

    // Non-blocking mode
    std::string pair_name = get_pairing_name(sender_id, peer_id, mode);
    check_socket_nbx(sender_id, pair_name);

    int sock = sockets[Utils::NONBLOCKING][sender_id];
    state->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_timeout);

    // Try to receive what we can
    ssize_t recvd = ::recv(sock, state->request->get() + state->processed,
                           state->request->len - state->processed, MSG_DONTWAIT);

    if (recvd > 0) {
        state->processed += recvd;
    }

    if (state->processed >= state->request->len) {
        // Complete
        if (state->callbackResult) {
            state->callbackResult(Utils::SUCCESS, "", state->context);
        }
    } else {
        // Register for progress
        io_states[Utils::recv][sock] = state;
    }
}

FMI::Utils::EventProcessStatus FMI::Comm::Direct::channel_event_progress(Utils::Operation op) {
    if (io_states.find(op) == io_states.end() || io_states[op].empty()) {
        return Utils::EMPTY;
    }

    auto& states = io_states[op];
    bool any_processed = false;

    for (auto it = states.begin(); it != states.end(); ) {
        int sock = it->first;
        auto& state = it->second;

        // Check timeout
        if (std::chrono::steady_clock::now() > state->deadline) {
            if (state->callbackResult) {
                state->callbackResult(Utils::NBX_TIMEOUT, "Operation timed out", state->context);
            }
            it = states.erase(it);
            continue;
        }

        it = handle_event(it, states, op);
        any_processed = true;
    }

    return any_processed ? Utils::PROCESSING : Utils::NOOP;
}

std::unordered_map<int, std::shared_ptr<FMI::Comm::IOState>>::iterator
FMI::Comm::Direct::handle_event(std::unordered_map<int, std::shared_ptr<FMI::Comm::IOState>>::iterator it,
                                 std::unordered_map<int, std::shared_ptr<FMI::Comm::IOState>>& states,
                                 Utils::Operation op) {
    if (it == states.end()) {
        return it;
    }

    int socketfd = it->first;
    auto& state = it->second;

    // Use poll to check readiness
    struct pollfd pfd;
    pfd.fd = socketfd;
    pfd.events = (op == Utils::send) ? POLLOUT : POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 0);  // Non-blocking poll
    if (ret <= 0) {
        return ++it;
    }

    if (pfd.revents & POLLOUT) {
        // Ready for writing
        ssize_t sent = ::send(socketfd, state->request->get() + state->processed,
                              state->request->len - state->processed, MSG_DONTWAIT);
        if (sent > 0) {
            state->processed += sent;
        }
    }

    if (pfd.revents & POLLIN) {
        // Ready for reading
        ssize_t recvd = ::recv(socketfd, state->request->get() + state->processed,
                               state->request->len - state->processed, MSG_DONTWAIT);
        if (recvd > 0) {
            state->processed += recvd;
        } else if (recvd == 0) {
            // Connection closed
            if (state->callbackResult) {
                state->callbackResult(Utils::CONNECTION_CLOSED_BY_PEER, "Connection closed", state->context);
            }
            return states.erase(it);
        }
    }

    // Check if complete
    if (state->processed >= state->request->len) {
        if (state->callbackResult) {
            state->callbackResult(Utils::SUCCESS, "", state->context);
        }
        if (state->callback) {
            state->callback();
        }
        return states.erase(it);
    }
    return ++it;
}

void FMI::Comm::Direct::check_socket(FMI::Utils::peer_num partner_id, std::string pair_name) {
    if (sockets.find(Utils::BLOCKING) == sockets.end() || sockets[Utils::BLOCKING].empty()) {
        sockets[Utils::BLOCKING] = std::vector<int>(num_peers, -1);
    }
    if (sockets[Utils::BLOCKING][partner_id] == -1) {
        try {
            sockets[Utils::BLOCKING][partner_id] = pair(pair_name, hostname, port, max_timeout);
        } catch (Timeout) {
            throw Utils::Timeout();
        }

        struct timeval timeout;
        timeout.tv_sec = max_timeout / 1000;
        timeout.tv_usec = (max_timeout % 1000) * 1000;
        setsockopt(sockets[Utils::BLOCKING][partner_id], SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
        setsockopt(sockets[Utils::BLOCKING][partner_id], SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof timeout);
        // Disable Nagle algorithm to avoid 40ms TCP ack delays
        int one = 1;
        // SOL_TCP not defined on macOS
        #if !defined(SOL_TCP) && defined(IPPROTO_TCP)
        #define SOL_TCP IPPROTO_TCP
        #endif
        setsockopt(sockets[Utils::BLOCKING][partner_id], SOL_TCP, TCP_NODELAY, &one, sizeof(one));
    }
}

void FMI::Comm::Direct::check_socket_nbx(FMI::Utils::peer_num partner_id, std::string pair_name) {
    if (sockets.find(Utils::NONBLOCKING) == sockets.end() || sockets[Utils::NONBLOCKING].empty()) {
        sockets[Utils::NONBLOCKING] = std::vector<int>(num_peers, -1);
    }
    if (sockets[Utils::NONBLOCKING][partner_id] == -1) {
        try {
            sockets[Utils::NONBLOCKING][partner_id] = pair(pair_name, hostname, port, max_timeout);
        } catch (Timeout) {
            throw Utils::Timeout();
        }

        int sock = sockets[Utils::NONBLOCKING][partner_id];

        // Set non-blocking mode
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags == -1) {
            BOOST_LOG_TRIVIAL(error) << "Failed to get socket flags";
        }
        if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
            BOOST_LOG_TRIVIAL(error) << "Failed to set non-blocking mode";
        }

        // Disable Nagle algorithm
        int one = 1;
        #if !defined(SOL_TCP) && defined(IPPROTO_TCP)
        #define SOL_TCP IPPROTO_TCP
        #endif
        setsockopt(sock, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
    }
}

std::string FMI::Comm::Direct::get_pairing_name(Utils::peer_num a, Utils::peer_num b, Utils::Mode mode) {
    std::string mode_suffix = (mode == Utils::NONBLOCKING) ? "_nbx" : "";
    return comm_name + std::to_string(a) + "_" + std::to_string(b) + mode_suffix;
}

double FMI::Comm::Direct::get_latency(Utils::peer_num producer, Utils::peer_num consumer, std::size_t size_in_bytes) {
    double agg_bandwidth = bandwidth;
    double trans_time = producer * consumer * ((double) size_in_bytes / 1000000.) / agg_bandwidth;
    return log2(producer + consumer) * overhead + trans_time;
}

double FMI::Comm::Direct::get_price(Utils::peer_num producer, Utils::peer_num consumer, std::size_t size_in_bytes) {
    double transfer_costs = 2 * consumer * producer * ((double) size_in_bytes / 1000000000.) * transfer_price;
    double total_costs = transfer_costs;
    if (include_infrastructure_costs) {
        total_costs += 1. / requests_per_hour * vm_price;
    }
    return total_costs;
}