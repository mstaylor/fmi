#ifndef FMI_COMMUNICATOR_H
#define FMI_COMMUNICATOR_H

#include "./utils/Configuration.h"
#include "comm/Channel.h"
#include "utils/ChannelPolicy.h"

namespace FMI {
    //! Interface that is exposed to the user for interaction with the FMI system.
    class Communicator {
    public:
        /*!
         * @param peer_id ID of the peer in the range [0 .. num_peers - 1]
         * @param num_peers Number of peers participating in the communicator
         * @param config_path Path to the FMI JSON configuration file
         * @param comm_name Name of the communicator, needs to be unique when multiple communicators are used concurrently
         * @param faas_memory Amount of memory (in MiB) that is allocated to the function, used for performance model calculations.
         */
        Communicator(FMI::Utils::peer_num peer_id, FMI::Utils::peer_num num_peers, std::string config_path, std::string comm_name,
                     unsigned int faas_memory = 128);

        //! Finalizes all active channels
        ~Communicator();

        //! Send buf to peer dest
        template<typename T>
        void send(Comm::Data<T> &buf, FMI::Utils::peer_num dest) {
            std::string channel = policy->get_channel({Utils::send, buf.size_in_bytes()});
            auto data = std::make_shared<channel_data>(buf.data(), buf.size_in_bytes(), noop_deleter);
            channels[channel]->send(data, dest);
        }

        //! Non-blocking send buf to peer dest
        template<typename T>
        void send(Comm::Data<T> &buf, FMI::Utils::peer_num dest,
                  FMI::Utils::fmiContext* context, FMI::Utils::Mode mode,
                  std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                    FMI::Utils::fmiContext*)> callback) {
            std::string channel = policy->get_channel({Utils::send, buf.size_in_bytes()});
            auto data = std::make_shared<channel_data>(buf.data(), buf.size_in_bytes(), noop_deleter);
            channels[channel]->send(data, dest, context, mode, callback);
        }

        //! Receive data from src and store data into the provided buf
        template<typename T>
        void recv(Comm::Data<T> &buf, FMI::Utils::peer_num src) {
            std::string channel = policy->get_channel({Utils::send, buf.size_in_bytes()});
            auto data = std::make_shared<channel_data>(buf.data(), buf.size_in_bytes(), noop_deleter);
            channels[channel]->recv(data, src);
        }

