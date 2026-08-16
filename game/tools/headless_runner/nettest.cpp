// Loopback self-test for the lockstep multiplayer stack (--nettest).
//
// Stands up BOTH peers inside this one process -- a host on 127.0.0.1 and a
// joiner connecting to it -- and plays a real match through the actual network
// path: TCP framing, the handshake, settings and command serialisation, turn
// scheduling with input delay, and per-turn checksum comparison.
//
// Why this exists rather than "just try it with two PCs": every failure mode
// this stack has is silent. A desync does not crash, a mis-serialised command
// does not throw, and an off-by-one in turn scheduling looks like mild lag
// until it does not. Running both ends over a real socket, with the real
// codec, and asserting the two independently-simulated worlds agree tick for
// tick is the only cheap way to know the protocol is correct before any of it
// is wired to a button.
//
// It is a genuine test of the protocol, but NOT of the client: the commands
// here are injected directly, so it proves the transport and the lockstep
// scheduler, not that every UI action has been converted into a Command.
#include "net/session.h"
#include "net/upnp.h"
#include "sim/command.h"
#include "sim/command_codec.h"
#include "sim/match.h"
#include "sim/world.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ww;

namespace {

// Pick the first free-ish port above the default so a stuck previous run does
// not make the test fail for the wrong reason.
constexpr uint16_t kTestPort = net::kDefaultPort + 7;

sim::SkirmishSettings test_settings() {
    sim::SkirmishSettings s;
    s.n_players = 2;
    s.map_size = 48;
    s.max_pop = 100;
    s.civs = {0, 2};
    s.reveal_mode = 2;
    return s;
}

// A command both peers can issue that actually changes the world: move every
// unit this team owns a little. Uses entity ids, which is what the wire format
// carries, so a mismatch in id handling shows up as a desync.
std::vector<sim::Command> make_commands(sim::World& world, int team, int turn) {
    std::vector<sim::Command> out;
    int n = 0;
    std::vector<uint32_t> builders;
    for (auto ref : world.active_units) {
        sim::Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team != team) continue;
        if (u->is_gatherer && builders.size() < 2) builders.push_back(u->common.id);
        if (++n > 3) continue;
        out.push_back(sim::MoveCommand{u->common.id, u->common.x + 32.0 * ((turn % 3) - 1),
                                       u->common.y + 32.0 * ((turn % 2) ? 1 : -1), -1.0});
    }

    // Exercise the two commands that are NOT simple field writes, because they
    // are the ones a naive test would miss. Placement spends resources and
    // spawns an entity (so an id-allocation difference between the peers shows
    // up immediately); deleting an unfinished building runs the pro-rata refund,
    // which puts RESOURCES back -- a divergence there is invisible on screen and
    // fatal to a match. Driving both here is what turns this from "the transport
    // works" into "these commands are deterministic".
    if (turn > 0 && turn % 7 == 0 && !builders.empty()) {
        sim::Building* home = nullptr;
        for (auto ref : world.active_buildings) {
            sim::Building* b = world.get_building(ref);
            if (b && b->common.alive && b->common.team == team && b->name == "base") { home = b; break; }
        }
        if (home) {
            sim::PlaceBuildingCommand pc;
            pc.team = team;
            pc.name = "house";
            // Walk the spot around so successive placements do not all collide
            // with the first one's footprint.
            pc.x = home->common.x + 160.0 + 96.0 * ((turn / 7) % 3);
            pc.y = home->common.y + 160.0;
            pc.builder_ids = builders;
            pc.assign_builders = true;
            out.push_back(pc);
        }
    }
    // Delete the most recent unfinished house again a few turns later, which
    // triggers the refund path.
    if (turn > 0 && turn % 11 == 0) {
        for (auto ref : world.active_buildings) {
            sim::Building* b = world.get_building(ref);
            if (b && b->common.alive && b->common.team == team && b->name == "house" && !b->complete) {
                out.push_back(sim::DeleteCommand{{b->common.id}});
                break;
            }
        }
    }
    return out;
}

} // namespace

