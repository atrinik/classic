"""Exercise the successful embedded Python plugin path."""

import Atrinik


# Exercise the real module exports used by content packet helpers.
assert Atrinik.CLIENT_CMD_NOTIFICATION == 26
assert Atrinik.CLIENT_CMD_MAPSTATS == 12


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
if inserted != monster or not monster.inv:
    raise RuntimeError("Map.InsertMonster did not insert the generated monster")

# Persistent non-player treasure needs the merchant-stock reason and no currency.
merchant = test_map.CreateObject("raas", 0, 3)
before = len(merchant.inv)
for kwargs in ({}, {"reason": "script.item-grant"},
               {"reason": "merchant.stock", "flags": Atrinik.GT_ENVIRONMENT}):
    try:
        merchant.CreateTreasure("random_wand", 1, **kwargs)
    except RuntimeError:
        pass
    else:
        raise RuntimeError(f"CreateTreasure stocked persistent state with {kwargs}")
if len(merchant.inv) != before:
    raise RuntimeError("Refused CreateTreasure still changed the inventory")
try:
    merchant.CreateTreasure("random_coin", 1, reason="merchant.stock")
except RuntimeError:
    pass
else:
    raise RuntimeError("CreateTreasure stocked currency into persistent state")
if (len(merchant.inv) != before or
        any(obj.type == Atrinik.Type.MONEY for obj in merchant.inv)):
    raise RuntimeError("Refused currency stock changed the inventory")
merchant.CreateTreasure("random_wand", 1, Atrinik.GT_ONLY_GOOD, reason="merchant.stock")
if not any(obj.type == Atrinik.Type.WAND for obj in merchant.inv):
    raise RuntimeError("CreateTreasure did not stock the merchant")
if any(obj.type == Atrinik.Type.MONEY for obj in merchant.inv):
    raise RuntimeError("Merchant stock contains currency")
merchant.Destroy()
