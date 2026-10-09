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
COIN_BACK_ID = 9000180      # the same four for a ricochet that reaches its target from behind: a raised shield does not stop them.
                            # (Dark Souls judges a block by where the attacker stands, not by where the projectile comes from.)
PUNCH_ID = 9000130          # the Feedbacker's punch: an unseen, very short projectile from the eye
PUNCH_DAMAGE = REVOLVER_DAMAGE   # Punch: damage 1, the same as a plain revolver shot
PUNCH_REACH = 2.0           # 4 units
# The other arm attacks are rows of the same kind. Dark Souls' own stagger settings do the rest:
# dmgLevel 3 is a heavy stagger and 4 knocks a character off its feet; atkSuperArmor is poise damage.
#                 id        damage (x a revolver shot)  reach  radius  dmgLevel  poise  knockback
ARM_ROWS = {
    "parry":   (9000131, 2.0,                         2.0,   0.5,    3,        200,   1.0),   # a punch that lands on an attacking enemy
    "knuckle": (9000132, 2.5,                         2.0,   0.5,    4,        400,   3.0),   # Knuckleblaster: damage 2.5, force 100 against 25
    "blast":   (9000133, 1.0,                         1.5,   6.0,    4,        400,   5.0),   # its blast wave ('Explosion Wave Knuckleblaster': 12 u): wide, mostly a shove
    # the shotgun fired into an attacking enemy at arm's length (Shotgun.Shoot's 4-unit "shotgunzone": 4 x 1.5)
    "shotgun_parry": (9000141, 6.0,                   2.0,   0.5,    4,        400,   3.0),
    # Explosions (v0.82). Until then an explosion was one projectile as wide as the explosion, set off at its
    # middle and sent 0.6 m upwards. Measured in the game on 2026-10-08 with the DLL's "boom" test command,
    # that hurt an enemy standing in it only now and then: the game counts a projectile's hit on a body it
    # already overlaps only if it is moving towards that body, and one sphere cannot be moving towards
    # everybody round it (a rocket going off a metre from a hollow did nothing to it, time after time). So the
    # DLL now sends one of these rows at each enemy inside the explosion, from 0.9 m short of its aim point:
    # every row here is 1.5 m of reach and half a metre of radius, and how wide each explosion is is the
    # DLL's business (EXPLOSIONS in mod.cpp).
    # Explosion: a core or a punched pellet going off is 6 units across at damage 3.5; a core shot in the
    # air is the "super" one, 12 units and twice the damage
    "explosion": (9000142, 3.5,                       1.5,   0.5,    4,        400,   4.0),
    "explosion_super": (9000143, 7.0,                 1.5,   0.5,    4,        600,   6.0),
    # the Pump Charge shotgun fired at three pumps: the same explosion half again as large, at damage 50 against 35
    "explosion_pump": (9000144, 5.0,                  1.5,   0.5,    4,        400,   4.0),
    # 'Explosion Malicious Railcannon': 13.5 units, damage 50 with enemyDamageMultiplier 1.25
    "explosion_malicious": (9000172, 6.25,            1.5,   0.5,   4,        600,   6.0),
    # a core set off by the Malicious Railcannon's beam, the "ultraboost": the super explosion at twice the size
    "explosion_ultra": (9000174, 7.0,                 1.5,   0.5,   4,        600,   6.0),
    # the whiplash's hook going in: HookArm deals 0.2. Half a revolver shot's attack value is what lands a
    # fifth of its damage on the hollow the pellets were measured on (see PELLET_DAMAGE); a light flinch, no shove.
    "whiplash": (9000190, 0.5,                        1.5,   0.5,    1,        20,    0.0),
    # One hit of a sawblade: the Attractor's 0.75, the Overheat's 0.6, the heated one's 1 (the Nail prefabs). The
    # attack values are the ones that land those shares of a revolver shot on the hollow the pellets were
    # measured on (0.86 and 0.79: see PELLET_DAMAGE).
    "saw_attractor": (9000191, 0.86,                  1.2,   0.5,    1,        20,    0.0),
    "saw_overheat":  (9000192, 0.79,                  1.2,   0.5,    1,        20,    0.0),
    "saw_heated":    (9000193, 1.0,                   1.2,   0.5,    2,        60,    0.0),
    # The rocket launcher. A rocket that reaches an enemy goes off as the plain explosion, or the super one; one
    # that does so while the rockets are frozen, or is shot in the air, is half again as large (Grenade.Explode).
    "explosion_big": (9000145, 3.5,                   1.5,   0.5,    4,        400,   4.0),
    "explosion_super_big": (9000146, 7.0,             1.5,   0.5,    4,        600,   6.0),
    # The S.R.S. Cannon's ball (Cannonball): damage 5, or its speed x 0.15 where that is less, and 7 for one
    # punched on after it has bounced off an enemy. Four rows: 2, 3.5, 5 and 7.
    "cannonball_2":  (9000200, 2.0,                   1.2,   0.5,    4,        400,   4.0),
    "cannonball_3":  (9000201, 3.5,                   1.2,   0.5,    4,        400,   4.0),
    "cannonball_5":  (9000202, 5.0,                   1.2,   0.5,    4,        600,   6.0),
    "cannonball_7":  (9000203, 7.0,                   1.2,   0.5,    4,        600,   6.0),
    # the wave where the ball lands ('PhysicalShockwavePlayer': 12 u, damage 0): it throws what it reaches
    # and does not hurt (the least an attack row can do here)
    "shockwave":     (9000204, 0.05,                  1.5,   0.5,    4,        400,   6.0),
    # The visceral attack (ours, after Bloodborne's): a parried enemy is kept reeling by a hit that does next to
    # nothing every 0.8 s, and a punch on it while it reels is this one blow.
    "stun":          (9000205, 0.05,                  1.5,   0.5,    3,        200,   0.0),
    "visceral":      (9000206, 12.0,                  1.5,   0.5,    4,        1000,  8.0),
}
PELLET_ID = 9000140         # one shotgun pellet. Shotgun.Shoot sends twelve; Projectile deals damage / 4 = a quarter of a revolver shot
# Dark Souls takes a flat defence off every hit, which punishes many small hits. Measured on a hollow that
# one revolver shot takes 64 health from: a pellet at 0.45 of the revolver's attack value took 10. By the
# game's damage formula for an attack below the defence (0.4 a^3/d^2 - 0.09 a^2/d + 0.1 a, which gives
# that 10 with d = 136), 0.57 of the revolver's attack value is what lands a quarter of its damage there.
PELLET_DAMAGE = int(REVOLVER_DAMAGE * 0.57)
PELLET_SPEED = 37.5         # 75 units a second
PELLET_RANGE = 60.0
SHARP_ID = 9000150          # the Sharpshooter's charged shot: goes through enemies (hitAmount 999); the DLL does the ricochets
# The alternate ("Slab") revolvers and the Railcannon. ULTRAKILL's beams hit one enemy up to maxHitsPerTarget
# times for `damage` each; a row here deals that product in one hit.
#   'Revolver Beam Alternative'         1.25 x 2: the Slab's plain shot
#   'Revolver Beam Super Alternative'   1.25 x 4, through enemies: the Slab Piercer's charged shot
#   'Revolver Beam Sharp Alternative'   1.25 x 2, through enemies: the Slab Sharpshooter's
#   'Railcannon Beam'                   2 x 4, through enemies: the Electric Railcannon
#   'Railcannon Beam Malicious'         2, stops at the first thing it meets and sets off its explosion there
#              id       damage  trail  through  radius  name
MORE_SHOTS = [(9000160, 2.5,    20131, 0,       0.15,   "slab"),
              (9000161, 5.0,    20133, 1,       0.3,    "slab_charged"),
              (9000162, 2.5,    20133, 1,       0.3,    "slab_sharpshooter"),
              (9000170, 8.0,    20133, 1,       0.3,    "railcannon"),
              (9000171, 2.0,    20133, 0,       0.3,    "railcannon_malicious")]
