# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""One-shot destructive visibility callbacks for native lifetime tests."""
import Atrinik

actor = Atrinik.WhoAmI()
observer = Atrinik.WhoIsActivator()
mode = Atrinik.GetOptions()
calls = int(actor.ReadKey("actor_callback_calls") or "0") + 1
actor.WriteKey("actor_callback_calls", str(calls))
deferred = mode in ("destroy_spell_second", "destroy_target_second")
if (not deferred or calls == 2) and actor.ReadKey("actor_callback_done") != "1":
    actor.WriteKey("actor_callback_done", "1")
    if mode == "kick_observer":
        observer.Controller().ExecuteCommand("/kick " + observer.name)
    elif mode in ("recycle_observer", "recycle_target"):
        victim = observer if mode == "recycle_observer" else actor
        origin, x, y = victim.map, victim.x, victim.y
        victim.Destroy()
        replacement = origin.CreateObject("raas", x, y)
        replacement.name = "callback replacement"
        replacement.f_sleep = True
        replacement.hp = 17
        replacement.maxhp = 99
        replacement.Con = 10
        replacement.last_heal = 5
    elif mode == "destroy_spell_second":
        spells = [obj for obj in observer.inv if obj.name == "callback ability"]
        assert len(spells) == 1
        spells[0].Destroy()
    elif mode == "destroy_target_second":
        actor.Destroy()
    elif mode == "destroy_caster":
        casters = [obj for obj in observer.map.Objects(observer.x, observer.y + 1)
                   if obj.name == "callback caster"]
        assert len(casters) == 1
        casters[0].Destroy()
    elif mode != "visible":
        raise ValueError("Unknown callback test mode: " + mode)
Atrinik.SetReturnValue(0)
