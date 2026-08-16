#include "sim/command.h"
#include "sim/world.h"

namespace ww::sim {

namespace {
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
} // namespace

void apply_command(World& world, const Command& cmd) {
    std::visit(overloaded{
                   [&](const MoveCommand& c) {
                       world.order_move(world.find_by_id(c.unit_id), c.x, c.y, /*from_queue=*/false,
                                        c.group_speed_px);
                   },
                   [&](const GatherCommand& c) {
                       world.order_gather(world.find_by_id(c.unit_id), world.find_by_id(c.target_id));
                   },
                   [&](const AttackCommand& c) {
                       world.order_attack(world.find_by_id(c.unit_id), world.find_by_id(c.target_id));
                   },
                   [&](const PlaceBuildingCommand& c) {
                       std::vector<EntityRef> builders;
                       builders.reserve(c.builder_ids.size());
                       for (uint32_t id : c.builder_ids) builders.push_back(world.find_by_id(id));
                       EntityRef foundation =
                           world.place_building(c.name, c.team, c.x, c.y, builders, c.assign_builders);
                       // place_building no-ops on an unaffordable or blocked
                       // spot, so an invalid placement simply queues nothing.
                       if (c.queue_build && foundation.valid()) {
                           for (EntityRef b : builders) {
                               Unit* u = world.get(b);
                               if (u && u->common.alive && u->common.team == c.team && u->is_gatherer)
                                   world.queue_order(b, {QueuedOrderKind::Build, 0, 0, foundation});
                           }
                       }
                   },
                   [&](const EnqueueCommand& c) {
                       world.enqueue(world.find_by_id(c.building_id), c.item);
                   },
                   [&](const ResearchCommand& c) {
                       world.control.research(c.key, c.team, world);
                   },
                   [&](const TradeCommand& c) {
                       world.control.trade(c.action, c.resource, c.team, world);
                   },
                   // ---- the actions the client used to perform by hand -----
                   // Each of these is the exact mutation the client's input
                   // handler used to do inline. Moving them here is what makes
                   // the two peers in a lockstep match run identical code from
                   // identical inputs; the client now issues the command and
                   // does nothing else.
                   [&](const RallyCommand& c) {
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive) continue;
                           u->rally = Vec2{c.x, c.y};
                           u->move_goal.reset();
                           u->attack_target = kNullRef;
                           u->gather_target = kNullRef;
                           u->gather_rtype = -1; // see World::order_move
                           u->build_target = kNullRef;
                           u->repair_target = kNullRef;
                           u->forced = false;
                           u->attack_ground.reset();
                           u->hold.reset();
                           u->path.clear();
                           // A fresh direct order discards any shift-queued
                           // follow-ups, same as every other order path.
                           u->order_queue.clear();
                           u->active_queue_watch = kNullRef;
                           u->queue_active = false;
                           // A fresh order un-garrisons a parked plane and
                           // launches it now even at partial fuel (see
                           // Unit::stationed / player_launch).
                           u->stationed = false;
                           u->player_launch = true;
                       }
                   },
                   [&](const AttackGroundCommand& c) {
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive) continue;
                           u->attack_ground = Vec2{c.x, c.y};
                           u->move_goal.reset();
                           u->rally.reset();
                           u->attack_target = kNullRef;
                           u->forced = false;
                           u->stationed = false;
                           u->player_launch = true;
                           u->order_queue.clear();
                           u->active_queue_watch = kNullRef;
                           u->queue_active = false;
                       }
                   },
                   [&](const LoadCommand& c) {
                       EntityRef ship = world.find_by_id(c.transport_id);
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive) continue;
                           u->load_target = ship;
                           u->attack_target = kNullRef;
                           u->gather_target = kNullRef;
                           u->build_target = kNullRef;
                           u->repair_target = kNullRef;
                           u->forced = false;
                       }
                   },
                   [&](const UnloadCommand& c) {
                       world.unload_transport(world.find_by_id(c.transport_id), c.x, c.y);
                   },
                   [&](const PackCommand& c) {
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive || !u->is_ballistic) continue;
                           // Only a launcher that is SETTLED in the opposite
                           // state starts a transition -- re-ordering one that
                           // is already mid-pack would restart its timer.
                           if (u->pack_t > 0.0 || u->packed == c.packed) continue;
                           u->pack_target = c.packed;
                           u->pack_t = 5.0; // 5-second deploy/stow, matches the sim gate
                           u->move_goal.reset();
                           u->attack_target = kNullRef;
                           u->attack_ground.reset();
                           u->rally.reset();
                       }
                   },
                   [&](const LandCommand& c) {
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive || !u->common.is_air) continue;
                           u->land_order = true;
                           u->stationed = true; // garrison there until re-ordered
                           u->attack_target = kNullRef;
                           u->forced = false;
                           u->rally.reset();
                           if (c.airbase_id != 0) u->land_target_id = c.airbase_id;
                       }
                   },
                   [&](const LaunchCommand& c) {
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive) continue;
                           u->stationed = false;
                           u->player_launch = true;
                       }
                   },
                   [&](const DeleteCommand& c) {
                       for (uint32_t id : c.ids) {
                           EntityRef ref = world.find_by_id(id);
                           if (!ref.valid()) continue;
                           if (Unit* u = world.get(ref)) {
                               // deleted => clean removal (no explosion, and it
                               // does not count as a combat loss).
                               u->deleted = true;
                               world.hurt(ref, u->common.hp);
                           } else if (Building* b = world.get_building(ref)) {
                               if (!b->common.alive) continue;
                               // Refund every item still in the production
                               // queue: each was pre-paid at enqueue time, so
                               // deleting the building would otherwise silently
                               // burn the resources of everything it had not
                               // finished. cancel_queue(ref, 0) refunds and pops
                               // the front slot.
                               while (!b->queue.empty()) world.cancel_queue(ref, 0);
                               if (!b->complete) {
                                   // Refund the UN-BUILT portion of the
                                   // building's own cost -- the full price was
                                   // paid when the foundation went down, so a
                                   // fresh foundation refunds in full and a
                                   // nearly-finished one very little. A
                                   // COMPLETED building refunds nothing and
                                   // collapses into rubble instead, which is
                                   // why `deleted` is set only here: it is what
                                   // suppresses the death FX for a cancelled
                                   // foundation. Same pro-rata rule the AI's
                                   // derelict-foundation sweep uses, so player
                                   // and AI cancels cost-match.
                                   double unspent = (100.0 - b->construction) / 100.0;
                                   int team = b->common.team;
                                   if (team >= 0 && team < 8) {
                                       Team& t = world.control.teams[team];
                                       for (auto& [k, v] : world.control.cost_of(b->name, team))
                                           t.res[k] += v * unspent;
                                   }
                                   b->deleted = true;
                               }
                               world.hurt(ref, b->common.hp);
                           }
                       }
                   },
                   [&](const RepairCommand& c) {
                       EntityRef target = world.find_by_id(c.building_id);
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive || !u->is_gatherer) continue;
                           u->repair_target = target;
                           u->gather_target = kNullRef;
                           u->gather_rtype = -1;
                           u->build_target = kNullRef;
                           u->attack_target = kNullRef;
                           u->forced = false;
                           u->rally.reset();
                           u->move_goal.reset();
                           u->path.clear();
                           u->approach_prev_pos.reset();
                           u->approach_progress_check_t = 0.0;
                           u->approach_target.reset();
                           // Repair is not shift-queueable -- always immediate.
                           u->order_queue.clear();
                           u->active_queue_watch = kNullRef;
                           u->queue_active = false;
                       }
                   },
                   [&](const AssignBuildCommand& c) {
                       EntityRef foundation = world.find_by_id(c.foundation_id);
                       for (uint32_t id : c.unit_ids) {
                           Unit* u = world.get(world.find_by_id(id));
                           if (!u || !u->common.alive || !u->is_gatherer) continue;
                           u->build_target = foundation;
                           u->gather_target = kNullRef;
                           u->gather_rtype = -1;
                           u->repair_target = kNullRef;
                           u->attack_target = kNullRef;
                           u->forced = false;
                           u->rally.reset();
                           u->move_goal.reset();
                           u->path.clear();
                           u->approach_prev_pos.reset();
                           u->approach_progress_check_t = 0.0;
                           u->approach_target.reset();
                           u->order_queue.clear();
                           u->active_queue_watch = kNullRef;
                           u->queue_active = false;
                       }
                   },
                   [&](const CancelQueueCommand& c) {
                       world.cancel_queue(world.find_by_id(c.building_id), c.index);
                   },
                   [&](const TeamToggleCommand& c) {
                       if (c.team < 0 || c.team >= 8) return;
                       Team& t = world.control.teams[c.team];
                       if (c.what == "replant") t.replant = !t.replant;
                       else if (c.what == "blitz") t.blitz = !t.blitz;
                   },
                   [&](const QueueOrderCommand& c) {
                       QueuedOrder q;
                       q.kind = static_cast<QueuedOrderKind>(c.kind);
                       q.x = c.x;
                       q.y = c.y;
                       q.target = c.target_id ? world.find_by_id(c.target_id) : kNullRef;
                       world.queue_order(world.find_by_id(c.unit_id), q);
                   },
               },
               cmd);
}

} // namespace ww::sim
