#pragma once
#include <string>
#include <unordered_map>

// Command-card hover tooltips: (title, description) per catalog item name,
// transcribed verbatim from assets/gmk/scripts/get_item_description.gml
// (the "desc" strings there) -- that script keyed off icon sprite, but
// every branch's icon 1:1-maps to the item name already passed to
// cost_string(), so keying by item name here instead lets the client look
// these up with the same catalog item names it already carries around
// (see GameClient::draw_command_card's item_icon lookup), no separate
// icon->item translation needed.
//
// The four command-card buttons that aren't real catalog items (build
// category pickers, attack-move, the eco/military back-arrow) are keyed by
// their CardButton::kind string instead, since they have no catalog entry.
namespace ww::client {

struct ItemTooltip {
    std::string title;
    std::string desc;
};

inline const std::unordered_map<std::string, ItemTooltip>& item_tooltips() {
    static const std::unordered_map<std::string, ItemTooltip> table = {
        // Per-civ unique fortress techs + Conscription (Scientific era).
        {"emergency fighter program", {"Research Emergency Fighter Program",
            "Germany's unique technology. Your fighter planes cost wood instead of iron."}},
        {"meiji restoration", {"Research Meiji Restoration",
            "Japan's unique technology. Your samurai cost FOOD instead of iron."}},
        {"420mm mortar", {"Research 5-Year Plan",
            "The Soviet Union's unique technology. Factory, airbase and artillery units cost 20% less, "
            "and your civilians gather wood, oil and iron 3% faster."}},
        {"maginot line", {"Research Maginot Line",
            "France's unique technology. Your walls, towers and fortresses have 33% more hit "
            "points, and your fortresses shoot 3 tiles further. Applies to the defences you have "
            "already built as well as new ones."}},
        {"conscription", {"Research Conscription",
            "Your land military units are trained 33% faster."}},
        {"naval hegemony", {"Research Naval Hegemony",
            "The United Kingdom's unique technology. All your ships move 15% faster, and your "
            "military ships also deal 15% more damage."}},
        {"aircraft carrier", {"Train Aircraft Carrier",
            "A mobile airbase at sea. Your planes can land on it to refuel and rearm, letting air "
            "groups operate far from land. Big, slow, and tough; parks up to 5 planes. Only the "
            "United Kingdom, USA, Japan, Italy and France can build one."}},
        {"aircraft carrier2", {"Train Supercarrier",
            "Upgraded aircraft carrier: more hit points, deck space (parks 7 planes), and a single "
            "AA gun to swat aircraft. A floating airbase that refuels and rearms your planes at sea."}},
        {"aircraft carrier upgrade", {"Research Supercarrier Upgrade",
            "Upgrades all your Aircraft Carriers into Supercarriers -- tougher, an extra plane of "
            "deck space, and an AA gun. Not available to Italy."}},
        {"house", {"Build House", "Houses population. Drop off point for wood and food."}},
        {"base", {"Build Capitol", "Centre of civilisation"}},
        {"barracks", {"Build Barracks", "Requires Academy. Train ranged infantry"}},
        {"academy", {"Build Academy", "Train melee infantry"}},
        {"palisade", {"Build Palisade Wall", "Wooden wall that is cheap and quick to build. "
            "Click and drag to build a line of segments."}},
        {"iron wall", {"Build Iron Wall", "Heavily fortified stone-and-iron wall with very high HP. "
            "Slow to build. Click and drag to build a line of segments."}},
        {"civilian", {"Train Civilian", "Collects resources"}},
        {"cavalry", {"Train Cavalry", "Mobile unit"}},
        {"cavalry2", {"Train Cavalry", "Armored mobile unit"}},
        {"cavalry3", {"Train Heavy Cavalry", "Heavily armored mobile unit"}},
        {"swordsman2 upgrade", {"Research Swordmastery",
            "Upgrade swordsman into more powerful, armored swordmasters."}},
        {"heavy bomber upgrade", {"Research Heavy Bomber",
            "Upgrades bomber into heavy bombers, which have more hitpoints, pierce armor, bomb "
            "carry capacity and fuel efficiency."}},
        {"cavalry2 upgrade", {"Research Heavy Cavalry",
            "Upgrade cavalry to become armored and more powerful."}},
        {"cavalry3 upgrade", {"Upgrade to Heavy Cavalry",
            "Upgrade cavalry further into elite, heavily armored heavy cavalry."}},
        {"infantryman upgrade", {"Research Infantryman",
            "Upgrade riflemen into infantrymen. They are more powerful and have faster reload."}},
        {"fighter", {"Create Fighter",
            "Quick and nimble aeroplane with twin machine guns. Attack bonus vs. other planes."}},
        {"infantryman", {"Create Infantryman",
            "Expert infantry unit carrying a rifle with a fast rate of fire."}},
        {"waffen", {"Create Waffen SS",
            "A powerful specialist infantry unit that underwent training in Nazi military tactics. "
            "+5 bonus damage vs. villagers."}},
        {"elite waffen", {"Create Elite Waffen SS",
            "A powerful specialist infantry unit that underwent advanced training in Nazi military "
            "tactics. +5 bonus damage vs. villagers."}},
        {"royal marine", {"Create Royal Marine",
            "The United Kingdom's unique rifle infantry. +5 bonus damage vs. cavalry, and each one "
            "takes only half a slot aboard a transport ship."}},
        {"elite royal marine", {"Create Elite Royal Marine",
            "The UK's elite rifle infantry: tougher and harder-hitting. +7 bonus damage vs. cavalry, "
            "and each one takes only half a slot aboard a transport ship."}},
        {"royal marine upgrade", {"Upgrade to Elite Royal Marine",
            "Upgrades your Royal Marines into Elite Royal Marines -- more HP, armour, attack, and a "
            "bigger bonus vs. cavalry."}},
        {"gasoline", {"Research Gasoline", "Improves the fuel efficiency of planes by 40%"}},
        {"radar", {"Research Radar",
            "Increases the line of sight of your towers, outposts, bases and fortresses by 3 tiles, "
            "and your ships and planes by 2 tiles. It also adds +1 tile of attack range to those "
            "defensive buildings and to your ships and planes."}},
        {"artillery", {"Create Artillery",
            "Slow moving ranged weapon that launches explosive projectiles. Has a minimum range. "
            "Effective vs. grouped units. Attack bonus vs. tanks."}},
        {"ballistic missile", {"Create Ballistic Missile",
            "A mobile launcher that lobs a devastating long-range missile with a huge blast radius "
            "(bonus damage vs. buildings). Must UNPACK to fire and PACK to move (each takes 5s); it "
            "cannot do both at once. Long reload. Can attack the ground."}},
        {"artillery1", {"Create Artillery",
            "Cheap Victorian-era siege gun. Lobs shells at range but is wildly inaccurate "
            "(most shots scatter wide). Upgrade it in the Industrial era for accuracy and punch."}},
        {"artillery upgrade", {"Research Artillery Upgrade",
            "Upgrades your Field Cannons into full Artillery: far more accurate, tougher, "
            "harder-hitting and longer-ranged."}},
        {"heavy artillery", {"Create Heavy Artillery",
            "A heavier, longer-ranged siege gun that lobs explosive shells with a large blast radius. "
            "Has a minimum range and a bonus vs. tanks. Soviet Union unique."}},
        {"heavy artillery upgrade", {"Research Heavy Artillery Upgrade",
            "Upgrades your Artillery into Heavy Artillery: more health, armour, damage, range and "
            "blast radius. Soviet Union unique."}},
        {"blowback reload", {"Research Blowback Reload", "Rifle units fire faster."}},
        {"synthetic fuel", {"Research Synthetic Fuel", "Improves the fuel efficiency of planes by 60%"}},
        {"elite waffen upgrade", {"Upgrade to Elite Waffen SS",
            "Upgrade Waffen SS to the elite rank. They have more HP, armor and power."}},
        {"camel", {"Camel", "A rider with a large sabre mounted on a camel. Attack bonus vs. cavalry."}},
        {"camel corps", {"Camel Corps",
            "Ottoman Empire unique unit. An elite rider with a large sabre mounted on an armored "
            "camel. Attack bonus vs. cavalry."}},
        {"camel corps upgrade", {"Upgrade to Camel Corps",
            "Upgrade camels into more powerful armored camels, which form a part of the specialist "
            "camelry regiment named Camel Corps."}},
        {"fortress", {"Build Fortress",
            "Defensive fortification that houses artillery cannons to fire upon approaching enemies."}},
        {"aa gun", {"Create AA Gun", "Anti-aircraft gun. Can only destroy aerial targets."}},
        {"flak", {"Create Flak", "Powerful anti-aircraft gun. Can only destroy aerial targets."}},
        {"flak upgrade", {"Upgrade to Flak", "Upgrade your AA guns into Flaks, which are more powerful."}},
        {"flak tower upgrade", {"Upgrade to Flak Tower",
            "Upgrade your towers into Flak Towers, which are more powerful."}},
        {"shipyard", {"Build Shipyard", "Construct military and economic ships."}},
        {"fishing boat", {"Create Fishing Boat", "Gathers food from fish."}},
        {"transport ship", {"Create Transport Ship",
            "Carries up to 50 population of land units across water. Right-click it to load "
            "selected units; use the Unload button, then click a shore to disembark."}},
        {"hydrodynamics", {"Research Hydrodynamics", "All of your ships move 10% faster."}},
        {"naval armour", {"Research Naval Armour",
            "All of your ships gain +10% hull HP and +1/+1 armour."}},
        {"trade agreement", {"Research Trade Agreement",
            "Reduces the market's trading fee to 10%."}},
        {"jet fighter", {"Create Jet Fighter",
            "Powerful fighter with extreme speed and fire power. Attack bonus vs. other planes."}},
        {"ohka", {"Create Ohka",
            "Japanese unique unit. Rocket powered kamikaze attack aircraft. Fast but difficult to "
            "maneuvre and poor range. Must be sacrificed and crash to detonate. Attack bonus vs. ships."}},
        {"biplane", {"Create Biplane",
            "A fixed-wing aircraft with stacked wings. A machine gun can be fired from the cockpit."}},
        {"light tank", {"Create Light Tank",
            "Heavily armored slow moving unit. Attack bonus vs. buildings."}},
        {"janissary", {"Create Janissary",
            "Ottoman Empire unique gunpowder unit. Carries a powerful blunderbuss with a slow reload."}},
        {"royal janissary", {"Create Royal Janissary",
            "Ottoman Empire unique elite gunpowder unit. Carries a powerful blunderbuss with a slow "
            "reload."}},
        {"royal janissary upgrade", {"Upgrade to Royal Janissary",
            "Upgrade Janissaries into Royal Janissaries, which have more armor and are more powerful."}},
        {"swordsman", {"Train Swordsman", "Melee unit"}},
        {"swordsman2", {"Train Swordmaster", "Armored melee unit"}},
        {"soldier", {"Train Soldier", "Ranged unit"}},
        {"uniform", {"Research Uniform", "Civilians gain +10 HP and +1 armor"}},
        {"leather", {"Research Uniform", "Civilians gain +10 HP and +1 armor"}},
        {"irrigation", {"Research Irrigation", "Farms provide +75 food."}},
        {"rifleman upgrade", {"Research Rifleman", "Upgrades muscateers into riflemen."}},
        {"ballistics", {"Research Ballistics", "Projectiles are fired more accurately at moving targets."}},
        {"horse wagon", {"Research Horse Wagon",
            "Civilians carry more resources, move 10% faster, and are 10% more efficient at "
            "collecting food."}},
        {"fertilizer", {"Research Fertilizer", "Farms provide +125 food."}},
        {"smelting", {"Research Smelting", "Improves iron ore mining efficiency by 15%"}},
        {"beneficiation", {"Research Beneficiation", "Improves iron ore mining efficiency by 15%"}},
        {"bomber", {"Create Bomber",
            "A large aircraft that drops an explosive payload. Attack bonus vs. buildings. Large "
            "blast radius. Vulnerable to fighters."}},
        {"heavy bomber", {"Create Heavy Bomber",
            "A large aircraft with strong armor that drops an explosive payload. Attack bonus vs. "
            "buildings. Large blast radius. Vulnerable to fighters."}},
        {"b29", {"B29 Superfortress", "An advanced heavy bomber with superior armor and defenses."}},
        {"battleship", {"Battleship", "A massive ship with an extremely powerful arsenal of missiles."}},
        {"steel plane armor", {"Steel Plane Armor", "Increases the pierce armor of all planes by 2."}},
        {"composite plane armor", {"Composite Plane Armor",
            "Increases the pierce armor of all planes by 2."}},
        {"yamato", {"Yamato",
            "Japanese unique naval unit. The heaviest and most powerful battleship ever constructed, "
            "this leads the Japanese Navy. Attack bonus vs. buildings."}},
        {"destroyer", {"Destroyer",
            "A military ship with two high-power cannons for shooting down enemy ships. Attack "
            "bonus vs. ships."}},
        {"torpedo boat", {"Torpedo Boat",
            "A military ship with a cannon to shoot torpedos at enemy ships. Attack bonus vs. ships."}},
        {"torpedo boat upgrade", {"Torpedo Boat Upgrade",
            "Upgrades frigates into steam driven torpedo boats."}},
        {"battleship upgrade", {"Research Battleship",
            "Enables construction of battleships, the most powerful unit in the navy."}},
        {"nuclear reactor", {"Build Nuclear Reactor",
            "Requires Nuclear Physics. Generates passive energy which is converted into oil for use. "
            "If destroyed by explosives may create a large explosion."}},
        {"destroyer upgrade", {"Destroyer Upgrade",
            "Upgrades torpedo boats into high power war-ready destroyers with dual cannons."}},
        {"frigate", {"Frigate",
            "A military ship with a cannon for shooting down enemy ships. Attack bonus vs. ships."}},
        {"pesticide", {"Research Pesticide", "Farms provide +200 food."}},
        {"vehicle", {"Research Vehicle",
            "Civilians carry more resources, move 10% faster, and are 10% more efficient at "
            "collecting food"}},
        {"steel frame", {"Research Steel Frame", "Buildings have +30% HP."}},
        {"jet engine", {"Research Jet Engine", "Required to build jet-powered aeroplanes"}},
        {"binoculars", {"Research Binoculars", "Infantry gain +2 sight"}},
        {"fighter upgrade", {"Research Fighter", "Upgrade biplanes into monoplanes"}},
        {"jet fighter upgrade", {"Research Jet Fighter", "Upgrade fighters into powerful jet fighters"}},
        {"farm", {"Build Farm", "Grow food"}},
        {"industrial", {"Industrial Era", "Advance to the Industrial Era."}},
        {"war", {"War Era", "Advance to the War Era."}},
        {"refinery", {"Build Refinery",
            "Drop off oil and iron resources. Used to research armor and weapon upgrades."}},
        {"scientific", {"Scientific Era", "Advance to the Scientific Era."}},
        {"bolt action rifle", {"Bolt-action Rifle",
            "Increases the power of firearm units by 1, and the range by 1."}},
        {"semi automatic rifle", {"Semi-automatic Rifle",
            "Increases the power of firearm units by 2, and the range by 1."}},
        {"assault rifle", {"Assault Rifle",
            "Increases the power of firearm units by 2, and the range by 1."}},
        {"muscateer", {"Train Muscateer", "Ranged unit with a long reload time."}},
        {"rifleman", {"Train Rifleman", "Ranged unit carrying a rifle."}},
        {"nuclear physics", {"Research Nuclear Physics",
            "Enables development of technology requiring nuclear power."}},
        {"atomic bomb", {"Create Atomic Bomb",
            "Uses the destructive power of atomic fission to level entire cities. Requires nuclear "
            "physics."}},
        {"electric drill", {"Research Electric Drilling", "Improves oil extraction efficiency by 15%"}},
        {"fracking", {"Research Fracking", "Improves oil extraction efficiency by 15%"}},
        {"factory", {"Build Factory", "Construct tanks"}},
        {"refined steel", {"Research Refined Steel", "Melee units gain +1 attack."}},
        {"electric arc furnace", {"Research Electric Arc Furnace", "Melee units gain +2 attack."}},
        {"alloys", {"Research Alloys", "Melee units gain +1 attack."}},
        {"imports", {"Research Trade Agreement", "Reduces the commodity trading fee to 10%"}},
        {"heavy tank", {"Create Heavy Tank",
            "Extremely powerful tank. Attack bonus vs. buildings. Projectiles have a blast radius. "
            "Tramples forests."}},
        {"tank", {"Create Tank",
            "Slow moving, well armored tank. Attack bonus vs. buildings. Projectiles have a blast "
            "radius."}},
        {"heavy tank upgrade", {"Upgrade to Heavy Tank",
            "Upgrades your tanks to heavy tanks, which have more HP, armor and firepower, and are "
            "able to crush forests."}},
        {"tiger tank", {"Create Tiger I",
            "Nazi Germany unique unit. Extremely powerful heavy tank. Attack bonus vs. buildings. "
            "Tramples forests."}},
        {"tiger2 tank", {"Create Tiger II",
            "Nazi Germany unique unit. Revised Tiger Tank known as the 'King Tiger' with superior "
            "armor and power. Attack bonus vs. buildings. Tramples forests."}},
        {"tiger2 tank upgrade", {"Upgrade to Tiger II",
            "Research design improvements to the Tiger I tank to create the upgraded Tiger II tank "
            "'King Tiger' with more a more powerful cannon and superior armor."}},
        {"university", {"Build University", "Research and develop technology"}},
        {"aa tower", {"Build AA Tower",
            "Defensive tower that can strike both aerial and grounded targets. Attack bonus vs. planes."}},
        {"tower", {"Build Tower",
            "Defensive tower that can strike both aerial and grounded targets. Attack bonus vs. planes."}},
        {"market", {"Build Market",
            "Economic building that can be used to upgrade agriculture and resource collection"}},
        {"power saw", {"Research Power Saw", "Increases wood chopping efficiency by 10%"}},
        {"mobile sawmill", {"Research Mobile Saw Mill", "Increases wood chopping efficiency by 10%"}},
        {"diesel engine", {"Research Diesel Engine", "Increases movement speed of tanks by 20%"}},
        {"airbase", {"Build Airport", "Produce and refuel aeroplanes"}},

        // Fixed command-card buttons with no catalog item (keyed by
        // CardButton::kind instead of an item name).
        {"build_eco", {"Build Economic building",
            "Command the civilian to construct an economic building."}},
        {"build_military", {"Build Military building",
            "Command the civilian to construct a military or defensive building."}},
        {"attack_move", {"Attack Move",
            "Units will move towards the designated area but will stop and attack if they see an "
            "enemy."}},
        {"land", {"Land at Nearest Airbase",
            "Send the selected aircraft to the nearest friendly airbase that has a free slot, and land "
            "there to refuel and rearm."}},
        {"toggle_park_planes", {"Park New Planes",
            "Toggle: when on, planes built at this airbase stay parked here (if a slot is free) instead "
            "of taking off immediately. Turn off to launch new planes straight away."}},
        {"replant", {"Auto-Replant Farms",
            "Toggle: when on, a farm worked to exhaustion is automatically re-sown "
            "for 40 wood (the price of a new farm) so the farmer keeps working without being re-tasked."}},
        {"build_back", {"Next Page", "Open a menu with other buildings to select from."}},
        {"shipyard_page", {"Next Page", "Switch between the shipyard's passive and warship pages."}},
        {"delete", {"Delete Unit", "Kill this unit."}},
    };
    return table;
}

} // namespace ww::client
