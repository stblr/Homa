/* Copyright (c) 2020, Stanford University
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <iostream>
#include <thread>
#include <vector>

#include <numa.h>

#include <Homa/Homa.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Cycles.h>
#include <TimeTrace.h>
#include <docopt.h>

#include "Output.h"

static const char USAGE[] = R"(DPDK Driver Test.

    Usage:
        dpdk_test [options] <port> <size> <frequency> <threads> (--server | <server_address>)

    Options:
        -h --help           Show this screen.
        --version           Show version.
)";

int
main(int argc, char* argv[])
{
    std::map<std::string, docopt::value> args =
            docopt::docopt(USAGE, {argv + 1, argv + argc},
                    true,                       // show help if requested
                    "DPDK Driver Test");        // version string

    int size = args["<size>"].asLong();
    int frequency = args["<frequency>"].asLong();
    int port = args["<port>"].asLong();
    int threads = 1;
    bool isServer = args["--server"].asBool();
    std::string server_address_string;
    if (!isServer) {
        threads = args["<threads>"].asLong();
        server_address_string = args["<server_address>"].asString();
    }

    Homa::Drivers::DPDK::DpdkDriver driver(port, threads);
    numa_run_on_node(0);
    std::unique_ptr<Homa::Transport> transport(Homa::Transport::create(&driver, 0));
    uint64_t duration = 10;

    if (isServer) {
        std::cout << "Server address: " << driver.addressToString(driver.getLocalAddress())
                 << " size: " << size
                 << " frequency: " << frequency
                 << " port: " << port
                 << std::endl;

        uint64_t count = frequency * duration;
        int64_t max = -1;
        uint64_t total_start;

        while (true) {
            Homa::unique_ptr<Homa::InMessage> in = transport->receive();
            if (in != nullptr) {
                in->acknowledge();
                uint64_t in_id;
                in->get(0, &in_id, sizeof(in_id));
                if (in_id % 100 == 0) { std::cout << in_id << std::endl; };
                if (in_id == 0) {
                    max = 0;
                    total_start = PerfUtils::Cycles::rdtsc();
                } else if (in_id < INT64_MAX) {
                    max = std::max(max, static_cast<int64_t>(in_id));
                    if (max == count - 1) {
                        uint64_t total_stop = PerfUtils::Cycles::rdtsc();
                        double total_time = PerfUtils::Cycles::toSeconds(total_stop - total_start);
                        uint64_t throughput = (count * size) / total_time * 8;
                        std::cout << "Throughput: " << throughput << " b/s" << std::endl;
                    }
                }
            }

            transport->poll();
        }
    } else {
        std::cout << "Client address: " << driver.addressToString(driver.getLocalAddress())
                 << " size: " << size
                 << " frequency: " << frequency
                 << " port: " << port
                 << std::endl;
        Homa::Driver::Address server_address =
           driver.getAddress(&server_address_string);

        while (true) {
            uint64_t out_id = UINT64_MAX;
            Homa::unique_ptr<Homa::OutMessage> out = transport->alloc();
            out->append(&out_id, sizeof(out_id));
            out->send(server_address);

            while (true) {
                transport->poll();
                PerfUtils::Cycles::sleep(1000 * 1000 /* 1 second */);

                switch (out->getStatus()) {
                default:
                    continue;
                case Homa::OutMessage::Status::COMPLETED:
                    break;
                case Homa::OutMessage::Status::FAILED:
                    out = transport->alloc();
                    out->append(&out_id, sizeof(out_id));
                    out->send(server_address);
                    continue;
                }
                break;
            }

            if (out->getStatus() == Homa::OutMessage::Status::COMPLETED)
                break;
        }

        std::cout << "Successfully connected to the server" << std::endl;

        uint64_t duration = 10;
        uint64_t count = frequency * duration / threads;
        uint64_t period = PerfUtils::Cycles::fromSeconds(duration) / count;
        uint64_t total_start = PerfUtils::Cycles::rdtsc();
        std::atomic<uint64_t> total_delay(0);

        auto thread_handles = std::make_unique<std::thread[]>(threads);
        for (int i = 0; i < threads; i++) {
            thread_handles[i] = std::thread{ [&, i]() {
                std::vector<uint8_t> data(size);
                std::fill(data.begin(), data.end(), 0);
                uint64_t thread_delay = 0;

                for (uint64_t j = 0; j < count;) {
                    uint64_t now = PerfUtils::Cycles::rdtsc();
                    int64_t delay = j * period - (now - total_start);
                    if (delay <= 0) {
                        auto out = transport->alloc();
                        uint64_t out_id = j * threads + i;
                        out->append(&out_id, sizeof(out_id));
                        out->append(data.data(), data.size());
                        out->send(server_address);
                        j++;
                        if (j % 100 == 0) { std::cout << j << std::endl; }
                        do {
                            transport->poll();
                        } while (out->getStatus() != Homa::OutMessage::Status::COMPLETED);
                    } else {
                        thread_delay += delay;
                        PerfUtils::Cycles::sleep(PerfUtils::Cycles::toMicroseconds(delay));
                    }
                }

                total_delay += thread_delay;
            } };
        }

        for (int i = 0; i < threads; i++) {
            thread_handles[i].join();
        }

        uint64_t total_stop = PerfUtils::Cycles::rdtsc();
        double total_time = PerfUtils::Cycles::toSeconds(total_stop - total_start);
        uint64_t throughput = (frequency * duration * size) / total_time * 8;
        double load = 1.0 - (double)total_delay / (total_stop - total_start);

        std::cout << "Throughput: " << throughput << " b/s" << std::endl;
        std::cout << "Load: " << load << std::endl;
    }

    return 0;
}