// Round-trip every command type through the wire codec and back, then encode
// the decoded copy again and compare bytes. Any field a new command forgets to
// write, or reads in the wrong order, shows up here as a mismatch -- whereas in
// a real match it would show up as a desync twenty minutes in with no clue as
// to which command was at fault.
bool codec_roundtrip() {
    using namespace ww::sim;
    std::vector<Command> samples = {
        MoveCommand{7, 123.5, -45.25, 12.0},
        GatherCommand{8, 9},
        AttackCommand{10, 11},
        PlaceBuildingCommand{1, "barracks", 640.0, 480.0, {3, 4, 5}, false, true},
        EnqueueCommand{12, "rifleman"},
        ResearchCommand{1, "assault rifle"},
        TradeCommand{0, "sell", "wood"},
        RallyCommand{{1, 2, 3}, 100.0, 200.0},
        AttackGroundCommand{{4, 5}, 300.0, 400.0},
        LoadCommand{{6, 7}, 42},
        UnloadCommand{42, 55.0, 66.0},
        PackCommand{{8}, true},
        LandCommand{{9, 10}, 77},
        LaunchCommand{{11}},
        DeleteCommand{{12, 13, 14}},
        RepairCommand{{15}, 16},
        AssignBuildCommand{{17, 18}, 19},
        CancelQueueCommand{20, 2},
        TeamToggleCommand{1, "replant"},
        QueueOrderCommand{21, 2, 1.5, 2.5, 22},
    };
    int failures = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        ByteWriter w1;
        encode_command(samples[i], w1);
        ByteReader r(w1.bytes.data(), w1.bytes.size());
        Command back;
        if (!decode_command(r, back)) {
            std::printf("  command %zu: DECODE FAILED\n", i);
            ++failures;
            continue;
        }
        if (back.index() != samples[i].index()) {
            std::printf("  command %zu: decoded as a different type (%zu != %zu)\n", i, back.index(),
                        samples[i].index());
            ++failures;
            continue;
        }
        ByteWriter w2;
        encode_command(back, w2);
        if (w1.bytes != w2.bytes) {
            std::printf("  command %zu (type %zu): re-encode differs -- a field is lost or reordered\n",
                        i, samples[i].index());
            ++failures;
        }
    }
    // Every alternative in the variant must be covered, or a command the client
    // can issue would be silently undeliverable.
    size_t kinds = std::variant_size_v<Command>;
    if (samples.size() != kinds) {
        std::printf("  %zu command types exist but only %zu are covered here\n", kinds,
                    samples.size());
        ++failures;
    }
    std::printf("codec round-trip: %zu command types, %d failures\n", samples.size(), failures);
    return failures == 0;
}

