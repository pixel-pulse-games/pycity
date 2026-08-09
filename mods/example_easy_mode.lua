-- Demonstrates: reading an existing default before overwriting it,
-- straightforward overwrites, and print() for sanity-checking your own
-- mod while it loads (shows up in the terminal/console the game was
-- launched from - there's no in-game console yet).

-- Building costs, halved.
pycity.cost_house = pycity.cost_house / 2
pycity.cost_factory = pycity.cost_factory / 2
pycity.cost_farm = pycity.cost_farm / 2
pycity.cost_police = pycity.cost_police / 2

-- Start with more cash in the bank.
pycity.starting_money = pycity.starting_money * 3

-- Crime grows more slowly and police cover more ground, so ignoring
-- police stations for a while is less punishing.
pycity.crime_growth_per_frame = pycity.crime_growth_per_frame * 0.5
pycity.police_coverage_radius = pycity.police_coverage_radius * 2

print("[example_easy_mode] loaded - costs halved, 3x starting money, easier crime")
