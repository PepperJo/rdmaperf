#include <iostream>
#include <sstream>
#include <cstdlib>

#include <boost/program_options.hpp>

#include <rdma/rdma_cma.h>

#include <psl/log.h>
#include <psl/net.h>

#include <common.h>

constexpr int connection_backlog = 128;

struct DeviceContext {
    ibv_mr& mr;
    ibv_device_attr dev_attr;
};

int main(int argc, char* argv[]) {
    namespace bop = boost::program_options;

    bop::options_description desc("Options");
    desc.add_options()("help", "produce this message")(
        "s", bop::value<Bytes>()->required(), "size (K/M/G)")(
        "ip", bop::value<psl::net::in_addr>()->default_value({}),
        "listen only from this ip")(
        "p", bop::value<psl::net::in_port_t>()->default_value(default_port),
        "listen on port");

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

    auto size = vm["s"].as<Bytes>();
    LOG_ERR_EXIT(!size.value, EINVAL, std::system_category());

    rdma_cm_id* id;
    LOG_ERR_EXIT(rdma_create_id(nullptr, &id, nullptr, RDMA_PS_TCP), errno,
                 std::system_category());

    psl::net::in_addr ip = vm["ip"].as<psl::net::in_addr>();
    psl::net::in_port_t port = vm["p"].as<psl::net::in_port_t>();
    sockaddr_in addr;
    addr.sin_addr = ip;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    LOG_ERR_EXIT(rdma_bind_addr(id, reinterpret_cast<sockaddr*>(&addr)), errno,
                 std::system_category());

    void* data;
    LOG_ERR_EXIT(posix_memalign(&data, alloc_alignment, size.value), errno,
                 std::system_category());
    std::memset(data, 0, size.value);

    std::cout << "Server listening on " << ip << ":" << port;
    if (id->verbs) {
        std::cout << " (dev = " << id->verbs->device->dev_name << ")";
    }
    std::cout << '\n';

    LOG_ERR_EXIT(rdma_listen(id, connection_backlog), errno,
                 std::system_category());

    size_t nclients = 0;
    std::map<ibv_context*, DeviceContext> contexts;
    while (true) {
        rdma_cm_id* child_id;
        LOG_ERR_EXIT(rdma_get_request(id, &child_id), errno,
                     std::system_category());

        sockaddr_in* child_addr =
            reinterpret_cast<sockaddr_in*>(rdma_get_peer_addr(child_id));
        sockaddr_in* listen_addr =
            reinterpret_cast<sockaddr_in*>(rdma_get_local_addr(child_id));
        std::cout << "#" << nclients << " " << listen_addr->sin_addr << ":"
                  << ntohs(listen_addr->sin_port) << " <- "
                  << psl::terminal::graphic_format::BOLD << child_addr->sin_addr
                  << ":" << ntohs(child_addr->sin_port)
                  << psl::terminal::graphic_format::RESET << '\n';
        nclients++;

        auto context = contexts.find(child_id->verbs);
        if (context == contexts.end()) {
            ibv_mr* mr;
            LOG_ERR_EXIT(!(mr = ibv_reg_mr(child_id->pd, data, size.value,
                                           IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_WRITE |
                                               IBV_ACCESS_REMOTE_READ |
                                               IBV_ACCESS_REMOTE_ATOMIC)),
                         errno, std::system_category());
            context = contexts.insert({child_id->verbs, {*mr, {}}}).first;

            ibv_device_attr& dev_attr = context->second.dev_attr;
            LOG_ERR_EXIT(ibv_query_device(child_id->verbs, &dev_attr), errno,
                         std::system_category());
        }

        ibv_cq* cq;
        LOG_ERR_EXIT(
            !(cq = ibv_create_cq(child_id->verbs, max_send_wr + max_recv_wr,
                                 nullptr, nullptr, 0)),
            errno, std::system_category());

        ibv_qp_init_attr qp_init_attr = {};
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.sq_sig_all = 0;
        qp_init_attr.send_cq = cq;
        qp_init_attr.recv_cq = cq;
        qp_init_attr.cap.max_inline_data = 0;
        qp_init_attr.cap.max_recv_wr = 1;
        qp_init_attr.cap.max_send_wr = 1;
        qp_init_attr.cap.max_recv_sge = 1;
        qp_init_attr.cap.max_send_sge = 1;
        LOG_ERR_EXIT(rdma_create_qp(child_id, child_id->pd, &qp_init_attr),
                     errno, std::system_category());

        ServerConnectionData conn_data;
        conn_data.address = reinterpret_cast<uint64_t>(data);
        conn_data.size = size.value;
        conn_data.rkey = context->second.mr.rkey;
        rdma_conn_param conn_param = {};
        conn_param.private_data = reinterpret_cast<void*>(&conn_data);
        conn_param.private_data_len = sizeof(conn_data);
        conn_param.responder_resources =
            context->second.dev_attr.max_qp_rd_atom;
        conn_param.initiator_depth = context->second.dev_attr.max_qp_rd_atom;
        LOG_ERR_EXIT(rdma_accept(child_id, &conn_param), errno,
                     std::system_category());
    }

    return 0;
}
