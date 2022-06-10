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
#include <random>
#include <thread>
#include <vector>

#include <numa.h>

#include <Homa/Homa.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Cycles.h>
#include <TimeTrace.h>
#include <docopt.h>

#include "Output.h"

static const char USAGE[] = R"(size_test

    Usage:
        size_test [options] <port> <frequency> <threads> (--server | <server_address>)

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
                    "send_test");               // version string

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
                 << " frequency: " << frequency
                 << " port: " << port
                 << std::endl;

        uint64_t count = frequency * duration;
        uint64_t total_size;
        uint64_t step = count / 20;
        uint64_t total_start;
        uint64_t start = PerfUtils::Cycles::rdtsc();
        std::vector<Output::Latency> times(count);

        while (true) {
            Homa::unique_ptr<Homa::InMessage> in = transport->receive();
            if (in != nullptr) {
                in->acknowledge();
                total_size += in->length();
                uint64_t stop = PerfUtils::Cycles::rdtsc();
                double time = PerfUtils::Cycles::toSeconds(stop - start);
                times.push_back(Output::Latency(time));
                uint64_t in_id;
                in->get(0, &in_id, sizeof(in_id));
                if ((in_id + 1) % step == 0) { std::cout << in_id + 1 << std::endl; };
                if (in_id == 0) {
                    total_size = 0;
                    total_start = stop;
                    times.clear();
                } else if (in_id == count) {
                    uint64_t total_stop = stop;
                    double total_time = PerfUtils::Cycles::toSeconds(total_stop - total_start);
                    uint64_t throughput = total_size / total_time * 8;

                    std::cout << Output::basicHeader() << std::endl;
                    std::cout << Output::basic(times, "send_test") << std::endl;
                    std::cout << "Throughput: " << throughput << " b/s" << std::endl;
                }
                start = stop;
            }

            transport->poll();
        }
    } else {
        std::cout << "Client address: " << driver.addressToString(driver.getLocalAddress())
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

            break;
        }

        std::cout << "Successfully connected to the server" << std::endl;

        uint64_t duration = 10;
        uint64_t count = frequency * duration;
        uint64_t period = PerfUtils::Cycles::fromSeconds(duration) / count;
        uint64_t step = count / 20;
        uint64_t total_start = PerfUtils::Cycles::rdtsc();
        uint64_t total_delay = 0;
        uint64_t sizes[3] = { 100000, 10000, 1000 };
        std::vector<uint8_t> idcs(count);
        std::vector<Output::Latency> times(count);

        std::atomic<uint64_t> status(UINT64_MAX);
        auto thread_handles = std::make_unique<std::thread[]>(threads);
        for (int i = 0; i < threads; i++) {
            thread_handles[i] = std::thread{ [&, i]() {
                std::default_random_engine engine;
                std::uniform_real_distribution<> distribution(0, 1);
                std::vector<uint8_t> data(100000);
                std::fill(data.begin(), data.end(), 0);

                while (true) {
                    uint64_t s = status;
                    if (s == count) {
                        break;
                    } else if (s != UINT64_MAX) {
                        if (status.compare_exchange_weak(s, UINT64_MAX)) {
                            float v = distribution(engine);
                            uint8_t j = v < 0.01f ? 0 : v < 0.1f ? 1 : 2;
                            uint64_t size = sizes[j];
                            auto out = transport->alloc();
                            out->append(&s, sizeof(s));
                            out->append(data.data(), data.size());
                            out->send(server_address);
                            do {
                                transport->poll();
                            } while (out->getStatus() != Homa::OutMessage::Status::COMPLETED);
                            uint64_t start = total_start + s * period;
                            uint64_t stop = PerfUtils::Cycles::rdtsc();
                            double time = PerfUtils::Cycles::toSeconds(stop - start);
                            idcs[s] = j;
                            times[s] = Output::Latency(time);
                        }
                    }
                }
            } };
        }

        for (uint64_t i = 0; i < count;) {
            uint64_t now = PerfUtils::Cycles::rdtsc();
            int64_t delay = i * period - (now - total_start);
            if (delay <= 0) {
                status = i;
                while (status != UINT64_MAX);
                i++;
                if (i % step == 0) { std::cout << i << std::endl; }
            } else {
                total_delay += delay;
                PerfUtils::Cycles::sleep(PerfUtils::Cycles::toMicroseconds(delay));
            }
        }
        status = count;

        for (int i = 0; i < threads; i++) {
            thread_handles[i].join();
        }

        while (true) {
            uint64_t out_id = count;
            Homa::unique_ptr<Homa::OutMessage> out = transport->alloc();
            out->append(&out_id, sizeof(out_id));
            out->send(server_address);

            while (true) {
                transport->poll();

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

           break;
        }

        uint64_t total_stop = PerfUtils::Cycles::rdtsc();
        double total_time = PerfUtils::Cycles::toSeconds(total_stop - total_start);
        uint64_t total_size = 0;
        for (size_t i = 0; i < count; i++) {
            total_size += sizes[idcs[i]];
        }
        uint64_t throughput = (total_size) / total_time * 8;
        double load = 1.0 - (double)total_delay / (total_stop - total_start);

        std::cout << Output::basicHeader() << std::endl;
        for (size_t i = 0; i < 3; i++) {
            std::vector<Output::Latency> size_times;
            for (size_t j = 0; j < count; j++) {
                if (idcs[j] == i) {
                    size_times.push_back(times[j]);
                }
            }
            std::cout << Output::basic(size_times, "size_test") << std::endl;
        }
        std::cout << "Throughput: " << throughput << " b/s" << std::endl;
        std::cout << "Load: " << load << std::endl;
    }

    return 0;
}