int run_nettest(int turns_to_play) {
    std::printf("== lockstep loopback test ==\n");
    if (!codec_roundtrip()) {
        std::printf("FAIL: command wire format is not self-consistent\n");
        return 1;
    }
    std::printf("port %u, %d turns of %d ticks (input delay %d)\n\n", kTestPort, turns_to_play,
                net::kTicksPerTurn, net::kInputDelay);

    net::Session host, joiner;
    sim::SkirmishSettings settings = test_settings();
    const uint64_t seed = 987654321ull;

    if (!host.host(kTestPort, settings, seed)) {
        std::printf("FAIL: could not host: %s\n", host.error().c_str());
        return 1;
    }
    if (!joiner.join("127.0.0.1", kTestPort)) {
        std::printf("FAIL: could not connect: %s\n", joiner.error().c_str());
        return 1;
    }

    // ---- handshake ------------------------------------------------------
    for (int i = 0; i < 500; ++i) {
        host.poll();
        joiner.poll();
        if (host.status() == net::Status::Ready && joiner.status() == net::Status::Ready) break;
        if (host.status() == net::Status::Failed || joiner.status() == net::Status::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (host.status() != net::Status::Ready || joiner.status() != net::Status::Ready) {
        std::printf("FAIL: handshake did not complete (host=%d joiner=%d)\n  host: %s\n  join: %s\n",
                    static_cast<int>(host.status()), static_cast<int>(joiner.status()),
                    host.error().c_str(), joiner.error().c_str());
        return 1;
    }
    std::printf("handshake OK\n");

    // The joiner learned the seed and settings from the host -- check that
    // survived serialisation, because everything downstream assumes it.
    if (joiner.seed() != seed) {
        std::printf("FAIL: seed did not survive the handshake (%llu != %llu)\n",
                    static_cast<unsigned long long>(joiner.seed()),
                    static_cast<unsigned long long>(seed));
        return 1;
    }
    if (joiner.settings().map_size != settings.map_size ||
        joiner.settings().max_pop != settings.max_pop ||
        joiner.settings().civs != settings.civs ||
        joiner.settings().reveal_mode != settings.reveal_mode) {
        std::printf("FAIL: settings did not survive the handshake\n");
        return 1;
    }
    std::printf("seed + settings agreed: seed=%llu map=%d pop=%d civs=%zu\n",
                static_cast<unsigned long long>(joiner.seed()), joiner.settings().map_size,
                joiner.settings().max_pop, joiner.settings().civs.size());

    // ---- two independent matches from the agreed seed --------------------
    sim::Match match_a(seed, settings);
    sim::Match match_b(joiner.seed(), joiner.settings());
    // Neither side is AI: this is a human-vs-human match, and the point is that
    // the ONLY inputs are the commands crossing the socket.
    for (int t = 0; t < 2; ++t) {
        match_a.control().teams[t].is_ai = false;
        match_b.control().teams[t].is_ai = false;
    }
    if (match_a.checksum() != match_b.checksum()) {
        std::printf("FAIL: the two worlds differ before a single turn ran\n"
                    "      (same seed must produce the same world)\n");
        return 1;
    }
    std::printf("initial world checksums match: %llu\n\n",
                static_cast<unsigned long long>(match_a.checksum()));

    host.start_match();
    joiner.start_match();

    // ---- play ------------------------------------------------------------
    const double dt = 0.05; // the sim's fixed tick
    int ran = 0, stalls = 0;
    for (int guard = 0; guard < turns_to_play * 200 && ran < turns_to_play; ++guard) {
        host.poll();
        joiner.poll();

        // Each side issues orders for its own team only -- exactly the
        // restriction the real client is under.
        for (auto& c : make_commands(match_a.world(), 0, ran)) host.submit(c);
        for (auto& c : make_commands(match_b.world(), 1, ran)) joiner.submit(c);

        std::vector<sim::Command> cmds_a, cmds_b;
        net::TurnState sa = host.begin_turn(match_a.checksum(), cmds_a);
        net::TurnState sb = joiner.begin_turn(match_b.checksum(), cmds_b);

        if (sa == net::TurnState::Stopped || sb == net::TurnState::Stopped) break;
        if (sa != net::TurnState::Run || sb != net::TurnState::Run) {
            ++stalls;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Both peers must receive the identical command list, in the identical
        // order. If this ever differs the match is already lost.
        if (cmds_a.size() != cmds_b.size()) {
            std::printf("FAIL: turn %d command lists differ in size (%zu vs %zu)\n", ran,
                        cmds_a.size(), cmds_b.size());
            return 1;
        }

        for (const auto& c : cmds_a) sim::apply_command(match_a.world(), c);
        for (const auto& c : cmds_b) sim::apply_command(match_b.world(), c);
        for (int t = 0; t < net::kTicksPerTurn; ++t) {
            match_a.step(dt);
            match_b.step(dt);
            match_a.events().clear();
            match_b.events().clear();
        }
        host.end_turn();
        joiner.end_turn();
        ++ran;

        if (match_a.checksum() != match_b.checksum()) {
            std::printf("FAIL: DESYNC after turn %d\n  host   %llu\n  joiner %llu\n", ran,
                        static_cast<unsigned long long>(match_a.checksum()),
                        static_cast<unsigned long long>(match_b.checksum()));
            return 1;
        }
        if (ran % 10 == 0)
            std::printf("  turn %3d  checksum %llu  ping %dms\n", ran,
                        static_cast<unsigned long long>(match_a.checksum()), host.ping_ms());
    }

    if (host.status() == net::Status::Desync || joiner.status() == net::Status::Desync) {
        std::printf("FAIL: session reported a desync at turn %llu (%llu vs %llu)\n",
                    static_cast<unsigned long long>(host.desync_turn()),
                    static_cast<unsigned long long>(host.local_checksum()),
                    static_cast<unsigned long long>(host.remote_checksum()));
        return 1;
    }
    if (ran < turns_to_play) {
        std::printf("FAIL: only %d of %d turns ran (host=%d joiner=%d): %s %s\n", ran, turns_to_play,
                    static_cast<int>(host.status()), static_cast<int>(joiner.status()),
                    host.error().c_str(), joiner.error().c_str());
        return 1;
    }

    std::printf("\nPASS: %d turns (%d ticks) played over a real socket, checksums identical\n", ran,
                ran * net::kTicksPerTurn);
    std::printf("      stalled %d poll iterations waiting for the peer\n", stalls);
    host.close();
    joiner.close();
    return 0;
}

// --upnp: probe the router and report, without touching anything else. Purely
// diagnostic -- tells a player whether hosting will work on their network.
int run_upnp_probe() {
    std::printf("== UPnP probe ==\n");
    std::printf("searching for a router (this can take a few seconds)...\n");
    net::PortMapResult r = net::map_port(net::kDefaultPort);
    std::printf("  router found : %s%s%s\n", r.discovered ? "yes" : "no",
                r.router_name.empty() ? "" : " -- ", r.router_name.c_str());
    std::printf("  port mapped  : %s (TCP %u)\n", r.mapped ? "yes" : "no", net::kDefaultPort);
    std::printf("  external IP  : %s\n", r.external_ip.empty() ? "(unknown)" : r.external_ip.c_str());
    std::printf("  local IP     : %s\n", net::local_address().c_str());
    if (!r.error.empty()) std::printf("  problem      : %s\n", r.error.c_str());
    if (r.mapped) {
        std::printf("\nremoving the test mapping again...\n");
        net::unmap_port(net::kDefaultPort);
        std::printf("done. Hosting should work on this network.\n");
    } else {
        std::printf("\nHosting would need the port forwarded manually,\n"
                    "or the other player to host instead.\n");
    }
    return r.mapped ? 0 : 1;
}
