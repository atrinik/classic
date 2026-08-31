"""Exercise the successful embedded Python plugin path."""

import Atrinik


origin = Atrinik.WhoIsActivator().map
test_map = Atrinik.CreateMap(5, 5, "plugin-insert-monster", origin, "sealed", 0)

for args in ((), (1, 2)):
    try:
        test_map.InsertMonster(*args)
    except TypeError:
        pass
    else:
        raise RuntimeError("Map.InsertMonster accepted invalid arguments")

invalid = Atrinik.CreateObject("raas")
invalid.randomitems = "random_coin"
try:
    test_map.InsertMonster(invalid, test_map.width, 0)
except Atrinik.AtrinikError:
    pass
else:
    raise RuntimeError("Map.InsertMonster accepted an invalid coordinate")
invalid.Destroy()

nonmonster = Atrinik.CreateObject("sword")
try:
    test_map.InsertMonster(nonmonster, 0, 0)
except Atrinik.AtrinikError:
    pass
else:
    raise RuntimeError("Map.InsertMonster accepted a non-monster")
nonmonster.Destroy()

existing = test_map.CreateObject("raas", 0, 1)
try:
    test_map.InsertMonster(existing, 0, 1)
except Atrinik.AtrinikError:
    pass
else:
    raise RuntimeError("Map.InsertMonster accepted an already inserted monster")
existing.Destroy()

monster = Atrinik.CreateObject("raas")
monster.randomitems = "random_coin"
inserted = test_map.InsertMonster(monster, 0, 2)
if inserted is not monster or not monster.inv:
    raise RuntimeError("Map.InsertMonster did not insert the generated monster")
