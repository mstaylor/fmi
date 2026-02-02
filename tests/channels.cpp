#include <boost/test/unit_test.hpp>

#include "../include/comm/Channel.h"
#include "../include/comm/S3.h"
#include <numeric>
#include <ctime>
#include <omp.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <chrono>

BOOST_AUTO_TEST_SUITE(Channels);

std::map<std::string, std::string> s3_test_params = {
        {"bucket_name", "romanboe-uploadtest"},
        {"s3_region", "eu-central-1"},
        {"timeout", "100"},
        {"max_timeout", "1000"}
};

std::map<std::string, std::string> s3_test_model_params = {
        {"bandwidth", "50.0"},
        {"overhead", "40.4"},
        {"transfer_price", "0.0"},
        {"download_price", "0.00000043"},
        {"upload_price", "0.0000054"}
};

std::map<std::string, std::string> redis_test_params = {
        {"host", "127.0.0.1"},
        {"port", "6379"},
        {"timeout", "1"},
        {"max_timeout", "1000"}
};

std::map<std::string, std::string> redis_test_model_params = {
        {"bandwidth_single", "100.0"},
        {"bandwidth_multiple", "400.0"},
        {"overhead", "5.2"},
        {"transfer_price", "0.0"},
        {"instance_price", "0.0038"},
        {"requests_per_hour", "1000"},
        {"include_infrastructure_costs", "true"}
};

std::map<std::string, std::string> direct_test_params = {
        {"host", "127.0.0.1"},
        {"port", "10000"},
        {"max_timeout", "5000"}
};

std::map<std::string, std::string> direct_test_model_params = {
        {"bandwidth", "250.0"},
        {"overhead", "0.34"},
        {"transfer_price", "0.0"},
        {"vm_price", "0.0134"},
        {"requests_per_hour", "1000"},
        {"include_infrastructure_costs", "true"},
        {"resolve_host_dns", "false"}
};

std::map< std::string, std::pair< std::map<std::string, std::string>, std::map<std::string, std::string> > > backends = {
        //{"S3", {s3_test_params, s3_test_model_params}},
       // {"Redis", {redis_test_params, redis_test_model_params}},
        {"Direct", {direct_test_params, direct_test_model_params}}
};

std::string comm_name = std::to_string(std::time(nullptr)) + "Tests";

BOOST_AUTO_TEST_CASE(sending_receiving) {
    for (auto const & backend_data : backends) {
        // Using C++ 17 [key, val] : map syntax here does not compile (on some clang versions) in combination with the omp section because of a clang bug:
        // https://stackoverflow.com/questions/65819317/openmp-clang-sometimes-fail-with-a-variable-declared-from-structured-binding
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        int val = 42;
        int recv;
        #pragma omp parallel num_threads(2)
        {
            int tid = omp_get_thread_num();
            auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
            ch->set_peer_id(tid);
            ch->set_num_peers(2);
            ch->set_comm_name(comm_name);
            if (tid == 0) {
                auto buf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(val), noop_deleter);
                ch->send(buf, 1);
            } else if (tid == 1) {
                auto recv_buf = std::make_shared<channel_data>(reinterpret_cast<char*>(&recv), sizeof(recv), noop_deleter);
                ch->recv(recv_buf, 0);
            }
            ch->finalize();
        }
        BOOST_CHECK_EQUAL(val, recv);
    }
}

BOOST_AUTO_TEST_CASE(sending_receiving_mult_times) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        int val1 = 42;
        int val2 = 4242;
        int recv1, recv2;
        #pragma omp parallel num_threads(2)
        {
            int tid = omp_get_thread_num();
            auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
            ch->set_peer_id(tid);
            ch->set_num_peers(2);
            ch->set_comm_name(comm_name);
            if (tid == 0) {
                auto buf1 = std::make_shared<channel_data>(reinterpret_cast<char*>(&val1), sizeof(val1), noop_deleter);
                auto buf2 = std::make_shared<channel_data>(reinterpret_cast<char*>(&val2), sizeof(val2), noop_deleter);
                ch->send(buf1, 1);
                ch->send(buf2, 1);
            } else if (tid == 1) {
                auto recv_buf1 = std::make_shared<channel_data>(reinterpret_cast<char*>(&recv1), sizeof(recv1), noop_deleter);
                auto recv_buf2 = std::make_shared<channel_data>(reinterpret_cast<char*>(&recv2), sizeof(recv2), noop_deleter);
                ch->recv(recv_buf1, 0);
                ch->recv(recv_buf2, 0);
            }
            ch->finalize();
        }
        BOOST_CHECK_EQUAL(val1, recv1);
        BOOST_CHECK_EQUAL(val2, recv2);
    }
}

