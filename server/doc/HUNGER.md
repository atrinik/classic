# Hunger is disabled

Classic players no longer get hungry. This is a deliberate, minimal interim
change: food is expected to become a cooking ingredient with crafting recipes
later, and until then starvation was more of a chore than a challenge.

## Player-visible behavior

- The food stat no longer drains over time, and regenerating hit points or
  mana no longer consumes food.
- An empty food bar has no effect. Hit points and mana keep regenerating, no
  hit points are lost, nobody starves to death, and the server no longer makes
  a starving character grab food from its inventory.
- Eating and drinking still work as before. Food still fills the food bar (up
  to its maximum) and heals a little; cursed food still empties it and keeps
  its other special effects. Food items, their values and shops are unchanged.
- Items with a food bonus ("sustenance" or "hunger" in their description) are
  unchanged and still announce that digestion slows down or speeds up, but
  digestion no longer has any effect.

## Maintainer notes

- `player_do_some_living()` in `src/types/player.c` only regenerates hit
  points and mana. It no longer reads or changes `stats.food`, `last_eat` or
  `player::digestion`. It still kills a player whose hit points were taken
  to zero outside an attack, such as by rotten food; that death keeps the
  killer its source recorded instead of being reported as starvation.
- The food stat, `player::digestion`, item food bonuses and the
  `CS_STAT_FOOD` status update are kept, so existing saves, archetypes, maps
  and scripts load and behave unchanged and the wire protocol is untouched.
- Death, the combat-defeat rescue and save-life still refill an empty stomach.
  That only keeps the food bar of older characters full.
- The `survival.death-environment` metric no longer receives starvation
  deaths; earlier saved counts are unchanged.
- The client still draws the food bar and its optional low-food warning. A
  character whose saved food is low keeps showing that warning until they
  eat something.
- `src/tests/unit/server/living.c` covers food not draining over many ticks
  an empty or negative food value keeping hit points and regeneration, and
  non-attack hit point loss still being fatal.
- To restore hunger, revert the change that introduced this document.