# A charged or railcannon beam that a coin sends on (Coin.ReflectRevolver, "altBeam") is that beam made
# stronger: every hit of it gains a quarter of the coin's power. The DLL works the total out and fires the
# row whose damage is that many quarters of a revolver shot: 9000304 (one shot's worth) to 9000396 (24).
COIN_ALT_ID, COIN_ALT_MAX = 9000300, 96
MORE_SHOTS += [(COIN_ALT_ID + q, q / 4.0, 20133, 1, 0.3, "coin_alt") for q in range(4, COIN_ALT_MAX + 1)]
PUNCH_SPEED = 30.0          # m/s: slow enough to exist for four frames. At the revolver's 300 m/s the punch
                            # lived 7 ms, less than a frame, and never hit anything (v0.49: 11 punches in reach, no damage)
WIDE_LIFE = 0.1             # seconds a wide row is there for: six frames at 60 a second
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
coin_rows = [(COIN_ID + i, power) for i, power in enumerate(COIN_POWERS)] + [(COIN_BACK_ID + i, power) for i, power in enumerate(COIN_POWERS)]
g.add_rows("AtkParam_Pc", [(REVOLVER_ID, 1050, "revolver"), (PIERCER_ID, 1050, "piercer")] + [(rid, 1050, "coin") for rid, _ in coin_rows]
           + [(PUNCH_ID, 1050, "punch")] + [(v[0], 1050, k) for k, v in ARM_ROWS.items()]
           + [(PELLET_ID, 1050, "pellet"), (SHARP_ID, 1050, "sharpshooter")] + [(m[0], 1050, m[5]) for m in MORE_SHOTS])
