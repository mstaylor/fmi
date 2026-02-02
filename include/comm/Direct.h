#ifndef FMI_DIRECT_H
#define FMI_DIRECT_H

#include "PeerToPeer.h"
#include <unordered_map>

namespace FMI::Comm {
    //! Channel that uses the TCPunch TCP NAT Hole Punching Library for connection establishment.
    class Direct : public PeerToPeer {
    public:
        explicit Direct(std::map<std::string, std::string> params, std::map<std::string, std::string> model_params);
        virtual ~Direct();

        //! Initialize non-blocking infrastructure
        void init() override;

        //! Get max timeout configuration
        int getMaxTimeout() override;

        //! Blocking send object
        void send_object(std::shared_ptr<channel_data> buf, Utils::peer_num rcpt_id) override;

        //! Blocking receive object
        void recv_object(std::shared_ptr<channel_data> buf, Utils::peer_num sender_id) override;

        //! Non-blocking send object
        void send_object(std::shared_ptr<IOState> state,
                        Utils::peer_num rcpt_id, Utils::Mode mode) override;

        //! Non-blocking receive object
        void recv_object(std::shared_ptr<IOState> state,
                        Utils::peer_num sender_id, Utils::Mode mode) override;

        //! Event progress for non-blocking operations
        Utils::EventProcessStatus channel_event_progress(Utils::Operation op) override;

        double get_latency(Utils::peer_num producer, Utils::peer_num consumer, std::size_t size_in_bytes) override;

        double get_price(Utils::peer_num producer, Utils::peer_num consumer, std::size_t size_in_bytes) override;

    private:
        //! Socket storage by mode (supports both blocking and non-blocking sockets)
        std::unordered_map<Utils::Mode, std::vector<int>> sockets;

        //! I/O state tracking for non-blocking operations
        std::unordered_map<Utils::Operation,
            std::unordered_map<int, std::shared_ptr<IOState>>> io_states;

        //! Current operating mode
        Utils::Mode mode = Utils::BLOCKING;

        std::string hostname;
        int port;
        bool resolve_host_dns;
        unsigned int max_timeout;
        // Model params
        double bandwidth;
        double overhead;
        double transfer_price;
        double vm_price;
        unsigned int requests_per_hour;
        bool include_infrastructure_costs;

        //! Checks if connection with a peer partner_id is already established, otherwise establishes it using TCPunch.
        void check_socket(Utils::peer_num partner_id, std::string pair_name);

        //! Check and setup non-blocking socket
        void check_socket_nbx(Utils::peer_num partner_id, std::string pair_name);

        //! Generate pairing name with mode distinction
        std::string get_pairing_name(Utils::peer_num a, Utils::peer_num b, Utils::Mode mode);

        //! Event handling for progress. Returns iterator to next element.
        std::unordered_map<int, std::shared_ptr<IOState>>::iterator
        handle_event(std::unordered_map<int, std::shared_ptr<IOState>>::iterator it,
                     std::unordered_map<int, std::shared_ptr<IOState>>& states,
                     Utils::Operation op);
    };
}



#endif //FMI_DIRECT_H
