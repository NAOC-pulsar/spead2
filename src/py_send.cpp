/**
 * @file
 */

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL spead2_ARRAY_API
#define NO_IMPORT_ARRAY
#include <boost/python.hpp>
#include <stdexcept>
#include <mutex>
#include <utility>
#include <memory>
#include "send_heap.h"
#include "send_stream.h"
#include "send_udp.h"
#include "send_streambuf.h"
#include "common_thread_pool.h"
#include "py_common.h"

namespace py = boost::python;

namespace spead
{
namespace send
{

class heap_wrapper : public heap
{
private:
    std::vector<buffer_view> item_buffers;

public:
    using heap::heap;
    void add_item(py::object item);
    void add_descriptor(py::object descriptor);
};

void heap_wrapper::add_item(py::object item)
{
    std::int64_t id = py::extract<std::int64_t>(item.attr("id"));
    py::object buffer = item.attr("to_buffer")();
    bool is_variable_size = py::extract<bool>(item.attr("is_variable_size")());
    item_buffers.emplace_back(buffer);
    const auto &view = item_buffers.back().view;
    heap::add_item(id, view.buf, view.len, !is_variable_size);
}

void heap_wrapper::add_descriptor(py::object object)
{
    heap::add_descriptor(py::extract<descriptor>(object.attr("to_raw")(get_bug_compat())));
}

class packet_generator_wrapper : public packet_generator
{
public:
    using packet_generator::packet_generator;

    bytestring next();
};

bytestring packet_generator_wrapper::next()
{
    packet pkt = next_packet();
    if (pkt.buffers.empty())
        throw stop_iteration();
    return bytestring(boost::asio::buffers_begin(pkt.buffers),
                      boost::asio::buffers_end(pkt.buffers));
}

template<typename Base>
class stream_wrapper : public Base
{
public:
    using Base::Base;

    /// Sends heap synchronously
    void send_heap(const heap_wrapper &h)
    {
        /* A simple future-promise sync here works, but prevents interruption
         * via KeyboardInterrupt. The semaphore needs to be in shared_ptr because
         * if we are interrupted it still needs to exist until the heap is sent.
         */
        auto sent_sem = std::make_shared<semaphore_gil>();
        Base::async_send_heap(h, [sent_sem]()
        {
            sent_sem->put();
        });
        while (sent_sem->get() == -1)
        {
            // retry if interrupted for other reason
        }
    }
};

template<typename Base>
class asyncio_stream_wrapper : public Base
{
private:
    semaphore_gil sem;
    std::vector<py::object> callbacks;
    std::mutex callbacks_mutex;
public:
    using Base::Base;

    int get_fd() const { return sem.get_fd(); }

    void async_send_heap(py::object h, py::object callback)
    {
        // Note that while h isn't used in the lambda, it is
        // bound to it so that its lifetime persists.
        py::extract<heap_wrapper &> h2(h);
        Base::async_send_heap(h2(), [this, callback, h] () mutable
        {
            {
                std::unique_lock<std::mutex> lock(callbacks_mutex);
                callbacks.push_back(std::move(callback));
            }
            sem.put();
        });
    }

    void process_callbacks()
    {
        sem.get();
        std::vector<py::object> current_callbacks;
        {
            std::unique_lock<std::mutex> lock(callbacks_mutex);
            current_callbacks.swap(callbacks);
        }
        for (const py::object &callback : current_callbacks)
        {
            callback();
        }
    }
};

template<typename Base>
class udp_stream_wrapper : public Base
{
private:
    static boost::asio::ip::udp::endpoint make_endpoint(
        boost::asio::io_service &io_service, const std::string &hostname, int port);

public:
    udp_stream_wrapper(
        thread_pool &pool,
        const std::string &hostname,
        int port,
        const stream_config &config = stream_config(),
        std::size_t buffer_size = Base::default_buffer_size)
        : Base(
            pool.get_io_service(),
            make_endpoint(pool.get_io_service(), hostname, port),
            config, buffer_size)
    {
    }
};

template<typename Base>
boost::asio::ip::udp::endpoint udp_stream_wrapper<Base>::make_endpoint(
    boost::asio::io_service &io_service, const std::string &hostname, int port)
{
    using boost::asio::ip::udp;
    udp::endpoint endpoint(boost::asio::ip::address_v4::any(), port);
    udp::resolver resolver(io_service);
    udp::resolver::query query(hostname, "", udp::resolver::query::address_configured);
    endpoint.address(resolver.resolve(query)->endpoint().address());
    return endpoint;
}

class bytes_stream : private std::stringbuf, public stream_wrapper<streambuf_stream>
{
public:
    bytes_stream(thread_pool &pool, const stream_config &config = stream_config())
        : stream_wrapper<streambuf_stream>(pool.get_io_service(), *this, config)
    {
    }