g.add_rows("Bullet", [(REVOLVER_ID, 603, "revolver"), (PIERCER_ID, 603, "piercer")] + [(rid, 603, "coin") for rid, _ in coin_rows]
           + [(PUNCH_ID, 603, "punch")] + [(v[0], 603, k) for k, v in ARM_ROWS.items()]
           + [(PELLET_ID, 603, "pellet"), (SHARP_ID, 603, "sharpshooter")] + [(m[0], 603, m[5]) for m in MORE_SHOTS])
g.add_rows("BehaviorParam_PC", [(REVOLVER_ID, 101103300, "revolver"), (PIERCER_ID, 101103300, "piercer")]
           + [(rid, 101103300, "coin") for rid, _ in coin_rows] + [(PUNCH_ID, 101103300, "punch")]
           + [(v[0], 101103300, k) for k, v in ARM_ROWS.items()]
           + [(PELLET_ID, 101103300, "pellet"), (SHARP_ID, 101103300, "sharpshooter")] + [(m[0], 101103300, m[5]) for m in MORE_SHOTS])
atk, bul, beh = g["AtkParam_Pc"], g["Bullet"], g["BehaviorParam_PC"]
# Dark Souls' own effect where a projectile lands (20230 and 20236 are bursts of sparks and lightning) is
# switched off for every shot: the DLL draws ULTRAKILL's hit particles there instead.
NO_EFFECT = -1
shots = [(REVOLVER_ID, REVOLVER_DAMAGE, 20131, NO_EFFECT, 0, 0.15), (PIERCER_ID, PIERCER_DAMAGE, 20133, NO_EFFECT, 1, 0.3)]
shots += [(rid, REVOLVER_DAMAGE * power, 20133, NO_EFFECT, 0, 0.3) for rid, power in coin_rows]
shots += [(SHARP_ID, REVOLVER_DAMAGE, 20133, NO_EFFECT, 1, 0.3)]
shots += [(rid, int(REVOLVER_DAMAGE * mult), sfx, NO_EFFECT, pierce, radius) for rid, mult, sfx, pierce, radius, _ in MORE_SHOTS]
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
             ("initVellocity", PUNCH_SPEED), ("maxVellocity", PUNCH_SPEED), ("minVellocity", PUNCH_SPEED),
             ("accelInRange", 0.0), ("accelOutRange", 0.0), ("gravityInRange", 0.0), ("gravityOutRange", 0.0),
             ("dist", PUNCH_REACH), ("life", PUNCH_REACH / PUNCH_SPEED), ("hitRadius", PUNCH_RADIUS), ("isPenetrate", 0)):
    bul.set(PUNCH_ID, k, v)
