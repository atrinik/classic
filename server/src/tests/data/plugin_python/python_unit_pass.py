# A successful embedded Python unit entry point returns normally.

import Atrinik


test_map = Atrinik.CreateMap(3, 3, "object-type-update")
test_object = test_map.CreateObject("sign", 1, 1)

# PLAYER is normalized for ordinary objects before map-side bookkeeping uses
# player-only state, and the object remains on the same map square.
test_object.type = Atrinik.Type.PLAYER
assert test_object.type == Atrinik.Type.MONSTER
assert test_object.map == test_map
assert (test_object.x, test_object.y) == (1, 1)

# A rejected assignment must not disturb the mapped object either.
try:
    test_object.type = "not-an-object-type"
except TypeError:
    pass
else:
    raise AssertionError("invalid object type assignment succeeded")

assert test_object.type == Atrinik.Type.MONSTER
assert test_object.map == test_map
assert (test_object.x, test_object.y) == (1, 1)