        //! Non-blocking receive data from src
        template<typename T>
        void recv(Comm::Data<T> &buf, FMI::Utils::peer_num src,
                  FMI::Utils::fmiContext* context, FMI::Utils::Mode mode,
                  std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                    FMI::Utils::fmiContext*)> callback) {
            std::string channel = policy->get_channel({Utils::send, buf.size_in_bytes()});
            auto data = std::make_shared<channel_data>(buf.data(), buf.size_in_bytes(), noop_deleter);
            channels[channel]->recv(data, src, context, mode, callback);
        }

        //! Broadcast the data that is in the provided buf of the root peer. Result is stored in buf for all peers.
        template<typename T>
        void bcast(Comm::Data<T> &buf, FMI::Utils::peer_num root) {
            std::string channel = policy->get_channel({Utils::bcast, buf.size_in_bytes()});
            auto data = std::make_shared<channel_data>(buf.data(), buf.size_in_bytes(), noop_deleter);
            channels[channel]->bcast(data, root);
        }

        //! Barrier synchronization collective
        void barrier() {
            std::string channel = policy->get_channel({Utils::barrier, 0});
            channels[channel]->barrier();
        }

        //! Gather the data of the individuals peers (in sendbuf) into the recvbuf of root.
        /*!
         * @param sendbuf Data to send to root, needs to be the same size for all peers.
         * @param recvbuf Receive buffer, only relevant for the root process. Size needs to be num_peers * sendbuf.size
         */
        template<typename T>
        void gather(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::peer_num root) {
            std::string channel = policy->get_channel({Utils::gather, sendbuf.size_in_bytes()});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            channels[channel]->gather(senddata, recvdata, root);
        }

        //! Variable-length gather
        template<typename T>
        void gatherv(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::peer_num root,
                     const std::vector<int32_t>& recvcounts, const std::vector<int32_t>& displs) {
            std::string channel = policy->get_channel({Utils::gatherv, sendbuf.size_in_bytes()});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            channels[channel]->gatherv(senddata, recvdata, root, recvcounts, displs);
        }

        //! All-gather - gather data and distribute to all peers
        template<typename T>
        void allgather(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::peer_num root) {
            std::string channel = policy->get_channel({Utils::allgather, sendbuf.size_in_bytes()});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            channels[channel]->allgather(senddata, recvdata, root);
        }

        //! Variable-length all-gather
        template<typename T>
        void allgatherv(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::peer_num root,
                        const std::vector<int32_t>& recvcounts, const std::vector<int32_t>& displs) {
            std::string channel = policy->get_channel({Utils::allgatherv, sendbuf.size_in_bytes()});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            channels[channel]->allgatherv(senddata, recvdata, root, recvcounts, displs);
        }

        //! Scatter the data from root's sendbuf to the recvbuf of all peers.
        /*!
         * @param sendbuf The data to scatter, size needs to be recvbuf.size * num_peers (i.e., divisible by the number of peers). Only relevant for the root peer.
         * @param recvbuf Buffer to receive the data, relevant for all peers.
         */
        template<typename T>
        void scatter(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::peer_num root) {
            std::string channel = policy->get_channel({Utils::scatter, recvbuf.size_in_bytes()});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            channels[channel]->scatter(senddata, recvdata, root);
        }

        //! Perform a reduction with the reduction function f.
        /*! Depending on the associativity / commutativity of f, a different implementation for the reduction may be used.
         * However, in the same topology, the evaluation order should always be the same, irrespectively of the associativity / commutativitiy.
         * @param sendbuf Data to send, relevant for all peers.
         * @param recvbuf Receive buffer that contains the final result, only relevant for root. Needs to have the same size as the sendbuf.
         */
        template <typename T>
        void reduce(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::peer_num root, FMI::Utils::Function<T> f) {
            if (peer_id == root && sendbuf.size_in_bytes() != recvbuf.size_in_bytes()) {
                throw std::runtime_error("Dimensions of send and receive data must match");
            }
            bool left_to_right = !(f.commutative && f.associative);
            std::string channel = policy->get_channel({Utils::reduce, sendbuf.size_in_bytes(), left_to_right});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            auto func = convert_to_raw_function(f, sendbuf.size_in_bytes());
            raw_function raw_f {
                func,
                f.associative,
                f.commutative
            };
            channels[channel]->reduce(senddata, recvdata, root, raw_f);
        }

        //! Perform a reduction with the reduction function f and make the result available to all peers.
        /*! Depending on the associativity / commutativity of f, a different implementation for the reduction may be used.
         * However, in the same topology, the evaluation order should always be the same, irrespectively of the associativity / commutativitiy.
         * @param sendbuf Data to send, relevant for all peers.
         * @param recvbuf Receive buffer that contains the final result, relevant for all peers. Needs to have the same size as the sendbuf.
         */
        template <typename T>
        void allreduce(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::Function<T> f) {
            if (sendbuf.size_in_bytes() != recvbuf.size_in_bytes()) {
                throw std::runtime_error("Dimensions of send and receive data must match");
            }
            bool left_to_right = !(f.commutative && f.associative);
            std::string channel = policy->get_channel({Utils::allreduce, sendbuf.size_in_bytes(), left_to_right});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            auto func = convert_to_raw_function(f, sendbuf.size_in_bytes());
            raw_function raw_f {
                func,
                f.associative,
                f.commutative
            };
            channels[channel]->allreduce(senddata, recvdata, raw_f);
        }

        //! Inclusive prefix scan.
        /*! Depending on the associativity / commutativity of f, a different implementation for the reduction may be used.
         * However, in the same topology, the evaluation order should always be the same, irrespectively of the associativity / commutativitiy.
         * @param sendbuf Data to send, relevant for all peers.
         * @param recvbuf Receive buffer that contains the final result, relevant for all peers. Needs to have the same size as the sendbuf.
         */
        template<typename T>
        void scan(Comm::Data<T> &sendbuf, Comm::Data<T> &recvbuf, FMI::Utils::Function<T> f) {
            if (sendbuf.size_in_bytes() != recvbuf.size_in_bytes()) {
                throw std::runtime_error("Dimensions of send and receive data must match");
            }
            std::string channel = policy->get_channel({Utils::scan, sendbuf.size_in_bytes()});
            auto senddata = std::make_shared<channel_data>(sendbuf.data(), sendbuf.size_in_bytes(), noop_deleter);
            auto recvdata = std::make_shared<channel_data>(recvbuf.data(), recvbuf.size_in_bytes(), noop_deleter);
            auto func = convert_to_raw_function(f, sendbuf.size_in_bytes());
            raw_function raw_f {
                func,
                f.associative,
                f.commutative
            };
            channels[channel]->scan(senddata, recvdata, raw_f);
        }

        //! Progress function for polling non-blocking completion
        FMI::Utils::EventProcessStatus progress(FMI::Utils::Operation op = FMI::Utils::send) {
            FMI::Utils::EventProcessStatus status = FMI::Utils::EMPTY;
            for (auto& [name, channel] : channels) {
                auto channel_status = channel->channel_event_progress(op);
                if (channel_status == FMI::Utils::PROCESSING) {
                    status = FMI::Utils::PROCESSING;
                } else if (channel_status == FMI::Utils::NOOP && status == FMI::Utils::EMPTY) {
                    status = FMI::Utils::NOOP;
                }
            }
            return status;
        }

        //! Add a new channel to the communicator with the given name by providing a pointer to it.
        void register_channel(std::string name, std::shared_ptr<FMI::Comm::Channel>);

        //! Change the channel policy the communicator is using.
        void set_channel_policy(std::shared_ptr<FMI::Utils::ChannelPolicy> policy);

        //! Set the hint (optimization objective) of the channel selection procedure.
        void hint(FMI::Utils::Hint hint);

    private:
        std::shared_ptr<FMI::Utils::ChannelPolicy> policy;
        std::map<std::string, std::shared_ptr<FMI::Comm::Channel>> channels;
        FMI::Utils::peer_num peer_id;
        FMI::Utils::peer_num num_peers;
        std::string comm_name;
        FMI::Utils::Hint channel_hint = FMI::Utils::Hint::cheap;

        //! Helper utility to convert a typed function to a raw function without type information.
        template <typename T>
        raw_func convert_to_raw_function(FMI::Utils::Function<T> f, std::size_t size_in_bytes) {
            auto func = [f](char* a, char* b) -> void {
                T* dest = reinterpret_cast<T*>(a);
                *dest = f(*((T*) a), *((T*) b));
            };
            return func;
        }

        //! Helper utility to convert a vector function to a raw function that operates directly on memory pointers.
        template <typename A>
        raw_func convert_to_raw_function(FMI::Utils::Function<std::vector<A>> f, std::size_t size_in_bytes) {
            auto func = [f, size_in_bytes](char* a, char* b) -> void {
                std::vector<A> vec_a((A*) a, (A*) (a + size_in_bytes));
                std::vector<A> vec_b((A*) b, (A*) (b + size_in_bytes));
                std::vector<A> res = f(vec_a, vec_b);
                std::memcpy(a, (char*) res.data(), size_in_bytes);
            };
            return func;
        }
    };
}



#endif //FMI_COMMUNICATOR_H