beh.set(PUNCH_ID, "refId", PUNCH_ID)
beh.set(PUNCH_ID, "variationId", 0)
beh.set(PUNCH_ID, "behaviorJudgeId", 0)

for rid, mult, reach, radius, level, poise, knock in ARM_ROWS.values():
    atk.set(rid, "atkPhys", int(REVOLVER_DAMAGE * mult))
    atk.set(rid, "dmgLevel", level)
    atk.set(rid, "atkSuperArmor", poise)
    atk.set(rid, "knockbackDist", knock)
    # The hit effect: the game's large burst (20236) suits the wide rows, which are explosions; on the
    # arm's-length ones it whited out the whole view for a third of a second (seen in a v0.59 recording),
    # so those use the small spark the plain punch has.
    # How long a row is there for: the arm's-length ones go their reach at the punch's speed; a wide one (the
    # Knuckleblaster's wave) takes WIDE_LIFE over its reach, so that it is there for several frames.
    life = WIDE_LIFE if radius > 1.0 else reach / PUNCH_SPEED
    speed = reach / life
    for k, v in (("atkId_Bullet", rid), ("sfxId_Bullet", -1), ("sfxId_Hit", 20236 if radius > 1.0 else 20230), ("sfxId_Flick", -1),
                 ("initVellocity", speed), ("maxVellocity", speed), ("minVellocity", speed),
                 ("accelInRange", 0.0), ("accelOutRange", 0.0), ("gravityInRange", 0.0), ("gravityOutRange", 0.0),
                 ("dist", reach), ("life", life), ("hitRadius", radius), ("isPenetrate", 1 if radius > 1.0 else 0)):
        bul.set(rid, k, v)
    beh.set(rid, "refId", rid)
    beh.set(rid, "variationId", 0)
    beh.set(rid, "behaviorJudgeId", 0)

# a pellet: a slower, short-lived row with no trail of its own (the DLL draws the pellets)
atk.set(PELLET_ID, "atkPhys", PELLET_DAMAGE)
for k, v in (("atkId_Bullet", PELLET_ID), ("sfxId_Bullet", -1), ("sfxId_Hit", NO_EFFECT), ("sfxId_Flick", -1),
             ("initVellocity", PELLET_SPEED), ("maxVellocity", PELLET_SPEED), ("minVellocity", PELLET_SPEED),
             ("accelInRange", 0.0), ("accelOutRange", 0.0), ("gravityInRange", 0.0), ("gravityOutRange", 0.0),
             ("dist", PELLET_RANGE), ("life", PELLET_RANGE / PELLET_SPEED), ("hitRadius", 0.1), ("isPenetrate", 0)):
    bul.set(PELLET_ID, k, v)
beh.set(PELLET_ID, "refId", PELLET_ID)
beh.set(PELLET_ID, "variationId", 0)
beh.set(PELLET_ID, "behaviorJudgeId", 0)

