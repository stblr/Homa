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
#include <vector>

#include <Homa/Homa.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Cycles.h>
#include <TimeTrace.h>
#include <docopt.h>

#include "Output.h"

static const char USAGE[] = R"(DPDK Driver Test.

    Usage:
        dpdk_test [options] <port> <size> <frequency> (--server | <server_address>)

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
    bool isServer = args["--server"].asBool();
    std::string server_address_string;
    if (!isServer) {
        server_address_string = args["<server_address>"].asString();
    }

    Homa::Drivers::DPDK::DpdkDriver driver(port);
    std::unique_ptr<Homa::Transport> transport(Homa::Transport::create(&driver, 0));

    if (isServer) {
        std::cout << "Server address: " << driver.addressToString(driver.getLocalAddress())
                 << std::endl;

        while (true) {
            Homa::unique_ptr<Homa::InMessage> in = transport->receive();
            if (in != nullptr) {
                in->acknowledge();
                uint64_t id;
                in->get(0, &id, sizeof(id));

                Homa::unique_ptr<Homa::OutMessage> out = transport->alloc();
                out->append(&id, sizeof(id));
                std::vector<uint8_t> data(size);
                std::fill(data.begin(), data.end(), 0);
                out->append(data.data(), data.size());
                out->send(in->address());
            }

            transport->poll();
        }
    } else {
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
                case Homa::OutMessage::Status::CANCELED:
                case Homa::OutMessage::Status::COMPLETED:
                case Homa::OutMessage::Status::FAILED:
                    break;
                default:
                    continue;
                }
                break;
            }

            if (out->getStatus() == Homa::OutMessage::Status::COMPLETED)
                break;
        }

        std::cout << "Successfully connected to the server" << std::endl;

        uint64_t duration = 10;
        uint64_t count = frequency * duration;
        uint64_t period = PerfUtils::Cycles::fromSeconds(duration) / count;
        uint64_t total_start = PerfUtils::Cycles::rdtsc();
        std::vector<Output::Latency> times;

        uint64_t out_id = 0;
        std::map<uint64_t, uint64_t> start_map;
        while (out_id < count || !start_map.empty()) {
            if (out_id < count && PerfUtils::Cycles::rdtsc() - total_start >= out_id * period) {
                uint64_t start = PerfUtils::Cycles::rdtsc();
                start_map[out_id] = start;
                Homa::unique_ptr<Homa::OutMessage> out = transport->alloc();
                out->append(&out_id, sizeof(out_id));
                std::vector<uint8_t> data(size);
                std::fill(data.begin(), data.end(), 0);
                out->append(data.data(), data.size());
                out->send(server_address);
                out_id++;
            }

            Homa::unique_ptr<Homa::InMessage> in = transport->receive();
            if (in != nullptr) {
                in->acknowledge();
                uint64_t in_id;
                in->get(0, &in_id, sizeof(in_id));
                auto it = start_map.find(in_id);
                if (it != start_map.end()) {
                    uint64_t start = it->second;
                    start_map.erase(it);
                    uint64_t stop = PerfUtils::Cycles::rdtsc();
                    times.emplace_back(PerfUtils::Cycles::toSeconds(stop - start));
                }
            }

            transport->poll();
        }

        uint64_t total_stop = PerfUtils::Cycles::rdtsc();
        double total_time = PerfUtils::Cycles::toSeconds(total_stop - total_start);
        double throughput = (count * size) / total_time * 8;

        std::cout << Output::basicHeader() << std::endl;
        std::cout << Output::basic(times, "Homa Messages") << std::endl;
        std::cout << "Throughput: " << throughput << "b/s" << std::endl;
    }

    return 0;
}