    bytestring getvalue() const
    {
        return str();
    }
};

/// Register the send module with Boost.Python
void register_module()
{
    using namespace boost::python;
    using namespace spead::send;

    // Create the module, and set it as the current boost::python scope so that
    // classes we define are added to this module rather than the root.
    py::object module(py::handle<>(py::borrowed(PyImport_AddModule("spead2._send"))));
    py::scope scope = module;

    class_<heap_wrapper, boost::noncopyable>("Heap", init<std::int64_t, int, bug_compat_mask>(
            (arg("cnt") = 0, arg("heap_address_bits") = heap::default_heap_address_bits, arg("bug_compat") = 0)))
        .add_property("cnt", &heap_wrapper::get_cnt, &heap_wrapper::set_cnt)
        .add_property("heap_address_bits", &heap_wrapper::get_heap_address_bits)
        .add_property("bug_compat", &heap_wrapper::get_bug_compat)
        .def_readonly("DEFAULT_HEAP_ADDRESS_BITS", &heap_wrapper::default_heap_address_bits)
        .def("add_item", &heap_wrapper::add_item,
             arg("item"),
             with_custodian_and_ward<1, 2>())
        .def("add_descriptor", &heap_wrapper::add_descriptor,
             (arg("descriptor")))
        .def("add_end", &heap_wrapper::add_end);

    class_<packet_generator_wrapper, boost::noncopyable>("PacketGenerator", init<heap_wrapper &, std::size_t>(
            (arg("heap"), arg("max_packet_size")))[
            with_custodian_and_ward<1, 2>()])
        .def("__iter__", objects::identity_function())
        .def(
#if PY_MAJOR_VERSION >= 3
              // Python 3 uses __next__ for the iterator protocol
              "__next__"
#else
              "next"
#endif
              , &packet_generator_wrapper::next);

    class_<stream_config>("StreamConfig", init<
            std::size_t, double, std::size_t, std::size_t>(
                (arg("max_packet_size") = stream_config::default_max_packet_size,
                 arg("rate") = 0.0,
                 arg("burst_size") = stream_config::default_burst_size,
                 arg("max_heaps") = stream_config::default_max_heaps)))
        .add_property("max_packet_size", &stream_config::get_max_packet_size, &stream_config::set_max_packet_size)
        .add_property("rate", &stream_config::get_rate, &stream_config::set_rate)
        .add_property("burst_size", &stream_config::get_burst_size, &stream_config::set_burst_size)
        .add_property("max_heaps", &stream_config::get_max_heaps, &stream_config::set_max_heaps);

    {
        typedef udp_stream_wrapper<stream_wrapper<udp_stream> > T;
        class_<T, boost::noncopyable>("UdpStream", init<
                thread_pool_wrapper &, std::string, int, const stream_config &, std::size_t>(
                    (arg("thread_pool"), arg("hostname"), arg("port"),
                     arg("config") = stream_config(),
                     arg("buffer_size") = T::default_buffer_size))[
                with_custodian_and_ward<1, 2>()])
            .def("send_heap", &T::send_heap, arg("heap"));
    }

    {
        typedef udp_stream_wrapper<asyncio_stream_wrapper<udp_stream> > T;
        class_<T, boost::noncopyable>("UdpStreamAsyncio", init<
                thread_pool_wrapper &, std::string, int, const stream_config &, std::size_t>(
                    (arg("thread_pool"), arg("hostname"), arg("port"),
                     arg("config") = stream_config(),
                     arg("buffer_size") = T::default_buffer_size))[
                with_custodian_and_ward<1, 2>()])
            .add_property("fd", &T::get_fd)
            .def("async_send_heap", &T::async_send_heap, arg("heap"))
            .def("flush", &T::flush)
            .def("process_callbacks", &T::process_callbacks);
    }

    class_<bytes_stream, boost::noncopyable>("BytesStream", init<
                thread_pool_wrapper &, const stream_config &>(
                    (arg("thread_pool"), arg("config") = stream_config()))[
                with_custodian_and_ward<1, 2>()])
        .def("getvalue", &bytes_stream::getvalue)
        .def("send_heap", &bytes_stream::send_heap, arg("heap"));
}

} // namespace send
} // namespace spead