# Every row the DLL fires is made silent and sparkless at its end. A projectile row carries three things
# for that moment: its hit effect (sfxId_Hit), a switch for the sparks its "material" makes on what it
# strikes (isAttackSFX), and the material itself, which also picks the sound (the throwing knife's is
# type 2, material 0: a blade on stone or flesh). With only the first cleared, every shot and punch still
# ended in a faint knock somewhere ahead, and punches in a burst of sparks. The values set here are the
# ones the game's own spell projectiles carry (571 of its 632 rows): type 0, material 6, no sparks.
OWN_ROWS = [REVOLVER_ID, PIERCER_ID, SHARP_ID, PUNCH_ID, PELLET_ID] + [rid for rid, _ in coin_rows] + [v[0] for v in ARM_ROWS.values()] + [m[0] for m in MORE_SHOTS]
for rid in OWN_ROWS:
    for k, v in (("sfxId_Hit", NO_EFFECT), ("sfxId_Flick", NO_EFFECT), ("isAttackSFX", 0), ("Material_AttackType", 0), ("Material_AttackMaterial", 6),
                 ("Material_Size", 0)):
        bul.set(rid, k, v)
    # The effect that flies with the projectile is switched off as well (v0.67). The revolver's rows had
    # kept the game's own (20131 and 20133) from the days before the DLL drew its own beams; an effect of
    # that kind carries its own sounds, and after v0.65 a faint impact could still be heard after every
    # revolver shot, though not after a punch or a shotgun shot, whose rows never had one.
    bul.set(rid, "sfxId_Bullet", NO_EFFECT)

# Shields (v0.68). An attack row can be made to ignore a guard altogether (disableGuard, which the game uses
# for a handful of its own rows), and breaks a guard by the stamina it takes from the one blocking (atkStam:
# the kick, row 1100, has 500 against a throwing knife's 10).
#   Not stopped by a shield: explosions, both railcannons, the Piercer's charged shots, a coin's shot from behind.
#   The Knuckleblaster's punch takes a kick's worth of stamina: one that is blocked breaks the guard.
UNBLOCKABLE = [PIERCER_ID, 9000161, 9000170, 9000171] + [ARM_ROWS[k][0] for k in ("explosion", "explosion_super", "explosion_pump", "explosion_malicious", "explosion_ultra",
                                                                          "explosion_big", "explosion_super_big", "shockwave", "stun", "visceral")] \
    + [COIN_BACK_ID + i for i in range(len(COIN_POWERS))] + [COIN_ALT_ID + q for q in range(4, COIN_ALT_MAX + 1)]
for rid in UNBLOCKABLE:
    atk.set(rid, "disableGuard", 1)
atk.set(ARM_ROWS["knuckle"][0], "atkStam", 500)

