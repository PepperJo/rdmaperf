#include <iostream>
#include <sstream>
#include <cstdlib>

#include <boost/program_options.hpp>

#include <rdma/rdma_cma.h>

#include <psl/log.h>
#include <psl/net.h>

#include <common.h>

constexpr int connection_backlog = 128;

int main(int argc, char* argv[]) {
    namespace bop = boost::program_options;

    bop::options_description desc("Options");
    desc.add_options()("help", "produce this message")(
        "l", bop::value<size_t>()->required(),
        "locations")("s", bop::value<size_t>()->required(), "size")(
        "ip", bop::value<psl::net::in_addr>()->default_value({}),
        "listen only from this ip")(
        "p", bop::value<psl::net::in_port_t>()->default_value(13345),
        "listen on port")("i", bop::value<bool>()->default_value(false),
                          "inline");

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

    size_t locations = vm["l"].as<size_t>();
    LOG_ERR_EXIT(!locations, EINVAL);

    size_t size = vm["s"].as<size_t>();
    LOG_ERR_EXIT(!size, EINVAL);

    psl::net::in_addr ip = vm["ip"].as<psl::net::in_addr>();
    psl::net::in_port_t port = vm["p"].as<psl::net::in_port_t>();

    rdma_cm_id* id;
    LOG_ERR_EXIT(rdma_create_id(nullptr, &id, nullptr, RDMA_PS_TCP), errno);

    sockaddr_in addr;
    addr.sin_addr = ip;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    LOG_ERR_EXIT(rdma_bind_addr(id, reinterpret_cast<sockaddr*>(&addr)), errno);

    void* data;
    size_t total_size = size * locations;
    LOG_ERR_EXIT(posix_memalign(&data, alloc_alignment, total_size), errno);
    std::memset(data, 0, total_size);

    int num_devices;
    ibv_context** context;
    std::cout << "Server listening on " << ip << ":" << port;
    if (id->verbs) {
        std::cout << " (dev = " << id->verbs->device->dev_name << ")";
        num_devices = 1;
        context = &id->verbs;
    } else {
        LOG_ERR_EXIT((context = rdma_get_devices(&num_devices)) == nullptr,
                     errno);
    }
    std::cout << '\n';

    LOG_ERR_EXIT(rdma_listen(id, connection_backlog), errno);

    bool inline_data = vm["i"].as<bool>();
    size_t nclients = 0;
    std::map<ibv_context*, ibv_mr&> mrs;
    while (true) {
        rdma_cm_id* child_id;
        LOG_ERR_EXIT(rdma_get_request(id, &child_id), errno);

        sockaddr_in* child_addr =
            reinterpret_cast<sockaddr_in*>(rdma_get_peer_addr(child_id));
        sockaddr_in* listen_addr =
            reinterpret_cast<sockaddr_in*>(rdma_get_local_addr(child_id));
        std::cout << "#" << nclients << " " << listen_addr->sin_addr << ":"
                  << ntohs(listen_addr->sin_port) << " <- "
                  << child_addr->sin_addr << ":" << ntohs(child_addr->sin_port)
                  << '\n';
        nclients++;

        auto mr_iter = mrs.find(child_id->verbs);
        if (mr_iter == mrs.end()) {
            ibv_mr* mr;
            LOG_ERR_EXIT(!(mr = ibv_reg_mr(child_id->pd, data, total_size,
                                           IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_WRITE |
                                               IBV_ACCESS_REMOTE_READ |
                                               IBV_ACCESS_REMOTE_ATOMIC)),
                         errno);
            mrs.insert({child_id->verbs, *mr});
        }

        ibv_cq* cq;
        LOG_ERR_EXIT(!(cq = ibv_create_cq(child_id->verbs, locations, nullptr,
                                          nullptr, 0)),
                     errno);

        ibv_qp_init_attr qp_init_attr = {};
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.sq_sig_all = 0;
        qp_init_attr.send_cq = cq;
        qp_init_attr.recv_cq = cq;
        qp_init_attr.cap.max_inline_data = inline_data ? size : 0;
        qp_init_attr.cap.max_recv_wr = max_recv_wr;
        qp_init_attr.cap.max_send_wr = max_send_wr;
        qp_init_attr.cap.max_recv_sge = 1;
        qp_init_attr.cap.max_send_sge = 1;
        LOG_ERR_EXIT(rdma_create_qp(child_id, child_id->pd, &qp_init_attr),
                     errno);

        rdma_conn_param conn_param = {};
        // conn_param.private_data =
    }

    return 0;
}
