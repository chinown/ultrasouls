"""ULTRASOULS patch v1: params only.

  1. No stamina cost on any player action that goes through BehaviorParam_PC.
  2. Blood healing: every melee weapon heals the attacker on hit.
  3. Arrows and bolts fly fast and flat, like hitscan shots.
  4. Merchants sell arrows and bolts for 1 soul.
  5. New rows for V1's Piercer revolver: a normal shot and a charged, piercing shot.

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
REVOLVER_ID = 9000100       # the DLL fires behaviour 9000100 (primary) and 9000110 (charged); keep in step with mod.cpp
PIERCER_ID = 9000110
REVOLVER_DAMAGE = 150       # flat attack value per shot (a throwing knife is 100); a tuning choice, not from ULTRAKILL
PIERCER_DAMAGE = 450        # charged shot: 3x, and it passes through enemies
COIN_ID = 9000120           # a shot ricocheted off a coin: 9000120 is power 2 (one coin), up to 9000123 for power 5
COIN_POWERS = (2, 3, 4, 5)  # Coin.power starts at 2 and gains 1 per extra coin in the chain; a plain shot is 1
PUNCH_ID = 9000130          # the Feedbacker's punch: an unseen, very short projectile from the eye
PUNCH_DAMAGE = REVOLVER_DAMAGE   # Punch: damage 1, the same as a plain revolver shot
PUNCH_REACH = 2.0           # 4 units
PUNCH_RADIUS = 0.5          # the 1-unit sphere ULTRAKILL sweeps when the straight line misses
BEAM_SPEED = 300.0          # m/s, as close to hitscan as a projectile gets
BEAM_RANGE = 150.0

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

# 5. V1's Piercer revolver, as new rows the DLL fires through the game's own shoot call.
#    BehaviorParam_PC row -> Bullet row -> AtkParam_Pc row, all sharing one id per shot type.
#    Damage is flat, from a copy of the throwing knife's attack row.
coin_rows = [(COIN_ID + i, power) for i, power in enumerate(COIN_POWERS)]
g.add_rows("AtkParam_Pc", [(REVOLVER_ID, 1050, "revolver"), (PIERCER_ID, 1050, "piercer")] + [(rid, 1050, "coin") for rid, _ in coin_rows]
           + [(PUNCH_ID, 1050, "punch")])
g.add_rows("Bullet", [(REVOLVER_ID, 603, "revolver"), (PIERCER_ID, 603, "piercer")] + [(rid, 603, "coin") for rid, _ in coin_rows]
           + [(PUNCH_ID, 603, "punch")])
g.add_rows("BehaviorParam_PC", [(REVOLVER_ID, 101103300, "revolver"), (PIERCER_ID, 101103300, "piercer")]
           + [(rid, 101103300, "coin") for rid, _ in coin_rows] + [(PUNCH_ID, 101103300, "punch")])
atk, bul, beh = g["AtkParam_Pc"], g["Bullet"], g["BehaviorParam_PC"]
shots = [(REVOLVER_ID, REVOLVER_DAMAGE, 20131, 20230, 0, 0.15), (PIERCER_ID, PIERCER_DAMAGE, 20133, 20236, 1, 0.3)]
shots += [(rid, REVOLVER_DAMAGE * power, 20133, 20236, 0, 0.3) for rid, power in coin_rows]
for rid, damage, sfx, sfx_hit, pierce, radius in shots:
    # The attack row stays exactly as the throwing knife's apart from the damage: the game's own
    # flat-damage rows all keep their "Correction" at 100, and zeroing it made the shots do nothing.
    atk.set(rid, "atkPhys", damage)
    for k, v in (("atkId_Bullet", rid), ("sfxId_Bullet", sfx), ("sfxId_Hit", sfx_hit), ("sfxId_Flick", -1),
                 ("initVellocity", BEAM_SPEED), ("maxVellocity", BEAM_SPEED), ("minVellocity", BEAM_SPEED),
                 ("accelInRange", 0.0), ("accelOutRange", 0.0), ("gravityInRange", 0.0), ("gravityOutRange", 0.0),
                 ("dist", BEAM_RANGE), ("life", BEAM_RANGE / BEAM_SPEED), ("hitRadius", radius), ("isPenetrate", pierce)):
        bul.set(rid, k, v)
    beh.set(rid, "refId", rid)
    beh.set(rid, "variationId", 0)
    beh.set(rid, "behaviorJudgeId", 0)

# the punch: the same kind of row, but it only travels the punch's reach, is wider, and has no visible trail
atk.set(PUNCH_ID, "atkPhys", PUNCH_DAMAGE)
for k, v in (("atkId_Bullet", PUNCH_ID), ("sfxId_Bullet", -1), ("sfxId_Hit", 20230), ("sfxId_Flick", -1),
             ("initVellocity", BEAM_SPEED), ("maxVellocity", BEAM_SPEED), ("minVellocity", BEAM_SPEED),
             ("accelInRange", 0.0), ("accelOutRange", 0.0), ("gravityInRange", 0.0), ("gravityOutRange", 0.0),
             ("dist", PUNCH_REACH), ("life", PUNCH_REACH / BEAM_SPEED), ("hitRadius", PUNCH_RADIUS), ("isPenetrate", 0)):
    bul.set(PUNCH_ID, k, v)
beh.set(PUNCH_ID, "refId", PUNCH_ID)
beh.set(PUNCH_ID, "variationId", 0)
beh.set(PUNCH_ID, "behaviorJudgeId", 0)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
g.save(OUT)

# verify by re-reading what we wrote
chk = GameParam(OUT, DEFS)
assert len(chk.bnd.buf) == len(g.bnd.buf)
assert chk["Bullet"].get(REVOLVER_ID, "atkId_Bullet") == REVOLVER_ID and chk["Bullet"].get(PIERCER_ID, "isPenetrate") == 1
assert chk["BehaviorParam_PC"].get(PIERCER_ID, "refId") == PIERCER_ID and chk["AtkParam_Pc"].get(REVOLVER_ID, "atkPhys") == REVOLVER_DAMAGE
assert chk["AtkParam_Pc"].get(COIN_ID + 1, "atkPhys") == REVOLVER_DAMAGE * 3 and chk["Bullet"].get(COIN_ID + 3, "atkId_Bullet") == COIN_ID + 3
assert chk["AtkParam_Pc"].get(COIN_ID + 1, "atkPhys") == REVOLVER_DAMAGE * 3 and chk["Bullet"].get(COIN_ID + 3, "atkId_Bullet") == COIN_ID + 3
assert chk["AtkParam_Pc"].get(COIN_ID + 1, "atkPhys") == REVOLVER_DAMAGE * 3 and chk["Bullet"].get(COIN_ID + 3, "atkId_Bullet") == COIN_ID + 3
assert chk["Bullet"].get(PUNCH_ID, "dist") == PUNCH_REACH and chk["AtkParam_Pc"].get(PUNCH_ID, "atkPhys") == PUNCH_DAMAGE
assert all(chk["BehaviorParam_PC"].get(r, "stamina") == 0 for r in chk["BehaviorParam_PC"].order)
assert chk["SpEffectParam"].get(HEAL_SPEFFECT, "changeHpPoint") == -HEAL_PER_HIT
assert chk["Bullet"].get(600, "initVellocity") == SHOT_SPEED
print(f"stamina zeroed on {n_stam} behaviours; heal-on-hit on {n_heal} melee weapons ({n_full} had no free slot); {n_shot} projectiles retuned; {n_shop} ammo shop prices set to 1")
print("built", os.path.normpath(OUT), os.path.getsize(OUT), "bytes")

if "--install" in sys.argv:
    shutil.copyfile(OUT, GAME)
    print("installed to", GAME)
