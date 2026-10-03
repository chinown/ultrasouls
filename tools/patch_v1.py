"""ULTRASOULS patch v1: params only.

  1. No stamina cost on any player action that goes through BehaviorParam_PC.
  2. Blood healing: every melee weapon heals the attacker on hit.
  3. Arrows and bolts fly fast and flat, like hitscan shots.
  4. Merchants sell arrows and bolts for 1 soul.

Always patches from backup/GameParam.parambnd.dcx.current, so re-running is safe.
Pass --install to copy the result into the game folder.
"""
import os
import shutil
import sys
from dsparam import GameParam

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
SRC = os.path.join(ROOT, "backup", "GameParam.parambnd.dcx.current")
DEFS = os.path.join(ROOT, "backup", "paramdef.paramdefbnd.dcx")
OUT = os.path.join(ROOT, "build", "GameParam.parambnd.dcx")
GAME = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED\param\GameParam\GameParam.parambnd.dcx"

HEAL_SPEFFECT = 6990        # Butcher Knife's heal-on-hit, reused for every melee weapon
HEAL_PER_HIT = 100
MELEE_CATEGORIES = range(0, 10)   # dagger .. fist; excludes bows, crossbows, shields, ammo
SHOT_SPEED = 120.0
SHOT_RANGE = 100.0
SHOT_RADIUS = 0.1
SHOT_BULLETS = list(range(500, 509)) + list(range(600, 605))

g = GameParam(SRC, DEFS)

# 1. stamina
beh = g["BehaviorParam_PC"]
n_stam = 0
for r in beh.order:
    if beh.get(r, "stamina"):
        beh.set(r, "stamina", 0)
        n_stam += 1

# 2. blood healing
sp = g["SpEffectParam"]
sp.set(HEAL_SPEFFECT, "changeHpPoint", -HEAL_PER_HIT)
wep = g["EquipParamWeapon"]
slots = ("spEffectBehaviorId0", "spEffectBehaviorId1", "spEffectBehaviorId2")
n_heal = n_full = 0
for r in wep.order:
    if wep.get(r, "weaponCategory") not in MELEE_CATEGORIES:
        continue
    cur = [wep.get(r, s) for s in slots]
    if HEAL_SPEFFECT in cur:
        n_heal += 1
        continue
    free = [s for s, v in zip(slots, cur) if v == -1]
    if not free:
        n_full += 1
        continue
    wep.set(r, free[0], HEAL_SPEFFECT)
    n_heal += 1

# 3. shots
bul = g["Bullet"]
n_shot = 0
for r in SHOT_BULLETS:
    if r not in bul.rows:
        continue
    for k, v in (("initVellocity", SHOT_SPEED), ("maxVellocity", SHOT_SPEED), ("minVellocity", SHOT_SPEED),
                 ("accelInRange", 0.0), ("accelOutRange", 0.0), ("gravityInRange", 0.0), ("gravityOutRange", 0.0),
                 ("dist", SHOT_RANGE), ("hitRadius", SHOT_RADIUS)):
        bul.set(r, k, v)
    n_shot += 1

# 4. arrows and bolts cost 1 soul at every merchant
shop = g["ShopLineupParam"]
n_shop = 0
for r in shop.order:
    if shop.get(r, "equipType") == 0 and 2000000 <= shop.get(r, "equipId") < 2200000 and shop.get(r, "value") > 1:
        shop.set(r, "value", 1)
        n_shop += 1

os.makedirs(os.path.dirname(OUT), exist_ok=True)
g.save(OUT)

# verify by re-reading what we wrote
chk = GameParam(OUT, DEFS)
assert len(chk.bnd.buf) == len(g.bnd.buf)
assert all(chk["BehaviorParam_PC"].get(r, "stamina") == 0 for r in chk["BehaviorParam_PC"].order)
assert chk["SpEffectParam"].get(HEAL_SPEFFECT, "changeHpPoint") == -HEAL_PER_HIT
assert chk["Bullet"].get(600, "initVellocity") == SHOT_SPEED
print(f"stamina zeroed on {n_stam} behaviours; heal-on-hit on {n_heal} melee weapons ({n_full} had no free slot); {n_shot} projectiles retuned; {n_shop} ammo shop prices set to 1")
print("built", os.path.normpath(OUT), os.path.getsize(OUT), "bytes")

if "--install" in sys.argv:
    shutil.copyfile(OUT, GAME)
    print("installed to", GAME)
