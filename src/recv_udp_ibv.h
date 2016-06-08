/* Copyright 2016 SKA South Africa
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 */

#ifndef SPEAD2_RECV_UDP_IBV_H
#define SPEAD2_RECV_UDP_IBV_H

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include "common_features.h"
#if SPEAD2_USE_IBV
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <boost/asio.hpp>
#include "recv_reader.h"
#include "recv_stream.h"

namespace spead2
{
namespace recv
{

namespace detail
{

struct rdma_cm_id_deleter
{
    void operator()(rdma_cm_id *cm_id)
    {
        rdma_destroy_id(cm_id);
    }
};

struct rdma_event_channel_deleter
{
    void operator()(rdma_event_channel *event_channel)
    {
        rdma_destroy_event_channel(event_channel);
    }
};

struct ibv_qp_deleter
{
    void operator()(ibv_qp *qp)
    {
        ibv_destroy_qp(qp);
    }
};

struct ibv_cq_deleter
{
    void operator()(ibv_cq *cq)
    {
        ibv_destroy_cq(cq);
    }
};

struct ibv_mr_deleter
{
    void operator()(ibv_mr *mr)
    {
        ibv_dereg_mr(mr);
    }
};

struct ibv_pd_deleter
{
    void operator()(ibv_pd *pd)
    {
        ibv_dealloc_pd(pd);
    }
};

struct ibv_comp_channel_deleter
{
    void operator()(ibv_comp_channel *comp_channel)
    {
        ibv_destroy_comp_channel(comp_channel);
    }
};

struct ibv_flow_deleter
{
    void operator()(ibv_flow *flow)
    {
        ibv_destroy_flow(flow);
    }
};

} // namespace detail

/**
 * Synchronous or asynchronous stream reader that reads UDP packets using
 * the Infiniband verbs API. It currently only supports multicast IPv4, with
 * no fragmentation, IP header options, or VLAN tags.
 */
class udp_ibv_reader : public reader
{
private:
    struct slot
    {
        ibv_recv_wr wr;
        ibv_sge sge;
    };

    const std::size_t max_size;
    const std::size_t n_slots;
    boost::asio::ip::udp::socket join_socket;
    std::unique_ptr<std::uint8_t[]> buffer;
    std::unique_ptr<rdma_event_channel, detail::rdma_event_channel_deleter> event_channel;
    std::unique_ptr<rdma_cm_id, detail::rdma_cm_id_deleter> cm_id;
    std::unique_ptr<ibv_pd, detail::ibv_pd_deleter> pd;
    std::unique_ptr<ibv_comp_channel, detail::ibv_comp_channel_deleter> comp_channel;
    boost::asio::posix::stream_descriptor comp_channel_wrapper;
    std::unique_ptr<ibv_cq, detail::ibv_cq_deleter> send_cq;
    std::unique_ptr<ibv_cq, detail::ibv_cq_deleter> recv_cq;
    std::unique_ptr<ibv_qp, detail::ibv_qp_deleter> qp;
    std::unique_ptr<ibv_flow, detail::ibv_flow_deleter> flow;
    std::unique_ptr<ibv_mr, detail::ibv_mr_deleter> mr;
    std::unique_ptr<slot[]> slots;
    std::unique_ptr<ibv_wc[]> wc;
    std::atomic<bool> stop_poll;  ///< Signals poll-mode to stop

    static std::unique_ptr<rdma_event_channel, detail::rdma_event_channel_deleter>
    create_event_channel();

    static std::unique_ptr<rdma_cm_id, detail::rdma_cm_id_deleter>
    create_id(rdma_event_channel *event_channel);

    static void bind_address(
        rdma_cm_id *cm_id,
        const boost::asio::ip::address &interface_address);

    static std::unique_ptr<ibv_comp_channel, detail::ibv_comp_channel_deleter>
    create_comp_channel(ibv_context *context);

    static boost::asio::posix::stream_descriptor wrap_comp_channel(
        boost::asio::io_service &io_service, ibv_comp_channel *comp_channel);

    static std::unique_ptr<ibv_cq, detail::ibv_cq_deleter>
    create_cq(
        ibv_context *context, int cqe, ibv_comp_channel *comp_channel, int comp_vector);

    static std::unique_ptr<ibv_pd, detail::ibv_pd_deleter>
    create_pd(ibv_context *context);

    static std::unique_ptr<ibv_qp, detail::ibv_qp_deleter>
    create_qp(ibv_pd *pd, ibv_cq *send_cq, ibv_cq *recv_cq, std::size_t n_slots);

    static std::unique_ptr<ibv_mr, detail::ibv_mr_deleter>
    create_mr(ibv_pd *pd, void *addr, std::size_t length);

    static void init_qp(ibv_qp *qp, int port_num);

    static std::unique_ptr<ibv_flow, detail::ibv_flow_deleter>
    create_flow(ibv_qp *qp, const boost::asio::ip::udp::endpoint &endpoint,
                int port_num);

    static void rtr_qp(ibv_qp *qp);

    static void req_notify_cq(ibv_cq *cq);

    void post_slot(std::size_t index);

    bool process_one_packet(const std::uint8_t *data, std::size_t length);

    void packet_handler(
        const boost::system::error_code &error,
        std::size_t bytes_transferred);

    void enqueue_receive();

public:
    /// Maximum packet size, if none is explicitly passed to the constructor
    static constexpr std::size_t default_max_size = 9200;
    /// Socket receive buffer size, if none is explicitly passed to the constructor
    static constexpr std::size_t default_buffer_size = 8 * 1024 * 1024;

    /**
     * Constructor.
     *
     * @param owner        Owning stream
     * @param endpoint     Multicast group and port
     * @param max_size     Maximum packet size that will be accepted
     * @param buffer_size  Requested memory allocation for work requests
     * @param interface_address  Address of the interface which should join the group and listen for data
     * @param comp_vector  Completion channel vector (interrupt) for asynchronous operation, or
     *                     a negative value to poll continuously. Polling
     *                     should not be used if there are other users of the
     *                     thread pool. If a non-negative value is provided, it
     *                     is taken modulo the number of available completion
     *                     vectors. This allows a number of readers to be
     *                     assigned sequential completion vectors and have them
     *                     load-balanced, without concern for the number
     *                     available.
     *
     * @throws std::invalid_argument If @a endpoint is not an IPv4 multicast address
     * @throws std::invalid_argument If @a interface_address is not an IPv4 address
     */
    udp_ibv_reader(
        stream &owner,
        const boost::asio::ip::udp::endpoint &endpoint,
        const boost::asio::ip::address &interface_address,
        std::size_t max_size = default_max_size,
        std::size_t buffer_size = default_buffer_size,
        int comp_vector = 0);

    virtual void stop() override;
};

} // namespace recv
} // namespace spead2

#endif // SPEAD2_USE_IBV

#endif // SPEAD2_RECV_UDP_IBV_H
