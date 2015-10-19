#include <iostream>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <iomanip>
#include <chrono>
#include <cstring>

#include <rdma/rdma_cma.h>

#include <psl/net.h>
#include <psl/log.h>
#include <psl/type_traits.h>

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>

#include <common.h>

// constexpr size_t sample_size = 1e6;

inline std::istream& operator>>(std::istream& in, ibv_wr_opcode& op) {
    std::string str;
    in >> str;
    if (boost::iequals("read", str)) {
        op = IBV_WR_RDMA_READ;
    } else if (boost::iequals("write", str)) {
        op = IBV_WR_RDMA_WRITE;
    } else if (boost::iequals("fadd", str)) {
        op = IBV_WR_ATOMIC_FETCH_AND_ADD;
    } else if (boost::iequals("cas", str)) {
        op = IBV_WR_ATOMIC_CMP_AND_SWP;
    } else if (boost::iequals("send", str)) {
        op = IBV_WR_SEND;
    } else {
        in.setstate(std::ios_base::failbit);
    }
    return in;
}

enum class Type {
    LAT,
    BW
};

inline std::ostream& operator<<(std::ostream& out, const Type& t) {
    out << psl::to_underlying(t);
    return out;
}

inline std::istream& operator>>(std::istream& in, Type& t) {
    std::string str;
    in >> str;
    if (boost::iequals("lat", str)) {
        t = Type::LAT;
    } else if (boost::iequals("bw", str)) {
        t = Type::BW;
    }
    return in;
}

