#ifndef FMI_PEERTOPEER_H
#define FMI_PEERTOPEER_H

#include "Channel.h"
#include <chrono>
#include <unordered_map>

namespace FMI::Comm {

    //! Helper struct for variable-length gather operations
    struct GatherVData {
        std::size_t buf_len;
        std::shared_ptr<channel_data> recvbuf;
        std::vector<int32_t> displs;
        std::shared_ptr<channel_data> buffer;
        Utils::peer_num real_src;
    };

    //! I/O state for tracking non-blocking operations
    struct IOState {
        std::shared_ptr<channel_data> request;
        size_t processed = 0;
        Utils::Operation operation = Utils::send;
        Utils::fmiContext* context = nullptr;
        char dummy = 0;

        std::function<void(Utils::NbxStatus, const std::string&,
                          Utils::fmiContext*)> callbackResult;
        std::function<void()> callback = nullptr;
        std::chrono::steady_clock::time_point deadline;

        //! Set the request data
        void setRequest(const std::shared_ptr<channel_data>& cdata) {
            request = cdata;
        }

        //! Set the callback with bound arguments
        template<typename Func, typename... Args>
        void setCallback(Func&& func, Args&&... args) {
            callback = std::bind(std::forward<Func>(func), std::forward<Args>(args)...);
        }
    };

    //! Peer-To-Peer channel type
    /*!
     * This class provides optimized collectives for channels where clients can address each other directly and defines the interface that these channels need to implement.
     */
    class PeerToPeer : public Channel {
    public:
        // Blocking send/recv
        void send(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num dest) override;
        void recv(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num src) override;