BOOST_AUTO_TEST_CASE(bcast) {
    for (auto const & backend_data : backends) {
        // Using many threads leads to race conditions (in the AWS SDK, raw sockets, hiredis, ...), therefore processes are used for these tests
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        FMI::Utils::peer_num root = 14;
        constexpr int num_peers = 32;
        int* vals = static_cast<int*>(mmap(nullptr, num_peers * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        vals[root] = 42;
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        auto buf = std::make_shared<channel_data>(reinterpret_cast<char*>(&vals[peer_id]), sizeof(vals[peer_id]), noop_deleter);
        ch->bcast(buf, root);
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK_EQUAL(vals[i], 42);
            }
        } else {
            exit(0);
        }

    }
}

BOOST_AUTO_TEST_CASE(barrier_unsucc) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 4;
        bool* caught = static_cast<bool*>(mmap(nullptr, num_peers * sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        std::chrono::steady_clock::time_point bef = std::chrono::steady_clock::now();
        if (peer_id != 1) {
            try {
                ch->barrier();
            } catch (FMI::Utils::Timeout) {
                caught[peer_id] = true;
            }

        }
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                if (i != 1) {
                    BOOST_CHECK_EQUAL(caught[i], true);
                }
            }
        } else {
            exit(0);
        }
    }
}

BOOST_AUTO_TEST_CASE(barrier_succ) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 2;
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        std::chrono::steady_clock::time_point bef = std::chrono::steady_clock::now();
        ch->barrier();
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            std::chrono::steady_clock::time_point after = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(after - bef).count();
            BOOST_TEST(elapsed_ms < std::stoi(test_params["max_timeout"]));
        } else {
            exit(0);
        }
    }

}

BOOST_AUTO_TEST_CASE(gather_one) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 2;
        std::vector<int> vals {1,2,3,4};
        FMI::Utils::peer_num root = 1;

        int* rcv_vals = static_cast<int*>(mmap(nullptr, num_peers * 2 * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        if (peer_id == root) {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(vals.data() + 2 * peer_id), sizeof(vals[0]) * 2, noop_deleter);
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(rcv_vals), sizeof(int) * num_peers * 2, noop_deleter);
            ch->gather(sendbuf, recvbuf, root);
        } else {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(vals.data() + 2 * peer_id), sizeof(vals[0]) * 2, noop_deleter);
            auto recvbuf = std::make_shared<channel_data>();
            ch->gather(sendbuf, recvbuf, root);
        }
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK_EQUAL(rcv_vals[i], i + 1);
            }
        } else {
            exit(0);
        }
    }
}

BOOST_AUTO_TEST_CASE(gather_multiple) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 14;
        std::vector<int> vals(2 * num_peers);
        for (size_t i = 0; i < vals.size(); i++) {
            vals[i] = i + 1;
        }
        FMI::Utils::peer_num root = 0;

        int* rcv_vals = static_cast<int*>(mmap(nullptr, num_peers * 2 * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        if (peer_id == root) {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(vals.data() + 2 * peer_id), sizeof(vals[0]) * 2, noop_deleter);
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(rcv_vals), sizeof(int) * num_peers * 2, noop_deleter);
            ch->gather(sendbuf, recvbuf, root);
        } else {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(vals.data() + 2 * peer_id), sizeof(vals[0]) * 2, noop_deleter);
            auto recvbuf = std::make_shared<channel_data>();
            ch->gather(sendbuf, recvbuf, root);
        }
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK_EQUAL(rcv_vals[i], i + 1);
            }
        } else {
            exit(0);
        }
    }
}

