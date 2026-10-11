# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""One-shot destructive visibility callbacks for native lifetime tests."""
import Atrinik

actor = Atrinik.WhoAmI()
observer = Atrinik.WhoIsActivator()
mode = Atrinik.GetOptions()
calls = int(actor.ReadKey("actor_callback_calls") or "0") + 1
actor.WriteKey("actor_callback_calls", str(calls))
trigger_call = 3 if mode == "recycle_observer_third" else (
    2 if mode in ("destroy_spell_second", "destroy_target_second", "kick_observer_second",
                  "destroy_target_reclaim_second") else 1)
if mode == "pickup_veto" or (calls == trigger_call and actor.ReadKey("actor_callback_done") != "1"):
    actor.WriteKey("actor_callback_done", "1")
    if mode.startswith("stop_"):
        players = [obj for obj in actor.map.Objects(actor.x - 1, actor.y)
                   if obj.name == "actor-callback-player"]
        assert len(players) == 1
        player = players[0]
        player.WriteKey("stop_callback_ran", "1")
        if "destroy" in mode:
            actor.Destroy()
        if "reclaim" in mode:
            player.Controller().ExecuteCommand("/memfree objects")
        if "kick" in mode:
            player.Controller().ExecuteCommand("/kick " + player.name)
    elif mode == "pickup_veto":
        observer.WriteKey("pickup_callback_ran", "1")
    elif mode in ("gate_destroy_guard_reclaim", "gate_kick_observer", "gate_visible"):
        players = [obj for obj in actor.map.Objects(actor.x - 1, actor.y)
                   if obj.name == "actor-callback-player"]
        assert len(players) == 1
        player = players[0]
        if mode == "gate_destroy_guard_reclaim":
            guards = [obj for obj in actor.map.Objects(actor.x, actor.y)
                      if obj.name == "callback gate guard"]
            assert len(guards) == 1
            guards[0].Destroy()
            player.Controller().ExecuteCommand("/memfree objects")
        elif mode == "gate_kick_observer":
            player.Controller().ExecuteCommand("/kick " + player.name)
    elif mode in ("kick_observer", "kick_observer_second"):
        observer.Controller().ExecuteCommand("/kick " + observer.name)
    elif mode == "teleport_observer":
        observer.TeleportTo(actor.ReadKey("callback_destination"), 17, 17)
    elif mode in ("kick_target", "kick_target_reclaim"):
        observer.Controller().ExecuteCommand("/kick " + actor.name)
        if mode == "kick_target_reclaim":
            observer.Controller().ExecuteCommand("/memfree objects")
    elif mode == "kick_caster":
        observer.Controller().ExecuteCommand("/kick " + actor.ReadKey("callback_caster"))
    elif mode in ("recycle_observer", "recycle_observer_third", "recycle_target"):
        victim = actor if mode == "recycle_target" else observer
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
    elif mode in ("destroy_target_reclaim", "destroy_target_reclaim_second"):
        actor.Destroy()
        observer.Controller().ExecuteCommand("/memfree objects")
    elif mode == "destroy_target_second":
        actor.Destroy()
    elif mode == "destroy_caster":
        casters = [obj for obj in observer.map.Objects(observer.x, observer.y + 1)
                   if obj.name == "callback caster"]
        assert len(casters) == 1
        casters[0].Destroy()
    elif mode != "visible":
        raise ValueError("Unknown callback test mode: " + mode)
Atrinik.SetReturnValue(1 if mode.endswith("_handled") or mode == "pickup_veto" else 0)