        // Non-blocking send/recv
        void send(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num dest,
                  FMI::Utils::fmiContext* context, FMI::Utils::Mode mode,
                  std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                    FMI::Utils::fmiContext*)> callback) override;
        void recv(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num src,
                  FMI::Utils::fmiContext* context, FMI::Utils::Mode mode,
                  std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                    FMI::Utils::fmiContext*)> callback) override;

        //! Binomial tree broadcast implementation
        void bcast(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num root) override;

        //! Non-blocking broadcast
        void bcast(std::shared_ptr<channel_data> buf, FMI::Utils::peer_num root,
                   FMI::Utils::Mode mode,
                   std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                     FMI::Utils::fmiContext*)> callback) override;

        //! Calls allreduce with a (associative and commutative) NOP operation
        void barrier() override;

        //! Binomial tree gather.
        /*!
         * In the beginning, the needed buffer size (largest value that this peer will receive) is determined and a buffer is allocated.
         * If the ID of the root is not 0, we cannot necessarily receive all values directly in recvbuf because we need to wrap around (e.g., when we get from peer N - 1 the values for N - 1, 0, and 1).
         * This is solved by allocating a temporary buffer and copying the values.
         */
        void gather(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root) override;

        //! Variable-length gather
        void gatherv(std::shared_ptr<channel_data> sendbuf,
                     std::shared_ptr<channel_data> recvbuf,
                     FMI::Utils::peer_num root,
                     const std::vector<int32_t>& recvcounts,
                     const std::vector<int32_t>& displs) override;

        //! Non-blocking variable-length gather
        void gatherv(std::shared_ptr<channel_data> sendbuf,
                     std::shared_ptr<channel_data> recvbuf,
                     FMI::Utils::peer_num root,
                     const std::vector<int32_t>& recvcounts,
                     const std::vector<int32_t>& displs,
                     FMI::Utils::Mode mode,
                     std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                       FMI::Utils::fmiContext*)> callback) override;

        //! All-gather
        void allgather(std::shared_ptr<channel_data> sendbuf,
                       std::shared_ptr<channel_data> recvbuf,
                       FMI::Utils::peer_num root) override;

        //! Non-blocking all-gather
        void allgather(std::shared_ptr<channel_data> sendbuf,
                       std::shared_ptr<channel_data> recvbuf,
                       FMI::Utils::peer_num root, FMI::Utils::Mode mode,
                       std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                         FMI::Utils::fmiContext*)> callback) override;

        //! Variable-length all-gather
        void allgatherv(std::shared_ptr<channel_data> sendbuf,
                        std::shared_ptr<channel_data> recvbuf,
                        FMI::Utils::peer_num root,
                        const std::vector<int32_t>& recvcounts,
                        const std::vector<int32_t>& displs) override;

        //! Non-blocking variable-length all-gather
        void allgatherv(std::shared_ptr<channel_data> sendbuf,
                        std::shared_ptr<channel_data> recvbuf,
                        FMI::Utils::peer_num root,
                        const std::vector<int32_t>& recvcounts,
                        const std::vector<int32_t>& displs,
                        FMI::Utils::Mode mode,
                        std::function<void(FMI::Utils::NbxStatus, const std::string&,
                                          FMI::Utils::fmiContext*)> callback) override;

        //! Binomial tree scatter
        /*!
         * Similarly to gather, the root may need to send values from its sendbuf that is not consecutive when its ID is not 0, which is solved with a temporary buffer.
         */
        void scatter(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root) override;

        //! Calls reduce_no_order for associative and commutative functions, reduce_ltr otherwise
        void reduce(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root, raw_function f) override;

        //! For associative and commutative functions, allreduce_no_order is called. Otherwise, reduce followed by bcast is used.
        void allreduce(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, raw_function f) override;

        //! For associative and commutative functions, scan_no_order is called. Otherwise, scan_ltr is called
        void scan(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, raw_function f) override;

        //! Event progress for non-blocking operations
        Utils::EventProcessStatus channel_event_progress(Utils::Operation op) override;

        //! Send an object to peer with ID peer_id. Needs to be implemented by the channels.
        virtual void send_object(std::shared_ptr<channel_data> buf, Utils::peer_num peer_id) = 0;

        //! Receive an object from peer with ID peer_id. Needs to be implemented by the channels.
        virtual void recv_object(std::shared_ptr<channel_data> buf, Utils::peer_num peer_id) = 0;

        //! Non-blocking send object
        virtual void send_object(std::shared_ptr<IOState> state,
                                 Utils::peer_num peer_id, Utils::Mode mode) = 0;

        //! Non-blocking receive object
        virtual void recv_object(std::shared_ptr<IOState> state,
                                 Utils::peer_num peer_id, Utils::Mode mode) = 0;

        double get_operation_latency(Utils::OperationInfo op_info) override;

        double get_operation_price(Utils::OperationInfo op_info) override;

    protected:
        //! Reduction with left-to-right evaluation, gather followed by a function evaluation on the root peer.
        void reduce_ltr(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root, const raw_function& f);

        //! Binomial tree reduction where all peers apply the function in every step.
        void reduce_no_order(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, FMI::Utils::peer_num root, const raw_function& f);

        //! Recursive doubling allreduce implementation. When num_peers is not a power of two, there is an additional message in the beginning and end for every peer where they send their value / receive the reduced value.
        void allreduce_no_order(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, const raw_function& f);

        //! Linear function application / sending
        void scan_ltr(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, const raw_function& f);

        //! Binomial tree with up- and down-phase
        void scan_no_order(std::shared_ptr<channel_data> sendbuf, std::shared_ptr<channel_data> recvbuf, const raw_function& f);

    private:
        //! Allows to implement all collectives as if root were 0
        /*!
         * Transforms peer IDs such that the the user-provided root ID has a transformed ID of 0.
         * Makes the implementation of many collectives easier, because they only need to be implemented for the case with root = 0, when the transformation is used in the appropriate places
         * @param id ID to transform
         * @param root User-chosen root ID
         * @param forward Forward (root -> 0) or backward (0 -> root) transformation
         * @return transformed peer ID
         */
        Utils::peer_num transform_peer_id(Utils::peer_num id, Utils::peer_num root, bool forward);

    };
}



#endif //FMI_PEERTOPEER_H