BOOST_AUTO_TEST_CASE(scatter_one) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 2;
        std::vector<int> root_vals {1,2,3,4};
        FMI::Utils::peer_num root = 0;

        int* rcv_vals = static_cast<int*>(mmap(nullptr, num_peers * 2 * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        if (peer_id == root) {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(root_vals.data()), sizeof(root_vals[0]) * root_vals.size(), noop_deleter);
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(rcv_vals + peer_id * 2), sizeof(int) * 2, noop_deleter);
            ch->scatter(sendbuf, recvbuf, root);
        } else {
            auto sendbuf = std::make_shared<channel_data>();
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(rcv_vals + peer_id * 2), sizeof(int) * 2, noop_deleter);
            ch->scatter(sendbuf, recvbuf, root);
        }
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK_EQUAL(rcv_vals[i], i + 1);
            }
        } else {
            exit(0);
        }
    }
}

BOOST_AUTO_TEST_CASE(scatter_multiple) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 14;
        std::vector<int> root_vals(2 * num_peers);
        for (size_t i = 0; i < root_vals.size(); i++) {
            root_vals[i] = i + 1;
        }
        FMI::Utils::peer_num root = 3;

        int* rcv_vals = static_cast<int*>(mmap(nullptr, num_peers * 2 * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        if (peer_id == root) {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(root_vals.data()), sizeof(root_vals[0]) * root_vals.size(), noop_deleter);
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(rcv_vals + peer_id * 2), sizeof(int) * 2, noop_deleter);
            ch->scatter(sendbuf, recvbuf, root);
        } else {
            auto sendbuf = std::make_shared<channel_data>();
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(rcv_vals + peer_id * 2), sizeof(int) * 2, noop_deleter);
            ch->scatter(sendbuf, recvbuf, root);
        }
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK_EQUAL(rcv_vals[i], i + 1);
            }
        } else {
            exit(0);
        }
    }
}

BOOST_AUTO_TEST_CASE(reduce_multiple) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        FMI::Utils::peer_num root = 5;
        constexpr int num_peers = 13;
        int* res = static_cast<int*>(mmap(nullptr, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto f = [] (char* a, char* b) {
            int* dest = reinterpret_cast<int*>(a);
            *dest = *((int*) a) * *((int*) b);
        };
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        int val = peer_id + 1;
        if (peer_id == root) {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(res), sizeof(int), noop_deleter);
            ch->reduce(sendbuf, recvbuf, root, {f, true, true});
        } else {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
            auto recvbuf = std::make_shared<channel_data>();
            ch->reduce(sendbuf, recvbuf, root, {f, true, true});
        }

        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            int expected = 1;
            for (int i = 1; i < num_peers; i++) {
                expected *= (i + 1);
            }
            BOOST_CHECK_EQUAL(expected, *res);
        } else {
            exit(0);
        }

    }
}

BOOST_AUTO_TEST_CASE(reduce_multiple_ltr) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        FMI::Utils::peer_num root = 0;
        constexpr int num_peers = 8;
        int* res = static_cast<int*>(mmap(nullptr, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto f = [] (char* a, char* b) {
            int* dest = reinterpret_cast<int*>(a);
            *dest = *((int*) a) - *((int*) b);
        };
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        int val = peer_id + 1;
        if (peer_id == root) {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(res), sizeof(int), noop_deleter);
            ch->reduce(sendbuf, recvbuf, root, {f, false, false});
        } else {
            auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
            auto recvbuf = std::make_shared<channel_data>();
            ch->reduce(sendbuf, recvbuf, root, {f, false, false});
        }

        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            int expected = 1;
            for (int i = 1; i < num_peers; i++) {
                expected -= (i + 1);
            }
            BOOST_CHECK_EQUAL(expected, *res);
        } else {
            exit(0);
        }

    }
}

BOOST_AUTO_TEST_CASE(allreduce_multiple) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 8;
        int* res = static_cast<int*>(mmap(nullptr, num_peers * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto f = [] (char* a, char* b) {
            int* dest = reinterpret_cast<int*>(a);
            *dest = *((int*) a) + *((int*) b);
        };
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        int val = peer_id + 1;
        auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
        auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(res + peer_id), sizeof(int), noop_deleter);
        ch->allreduce(sendbuf, recvbuf, {f, true, true});

        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            int expected = 1;
            for (int i = 1; i < num_peers; i++) {
                expected += (i + 1);
            }
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK_EQUAL(expected, res[i]);
            }
        } else {
            exit(0);
        }

    }
}