int main(int argc, char* argv[]) {
    namespace bop = boost::program_options;

    bop::options_description desc("Options");
    desc.add_options()
        ("help", "produce this message")
        ("l", bop::value<ssize_t>()->default_value(1), "locations to access (random order) or <=0 index to location")
        ("tx", bop::value<size_t>()->default_value(1), "tx depth")
        ("cq_mod", bop::value<size_t>()->default_value(1), "signaled wr every nth (<tx depth)")
        ("op", bop::value<ibv_wr_opcode>()->default_value(IBV_WR_RDMA_WRITE), "opcode: read/write/fadd/cas/send")
        ("t", bop::value<Type>()->default_value(Type::BW), "lat/bw")
        ("ip", bop::value<psl::net::in_addr>()->required(), "server ip")
        ("p", bop::value<psl::net::in_port_t>()->default_value(13345), "port")
        ("d", bop::value<size_t>()->default_value(10), "duration (seconds)")
        ("i", bop::value<size_t>()->default_value(0), "inline data size (bytes)")
        ("s", bop::value<Bytes>()->default_value({8}), "size")
        ("a", bop::value<Bytes>()->default_value({64}), "alignment");

    bop::positional_options_description p;
    p.add("ip", 1);
    p.add("p", 1);

    bop::variables_map vm;
    bop::store(
        bop::command_line_parser(argc, argv).options(desc).positional(p).run(),
        vm);

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 1;
    }
    bop::notify(vm);

    ssize_t locations = vm["l"].as<ssize_t>();
    std::vector<size_t> location_indices;
    if (locations <= 0) {
        location_indices.push_back(-locations);
    } else {
        location_indices.resize(locations);
        std::iota(location_indices.begin(), location_indices.end(), 0);
        std::mt19937_64 r(std::random_device{}());
        std::shuffle(location_indices.begin(), location_indices.end(), r);
    }
    const size_t max_location = locations <= 0 ? (-locations + 1) : locations;
    auto location_index = location_indices.cbegin();

    rdma_cm_id* id;
    LOG_ERR_EXIT(rdma_create_id(nullptr, &id, nullptr, RDMA_PS_TCP), errno,
            std::system_category());

    psl::net::in_addr ip = vm["ip"].as<psl::net::in_addr>();
    psl::net::in_port_t port = vm["p"].as<psl::net::in_port_t>();
    sockaddr_in addr;
    addr.sin_addr = ip;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    LOG_ERR_EXIT(rdma_resolve_addr(id, NULL,
                reinterpret_cast<sockaddr*>(&addr), 1000), errno,
            std::system_category());

    LOG_ERR_EXIT(rdma_resolve_route(id, 1000), errno, std::system_category());

    ibv_wr_opcode opcode = vm["op"].as<ibv_wr_opcode>();
    size_t tx_depth = vm["tx"].as<size_t>();
    size_t ncqe = tx_depth;
    if (opcode == IBV_WR_SEND) {
        ncqe *= 2;
    }
    ibv_cq* cq;
    LOG_ERR_EXIT(!(cq = ibv_create_cq(id->verbs, ncqe, NULL, NULL, 0)), errno,
            std::system_category());

    Bytes size = vm["s"].as<Bytes>();
    size_t inline_data = vm["i"].as<size_t>();
    LOG_ERR_EXIT(inline_data < size.value, EINVAL, std::system_category());
    LOG_ERR_EXIT(inline_data && (opcode == IBV_WR_ATOMIC_CMP_AND_SWP
             || opcode == IBV_WR_ATOMIC_FETCH_AND_ADD
             || opcode == IBV_WR_RDMA_READ), EINVAL, std::system_category());
    ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 0;
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.cap.max_inline_data = inline_data;
    qp_init_attr.cap.max_recv_wr = opcode == IBV_WR_SEND ? tx_depth : 1;
    qp_init_attr.cap.max_send_wr = tx_depth;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_send_sge = 1;
    LOG_ERR_EXIT(rdma_create_qp(id, id->pd, &qp_init_attr), errno,
            std::system_category());

    ibv_device_attr dev_attr;
    LOG_ERR_EXIT(ibv_query_device(id->verbs, &dev_attr), errno,
            std::system_category());

    ClientConnectionData conn_data;
    conn_data.send = (opcode == IBV_WR_SEND);
    conn_data.locations = locations;
    rdma_conn_param conn_param = {};
    conn_param.private_data = reinterpret_cast<void*>(&conn_data);
    conn_param.private_data_len = sizeof(conn_data);
    conn_param.responder_resources = dev_attr.max_qp_rd_atom;
    conn_param.initiator_depth = dev_attr.max_qp_rd_atom;
    LOG_ERR_EXIT(rdma_connect(id, &conn_param), errno, std::system_category());

    LOG_ERR_EXIT(id->event->param.conn.private_data_len <
            sizeof(ServerConnectionData), EINVAL, std::system_category());
    ServerConnectionData server_conn_data =
        *reinterpret_cast<const ServerConnectionData*>(
                id->event->param.conn.private_data);

    Bytes alignment = vm["a"].as<Bytes>();
    size_t aligned_size = align(size.value, alignment.value);
    LOG_ERR_EXIT(aligned_size * max_location > server_conn_data.size, EINVAL,
            std::system_category());

    void* data;
    size_t max_local_size = aligned_size * location_indices.size();
    LOG_ERR_EXIT(posix_memalign(&data, alloc_alignment, max_local_size), errno,
            std::system_category());
    std::memset(data, 0, max_local_size);

    ibv_mr* mr;
    LOG_ERR_EXIT(!(mr = ibv_reg_mr(id->pd, data, max_local_size,
                    IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_WRITE |
                                               IBV_ACCESS_REMOTE_READ |
                                               IBV_ACCESS_REMOTE_ATOMIC)),
            errno, std::system_category());

    Type type = vm["t"].as<Type>();
    // std::vector<uint64_t>* times = nullptr;
    std::atomic<uint64_t> operations{0};

    size_t duration = vm["d"].as<size_t>();
    std::thread([&](){
                using namespace std::chrono;

                while (duration-- > 0) {
                    auto now = system_clock::now();
                    nanoseconds ns;
                    do {
                        now = system_clock::now();
                        ns = duration_cast<nanoseconds>(now.time_since_epoch()) -
                                                    duration_cast<seconds>(now.time_since_epoch());
                    } while (ns.count() > 1000);

                    if (type == Type::BW) {
                        auto i = operations.exchange(0);
                        auto ttnow = system_clock::to_time_t(now);
                        auto tmnow = std::localtime(&ttnow);
                        char buf[80];
                        strftime(buf, sizeof(buf), "%d.%m.%y %X", tmnow);
                        std::cout << buf << "." << std::setfill('0')
                            << std::setw(9) << ns.count() << "\t"
                            << i << " ops/sec\n";
                    }
                }
                /* we are done */
                std::exit(0);
            }).detach();

    ibv_send_wr wr;
    wr.wr_id = 0;
    ibv_sge sge;
    sge.lkey = mr->lkey;
    if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP ||
        opcode == IBV_WR_ATOMIC_FETCH_AND_ADD) {
        LOG_ERR_EXIT(size.value != 8, EINVAL, std::system_category());
        sge.length = 8;
    } else {
        sge.length = server_conn_data.size;
    }
    wr.sg_list = &sge;
    wr.num_sge = opcode;
    wr.send_flags = inline_data ? IBV_SEND_INLINE : 0;
    uint64_t* remote_addr = nullptr;
    if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP ||
        opcode == IBV_WR_ATOMIC_FETCH_AND_ADD) {
        remote_addr = &wr.wr.atomic.remote_addr;
        wr.wr.atomic.rkey = server_conn_data.rkey;
        wr.wr.atomic.compare_add = 1;
        wr.wr.atomic.swap = 1;
    } else {
        remote_addr = &wr.wr.rdma.remote_addr;
        wr.wr.rdma.rkey = server_conn_data.rkey;
    }

    size_t in_flight = 0;
    ibv_wc* wc = new ibv_wc[tx_depth];
    while (true) {

        /* 1. post */
        while (in_flight < tx_depth) {
            if (location_index == location_indices.cend()) {
                location_index = location_indices.cbegin();
            }
            /* local location */
            sge.addr = reinterpret_cast<uint64_t>(mr->addr);
            if (location_indices.size() > 1) {
                sge.addr += *location_index * aligned_size;
            }
            /* remote location */
            *remote_addr = server_conn_data.address + aligned_size * *location_index;
            ibv_send_wr* bad_wr;
            LOG_ERR_EXIT(ibv_post_send(id->qp, &wr, &bad_wr), errno,
                    std::system_category());
            location_index++; in_flight++;
        }

        /* 2. poll */
        int polled;
        do {
            LOG_ERR_EXIT((polled = ibv_poll_cq(cq, tx_depth, wc) < 0), errno,
                    std::system_category());
        } while (polled == 0);
        for (size_t i = 0; i < polled; i++) {
            LOG_ERR_EXIT(wc[i].status != IBV_WC_SUCCESS, wc[i].status,
                    ibv_wc_error_category());
        }
   }

    delete[] wc;
    return 0;
}
