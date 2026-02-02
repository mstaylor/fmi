#ifndef FMI_COMMON_H
#define FMI_COMMON_H
#include <exception>

//! Contains various utilities that are used in FMI
namespace FMI::Utils {
    //! Type for peer IDs / numbers
    using peer_num = unsigned int;

    //! Custom exception that is thrown on timeouts
    struct Timeout : public std::exception {
        [[nodiscard]] const char * what () const noexcept {
            return "Timeout was reached";
        }
    };

    //! Set by the client, controls the optimization goal of the Channel Policy
    enum Hint {
        fast, cheap
    };

    //! Mode for blocking/non-blocking selection
    enum Mode {
        BLOCKING,
        NONBLOCKING
    };

    //! Detailed error codes for non-blocking operations
    enum NbxStatus {
        SUCCESS,
        SEND_FAILED,
        RECEIVE_FAILED,
        DUMMY_SEND_FAILED,
        CONNECTION_CLOSED_BY_PEER,
        SOCKET_CREATE_FAILED,
        TCP_NODELAY_FAILED,
        FCNTL_GET_FAILED,
        FCNTL_SET_FAILED,
        ADD_EVENT_FAILED,
        EPOLL_WAIT_FAILED,
        SOCKET_PAIR_FAILED,
        SOCKET_SET_SO_RCVTIMEO_FAILED,
        SOCKET_SET_SO_SNDTIMEO_FAILED,
        SOCKET_SET_TCP_NODELAY_FAILED,
        SOCKET_SET_NONBLOCKING_FAILED,
        NBX_TIMEOUT
    };

    //! Event processing status for progress tracking
    enum EventProcessStatus {
        PROCESSING,
        EMPTY,
        NOOP
    };

    //! Completion context structure for non-blocking operations
    struct fmiContext {
        int completed;
    };

    //! List of currently supported collectives
    enum Operation {
        send, recv, bcast, barrier, gather, gatherv, allgather, allgatherv,
        scatter, reduce, allreduce, scan
    };

    //! All the information about an operation, passed to the Channel Policy for its decision on which channel to use.
    struct OperationInfo {
        Operation op;
        std::size_t data_size;
        bool left_to_right = false;
    };

}

#endif //FMI_COMMON_H