BOOST_AUTO_TEST_CASE(allreduce_multiple_ltr) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        FMI::Utils::peer_num root = 0;
        constexpr int num_peers = 8;
        int* res = static_cast<int*>(mmap(nullptr, num_peers * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto f = [] (char* a, char* b) {
            int* dest = reinterpret_cast<int*>(a);
            *dest = *((int*) a) - *((int*) b);
        };
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        int val = peer_id + 1;
        auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
        auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(res + peer_id), sizeof(int), noop_deleter);
        ch->allreduce(sendbuf, recvbuf, {f, false, false});

        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            int expected = 1;
            for (int i = 1; i < num_peers; i++) {
                expected -= (i + 1);
            }
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK_EQUAL(expected, res[i]);
            }
        } else {
            exit(0);
        }

    }
}

BOOST_AUTO_TEST_CASE(scan) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 32;
        int* res = static_cast<int*>(mmap(nullptr, sizeof(int) * num_peers, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto f = [] (char* a, char* b) {
            int* dest = reinterpret_cast<int*>(a);
            *dest = *((int*) a) + *((int*) b);
        };
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        int val = peer_id + 1;
        auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
        auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(res + peer_id), sizeof(int), noop_deleter);
        ch->scan(sendbuf, recvbuf, {f, true, true});
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            int prefix_sum = 0;
            for (int i = 0; i < num_peers; i++) {
                prefix_sum += (i + 1);
                BOOST_CHECK_EQUAL(prefix_sum, res[i]);
            }
        } else {
            exit(0);
        }


    }
}

BOOST_AUTO_TEST_CASE(scan_ltr) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 8;
        int* res = static_cast<int*>(mmap(nullptr, sizeof(int) * num_peers, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i ++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }
        auto f = [] (char* a, char* b) {
            int* dest = reinterpret_cast<int*>(a);
            *dest = *((int*) a) - *((int*) b);
        };
        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name);
        int val = peer_id;
        auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(int), noop_deleter);
        auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(res + peer_id), sizeof(int), noop_deleter);
        ch->scan(sendbuf, recvbuf, {f, false, false});
        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            int result = 0;
            for (int i = 0; i < num_peers; i++) {
                result = result - i;
                BOOST_CHECK_EQUAL(result, res[i]);
            }
        } else {
            exit(0);
        }


    }
}

// Variable-length collective tests
BOOST_AUTO_TEST_CASE(gatherv_basic) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 4;
        FMI::Utils::peer_num root = 0;

        // Each peer sends different amounts: peer 0 sends 1 int, peer 1 sends 2 ints, etc.
        // Note: recvcounts and displs are in BYTES (not elements) for channel_data
        constexpr int int_size = sizeof(int);
        std::vector<int32_t> recvcounts = {1 * int_size, 2 * int_size, 3 * int_size, 4 * int_size};
        std::vector<int32_t> displs = {0, 1 * int_size, 3 * int_size, 6 * int_size};  // Cumulative byte displacements
        int total_size = 10;  // 1 + 2 + 3 + 4 elements

        int* rcv_vals = static_cast<int*>(mmap(nullptr, total_size * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        int peer_id = 0;
        for (int i = 1; i < num_peers; i++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }

        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name + "_gatherv");

        // Each peer fills its send buffer with its peer_id
        int send_count = peer_id + 1;
        std::vector<int> send_vals(send_count, peer_id + 1);

        auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(send_vals.data()), send_count * sizeof(int), noop_deleter);
        if (peer_id == root) {
            auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(rcv_vals), total_size * sizeof(int), noop_deleter);
            ch->gatherv(sendbuf, recvbuf, root, recvcounts, displs);
        } else {
            auto recvbuf = std::make_shared<channel_data>();
            ch->gatherv(sendbuf, recvbuf, root, recvcounts, displs);
        }

        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            // Verify: peer i contributed (i+1) values of value (i+1)
            BOOST_CHECK_EQUAL(rcv_vals[0], 1);  // peer 0: 1 value of 1
            BOOST_CHECK_EQUAL(rcv_vals[1], 2);  // peer 1: 2 values of 2
            BOOST_CHECK_EQUAL(rcv_vals[2], 2);
            BOOST_CHECK_EQUAL(rcv_vals[3], 3);  // peer 2: 3 values of 3
            BOOST_CHECK_EQUAL(rcv_vals[4], 3);
            BOOST_CHECK_EQUAL(rcv_vals[5], 3);
        } else {
            exit(0);
        }
    }
}

