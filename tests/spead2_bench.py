#!/usr/bin/env python

"""Benchmark tool to estimate the sustainable SPEAD bandwidth between two
machines, for a specific set of configurations.

Since UDP is lossy, this is not a trivial problem. We binary search for the
speed that is just sustainable. To make the test at a specific speed more
reliable, it is repeated several times, opening a new stream each time, and
with a delay to allow processors to return to idle states. A TCP control
stream is used to synchronise the two ends. All configuration is done on
the master end.
"""

from __future__ import division, print_function
import numpy as np
import spead2
import spead2.recv
import spead2.recv.trollius
import spead2.send
import spead2.send.trollius
import argparse
import json
import trollius
from trollius import From, Return
import collections
import logging
import traceback

class SlaveConnection(object):
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer

    @trollius.coroutine
    def run_stream(self, stream):
        num_heaps = 0
        while True:
            try:
                heap = yield From(stream.get())
                num_heaps += 1
            except spead2.Stopped:
                raise Return(num_heaps)

    @trollius.coroutine
    def run_control(self):
        try:
            stream_task = None
            while True:
                command = yield From(self.reader.readline())
                logging.debug("command = %s", command)
                if not command:
                    break
                command = json.loads(command)
                if command['cmd'] == 'start':
                    if stream_task is not None:
                        logging.warning("Start received while already running: %s", command)
                        continue
                    args = argparse.Namespace(**command['args'])
                    thread_pool = spead2.ThreadPool()
                    memory_pool = spead2.MemoryPool(
                        args.heap_size, args.heap_size + 1024, args.mem_max_free, args.mem_initial)
                    stream = spead2.recv.trollius.Stream(thread_pool, 0, args.heaps)
                    stream.set_memory_pool(memory_pool)
                    stream.add_udp_reader(args.port, args.packet, args.recv_buffer)
                    thread_pool = None
                    memory_pool = None
                    stream_task = trollius.async(self.run_stream(stream))
                    self.writer.write('ready\n')
                elif command['cmd'] == 'stop':
                    if stream_task is None:
                        log.warn("Stop received when already stopped")
                        continue
                    stream.stop()
                    received_heaps = yield From(stream_task)
                    self.writer.write(json.dumps({'received_heaps': received_heaps}) + '\n')
                    stream_task = None
                    stream = None
                elif command['cmd'] == 'exit':
                    break
                else:
                    logging.warning("Bad command: %s", command)
                    continue
            logging.debug("Connection closed")
            if stream_task is not None:
                stream.stop()
                yield From(stream_task)
        except Exception:
            traceback.print_exc()


@trollius.coroutine
def slave_connection(reader, writer):
    try:
        conn = SlaveConnection(reader, writer)
        yield From(conn.run_control())
    except Exception:
        traceback.print_exc()

def run_slave(args):
    server = yield From(trollius.start_server(slave_connection, port=args.port))
    yield From(server.wait_closed())

@trollius.coroutine
def send_stream(item_group, stream, num_heaps):
    tasks = collections.deque()
    for i in range(num_heaps):
        if len(tasks) >= 2:
            yield From(trollius.wait([tasks.popleft()]))
        if i == num_heaps - 1:
            task = trollius.async(stream.async_send_heap(item_group.get_end()))
        else:
            for item in item_group.values():
                item.version += 1
            task = trollius.async(stream.async_send_heap(item_group.get_heap()))
        tasks.append(task)
    while len(tasks) > 0:
        yield From(trollius.wait([tasks.popleft()]))