# V1's body in third person (v0.71). tools/ds_v1_body.py builds V1 as three armour pieces of a model number
# of their own (9800) and puts them in the game's parts folder. An empty armour slot is itself a row here
# (900000 head, 901000 body, 902000 arms, 903000 legs, each naming model 0, the bare body): the body, arms
# and legs rows are pointed at the new pieces, so a player with no armour on is drawn as V1 and any piece
# of armour put on takes its slot back. V1's head is part of the body piece and the body row also hides
# the human head (the first sixteen "invisible" flags, which the game's own all-covering helmets set: the
# face is masks 12 and 13, the back of the head 15, hair 10 and 11). The bare-head row is left alone: every
# helmetless character in the game uses it. Only done when the pieces are there, since a row that names a
# model with no file would leave the player with no body, or worse.
V1_MODEL_ID = 9800
V1_PIECES = ["%s_A_%04d%s.partsbnd.dcx" % (p, V1_MODEL_ID, h) for p in ("BD", "AM", "LG") for h in ("", "_M")]
v1_body = all(os.path.exists(os.path.join(os.path.dirname(GAME), "..", "..", "parts", n)) for n in V1_PIECES)
if v1_body:
    pro = g["EquipParamProtector"]
    for rid in (901000, 902000, 903000):
        pro.set(rid, "equipModelId", V1_MODEL_ID)
        pro.set(rid, "equipModelGender", 0)          # 0: one file for both sexes ("_A_")
    for i in range(16):
        pro.set(901000, "invisibleFlag%02d" % i, 1)

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
assert abs(chk["Bullet"].get(PUNCH_ID, "life") - PUNCH_REACH / PUNCH_SPEED) < 1e-4 and chk["Bullet"].get(PUNCH_ID, "initVellocity") == PUNCH_SPEED
assert chk["AtkParam_Pc"].get(ARM_ROWS["knuckle"][0], "dmgLevel") == 4 and chk["Bullet"].get(ARM_ROWS["blast"][0], "hitRadius") == 6.0
assert chk["Bullet"].get(PELLET_ID, "initVellocity") == PELLET_SPEED and chk["AtkParam_Pc"].get(PELLET_ID, "atkPhys") == PELLET_DAMAGE
assert chk["Bullet"].get(SHARP_ID, "isPenetrate") == 1 and chk["Bullet"].get(ARM_ROWS["explosion_super"][0], "hitRadius") == 0.5
assert all(chk["Bullet"].get(r, "sfxId_Bullet") == NO_EFFECT for r in OWN_ROWS)
assert all(chk["Bullet"].get(r, "sfxId_Hit") == NO_EFFECT and chk["Bullet"].get(r, "isAttackSFX") == 0 and chk["Bullet"].get(r, "Material_AttackMaterial") == 6 for r in OWN_ROWS)
assert chk["AtkParam_Pc"].get(9000170, "atkPhys") == REVOLVER_DAMAGE * 8 and chk["Bullet"].get(9000161, "isPenetrate") == 1 and chk["Bullet"].get(9000171, "isPenetrate") == 0
assert chk["Bullet"].get(ARM_ROWS["explosion_malicious"][0], "hitRadius") == 0.5 and chk["BehaviorParam_PC"].get(9000162, "refId") == 9000162
assert all(chk["AtkParam_Pc"].get(r, "disableGuard") == 1 for r in UNBLOCKABLE) and chk["AtkParam_Pc"].get(REVOLVER_ID, "disableGuard") == 0
assert chk["AtkParam_Pc"].get(ARM_ROWS["knuckle"][0], "atkStam") == 500 and chk["AtkParam_Pc"].get(COIN_BACK_ID + 2, "atkPhys") == REVOLVER_DAMAGE * 4
assert chk["Bullet"].get(PUNCH_ID, "dist") == PUNCH_REACH and chk["AtkParam_Pc"].get(PUNCH_ID, "atkPhys") == PUNCH_DAMAGE
assert all(chk["BehaviorParam_PC"].get(r, "stamina") == 0 for r in chk["BehaviorParam_PC"].order)
assert chk["SpEffectParam"].get(HEAL_SPEFFECT, "changeHpPoint") == -HEAL_PER_HIT
assert chk["Bullet"].get(600, "initVellocity") == SHOT_SPEED
assert chk["AtkParam_Pc"].get(COIN_ALT_ID + 49, "atkPhys") == int(REVOLVER_DAMAGE * 12.25) and chk["Bullet"].get(COIN_ALT_ID + 96, "isPenetrate") == 1
assert chk["Bullet"].get(ARM_ROWS["explosion_ultra"][0], "hitRadius") == 0.5 and chk["AtkParam_Pc"].get(COIN_ALT_ID + 4, "disableGuard") == 1
assert chk["EquipParamProtector"].get(901000, "equipModelId") == (V1_MODEL_ID if v1_body else 0) and chk["EquipParamProtector"].get(900000, "equipModelId") == 0
assert chk["EquipParamProtector"].get(901000, "invisibleFlag12") == (1 if v1_body else 0) and chk["EquipParamProtector"].get(903000, "equipModelGender") == (0 if v1_body else 3)
assert chk["AtkParam_Pc"].get(ARM_ROWS["whiplash"][0], "atkPhys") == REVOLVER_DAMAGE // 2 and chk["BehaviorParam_PC"].get(9000190, "refId") == 9000190
print(f"stamina zeroed on {n_stam} behaviours; heal-on-hit on {n_heal} melee weapons ({n_full} had no free slot); {n_shot} projectiles retuned; {n_shop} ammo shop prices set to 1")
print("V1's body for an unarmoured player: %s" % ("on (the pieces are in the game's parts folder)" if v1_body else "off (the pieces are not installed: tools/ds_v1_body.py)"))
print("built", os.path.normpath(OUT), os.path.getsize(OUT), "bytes")

if "--install" in sys.argv:
    shutil.copyfile(OUT, GAME)
    print("installed to", GAME)