BOOST_AUTO_TEST_CASE(allgather_basic) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 4;
        FMI::Utils::peer_num root = 0;

        int* rcv_vals = static_cast<int*>(mmap(nullptr, num_peers * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        bool* success = static_cast<bool*>(mmap(nullptr, num_peers * sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));

        int peer_id = 0;
        for (int i = 1; i < num_peers; i++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }

        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name + "_allgather");

        int send_val = peer_id + 1;
        std::vector<int> recv_vals(num_peers);

        auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(&send_val), sizeof(int), noop_deleter);
        auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(recv_vals.data()), num_peers * sizeof(int), noop_deleter);

        ch->allgather(sendbuf, recvbuf, root);

        // Verify all peers got the complete data
        success[peer_id] = true;
        for (int i = 0; i < num_peers; i++) {
            if (recv_vals[i] != i + 1) {
                success[peer_id] = false;
            }
        }

        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK(success[i]);
            }
        } else {
            exit(0);
        }
    }
}

BOOST_AUTO_TEST_CASE(allgatherv_basic) {
    for (auto const & backend_data : backends) {
        auto channel_name = backend_data.first;
        auto test_params = backend_data.second.first;
        auto model_params = backend_data.second.second;

        constexpr int num_peers = 3;
        FMI::Utils::peer_num root = 0;

        // Each peer sends different amounts
        // Note: recvcounts and displs are in BYTES for channel_data
        constexpr int int_size = sizeof(int);
        std::vector<int32_t> recvcounts = {1 * int_size, 2 * int_size, 3 * int_size};
        std::vector<int32_t> displs = {0, 1 * int_size, 3 * int_size};
        int total_size = 6;  // 1 + 2 + 3 elements

        bool* success = static_cast<bool*>(mmap(nullptr, num_peers * sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));

        int peer_id = 0;
        for (int i = 1; i < num_peers; i++) {
            int pid = fork();
            if (pid == 0) {
                peer_id = i;
                break;
            }
        }

        auto ch = FMI::Comm::Channel::get_channel(channel_name, test_params, model_params);
        ch->set_peer_id(peer_id);
        ch->set_num_peers(num_peers);
        ch->set_comm_name(comm_name + "_allgatherv");

        int send_count = peer_id + 1;
        std::vector<int> send_vals(send_count, peer_id + 1);
        std::vector<int> recv_vals(total_size);

        auto sendbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(send_vals.data()), send_count * sizeof(int), noop_deleter);
        auto recvbuf = std::make_shared<channel_data>(reinterpret_cast<char*>(recv_vals.data()), total_size * sizeof(int), noop_deleter);

        ch->allgatherv(sendbuf, recvbuf, root, recvcounts, displs);

        // Verify all peers got the complete data
        success[peer_id] = (recv_vals[0] == 1 &&
                           recv_vals[1] == 2 && recv_vals[2] == 2 &&
                           recv_vals[3] == 3 && recv_vals[4] == 3 && recv_vals[5] == 3);

        ch->finalize();
        if (peer_id == 0) {
            int status = 0;
            while (wait(&status) > 0);
            for (int i = 0; i < num_peers; i++) {
                BOOST_CHECK(success[i]);
            }
        } else {
            exit(0);
        }
    }
}