def measure_connection_once(args, rate, num_heaps, required_heaps):
    reader, writer = yield From(trollius.open_connection(args.host, args.port))
    writer.write(json.dumps({'cmd': 'start', 'args': vars(args)}) + '\n')
    # Wait for "ready" response
    response = yield From(reader.readline())
    assert response == 'ready\n'
    thread_pool = spead2.ThreadPool()
    config = spead2.send.StreamConfig(
        max_packet_size=args.packet,
        burst_size=args.burst,
        rate=rate)
    stream = spead2.send.trollius.UdpStream(
        thread_pool, args.host, args.port, config, args.send_buffer)
    item_group = spead2.send.ItemGroup(
        flavour=spead2.Flavour(4, 64, args.addr_bits, 0))
    for i in range(1):
        item_group.add_item(id=None, name='Test item {}'.format(i),
                            description='A test item with arbitrary value',
                            shape=(args.heap_size,), dtype=np.uint8,
                            value=np.zeros((args.heap_size,), dtype=np.uint8))
    yield From(send_stream(item_group, stream, num_heaps))
    # Give receiver time to catch up with any queue
    yield From(trollius.sleep(0.1))
    writer.write(json.dumps({'cmd': 'stop'}) + '\n')
    # Read number of heaps received
    response = yield From(reader.readline())
    response = json.loads(response)
    received_heaps = response['received_heaps']
    yield From(trollius.sleep(0.5))
    yield From(writer.drain())
    writer.close()
    raise Return(received_heaps >= required_heaps)


def measure_connection(args, rate, num_heaps, required_heaps):
    total = 0
    for i in range(5):
        total += yield From(measure_connection_once(args, rate, num_heaps, required_heaps))
    raise Return(total >= 3)

def run_master(args):
    # These rates are in bytes
    low = 0.5e9
    high = 5e9
    while high - low > 1e8 / 8:
        # Need at least 1GB of data to overwhelm cache effects, and want at least
        # 1 second for warmup effects.
        rate = (low + high) * 0.5
        num_heaps = int(max(1024**3, rate) / args.heap_size) + 2
        good = yield From(measure_connection(args, rate, num_heaps, num_heaps - 1))
        if not args.quiet:
            print("Rate: {:.3f} Gbps: {}".format(rate * 8 / 1024**3, "GOOD" if good else "BAD"))
        if good:
            low = rate
        else:
            high = rate
    rate = (low + high) * 0.5
    rate_gbps = rate * 8 / 1024**3
    if args.quiet:
        print(rate_gbps)
    else:
        print("Sustainable rate: {} Gbps".format(rate_gbps))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--log', help='Log configuration file')
    subparsers = parser.add_subparsers(title='subcommands')
    master = subparsers.add_parser('master')
    master.add_argument('--quiet', action='store_true', default=False, help='Print only the final result')
    master.add_argument('--packet', type=int, default=9172, help='Maximum packet size to use for UDP')
    master.add_argument('--heap-size', metavar='BYTES', type=int, default=4194304, help='Payload size for heap')
    master.add_argument('--addr-bits', type=int, default=40, help='Heap address bits')
    master.add_argument('--send-buffer', type=int, default=512 * 1024, help='Socket buffer size (sender)')
    master.add_argument('--recv-buffer', type=int, default=8 * 1024**2, help='Socket buffer size (receiver)')
    master.add_argument('--burst', type=int, default=65536, help='Send burst size')
    master.add_argument('--heaps', type=int, default=4, help='Maximum number of in-flight heaps')
    master.add_argument('--mem-max-free', type=int, default=12, help='Maximum free memory buffers')
    master.add_argument('--mem-initial', type=int, default=8, help='Initial free memory buffers')
    master.add_argument('host')
    master.add_argument('port', type=int)
    slave = subparsers.add_parser('slave')
    slave.add_argument('port', type=int)

    args = parser.parse_args()
    if args.log is not None:
        logging.basicConfig(level=getattr(logging, args.log.upper()))
    else:
        logging.basicConfig(level=logging.INFO)
    if 'host' in args:
        task = run_master(args)
    else:
        task = run_slave(args)
    task = trollius.async(task)
    trollius.get_event_loop().run_until_complete(task)
    task.result()

if __name__ == '__main__':
    main()