// Non-blocking tests
BOOST_AUTO_TEST_CASE(nonblocking_send_recv) {
    // Test non-blocking with Direct channel only
    auto test_params = direct_test_params;
    auto model_params = direct_test_model_params;

    int val = 42;
    int recv_val = 0;
    bool* send_complete = static_cast<bool*>(mmap(nullptr, sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    bool* recv_complete = static_cast<bool*>(mmap(nullptr, sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    int* recv_result = static_cast<int*>(mmap(nullptr, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    *send_complete = false;
    *recv_complete = false;

    int peer_id = 0;
    int pid = fork();
    if (pid == 0) {
        peer_id = 1;
    }

    auto ch = FMI::Comm::Channel::get_channel("Direct", test_params, model_params);
    ch->set_peer_id(peer_id);
    ch->set_num_peers(2);
    ch->set_comm_name(comm_name + "_nbx");
    ch->init();

    FMI::Utils::fmiContext ctx{0};

    if (peer_id == 0) {
        auto buf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(val), noop_deleter);
        ch->send(buf, 1, &ctx, FMI::Utils::NONBLOCKING,
            [send_complete](FMI::Utils::NbxStatus status, const std::string& msg, FMI::Utils::fmiContext* ctx) {
                if (status == FMI::Utils::SUCCESS) {
                    *send_complete = true;
                }
            });

        // Poll for completion
        int timeout_counter = 0;
        while (!*send_complete && timeout_counter < 1000) {
            ch->channel_event_progress(FMI::Utils::send);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            timeout_counter++;
        }
    } else {
        auto buf = std::make_shared<channel_data>(reinterpret_cast<char*>(recv_result), sizeof(int), noop_deleter);
        ch->recv(buf, 0, &ctx, FMI::Utils::NONBLOCKING,
            [recv_complete](FMI::Utils::NbxStatus status, const std::string& msg, FMI::Utils::fmiContext* ctx) {
                if (status == FMI::Utils::SUCCESS) {
                    *recv_complete = true;
                }
            });

        // Poll for completion
        int timeout_counter = 0;
        while (!*recv_complete && timeout_counter < 1000) {
            ch->channel_event_progress(FMI::Utils::send);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            timeout_counter++;
        }
    }

    ch->finalize();

    if (peer_id == 0) {
        int status = 0;
        while (wait(&status) > 0);
        BOOST_CHECK(*send_complete);
        BOOST_CHECK(*recv_complete);
        BOOST_CHECK_EQUAL(val, *recv_result);
    } else {
        exit(0);
    }
}

BOOST_AUTO_TEST_CASE(nonblocking_progress_empty) {
    // Test that progress returns correct status when no operations pending
    auto ch = FMI::Comm::Channel::get_channel("Direct", direct_test_params, direct_test_model_params);
    ch->set_peer_id(0);
    ch->set_num_peers(1);
    ch->set_comm_name(comm_name + "_progress");
    ch->init();

    // With no pending operations, should return EMPTY or NOOP
    auto status = ch->channel_event_progress(FMI::Utils::send);
    BOOST_CHECK(status == FMI::Utils::EMPTY || status == FMI::Utils::NOOP);

    ch->finalize();
}

BOOST_AUTO_TEST_CASE(blocking_mode_with_callback) {
    // Test that blocking mode still works with callback API
    auto test_params = direct_test_params;
    auto model_params = direct_test_model_params;

    int val = 123;
    int recv_val = 0;
    bool* completed = static_cast<bool*>(mmap(nullptr, 2 * sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    completed[0] = false;
    completed[1] = false;

    int peer_id = 0;
    int pid = fork();
    if (pid == 0) {
        peer_id = 1;
    }

    auto ch = FMI::Comm::Channel::get_channel("Direct", test_params, model_params);
    ch->set_peer_id(peer_id);
    ch->set_num_peers(2);
    ch->set_comm_name(comm_name + "_blocking_cb");

    FMI::Utils::fmiContext ctx{0};

    if (peer_id == 0) {
        auto buf = std::make_shared<channel_data>(reinterpret_cast<char*>(&val), sizeof(val), noop_deleter);
        // Use BLOCKING mode with callback - should complete immediately
        ch->send(buf, 1, &ctx, FMI::Utils::BLOCKING,
            [&completed](FMI::Utils::NbxStatus status, const std::string& msg, FMI::Utils::fmiContext* ctx) {
                completed[0] = true;
            });
    } else {
        auto buf = std::make_shared<channel_data>(reinterpret_cast<char*>(&recv_val), sizeof(recv_val), noop_deleter);
        ch->recv(buf, 0, &ctx, FMI::Utils::BLOCKING,
            [&completed](FMI::Utils::NbxStatus status, const std::string& msg, FMI::Utils::fmiContext* ctx) {
                completed[1] = true;
            });
    }

    ch->finalize();

    if (peer_id == 0) {
        int status = 0;
        while (wait(&status) > 0);
        BOOST_CHECK(completed[0]);
        BOOST_CHECK(completed[1]);
    } else {
        exit(0);
    }
}

BOOST_AUTO_TEST_SUITE_END();