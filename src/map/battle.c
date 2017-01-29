// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/timer.h"
#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/showmsg.h"
#include "../common/ers.h"
#include "../common/random.h"
#include "../common/socket.h"
#include "../common/strlib.h"
#include "../common/utils.h"

#include "map.h"
#include "path.h"
#include "pc.h"
#include "status.h"
#include "skill.h"
#include "homunculus.h"
#include "mercenary.h"
#include "elemental.h"
#include "mob.h"
#include "itemdb.h"
#include "clif.h"
#include "pet.h"
#include "guild.h"
#include "party.h"
#include "battle.h"
#include "battleground.h"
#include "chrif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int attr_fix_table[4][ELE_MAX][ELE_MAX];
int64 battle_damage_temp[2];

struct Battle_Config battle_config;
static struct eri *delay_damage_ers; //For battle delay damage structures

int battle_getcurrentskill(struct block_list *bl) { //Returns the current/last skill in use by this bl
	struct unit_data *ud;

	if( bl->type == BL_SKILL ) {
		struct skill_unit *su = (struct skill_unit *)bl;

		return (su && su->group ? su->group->skill_id : 0);
	}

	ud = unit_bl2ud(bl);

	return (ud ? ud->skill_id : 0);
}

/*==========================================
 * Get random targetting enemy
 *------------------------------------------*/
static int battle_gettargeted_sub(struct block_list *bl, va_list ap) {
	struct block_list **bl_list;
	struct unit_data *ud;
	int target_id;
	int *c;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	target_id = va_arg(ap, int);

	if (bl->id == target_id)
		return 0;

	if (*c >= 24)
		return 0;

	if (!(ud = unit_bl2ud(bl)))
		return 0;

	if (ud->target == target_id || ud->skilltarget == target_id) {
		bl_list[(*c)++] = bl;
		return 1;
	}

	return 0;
}

struct block_list *battle_gettargeted(struct block_list *target) {
	struct block_list *bl_list[24];
	int c = 0;

	nullpo_retr(NULL, target);

	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinrange(battle_gettargeted_sub, target, AREA_SIZE, BL_CHAR, bl_list, &c, target->id);
	if (c == 0)
		return NULL;
	if (c > 24)
		c = 24;
	return bl_list[rnd()%c];
}

//Returns the id of the current targetted character of the passed bl. [Skotlex]
int battle_gettarget(struct block_list *bl) {

	switch (bl->type) {
		case BL_PC:  return ((struct map_session_data *)bl)->ud.target;
		case BL_MOB: return ((struct mob_data *)bl)->target_id;
		case BL_PET: return ((struct pet_data *)bl)->target_id;
		case BL_HOM: return ((struct homun_data *)bl)->ud.target;
		case BL_MER: return ((struct mercenary_data *)bl)->ud.target;
		case BL_ELEM: return ((struct elemental_data *)bl)->ud.target;
	}

	return 0;
}

static int battle_getenemy_sub(struct block_list *bl, va_list ap) {
	struct block_list **bl_list;
	struct block_list *target;
	int *c;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	target = va_arg(ap, struct block_list *);

	if (bl->id == target->id)
		return 0;

	if (*c >= 24)
		return 0;

	if (status_isdead(bl))
		return 0;

	if (battle_check_target(target, bl, BCT_ENEMY) > 0) {
		bl_list[(*c)++] = bl;
		return 1;
	}

	return 0;
}

//Picks a random enemy of the given type (BL_PC, BL_CHAR, etc) within the range given. [Skotlex]
struct block_list *battle_getenemy(struct block_list *target, int type, int range) {
	struct block_list *bl_list[24];
	int c = 0;

	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinrange(battle_getenemy_sub, target, range, type, bl_list, &c, target);

	if (c == 0)
		return NULL;

	if (c > 24)
		c = 24;

	return bl_list[rnd()%c];
}
static int battle_getenemyarea_sub(struct block_list *bl, va_list ap) {
	struct block_list **bl_list, *src;
	int *c, ignore_id;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	src = va_arg(ap, struct block_list *);
	ignore_id = va_arg(ap, int);

	if( bl->id == src->id || bl->id == ignore_id )
		return 0; //Ignores Caster and a possible pre-target

	if( *c >= 23 )
		return 0;

	if( status_isdead(bl) )
		return 0;

	if( battle_check_target(src, bl, BCT_ENEMY) > 0 ) { //Is Enemy!
		bl_list[(*c)++] = bl;
		return 1;
	}

	return 0;
}

//Pick a random enemy
struct block_list *battle_getenemyarea(struct block_list *src, int x, int y, int range, int type, int ignore_id) {
	struct block_list *bl_list[24];
	int c = 0;

	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinarea(battle_getenemyarea_sub, src->m, x - range, y - range, x + range, y + range, type, bl_list, &c, src, ignore_id);

	if( !c )
		return NULL;
	if( c >= 24 )
		c = 23;

	return bl_list[rnd()%c];
}

/**
 * Deals damage without delay, applies additional effects and triggers monster events
 * This function is called from battle_delay_damage or battle_delay_damage_sub
 * @author [Playtester]
 * @param src: Source of damage
 * @param target: Target of damage
 * @param damage: Damage to be dealt
 * @param delay: Damage delay
 * @param skill_lv: Level of skill used
 * @param skill_id: ID o skill used
 * @param dmg_lv: State of the attack (miss, etc.)
 * @param attack_type: Type of the attack (BF_NORMAL|BF_SKILL|BF_SHORT|BF_LONG|BF_WEAPON|BF_MAGIC|BF_MISC)
 * @param additional_effects: Whether additional effect should be applied
 * @param isspdamage: If the damage is done to SP
 * @param tick: Current tick
 */
void battle_damage(struct block_list *src, struct block_list *target, int64 damage, int delay, uint16 skill_lv, uint16 skill_id, enum damage_lv dmg_lv, unsigned short attack_type, bool additional_effects, unsigned int tick, bool isspdamage) {
	if( isspdamage )
		status_fix_spdamage(src, target, damage, delay);
	else
		status_fix_damage(src, target, damage, delay); //We have to separate here between reflect damage and others [icescope]
	if( attack_type && !status_isdead(target) && additional_effects )
		skill_additional_effect(src, target, skill_id, skill_lv, attack_type, dmg_lv, tick);
	if( dmg_lv > ATK_BLOCK && attack_type )
		skill_counter_additional_effect(src, target, skill_id, skill_lv, attack_type, tick);
	//This is the last place where we have access to the actual damage type, so any monster events depending on type must be placed here
	if( target->type == BL_MOB && damage && (attack_type&BF_NORMAL) ) {
		//Monsters differentiate whether they have been attacked by a skill or a normal attack
		struct mob_data *md = BL_CAST(BL_MOB, target);

		md->norm_attacked_id = md->attacked_id;
	}
}

//Damage delayed info
struct delay_damage {
	int src_id;
	int target_id;
	int64 damage;
	int delay;
	unsigned short distance;
	uint16 skill_lv;
	uint16 skill_id;
	enum damage_lv dmg_lv;
	unsigned short attack_type;
	bool additional_effects;
	enum bl_type src_type;
	bool isspdamage;
};

int battle_delay_damage_sub(int tid, unsigned int tick, int id, intptr_t data) {
	struct delay_damage *dat = (struct delay_damage *)data;

	if( dat ) {
		struct block_list *src = NULL;
		struct block_list *target = map_id2bl(dat->target_id);

		if( !target || status_isdead(target) ) { //Nothing we can do
			if( dat->src_type == BL_PC && (src = map_id2bl(dat->src_id)) &&
				--((TBL_PC *)src)->delayed_damage == 0 && ((TBL_PC *)src)->state.hold_recalc ) {
				((TBL_PC *)src)->state.hold_recalc = 0;
				status_calc_pc(((TBL_PC *)src), SCO_FORCE);
			}
			ers_free(delay_damage_ers, dat);
			return 0;
		}

		src = map_id2bl(dat->src_id);

		if( src &&
			(target->type != BL_PC || ((TBL_PC *)target)->invincible_timer == INVALID_TIMER) &&
			(dat->skill_id == MO_EXTREMITYFIST || (target->m == src->m && check_distance_bl(src, target, dat->distance))) )
		{ //Check to see if you haven't teleported [Skotlex]
			map_freeblock_lock();
			//Deal damage
			battle_damage(src, target, dat->damage, dat->delay, dat->skill_lv, dat->skill_id, dat->dmg_lv, dat->attack_type, dat->additional_effects, tick, dat->isspdamage);
			map_freeblock_unlock();
		} else if( !src && dat->skill_id == CR_REFLECTSHIELD ) {
			//It was monster reflected damage, and the monster died, we pass the damage to the character as expected
			map_freeblock_lock();
			status_fix_damage(target, target, dat->damage, dat->delay);
			map_freeblock_unlock();
		}

		if( src && src->type == BL_PC && --((TBL_PC *)src)->delayed_damage == 0 && ((TBL_PC *)src)->state.hold_recalc ) {
			((TBL_PC *)src)->state.hold_recalc = 0;
			status_calc_pc(((TBL_PC *)src), SCO_FORCE);
		}
	}
	ers_free(delay_damage_ers, dat);
	return 0;
}

int battle_delay_damage(unsigned int tick, int amotion, struct block_list *src, struct block_list *target, int attack_type, uint16 skill_id, uint16 skill_lv, int64 damage, enum damage_lv dmg_lv, int ddelay, bool additional_effects, bool isspdamage)
{
	struct delay_damage *dat;
	struct status_change *sc;
	struct block_list *d_tbl = NULL;
	struct block_list *e_tbl = NULL;

	nullpo_ret(src);
	nullpo_ret(target);

	sc = status_get_sc(target);

	if( sc ) {
		if( sc->data[SC_DEVOTION] && sc->data[SC_DEVOTION]->val1 )
			d_tbl = map_id2bl(sc->data[SC_DEVOTION]->val1);
		if( sc->data[SC_WATER_SCREEN_OPTION] && sc->data[SC_WATER_SCREEN_OPTION]->val1 )
			e_tbl = map_id2bl(sc->data[SC_WATER_SCREEN_OPTION]->val1);
	}

	if( ((d_tbl && check_distance_bl(target, d_tbl, sc->data[SC_DEVOTION]->val3)) || e_tbl) &&
		damage > 0 && skill_id != PA_PRESSURE && skill_id != CR_REFLECTSHIELD )
		damage = 0;

	if( !battle_config.delay_battle_damage || amotion <= 1 ) {
		map_freeblock_lock();
		//Deal damage
		battle_damage(src, target, damage, ddelay, skill_lv, skill_id, dmg_lv, attack_type, additional_effects, gettick(), isspdamage);
		map_freeblock_unlock();
		return 0;
	}

	dat = ers_alloc(delay_damage_ers, struct delay_damage);
	dat->src_id = src->id;
	dat->target_id = target->id;
	dat->skill_id = skill_id;
	dat->skill_lv = skill_lv;
	dat->attack_type = attack_type;
	dat->damage = damage;
	dat->dmg_lv = dmg_lv;
	dat->delay = ddelay;
	dat->distance = distance_bl(src, target) + (battle_config.snap_dodge ? 10 : AREA_SIZE);
	dat->additional_effects = additional_effects;
	dat->src_type = src->type;
	dat->isspdamage = isspdamage;

	if( src->type != BL_PC && amotion > 1000 )
		amotion = 1000; //Aegis places a damage-delay cap of 1 sec to non player attacks [Skotlex]

	if( src->type == BL_PC )
		((TBL_PC *)src)->delayed_damage++;

	add_timer(tick + amotion, battle_delay_damage_sub, 0, (intptr_t)dat);

	return 0;
}

/**
 * Get attribute ratio
 * @param atk_elem Attack element enum e_element
 * @param def_type Defense element enum e_element
 * @param def_lv Element level 1 ~ MAX_ELE_LEVEL
 */
int battle_attr_ratio(int atk_elem, int def_type, int def_lv)
{
	if( !CHK_ELEMENT(atk_elem) || !CHK_ELEMENT(def_type) || !CHK_ELEMENT_LEVEL(def_lv) )
		return 100;

	return attr_fix_table[def_lv - 1][atk_elem][def_type];
}

/*==========================================
 * Does attribute fix modifiers.
 * Added passing of the chars so that the status changes can affect it. [Skotlex]
 * NOTE: Passing src/target == NULL is perfectly valid, it skips SC_ checks.
 *------------------------------------------*/
int64 battle_attr_fix(struct block_list *src, struct block_list *target, int64 damage, int atk_elem, int def_type, int def_lv)
{
	struct status_change *sc = NULL, *tsc = NULL;
	int ratio;

	if( src )
		sc = status_get_sc(src);

	if( target )
		tsc = status_get_sc(target);

	if( !CHK_ELEMENT(atk_elem) )
		atk_elem = rnd()%ELE_ALL;

	if( !CHK_ELEMENT(def_type) || !CHK_ELEMENT_LEVEL(def_lv) ) {
		ShowError("battle_attr_fix: unknown attr type: atk=%d def_type=%d def_lv=%d\n",atk_elem,def_type,def_lv);
		return damage;
	}

	ratio = attr_fix_table[def_lv - 1][atk_elem][def_type];
	if( sc && sc->count ) { //Increase damage by src status
		switch( atk_elem ) {
			case ELE_FIRE:
				if( sc->data[SC_VOLCANO] ) {
#ifdef RENEWAL
					ratio += sc->data[SC_VOLCANO]->val3;
#else
					damage += (int64)(damage * sc->data[SC_VOLCANO]->val3 / 100);
#endif
				}
				break;
			case ELE_WIND:
				if( sc->data[SC_VIOLENTGALE] ) {
#ifdef RENEWAL
					ratio += sc->data[SC_VIOLENTGALE]->val3;
#else
					damage += (int64)(damage * sc->data[SC_VIOLENTGALE]->val3 / 100);
#endif
				}
				break;
			case ELE_WATER:
				if( sc->data[SC_DELUGE] ) {
#ifdef RENEWAL
					ratio += sc->data[SC_DELUGE]->val3;
#else
					damage += (int64)(damage * sc->data[SC_DELUGE]->val3 / 100);
#endif
				}
				break;
		}
	}

	if( tsc && tsc->count ) { //Since an atk can only have one type let's optimise this a bit
		switch( atk_elem ) {
			case ELE_FIRE:
				if( tsc->data[SC_SPIDERWEB] ) { //Double damage
#ifdef RENEWAL
					ratio += 100;
#else
					damage *= 2;
#endif
					status_change_end(target,SC_SPIDERWEB,INVALID_TIMER);
				}
				if( tsc->data[SC_THORNSTRAP] ) {
					struct skill_unit_group *group = skill_id2group(tsc->data[SC_THORNSTRAP]->val3);

					if( group )
						skill_delunitgroup(group);
				}
				if( tsc->data[SC_CRYSTALIZE] )
					status_change_end(target,SC_CRYSTALIZE,INVALID_TIMER);
				if( tsc->data[SC_EARTH_INSIGNIA] ) {
#ifdef RENEWAL
					ratio += 50;
#else
					damage += (int64)(damage * 50 / 100);
#endif
				}
				break;
			case ELE_HOLY:
				if( tsc->data[SC_ORATIO] ) {
#ifdef RENEWAL
					ratio += tsc->data[SC_ORATIO]->val2;
#else
					damage += (int64)(damage * tsc->data[SC_ORATIO]->val2 / 100);
#endif
				}
				break;
			case ELE_POISON:
				if( tsc->data[SC_VENOMIMPRESS] ) {
#ifdef RENEWAL
					ratio += tsc->data[SC_VENOMIMPRESS]->val2;
#else
					damage += (int64)(damage * tsc->data[SC_VENOMIMPRESS]->val2 / 100);
#endif
				}
				break;
			case ELE_WIND:
				if( tsc->data[SC_CRYSTALIZE] ) {
#ifdef RENEWAL
					ratio += 50;
#else
					damage += (int64)(damage * 50 / 100);
#endif
				}
				if( tsc->data[SC_WATER_INSIGNIA] ) {
#ifdef RENEWAL
					ratio += 50;
#else
					damage += (int64)(damage * 50 / 100);
#endif
				}
				break;
			case ELE_WATER:
				if( tsc->data[SC_FIRE_INSIGNIA] ) {
#ifdef RENEWAL
					ratio += 50;
#else
					damage += (int64)(damage * 50 / 100);
#endif
				}
				break;
			case ELE_EARTH:
				if( tsc->data[SC_WIND_INSIGNIA] ) {
#ifdef RENEWAL
					ratio += 50;
#else
					damage += (int64)(damage * 50 / 100);
#endif
				}
				if( tsc->data[SC_MAGNETICFIELD] ) //Freed if received earth damage
					status_change_end(target,SC_MAGNETICFIELD,INVALID_TIMER);
				break;
			case ELE_NEUTRAL:
				if( tsc->data[SC_ANTI_M_BLAST] ) {
#ifdef RENEWAL
					ratio += tsc->data[SC_ANTI_M_BLAST]->val2;
#else
					damage += (int64)(damage * tsc->data[SC_ANTI_M_BLAST]->val2 / 100);
#endif
				}
				break;
		}
	}

	if( !battle_config.attr_recover && ratio < 0 )
		ratio = 0;

#ifdef RENEWAL
	//In renewal, reductions are always rounded down so damage can never reach 0 unless ratio is 0
	damage = damage - (int64)(damage * (100 - ratio) / 100);
#else
	damage = (int64)(damage * ratio / 100);
#endif

	//Damage can be negative, see battle_config.attr_recover
	return damage;
}

/**
 * Calculates card bonuses damage adjustments.
 * @param attack_type @see enum e_battle_flag
 * @param src Attacker
 * @param target Target
 * @param nk Skill's nk @see enum e_skill_nk [NK_NO_CARDFIX_ATK|NK_NO_ELEFIX|NK_NO_CARDFIX_DEF]
 * @param rh_ele Right-hand weapon element
 * @param lh_ele Left-hand weapon element (BF_MAGIC and BF_MISC ignore this value)
 * @param damage Original damage
 * @param left Left hand flag (BF_MISC and BF_MAGIC ignore flag value)
 *         3: Calculates attacker bonuses in both hands.
 *         2: Calculates attacker bonuses in right-hand only.
 *         0 or 1: Only calculates target bonuses.
 * @param flag Misc value of skill & damage flags
 * @return damage Damage diff between original damage and after calculation
 */
int battle_calc_cardfix(int attack_type, struct block_list *src, struct block_list *target, int nk, int rh_ele, int lh_ele, int64 damage, int left, int flag) {
	struct map_session_data *sd, //Attacker session data if BL_PC
		*tsd; //Target session data if BL_PC
	short cardfix = 1000;
	enum e_classAE s_class, //Attacker class
		t_class; //Target class
	enum e_race2 s_race2, //Attacker Race2
		t_race2; //Target Race2
	enum e_element s_defele; //Attacker Element (not a weapon or skill element!)
	struct status_data *sstatus, //Attacker status data
		*tstatus; //Target status data
	int64 original_damage;
	int i;

	if( !damage )
		return 0;

	original_damage = damage;

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);
	t_class = (enum e_classAE)status_get_class(target);
	s_class = (enum e_classAE)status_get_class(src);
	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(target);
	s_race2 = (enum e_race2)status_get_race2(src);
	s_defele = (tsd ? (enum e_element)status_get_element(src) : ELE_NONE);

//Official servers apply the cardfix value on a base of 1000 and round down the reduction/increase
#define APPLY_CARDFIX(damage, fix) { (damage) = (damage) - (int64)((damage) * (1000 - (fix)) / 1000); }

	switch( attack_type ) {
		case BF_MAGIC:
			t_race2 = (enum e_race2)status_get_race2(target);
			if( sd && !(nk&NK_NO_CARDFIX_ATK) ) { //Affected by attacker ATK bonuses
				cardfix = cardfix * (100 + sd->magic_addrace[tstatus->race] + sd->magic_addrace[RC_ALL]) / 100;
				if( !(nk&NK_NO_ELEFIX) ) { //Affected by element modifier bonuses
					cardfix = cardfix * (100 + sd->magic_adddefele[tstatus->def_ele] + sd->magic_adddefele[ELE_ALL]) / 100;
					cardfix = cardfix * (100 + sd->magic_atkele[rh_ele] + sd->magic_atkele[ELE_ALL]) / 100;
				}
				cardfix = cardfix * (100 + sd->magic_addsize[tstatus->size] + sd->magic_addsize[SZ_ALL]) / 100;
				cardfix = cardfix * (100 + sd->magic_addrace2[t_race2]) / 100;
				cardfix = cardfix * (100 + sd->magic_addclass[tstatus->class_] + sd->magic_addclass[CLASS_ALL]) / 100;
				for( i = 0; i < ARRAYLENGTH(sd->add_mdmg) && sd->add_mdmg[i].rate;i++ ) {
					if( sd->add_mdmg[i].class_ != t_class )
						continue;
					cardfix = cardfix * (100 + sd->add_mdmg[i].rate) / 100;
				}
				APPLY_CARDFIX(damage,cardfix);
			}
			if( tsd && !(nk&NK_NO_CARDFIX_DEF) ) { //Affected by target DEF bonuses
				cardfix = 1000; //Reset var for target
				if( !(nk&NK_NO_ELEFIX) ) { //Affected by element modifier bonuses
					int ele_fix = tsd->subele[rh_ele] + tsd->subele[ELE_ALL];

					for( i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++ ) {
						if( tsd->subele2[i].ele != rh_ele )
							continue;
						if( !(((tsd->subele2[i].flag)&flag)&BF_WEAPONMASK &&
							((tsd->subele2[i].flag)&flag)&BF_RANGEMASK &&
							((tsd->subele2[i].flag)&flag)&BF_SKILLMASK) )
							continue;
						ele_fix += tsd->subele2[i].rate;
					}
					if( s_defele != ELE_NONE )
						ele_fix += tsd->subdefele[s_defele] + tsd->subdefele[ELE_ALL];
					cardfix = cardfix * (100 - ele_fix) / 100;
				}
				cardfix = cardfix * (100 - (tsd->subsize[sstatus->size] + tsd->subsize[SZ_ALL])) / 100;
				cardfix = cardfix * (100 - tsd->subrace2[s_race2]) / 100;
				cardfix = cardfix * (100 - (tsd->subrace[sstatus->race] + tsd->subrace[RC_ALL])) / 100;
				cardfix = cardfix * (100 - (tsd->subclass[sstatus->class_] + tsd->subclass[CLASS_ALL])) / 100;
				for( i = 0; i < ARRAYLENGTH(tsd->add_mdef) && tsd->add_mdef[i].rate; i++ ) {
					if( tsd->add_mdef[i].class_ != s_class )
						continue;
					cardfix = cardfix * (100 - tsd->add_mdef[i].rate) / 100;
				}
#ifndef RENEWAL //It was discovered that ranged defense also counts vs magic! [Skotlex]
				if( flag&BF_SHORT )
					cardfix = cardfix * (100 - tsd->bonus.near_attack_def_rate) / 100;
				else
					cardfix = cardfix * (100 - tsd->bonus.long_attack_def_rate) / 100;
#endif
				cardfix = cardfix * (100 - tsd->bonus.magic_def_rate) / 100;
				if( tsd->sc.data[SC_MDEF_RATE] )
					cardfix = cardfix * (100 - tsd->sc.data[SC_MDEF_RATE]->val1) / 100;
				APPLY_CARDFIX(damage,cardfix);
			}
			break;
		case BF_WEAPON:
			t_race2 = (enum e_race2)status_get_race2(target);
			if( sd && !(nk&NK_NO_CARDFIX_ATK) && (left&2) ) { //Affected by attacker ATK bonuses
				short cardfix_ = 1000;

				if( sd->state.arrow_atk ) { //Ranged attack
					cardfix = cardfix * (100 + sd->right_weapon.addrace[tstatus->race] + sd->arrow_addrace[tstatus->race] +
						sd->right_weapon.addrace[RC_ALL] + sd->arrow_addrace[RC_ALL]) / 100;
					if( !(nk&NK_NO_ELEFIX) ) { //Affected by element modifier bonuses
						int ele_fix = sd->right_weapon.adddefele[tstatus->def_ele] + sd->arrow_adddefele[tstatus->def_ele] +
							sd->right_weapon.adddefele[ELE_ALL] + sd->arrow_adddefele[ELE_ALL];

						for( i = 0; ARRAYLENGTH(sd->right_weapon.adddefele2) > i && sd->right_weapon.adddefele2[i].rate != 0; i++ ) {
							if( sd->right_weapon.adddefele2[i].ele != tstatus->def_ele )
								continue;
							if( !(((sd->right_weapon.adddefele2[i].flag)&flag)&BF_WEAPONMASK &&
								((sd->right_weapon.adddefele2[i].flag)&flag)&BF_RANGEMASK &&
								((sd->right_weapon.adddefele2[i].flag)&flag)&BF_SKILLMASK) )
								continue;
							ele_fix += sd->right_weapon.adddefele2[i].rate;
						}
						cardfix = cardfix * (100 + ele_fix) / 100;
					}
					cardfix = cardfix * (100 + sd->right_weapon.addsize[tstatus->size] + sd->arrow_addsize[tstatus->size] +
						sd->right_weapon.addsize[SZ_ALL] + sd->arrow_addsize[SZ_ALL]) / 100;
					cardfix = cardfix * (100 + sd->right_weapon.addrace2[t_race2]) / 100;
					cardfix = cardfix * (100 + sd->right_weapon.addclass[tstatus->class_] + sd->arrow_addclass[tstatus->class_] +
						sd->right_weapon.addclass[CLASS_ALL] + sd->arrow_addclass[CLASS_ALL]) / 100;
				} else { //Melee attack
					uint16 lv;

					if( !battle_config.left_cardfix_to_right ) { //Calculates each right & left hand weapon bonuses separatedly
						//Right-handed weapon
						cardfix = cardfix * (100 + sd->right_weapon.addrace[tstatus->race] + sd->right_weapon.addrace[RC_ALL]) / 100;
						if( !(nk&NK_NO_ELEFIX) ) { //Affected by element modifier bonuses
							int ele_fix = sd->right_weapon.adddefele[tstatus->def_ele] + sd->right_weapon.adddefele[ELE_ALL];

							for( i = 0; ARRAYLENGTH(sd->right_weapon.adddefele2) > i && sd->right_weapon.adddefele2[i].rate != 0; i++ ) {
								if( sd->right_weapon.adddefele2[i].ele != tstatus->def_ele )
									continue;
								if( !(((sd->right_weapon.adddefele2[i].flag)&flag)&BF_WEAPONMASK &&
									((sd->right_weapon.adddefele2[i].flag)&flag)&BF_RANGEMASK &&
									((sd->right_weapon.adddefele2[i].flag)&flag)&BF_SKILLMASK) )
									continue;
								ele_fix += sd->right_weapon.adddefele2[i].rate;
							}
							cardfix = cardfix * (100 + ele_fix) / 100;
						}
						cardfix = cardfix * (100 + sd->right_weapon.addsize[tstatus->size] + sd->right_weapon.addsize[SZ_ALL]) / 100;
						cardfix = cardfix * (100 + sd->right_weapon.addrace2[t_race2]) / 100;
						cardfix = cardfix * (100 + sd->right_weapon.addclass[tstatus->class_] + sd->right_weapon.addclass[CLASS_ALL]) / 100;
						for( i = 0; i < ARRAYLENGTH(sd->right_weapon.add_dmg) && sd->right_weapon.add_dmg[i].rate; i++ ) {
							if( sd->right_weapon.add_dmg[i].class_ != t_class )
								continue;
							cardfix = cardfix * (100 + sd->right_weapon.add_dmg[i].rate) / 100;
						}
						if( left&1 ) { //Left-handed weapon
							cardfix_ = cardfix_ * (100 + sd->left_weapon.addrace[tstatus->race] + sd->left_weapon.addrace[RC_ALL]) / 100;
							if( !(nk&NK_NO_ELEFIX) ) { //Affected by Element modifier bonuses
								int ele_fix_lh = sd->left_weapon.adddefele[tstatus->def_ele] + sd->left_weapon.adddefele[ELE_ALL];

								for( i = 0; ARRAYLENGTH(sd->left_weapon.adddefele2) > i && sd->left_weapon.adddefele2[i].rate != 0; i++ ) {
									if( sd->left_weapon.adddefele2[i].ele != tstatus->def_ele )
										continue;
									if( !(((sd->left_weapon.adddefele2[i].flag)&flag)&BF_WEAPONMASK &&
										((sd->left_weapon.adddefele2[i].flag)&flag)&BF_RANGEMASK &&
										((sd->left_weapon.adddefele2[i].flag)&flag)&BF_SKILLMASK) )
										continue;
									ele_fix_lh += sd->left_weapon.adddefele2[i].rate;
								}
								cardfix_ = cardfix_ * (100 + ele_fix_lh) / 100;
							}
							cardfix_ = cardfix_ * (100 + sd->left_weapon.addsize[tstatus->size] + sd->left_weapon.addsize[SZ_ALL]) / 100;
							cardfix_ = cardfix_ * (100 + sd->left_weapon.addrace2[t_race2]) / 100;
							cardfix_ = cardfix_ * (100 + sd->left_weapon.addclass[tstatus->class_] + sd->left_weapon.addclass[CLASS_ALL]) / 100;
							for( i = 0; i < ARRAYLENGTH(sd->left_weapon.add_dmg) && sd->left_weapon.add_dmg[i].rate; i++ ) {
								if( sd->left_weapon.add_dmg[i].class_ != t_class )
									continue;
								cardfix_ = cardfix_ * (100 + sd->left_weapon.add_dmg[i].rate) / 100;
							}
						}
					} else { //Calculates right & left hand weapon as unity
						int add_dmg = 0;

						cardfix = cardfix * (100 + sd->right_weapon.addrace[tstatus->race] + sd->left_weapon.addrace[tstatus->race] +
							sd->right_weapon.addrace[RC_ALL] + sd->left_weapon.addrace[RC_ALL]) / 100;
						if( !(nk&NK_NO_ELEFIX) ) {
							int ele_fix = sd->right_weapon.adddefele[tstatus->def_ele] + sd->left_weapon.adddefele[tstatus->def_ele] +
								sd->right_weapon.adddefele[ELE_ALL] + sd->left_weapon.adddefele[ELE_ALL];

							for( i = 0; ARRAYLENGTH(sd->right_weapon.adddefele2) > i && sd->right_weapon.adddefele2[i].rate != 0; i++ ) {
								if( sd->right_weapon.adddefele2[i].ele != tstatus->def_ele )
									continue;
								if( !(((sd->right_weapon.adddefele2[i].flag)&flag)&BF_WEAPONMASK &&
									((sd->right_weapon.adddefele2[i].flag)&flag)&BF_RANGEMASK &&
									((sd->right_weapon.adddefele2[i].flag)&flag)&BF_SKILLMASK) )
									continue;
								ele_fix += sd->right_weapon.adddefele2[i].rate;
							}
							for( i = 0; ARRAYLENGTH(sd->left_weapon.adddefele2) > i && sd->left_weapon.adddefele2[i].rate != 0; i++ ) {
								if( sd->left_weapon.adddefele2[i].ele != tstatus->def_ele )
									continue;
								if( !(((sd->left_weapon.adddefele2[i].flag)&flag)&BF_WEAPONMASK &&
									((sd->left_weapon.adddefele2[i].flag)&flag)&BF_RANGEMASK &&
									((sd->left_weapon.adddefele2[i].flag)&flag)&BF_SKILLMASK) )
									continue;
								ele_fix += sd->left_weapon.adddefele2[i].rate;
							}
							cardfix = cardfix * (100 + ele_fix) / 100;
						}
						cardfix = cardfix * (100 + sd->right_weapon.addsize[tstatus->size] + sd->left_weapon.addsize[tstatus->size] +
							sd->right_weapon.addsize[SZ_ALL] + sd->left_weapon.addsize[SZ_ALL]) / 100;
						cardfix = cardfix * (100 + sd->right_weapon.addrace2[t_race2] + sd->left_weapon.addrace2[t_race2]) / 100;
						cardfix = cardfix * (100 + sd->right_weapon.addclass[tstatus->class_] + sd->left_weapon.addclass[tstatus->class_] +
							sd->right_weapon.addclass[CLASS_ALL] + sd->left_weapon.addclass[CLASS_ALL]) / 100;
						for( i = 0; i < ARRAYLENGTH(sd->right_weapon.add_dmg) && sd->right_weapon.add_dmg[i].rate; i++ ) {
							if( sd->right_weapon.add_dmg[i].class_ != t_class )
								continue;
							add_dmg += sd->right_weapon.add_dmg[i].rate;
						}
						for( i = 0; i < ARRAYLENGTH(sd->left_weapon.add_dmg) && sd->left_weapon.add_dmg[i].rate; i++ ) {
							if( sd->left_weapon.add_dmg[i].class_ != t_class )
								continue;
							add_dmg += sd->left_weapon.add_dmg[i].rate;
						}
						cardfix = cardfix * (100 + add_dmg) / 100;
					}
					//Adv. Katar Mastery functions similar to a +%ATK card on official [helvetica]
					if( (lv = pc_checkskill(sd,ASC_KATAR)) > 0 && sd->status.weapon == W_KATAR )
						cardfix = cardfix * (100 + (10 + 2 * lv)) / 100;
				}
#ifndef RENEWAL
				if( flag&BF_LONG )
					cardfix = cardfix * (100 + sd->bonus.long_attack_atk_rate) / 100;
#endif
				if( left&1 ) {
					APPLY_CARDFIX(damage,cardfix_);
				} else {
					APPLY_CARDFIX(damage,cardfix);
				}
			} else if( tsd && !(nk&NK_NO_CARDFIX_DEF) && !(left&2) ) { //Affected by target DEF bonuses
				if( !(nk&NK_NO_ELEFIX) ) { //Affected by Element modifier bonuses
					int ele_fix = tsd->subele[rh_ele] + tsd->subele[ELE_ALL];

					for( i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++ ) {
						if( tsd->subele2[i].ele != rh_ele )
							continue;
						if( !(((tsd->subele2[i].flag)&flag)&BF_WEAPONMASK &&
							((tsd->subele2[i].flag)&flag)&BF_RANGEMASK &&
							((tsd->subele2[i].flag)&flag)&BF_SKILLMASK) )
							continue;
						ele_fix += tsd->subele2[i].rate;
					}
					cardfix = cardfix * (100 - ele_fix) / 100;
					if( left&1 && lh_ele != rh_ele ) {
						int ele_fix_lh = tsd->subele[lh_ele] + tsd->subele[ELE_ALL];

						for( i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++ ) {
							if( tsd->subele2[i].ele != lh_ele )
								continue;
							if( !(((tsd->subele2[i].flag)&flag)&BF_WEAPONMASK &&
								((tsd->subele2[i].flag)&flag)&BF_RANGEMASK &&
								((tsd->subele2[i].flag)&flag)&BF_SKILLMASK) )
								continue;
							ele_fix_lh += tsd->subele2[i].rate;
						}
						cardfix = cardfix * (100 - ele_fix_lh) / 100;
					}
					cardfix = cardfix * (100 - (tsd->subdefele[s_defele] + tsd->subdefele[ELE_ALL])) / 100;
				}
				cardfix = cardfix * (100 - (tsd->subsize[sstatus->size] + tsd->subsize[SZ_ALL])) / 100;
				cardfix = cardfix * (100 - tsd->subrace2[s_race2]) / 100;
				cardfix = cardfix * (100 - (tsd->subrace[sstatus->race] + tsd->subrace[RC_ALL])) / 100;
				cardfix = cardfix * (100 - (tsd->subclass[sstatus->class_] + tsd->subclass[CLASS_ALL])) / 100;
				for( i = 0; i < ARRAYLENGTH(tsd->add_def) && tsd->add_def[i].rate;i++ ) {
					if( tsd->add_def[i].class_ != s_class )
						continue;
					cardfix = cardfix * (100 - tsd->add_def[i].rate) / 100;
				}
				if( flag&BF_SHORT )
					cardfix = cardfix * (100 - tsd->bonus.near_attack_def_rate) / 100;
				else
					cardfix = cardfix * (100 - tsd->bonus.long_attack_def_rate) / 100;
				if( tsd->sc.data[SC_DEF_RATE] )
					cardfix = cardfix * (100 - tsd->sc.data[SC_DEF_RATE]->val1) / 100;
				APPLY_CARDFIX(damage,cardfix);
			}
			break;
		case BF_MISC:
			if( tsd && !(nk&NK_NO_CARDFIX_DEF) ) { //Affected by target DEF bonuses
				if( !(nk&NK_NO_ELEFIX) ) { //Affected by Element modifier bonuses
					int ele_fix = tsd->subele[rh_ele] + tsd->subele[ELE_ALL];

					for( i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++ ) {
						if( tsd->subele2[i].ele != rh_ele )
							continue;
						if( !(((tsd->subele2[i].flag)&flag)&BF_WEAPONMASK &&
							((tsd->subele2[i].flag)&flag)&BF_RANGEMASK &&
							((tsd->subele2[i].flag)&flag)&BF_SKILLMASK))
							continue;
						ele_fix += tsd->subele2[i].rate;
					}
					if( s_defele != ELE_NONE )
						ele_fix += tsd->subdefele[s_defele] + tsd->subdefele[ELE_ALL];
					cardfix = cardfix * (100 - ele_fix) / 100;
				}
				cardfix = cardfix * (100 - (tsd->subsize[sstatus->size] + tsd->subsize[SZ_ALL])) / 100;
				cardfix = cardfix * (100 - tsd->subrace2[s_race2]) / 100;
				cardfix = cardfix * (100 - (tsd->subrace[sstatus->race] + tsd->subrace[RC_ALL])) / 100;
				cardfix = cardfix * (100 - (tsd->subclass[sstatus->class_] + tsd->subclass[CLASS_ALL])) / 100;
				cardfix = cardfix * (100 - tsd->bonus.misc_def_rate) / 100;
				if( flag&BF_SHORT )
					cardfix = cardfix * (100 - tsd->bonus.near_attack_def_rate) / 100;
				else
					cardfix = cardfix * (100 - tsd->bonus.long_attack_def_rate) / 100;
				APPLY_CARDFIX(damage,cardfix);
			}
			break;
	}

#undef APPLY_CARDFIX

	return (int)cap_value(damage - original_damage,INT_MIN,INT_MAX);
}

/**
 * Absorb damage based on criteria
 * @param bl
 * @param d Damage
 */
static void battle_absorb_damage(struct block_list *bl, struct Damage *d)
{
	int64 dmg_ori = 0, dmg_new = 0;

	nullpo_retv(bl);
	nullpo_retv(d);

	if( !d->damage && !d->damage2 )
		return;

	switch( bl->type ) {
		case BL_PC: {
				struct map_session_data *sd = BL_CAST(BL_PC,bl);

				if( !sd )
					return;
				if( sd->bonus.absorb_dmg_maxhp ) {
					int hp = sd->bonus.absorb_dmg_maxhp * status_get_max_hp(bl) / 100;

					dmg_ori = dmg_new = d->damage + d->damage2;
					if( dmg_ori > hp )
						dmg_new = dmg_ori - hp;
				}
			}
			break;
	}

	if( dmg_ori == dmg_new )
		return;

	if( !d->damage2 )
		d->damage = dmg_new;
	else if( !d->damage )
		d->damage2 = dmg_new;
	else {
		d->damage = dmg_new;
		d->damage2 = max(dmg_new * d->damage2 / dmg_ori / 100,1);
		d->damage = d->damage - d->damage2;
	}
}

/**
 * Check if bl is shadow forming someone
 * And shadow target have the specific status type
 * @param bl
 * @param type
 */
struct status_change_entry *battle_check_shadowform(struct block_list *bl, enum sc_type type) {
	struct status_change *sc = status_get_sc(bl);
	struct map_session_data *s_sd = NULL; //Shadow target

	//Check if shadow target have the status type [exneval]
	if(sc && sc->data[SC__SHADOWFORM] && (s_sd = map_id2sd(sc->data[SC__SHADOWFORM]->val2)) && s_sd->shadowform_id == bl->id) {
		struct status_change *s_sc;

		if((s_sc = &s_sd->sc) && s_sc->data[type])
			return s_sc->data[type];
	}
	return NULL;
}

/*==========================================
 * Check damage through status.
 * ATK may be MISS, BLOCKED FAIL, reduc, increase, end status.
 * After this we apply bg/gvg reduction
 *------------------------------------------*/
int64 battle_calc_damage(struct block_list *src, struct block_list *bl, struct Damage *d, int64 damage, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = NULL, *tsd = NULL;
	struct status_change *sc, *tsc;
	struct status_data *status, *tstatus;
	struct status_change_entry *sce;
	int div = d->div_, flag = d->flag;

	nullpo_ret(bl);

	if( !damage )
		return 0;

	if( battle_config.ksprotection && mob_ksprotected(src,bl) )
		return 0;

	sd = BL_CAST(BL_PC,bl);
	sc = status_get_sc(bl);
	status = status_get_status_data(bl);
	tsd = BL_CAST(BL_PC,src);
	tsc = status_get_sc(src);
	tstatus = status_get_status_data(src);

	if( sd ) {
		if( flag&BF_WEAPON && sd->special_state.no_weapon_damage )
			damage -= damage * sd->special_state.no_weapon_damage / 100;

		if( flag&BF_MAGIC && sd->special_state.no_magic_damage )
			damage -= damage * sd->special_state.no_magic_damage / 100;

		if( flag&BF_MISC && sd->special_state.no_misc_damage )
			damage -= damage * sd->special_state.no_misc_damage / 100;

		if( !damage )
			return 0;
	}

	if( sc && sc->data[SC_INVINCIBLE] && !sc->data[SC_INVINCIBLEOFF] )
		return 1;

	switch( skill_id ) {
		case CR_GRANDCROSS:
		case NPC_GRANDDARKNESS:
			if( tsd )
				break;
			d->dmg_lv = ATK_MISS;
			return 0;
		case PA_PRESSURE:
		case HW_GRAVITATION:
			return damage; //This skill bypass everything else
	}

	if( d->isvanishdamage )
		return damage;

	if( sc && sc->count ) { //SC_* that reduce damage to 0
		if( sc->data[SC_BASILICA] && !is_boss(src) ) {
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}

		//Gravitation and Pressure do damage without removing the effect
		if( sc->data[SC_WHITEIMPRISON] ) {
			if( (skill_id && skill_get_ele(skill_id,skill_lv) == ELE_GHOST) || (!skill_id && tstatus->rhw.ele == ELE_GHOST) )
				status_change_end(bl,SC_WHITEIMPRISON,INVALID_TIMER); //Those skills do damage and removes effect
			else {
				d->dmg_lv = ATK_BLOCK;
				return 0;
			}
		}

		//Block all ranged attacks, all short-ranged skills
		//Block targeted magic skills with 70% success chance
		//Normal melee attacks and ground magic skills can still hit the player inside Zephyr
		if( sc->data[SC_ZEPHYR] && (((flag&(BF_SHORT|BF_MAGIC)) == BF_SHORT && skill_id) ||
			(flag&(BF_LONG|BF_MAGIC)) == BF_LONG || (flag&BF_MAGIC &&
			!(skill_get_inf(skill_id)&(INF_GROUND_SKILL|INF_SELF_SKILL)) && rnd()%100 < 70)) )
		{
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}

		if( (sce = sc->data[SC_SAFETYWALL]) && (flag&(BF_SHORT|BF_MAGIC)) == BF_SHORT ) {
			struct skill_unit_group *group = skill_id2group(sce->val3);
			uint16 skill_id = sce->val2;

			if( group ) {
				d->dmg_lv = ATK_BLOCK;
				switch( skill_id ) {
					case MG_SAFETYWALL:
						if( --group->val2 <= 0 )
							skill_delunitgroup(group);
#ifdef RENEWAL
						if( (group->val3 - damage) > 0 )
							group->val3 -= (int)cap_value(damage,INT_MIN,INT_MAX);
						else
							skill_delunitgroup(group);
#endif
						break;
					case MH_STEINWAND:
						if( --group->val2 <= 0 )
							skill_delunitgroup(group);
						if( (group->val3 - damage) > 0 )
							group->val3 -= (int)cap_value(damage,INT_MIN,INT_MAX);
						else
							skill_delunitgroup(group);
						break;
				}
				skill_unit_move(bl,gettick(),1); //For stacked units [exneval]
				return 0;
			}
			status_change_end(bl,SC_SAFETYWALL,INVALID_TIMER);
		}

		if( (sc->data[SC_PNEUMA] && (flag&(BF_LONG|BF_MAGIC)) == BF_LONG) || sc->data[SC__MANHOLE] || sc->data[SC_KINGS_GRACE] ) {
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}

		if( (sce = sc->data[SC_WEAPONBLOCKING]) && (flag&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON) &&
			rnd()%100 < sce->val2 ) {
			clif_skill_nodamage(bl,src,GC_WEAPONBLOCKING,sce->val1,1);
			sc_start2(src,bl,SC_COMBO,100,GC_WEAPONBLOCKING,src->id,skill_get_time2(GC_WEAPONBLOCKING,sce->val1));
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}

		if( ((sce = sc->data[SC_AUTOGUARD]) || (sce = battle_check_shadowform(bl,SC_AUTOGUARD))) && flag&BF_WEAPON &&
#ifdef RENEWAL
			skill_id != WS_CARTTERMINATION &&
#endif
			(skill_id == RK_DRAGONBREATH || skill_id == RK_DRAGONBREATH_WATER ||
			!(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK)) && rnd()%100 < sce->val2 )
		{
			int delay;
			struct status_change_entry *sce_d = sc->data[SC_DEVOTION];
			struct status_change_entry *sce_s = sc->data[SC__SHADOWFORM];
			struct map_session_data *s_sd = NULL;
			struct block_list *d_bl = NULL;

			//Different delay depending on skill level [celest]
			if( sce->val1 <= 5 )
				delay = 300;
			else if( sce->val1 > 5 && sce->val1 <= 9 )
				delay = 200;
			else
				delay = 100;
			if( sd && pc_issit(sd) )
				pc_setstand(sd);
			if( sce_d && (d_bl = map_id2bl(sce_d->val1)) &&
				((d_bl->type == BL_MER && ((TBL_MER *)d_bl)->master && ((TBL_MER *)d_bl)->master->bl.id == bl->id) ||
				(d_bl->type == BL_PC && ((TBL_PC *)d_bl)->devotion[sce_d->val2] == bl->id)) &&
				check_distance_bl(bl,d_bl,sce_d->val3) )
			{ //If player is target of devotion, show guard effect on the devotion caster rather than the target
				clif_skill_nodamage(d_bl,d_bl,CR_AUTOGUARD,sce->val1,1);
				unit_set_walkdelay(d_bl,gettick(),delay,1);
				d->dmg_lv = ATK_MISS;
				return 0;
			} else if( sce_s && (s_sd = map_id2sd(sce_s->val2)) && s_sd->shadowform_id == bl->id ) {
				clif_skill_nodamage(&s_sd->bl,&s_sd->bl,CR_AUTOGUARD,sce->val1,1);
				unit_set_walkdelay(&s_sd->bl,gettick(),delay,1);
				d->dmg_lv = ATK_MISS;
				return 0;
			} else {
				clif_skill_nodamage(bl,bl,CR_AUTOGUARD,sce->val1,1);
				unit_set_walkdelay(bl,gettick(),delay,1);
				if( sc->data[SC_SHRINK] && rnd()%100 < 5 * sce->val1 )
					skill_blown(bl,src,skill_get_blewcount(CR_SHRINK,1),-1,0);
				d->dmg_lv = ATK_MISS;
				return 0;
			}
		}

		if( damage > 0 && (sce = sc->data[SC_MILLENNIUMSHIELD]) && sce->val2 > 0 ) {
			sce->val3 -= (int)cap_value(damage,INT_MIN,INT_MAX); //Absorb damage
			d->dmg_lv = ATK_BLOCK;
			if( sce->val3 <= 0 ) { //Shield down
				sce->val2--;
				if( sce->val2 >= 0 ) {
					clif_millenniumshield(bl,sce->val2);
					if( !sce->val2 )
						status_change_end(bl,SC_MILLENNIUMSHIELD,INVALID_TIMER); //All shields down
					else
						sce->val3 = 1000; //Next shield
				}
				status_change_start(src,bl,SC_STUN,10000,0,0,0,0,1000,SCFLAG_FIXEDTICK);
			}
			return 0;
		}

		if( (sce = sc->data[SC_PARRYING]) && flag&BF_WEAPON &&
#ifdef RENEWAL
			skill_id != WS_CARTTERMINATION &&
#endif
			(skill_id == RK_DRAGONBREATH || skill_id == RK_DRAGONBREATH_WATER ||
			!(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK)) && rnd()%100 < sce->val2 )
		{
			clif_skill_nodamage(bl,bl,LK_PARRYING,sce->val1,1);
			return 0; //Attack blocked by Parrying
		}

		if( (sce = sc->data[SC_DODGE]) && ((flag&BF_LONG) || sc->data[SC_STRUP]) && rnd()%100 < 20 ) {
			if( sd && pc_issit(sd) )
				pc_setstand(sd); //Stand it to dodge
			clif_skill_nodamage(bl,bl,TK_DODGE,sce->val1,1);
			sc_start4(src,bl,SC_COMBO,100,TK_JUMPKICK,src->id,1,0,2000);
			return 0;
		}

		if( sc->data[SC_HERMODE] && flag&BF_MAGIC )
			return 0;

		if( sc->data[SC_TATAMIGAESHI] && (flag&(BF_LONG|BF_MAGIC)) == BF_LONG )
			return 0;

		if( sc->data[SC_NEUTRALBARRIER] && (flag&(BF_LONG|BF_MAGIC)) == BF_LONG &&
			skill_id != NJ_ZENYNAGE && skill_id != KO_MUCHANAGE ) {
			d->dmg_lv = ATK_MISS;
			return 0;
		}

		//Kaupe blocks damage (skill or otherwise) from players, mobs, homuns, mercenaries
		if( (sce = sc->data[SC_KAUPE]) && rnd()%100 < sce->val2 ) {
			clif_specialeffect(bl,462,AREA);
#ifndef RENEWAL //Shouldn't end until Breaker's non-weapon part connects
			if( skill_id != ASC_BREAKER || !(flag&BF_WEAPON) )
#endif
				if( --(sce->val3) <= 0 ) //We make it work like Safety Wall, even though it only blocks 1 time
					status_change_end(bl,SC_KAUPE,INVALID_TIMER);
			return 0;
		}

#ifdef RENEWAL //Renewal: Increases the physical damage the target takes by 400% [exneval]
		if( (sce = sc->data[SC_KAITE]) && sce->val3 && (flag&(BF_SHORT|BF_MAGIC)) == BF_SHORT )
			damage <<= 2;
#endif

		if( (sce = sc->data[SC_PRESTIGE]) && flag&BF_MAGIC && rnd()%100 < sce->val2 ) {
			clif_specialeffect(bl,462,AREA); //Still need confirm it
			return 0;
		}

		if( ((sce = sc->data[SC_UTSUSEMI]) || sc->data[SC_BUNSINJYUTSU]) && flag&BF_WEAPON &&
#ifdef RENEWAL
			skill_id != WS_CARTTERMINATION &&
#endif
			(skill_id == RK_DRAGONBREATH || skill_id == RK_DRAGONBREATH_WATER ||
			!(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK)) )
		{
			skill_additional_effect(src,bl,skill_id,skill_lv,flag,ATK_BLOCK,gettick());
			if( !status_isdead(src) )
				skill_counter_additional_effect(src,bl,skill_id,skill_lv,flag,gettick());
			if( sce ) {
				clif_specialeffect(bl,462,AREA);
				skill_blown(src,bl,sce->val3,-1,0);
			}
			//Both need to be consumed if they are active
			if( sce && --(sce->val2) <= 0 )
				status_change_end(bl,SC_UTSUSEMI,INVALID_TIMER);
			if( (sce = sc->data[SC_BUNSINJYUTSU]) && --(sce->val2) <= 0 )
				status_change_end(bl,SC_BUNSINJYUTSU,INVALID_TIMER);
			return 0;
		}

		if( sc->data[SC_AETERNA] && skill_id != PF_SOULBURN ) { //Now damage increasing effects
			if( src->type != BL_MER || !skill_id )
				damage <<= 1; //Lex Aeterna only doubles damage of regular attacks from mercenaries
#ifndef RENEWAL //Shouldn't end until Breaker's non-weapon part connects
			if( skill_id != ASC_BREAKER || !(flag&BF_WEAPON) )
#endif
				status_change_end(bl,SC_AETERNA,INVALID_TIMER);
		}

#ifdef RENEWAL
		if( sc->data[SC_RAID] ) {
			damage += damage * 20 / 100;
			if( --sc->data[SC_RAID]->val1 == 0 )
				status_change_end(bl,SC_RAID,INVALID_TIMER);
		}
#else
		//Damage reductions
		if( sc->data[SC_ASSUMPTIO] ) {
			if( map_flag_vs(bl->m) )
				damage = damage * 2 / 3; //Receive 66% damage
			else
				damage >>= 1; //Receive 50% damage
		}
#endif

		if( damage > 0 ) {
			if( sc->data[SC_DEEPSLEEP] ) {
				damage += damage / 2; //1.5 times more damage while in Deep Sleep
				status_change_end(bl,SC_DEEPSLEEP,INVALID_TIMER);
			}
			if( tsd && sd && sc->data[SC_CRYSTALIZE] && flag&BF_WEAPON ) {
				switch( tsd->status.weapon ) {
					case W_MACE:
					case W_2HMACE:
					case W_1HAXE:
					case W_2HAXE:
						damage += damage / 2;
						break;
					case W_MUSICAL:
					case W_WHIP:
						if( !sd->state.arrow_atk )
							break;
					//Fall through
					case W_BOW:
					case W_REVOLVER:
					case W_RIFLE:
					case W_GATLING:
					case W_SHOTGUN:
					case W_GRENADE:
					case W_DAGGER:
					case W_1HSWORD:
					case W_2HSWORD:
						damage -= damage / 2;
						break;
				}
			}
			if( sc->data[SC_VOICEOFSIREN] )
				status_change_end(bl,SC_VOICEOFSIREN,INVALID_TIMER);
		}

		if( ((sce = sc->data[SC_DEFENDER]) || (sce = battle_check_shadowform(bl,SC_DEFENDER))) &&
			(flag&(BF_LONG|BF_MAGIC)) == BF_LONG &&
#ifndef RENEWAL
			skill_id != HT_BLITZBEAT && skill_id != SN_FALCONASSAULT &&
			skill_id != ASC_BREAKER && skill_id != CR_ACIDDEMONSTRATION &&
#endif
			skill_id != NJ_ZENYNAGE && skill_id != KO_MUCHANAGE )
			damage -= damage * sce->val2 / 100;

#ifndef RENEWAL
		if( sc->data[SC_ADJUSTMENT] && (flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON) )
			damage -= damage * 20 / 100;
#endif

		if( sc->data[SC_FOGWALL] ) {
			if( flag&BF_SKILL ) {
				if( !(skill_get_inf(skill_id)&INF_GROUND_SKILL) && !(skill_get_nk(skill_id)&NK_SPLASH) )
					damage -= damage * 25 / 100; //25% reduction
			} else if( (flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON) )
				damage >>= 2; //75% reduction
		}

		if( sc->data[SC_SMOKEPOWDER] ) {
			if( (flag&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON) )
				damage -= damage * 15 / 100; //15% reduction to physical melee attacks
			else if( (flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON) )
				damage -= damage * 50 / 100; //50% reduction to physical ranged attacks
		}

		if( sc->data[SC_WATER_BARRIER] )
			damage = damage * 80 / 100; //20% reduction to all type attacks

		if( sc->data[SC_SU_STOOP] )
			damage -= damage * 90 / 100;

		if( (sce = sc->data[SC_ARMOR]) && //NPC_DEFENDER
			sce->val3&flag && sce->val4&flag )
			damage -= damage * sce->val2 / 100;

		if( sc->data[SC_ENERGYCOAT]
#ifndef RENEWAL
			&& flag&BF_WEAPON && (skill_id == RK_DRAGONBREATH || skill_id == RK_DRAGONBREATH_WATER ||
			!(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK))
#endif
		) {
			int per = 100 * status->sp / status->max_sp - 1; //100% should be counted as the 80~99% interval

			per /= 20; //Uses 20% SP intervals
			//SP Cost: 1% + 0.5% per every 20% SP
			if( !status_charge(bl,0,(10 + 5 * per) * status->max_sp / 1000) )
				status_change_end(bl,SC_ENERGYCOAT,INVALID_TIMER);
			damage -= damage * 6 * (1 + per) / 100; //Reduction: 6% + 6% every 20%
		}

		if( (sce = sc->data[SC_GRANITIC_ARMOR]) )
			damage -= damage * sce->val2 / 100;

		if( (sce = sc->data[SC_PAIN_KILLER]) ) {
			int div_ = (skill_id ? skill_get_num(skill_id,skill_lv) : div);

			damage -= (div_ < 0 ? sce->val3 : div_ * sce->val3);
			damage = max(damage,1);
		}

		if( (sce = sc->data[SC_DARKCROW]) && (flag&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON) )
			damage += damage * sce->val2 / 100;

		if( (sce = sc->data[SC_MAGMA_FLOW]) && rnd()%100 < sce->val2 )
			skill_castend_nodamage_id(bl,bl,MH_MAGMA_FLOW,sce->val1,gettick(),flag|2);

		if( damage > 0 && (sce = sc->data[SC_STONEHARDSKIN]) && (flag&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON) ) {
			if( src->type == BL_MOB ) //Using explicit call instead break_equip for duration
				sc_start(src,src,SC_STRIPWEAPON,30,0,skill_get_time2(RK_STONEHARDSKIN,sce->val1));
			else
				skill_break_equip(src,src,EQP_WEAPON,3000,BCT_SELF);
		}

#ifdef RENEWAL
		if( sc->data[SC_STEELBODY] ) //Renewal: Steel Body reduces all incoming damage to 1/10 [helvetica]
			damage = (damage > 10 ? damage / 10 : 1);

		if( (sce = sc->data[SC_ARMORCHANGE]) ) {
			if( flag&BF_WEAPON )
				damage -= damage * sce->val2 / 100;
			if( flag&BF_MAGIC )
				damage -= damage * sce->val3 / 100;
		}
#endif

		//Finally added to remove the status of immobile when Aimed Bolt is used [Jobbie]
		if( skill_id == RA_AIMEDBOLT && (sc->data[SC_ANKLE] || sc->data[SC_ELECTRICSHOCKER] || sc->data[SC_BITE]) ) {
			status_change_end(bl,SC_ANKLE,INVALID_TIMER);
			status_change_end(bl,SC_ELECTRICSHOCKER,INVALID_TIMER);
			status_change_end(bl,SC_BITE,INVALID_TIMER);
		}

		if( damage > 0 ) {
			if( (sce = sc->data[SC_KYRIE]) ) { //Finally Kyrie because it may, or not, reduce damage to 0
				sce->val2 -= (int)cap_value(damage,INT_MIN,INT_MAX);
				if( flag&BF_WEAPON ) {
					if( sce->val2 >= 0 )
						damage = 0;
					else
						damage = -sce->val2;
				}
				if( --sce->val3 <= 0 || sce->val2 <= 0 || skill_id == AL_HOLYLIGHT )
					status_change_end(bl,SC_KYRIE,INVALID_TIMER);
			}
			if( (sce = sc->data[SC_TUNAPARTY]) ) {
				sce->val2 -= (int)cap_value(damage,INT_MIN,INT_MAX);
				if( sce->val2 >= 0 )
					damage = 0;
				else
					damage = -sce->val2;
				if( sce->val2 <= 0 )
					status_change_end(bl,SC_TUNAPARTY,INVALID_TIMER);
			}
			if( sc->data[SC_MEIKYOUSISUI] && rnd()%100 < 40 ) //Custom value
				status_change_end(bl,SC_MEIKYOUSISUI,INVALID_TIMER);
		} else
			return 0;

		if( (sce = sc->data[SC_LIGHTNINGWALK]) && (flag&(BF_LONG|BF_MAGIC)) == BF_LONG && rnd()%100 < sce->val2 ) {
			int dx[8] = { 0,-1,-1,-1,0,1,1,1 };
			int dy[8] = { 1,1,0,-1,-1,-1,0,1 };
			uint8 dir = map_calc_dir(bl,src->x,src->y);

			if( !map_flag_gvg2(src->m) && !map[src->m].flag.battleground &&
				unit_movepos(bl,src->x - dx[dir],src->y - dy[dir],1,true) ) {
				clif_blown(bl,src);
				unit_setdir(bl,dir);
			}
			d->dmg_lv = ATK_DEF;
			status_change_end(bl,SC_LIGHTNINGWALK,INVALID_TIMER);
			return 0;
		}

		if( sd && (sce = sc->data[SC_FORCEOFVANGUARD]) && flag&BF_WEAPON && rnd()%100 < sce->val2 )
			pc_addspiritball(sd,skill_get_time(LG_FORCEOFVANGUARD,sce->val1),sce->val3);

		if( sd && (sce = sc->data[SC_GT_ENERGYGAIN]) && flag&BF_WEAPON && rnd()%100 < sce->val2 )
			pc_addspiritball(sd,skill_get_time2(SR_GENTLETOUCH_ENERGYGAIN,sce->val1),pc_getmaxspiritball(sd,5));

		if( (sce = sc->data[SC__DEADLYINFECT]) && (flag&(BF_SHORT|BF_MAGIC)) == BF_SHORT && rnd()%100 < 30 + 10 * sce->val1 )
			status_change_spread(bl,src,true); //Deadly infect attacked side

		if( (sce = sc->data[SC_STYLE_CHANGE]) && sce->val1 == MH_MD_GRAPPLING ) { //When being hit
			TBL_HOM *hd = BL_CAST(BL_HOM,bl);

			if( hd && rnd()%100 < status_get_lv(&hd->bl) / 2 )
				hom_addspiritball(hd,10);
		}
	}

	//SC effects from caster's side
	if( tsc && tsc->count ) {
		if( tsc->data[SC_INVINCIBLE] && !tsc->data[SC_INVINCIBLEOFF] )
			damage += damage * 75 / 100;

		if( damage > 0 ) {
			if( flag&BF_WEAPON ) {
				if( (sce = tsc->data[SC_POISONINGWEAPON]) && skill_id != GC_VENOMPRESSURE && rnd()%100 < sce->val3 )
					sc_start(src,bl,(sc_type)sce->val2,100,sce->val1,skill_get_time2(GC_POISONINGWEAPON,1));
				if( (sce = tsc->data[SC_SHIELDSPELL_REF]) && sce->val1 == 1 ) {
					skill_break_equip(src,bl,EQP_ARMOR,10000,BCT_ENEMY);
					status_change_end(src,SC_SHIELDSPELL_REF,INVALID_TIMER);
				}
				if( (sce = tsc->data[SC_GT_ENERGYGAIN]) && rnd()%100 < sce->val2 && tsd )
					pc_addspiritball(tsd,skill_get_time2(SR_GENTLETOUCH_ENERGYGAIN,sce->val1),pc_getmaxspiritball(tsd,5));
				if( (sce = tsc->data[SC_BLOODLUST]) && rnd()%100 < sce->val3 )
					status_heal(src,damage * sce->val4 / 100,0,3);
			}
			if( (sce = tsc->data[SC__DEADLYINFECT]) && (flag&(BF_SHORT|BF_MAGIC)) == BF_SHORT && rnd()%100 < 30 + 10 * sce->val1 )
				status_change_spread(src,bl,false);
			if( (sce = tsc->data[SC_STYLE_CHANGE]) && sce->val1 == MH_MD_FIGHTING ) { //When attacking
				TBL_HOM *hd = BL_CAST(BL_HOM,src);

				if( hd && rnd()%100 < 20 + status_get_lv(&hd->bl) / 5 )
					hom_addspiritball(hd,10);
			}
		}
	}

	//PK damage rates
	if( damage > 0 && battle_config.pk_mode && sd && bl->type == BL_PC && map[bl->m].flag.pvp ) {
		if( flag&BF_SKILL ) { //Skills get a different reduction than non-skills [Skotlex]
			if( flag&BF_WEAPON )
				damage = damage * battle_config.pk_weapon_damage_rate / 100;
			if( flag&BF_MAGIC )
				damage = damage * battle_config.pk_magic_damage_rate / 100;
			if( flag&BF_MISC )
				damage = damage * battle_config.pk_misc_damage_rate / 100;
		} else { //Normal attacks get reductions based on range
			if( flag&BF_SHORT )
				damage = damage * battle_config.pk_short_damage_rate / 100;
			if( flag&BF_LONG )
				damage = damage * battle_config.pk_long_damage_rate / 100;
		}
		damage = max(damage,1); //Min 1 damage
	}

	if( bl->type == BL_MOB && !status_isdead(bl) && bl->id != src->id ) {
		if( damage > 0 )
			mobskill_event((TBL_MOB *)bl,src,gettick(),flag);
		if( skill_id )
			mobskill_event((TBL_MOB *)bl,src,gettick(),MSC_SKILLUSED|(skill_id<<16));
	}

	if( sd ) {
		if( pc_ismadogear(sd) && rnd()%100 < 50 ) {
			int element = skill_get_ele(skill_id,skill_lv);

			if( !skill_id || element == -1 ) //Take weapon's element
				element = (tsd && tsd->bonus.arrow_ele ? tsd->bonus.arrow_ele : tstatus->rhw.ele);
			else if( element == -2 ) //Use enchantment's element
				element = status_get_attack_sc_element(src,status_get_sc(src));
			else if( element == -3 ) //Use random element
				element = rnd()%ELE_ALL;
			if( element == ELE_FIRE || element == ELE_WATER )
				pc_overheat(sd,(element == ELE_FIRE ? 1 : -1));
		}
	}

	if( damage && status->mode&MD_PLANT && battle_config.skill_min_damage ) {
		if( (flag&BF_WEAPON && battle_config.skill_min_damage&1) ||
			(flag&BF_MAGIC && battle_config.skill_min_damage&2) ||
			(flag&BF_MISC && battle_config.skill_min_damage&4) )
		{
			int div_ = (skill_id ? skill_get_num(skill_id,skill_lv) : div);

			switch( skill_id ) {
				case SU_CN_METEOR:
				case SU_LUNATICCARROTBEAT:
					damage = div_ * -1;
					break;
				default:
					damage = max(div_,0);
					//Damage that just look like multiple hits but are actually one will show "miss" but still do 1 damage to plants
					if( damage > 1 && d->miscflag&1 ) {
						damage = 1;
						d->dmg_lv = ATK_FLEE;
					}
					break;
			}
		}
	}

	return damage;
}

/*==========================================
 * Calculates BG related damage adjustments.
 *------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
int64 battle_calc_bg_damage(struct block_list *src, struct block_list *bl, int64 damage, uint16 skill_id, int flag)
{
	if( !damage )
		return 0;

	if( bl->type == BL_MOB ) {
		struct mob_data *md = BL_CAST(BL_MOB, bl);

		if( map[bl->m].flag.battleground && (md->mob_id == MOBID_BLUE_CRYST || md->mob_id == MOBID_PINK_CRYST) && flag&BF_SKILL )
			return 0; //Crystal cannot receive skill damage on battlegrounds
	}

	if( skill_get_inf2(skill_id)&INF2_NO_BG_GVG_DMG )
		return damage; //Skill that ignore bg map reduction

	if( flag&BF_SKILL ) { //Skills get a different reduction than non-skills. [Skotlex]
		if( flag&BF_WEAPON )
			damage = damage * battle_config.bg_weapon_damage_rate / 100;
		if( flag&BF_MAGIC )
			damage = damage * battle_config.bg_magic_damage_rate / 100;
		if(	flag&BF_MISC )
			damage = damage * battle_config.bg_misc_damage_rate / 100;
	} else { //Normal attacks get reductions based on range.
		if( flag&BF_SHORT )
			damage = damage * battle_config.bg_short_damage_rate / 100;
		if( flag&BF_LONG )
			damage = damage * battle_config.bg_long_damage_rate / 100;
	}

	damage = max(damage,1);
	return damage;
}

bool battle_can_hit_gvg_target(struct block_list *src, struct block_list *bl, uint16 skill_id, int flag)
{
	struct mob_data *md = BL_CAST(BL_MOB, bl);
	short mob_id = ((TBL_MOB *)bl)->mob_id;

	if( md && md->guardian_data ) {
		if( mob_id == MOBID_EMPERIUM && flag&BF_SKILL && !(skill_get_inf3(skill_id)&INF3_HIT_EMP) )
			return false; //Skill immunity
		if( src->type != BL_MOB || mob_is_clone(((struct mob_data *)src)->mob_id) ) {
			struct guild *g = (src->type == BL_PC ? ((TBL_PC *)src)->guild : guild_search(status_get_guild_id(src)));

			if( mob_id == MOBID_EMPERIUM && (!g || guild_checkskill(g,GD_APPROVAL) <= 0) )
				return false;
			if( g && battle_config.guild_max_castles && guild_checkcastles(g) >= battle_config.guild_max_castles )
				return false; //[MouseJstr]
		}
	}

	return true;
}

/*==========================================
 * Calculates GVG related damage adjustments.
 *------------------------------------------*/
int64 battle_calc_gvg_damage(struct block_list *src, struct block_list *bl, int64 damage, uint16 skill_id, int flag)
{
	if( !damage ) //No reductions to make
		return 0;

	if( !battle_can_hit_gvg_target(src,bl,skill_id,flag) )
		return 0;

	if( skill_get_inf2(skill_id)&INF2_NO_BG_GVG_DMG )
		return damage;

#ifndef RENEWAL
	//if( md && md->guardian_data ) //Uncomment if you want god-mode Emperiums at 100 defense [Kisuka]
		//damage -= damage * md->guardian_data->castle->defense / 100 * battle_config.castle_defense_rate / 100;
#endif

	if( flag&BF_SKILL ) { //Skills get a different reduction than non-skills [Skotlex]
		if( flag&BF_WEAPON )
			damage = damage * battle_config.gvg_weapon_damage_rate / 100;
		if( flag&BF_MAGIC )
			damage = damage * battle_config.gvg_magic_damage_rate / 100;
		if( flag&BF_MISC )
			damage = damage * battle_config.gvg_misc_damage_rate / 100;
	} else { //Normal attacks get reductions based on range
		if( flag&BF_SHORT )
			damage = damage * battle_config.gvg_short_damage_rate / 100;
		if( flag&BF_LONG )
			damage = damage * battle_config.gvg_long_damage_rate / 100;
	}

	damage  = max(damage,1);
	return damage;
}

/*==========================================
 * HP/SP drain calculation
 *------------------------------------------*/
static int battle_calc_drain(int64 damage, int rate, int per)
{
	int64 diff = 0;

	if(per && (rate > 1000 || rnd()%1000 < rate)) {
		diff = damage * per / 100;
		if(diff == 0) {
			if(per > 0)
				diff = 1;
			else
				diff = -1;
		}
	}
	return (int)cap_value(diff,INT_MIN,INT_MAX);
}

/*==========================================
 * Passive skill damage increases
 *------------------------------------------*/
int64 battle_addmastery(struct map_session_data *sd, struct block_list *target, int64 dmg, int type)
{
	int64 damage;
	struct status_data *status = status_get_status_data(target);
	int weapon;
	uint16 lv;

#ifdef RENEWAL
	damage = 0;
#else
	damage = dmg;
#endif

	nullpo_ret(sd);

	if((lv = pc_checkskill(sd,AL_DEMONBANE)) > 0 &&
		target->type == BL_MOB && //This bonus doesn't work against players.
		(battle_check_undead(status->race,status->def_ele) || status->race == RC_DEMON))
		damage += lv * (int)(3 + (sd->status.base_level + 1) * 0.05); //[orn]

	if((lv = pc_checkskill(sd,HT_BEASTBANE)) > 0 && (status->race == RC_BRUTE || status->race == RC_INSECT) ) {
		damage += lv * 4;
		if(sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_HUNTER)
			damage += sd->status.str;
	}

#ifdef RENEWAL
	if((lv = pc_checkskill(sd,BS_WEAPONRESEARCH)) > 0) //Weapon Research bonus applies to all weapons
		damage += lv * 2;
#endif

	if((lv = pc_checkskill(sd,RA_RANGERMAIN)) > 0 && (status->race == RC_BRUTE || status->race == RC_PLANT || status->race == RC_FISH))
		damage += lv * 5;

	if((lv = pc_checkskill(sd,NC_RESEARCHFE)) > 0 && (status->def_ele == ELE_FIRE || status->def_ele == ELE_EARTH))
		damage += lv * 10;

	damage += pc_checkskill(sd,NC_MADOLICENCE) * 15; //Attack bonus is granted even without the Madogear
	weapon = (!type ? sd->weapontype1 : sd->weapontype2);

	switch(weapon) {
		case W_1HSWORD:
#ifdef RENEWAL
			if((lv = pc_checkskill(sd,AM_AXEMASTERY)) > 0)
				damage += lv * 3;
#endif
		//Fall through
		case W_DAGGER:
			if((lv = pc_checkskill(sd,SM_SWORD)) > 0)
				damage += lv * 4;
			if((lv = pc_checkskill(sd,GN_TRAINING_SWORD)) > 0)
				damage += lv * 10;
			break;
		case W_2HSWORD:
#ifdef RENEWAL
			if((lv = pc_checkskill(sd,AM_AXEMASTERY)) > 0)
				damage += lv * 3;
#endif
			if((lv = pc_checkskill(sd,SM_TWOHAND)) > 0)
				damage += lv * 4;
			break;
		case W_1HSPEAR:
		case W_2HSPEAR:
			if((lv = pc_checkskill(sd,KN_SPEARMASTERY)) > 0) {
				if(!pc_isriding(sd) && !pc_isridingdragon(sd))
					damage += lv * 4;
				else
					damage += lv * 5;
				if(pc_checkskill(sd,RK_DRAGONTRAINING) > 0)
					damage += lv * 10; //Increase damage by level of KN_SPEARMASTERY * 10
			}
			break;
		case W_1HAXE:
		case W_2HAXE:
			if((lv = pc_checkskill(sd,AM_AXEMASTERY)) > 0)
				damage += lv * 3;
			if((lv = pc_checkskill(sd,NC_TRAININGAXE)) > 0)
				damage += lv * 5;
			break;
		case W_MACE:
		case W_2HMACE:
			if((lv = pc_checkskill(sd,PR_MACEMASTERY)) > 0)
				damage += lv * 3;
			if((lv = pc_checkskill(sd,NC_TRAININGAXE)) > 0)
				damage += lv * 4;
			break;
		case W_FIST:
			if((lv = pc_checkskill(sd,TK_RUN)) > 0)
				damage += lv * 10; //+ATK (Bare Handed)
		//Fall through
		case W_KNUCKLE:
			if((lv = pc_checkskill(sd,MO_IRONHAND)) > 0)
				damage += lv * 3;
			break;
		case W_MUSICAL:
			if((lv = pc_checkskill(sd,BA_MUSICALLESSON)) > 0)
				damage += lv * 3;
			break;
		case W_WHIP:
			if((lv = pc_checkskill(sd,DC_DANCINGLESSON)) > 0)
				damage += lv * 3;
			break;
		case W_BOOK:
			if((lv = pc_checkskill(sd,SA_ADVANCEDBOOK)) > 0)
				damage += lv * 3;
			break;
		case W_KATAR:
			if((lv = pc_checkskill(sd,AS_KATAR)) > 0)
				damage += lv * 3;
			break;
	}

	return damage;
}

#ifdef RENEWAL
static int battle_calc_sizefix(int64 damage, struct map_session_data *sd, short t_size, unsigned char weapon_type, bool flag)
{
	if(sd && !sd->special_state.no_sizefix && !flag) //Size fix only for player
		damage = damage * (weapon_type == EQI_HAND_L ? sd->left_weapon.atkmods[t_size] : sd->right_weapon.atkmods[t_size]) / 100;
	return (int)cap_value(damage,INT_MIN,INT_MAX);
}

static int battle_calc_status_attack(struct status_data *status, short hand)
{
	if(hand == EQI_HAND_L)
		return status->batk; //Left-hand penalty on sATK is always 50% [Baalberith]
	else
		return status->batk * 2;
}

static int battle_calc_base_weapon_attack(struct block_list *src, struct status_data *tstatus, struct weapon_atk *watk, struct map_session_data *sd, uint16 skill_id)
{
	struct status_data *status = status_get_status_data(src);
	struct status_change *sc = status_get_sc(src);
	uint8 type = (watk == &status->lhw ? EQI_HAND_L : EQI_HAND_R), refine;
	uint16 atkmin = (type == EQI_HAND_L ? status->watk2 : status->watk);
	uint16 atkmax = atkmin;
	int64 damage;
	bool weapon_perfection = false;
	short index = sd->equip_index[type];

	if(index >= 0 && sd->inventory_data[index]) {
		float strdex_bonus, variance;
		short flag = 0, dstr;

		switch(sd->status.weapon) {
			case W_BOW:	case W_MUSICAL:
			case W_WHIP:	case W_REVOLVER:
			case W_RIFLE:	case W_GATLING:
			case W_SHOTGUN:	case W_GRENADE:
				flag = 1;
				break;
		}
		if(flag)
			dstr = status->dex;
		else
			dstr = status->str;
		variance = 5.0f * watk->atk * watk->wlv / 100.0f;
		strdex_bonus = watk->atk * dstr / 200.0f;
		atkmin = max(0, (int)(atkmin - variance + strdex_bonus));
		atkmax = min(UINT16_MAX, (int)(atkmax + variance + strdex_bonus));
	}

	if(!(sc && sc->data[SC_MAXIMIZEPOWER])) {
		if(atkmax > atkmin)
			atkmax = atkmin + rnd()%(atkmax - atkmin + 1);
		else
			atkmax = atkmin;
	}

	if(type == EQI_HAND_R && index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON &&
		watk->range <= 3 && (refine = sd->status.inventory[index].refine) < 16 && refine) {
		int r = refine_info[watk->wlv].randombonus_max[refine + (4 - watk->wlv)] / 100;

		if(r)
			atkmax += (rnd()%100)%r + 1;
	}

	damage = atkmax;

	switch(skill_id) { //Ignore size fix
		case MO_EXTREMITYFIST:
		case NJ_ISSEN:
		case CR_SHIELDBOOMERANG:
		case PA_SHIELDCHAIN:
			weapon_perfection = true;
			break;
		default:
			if(sc && sc->data[SC_WEAPONPERFECTION])
				weapon_perfection = true;
			break;
	}

	damage = battle_calc_sizefix(damage, sd, tstatus->size, type, weapon_perfection);
	return (int)cap_value(damage, INT_MIN, INT_MAX);
}
#endif

/*==========================================
 * Calculates the standard damage of a normal attack assuming it hits,
 * it calculates nothing extra fancy, is needed for magnum break's WATK_ELEMENT bonus. [Skotlex]
 * This applies to pre-renewal and non-sd in renewal
 *------------------------------------------
 * Pass damage2 as NULL to not calc it.
 * Flag values:
 * &1 : Critical hit
 * &2 : Arrow attack
 * &4 : Skill is Magic Crasher / Lif Change
 * &8 : Skip target size adjustment (Extremity Fist)
 * &16: Arrow attack but BOW, REVOLVER, RIFLE, SHOTGUN, GATLING or GRENADE type weapon not equipped (i.e. shuriken, kunai and venom knives not affected by DEX)
 *
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static int64 battle_calc_base_damage(struct block_list *src, struct status_data *status, struct weapon_atk *watk, struct status_change *sc, unsigned short t_size, struct map_session_data *sd, int flag)
{
	unsigned int atkmin = 0, atkmax = 0;
	short type = 0;
	int64 damage = 0;

	if(!sd) { //Mobs/Pets
		if(flag&4) {
			atkmin = status->matk_min;
			atkmax = status->matk_max;
		} else {
			atkmin = watk->atk;
			atkmax = watk->atk2;
#ifdef RENEWAL
			if(src->type == BL_MOB) {
				atkmin = watk->atk * 80 / 100;
				atkmax = watk->atk * 120 / 100;
			}
#endif
		}
		if(atkmin > atkmax)
			atkmin = atkmax;
	} else { //PCs
		atkmax = watk->atk;
		type = (watk == &status->lhw ? EQI_HAND_L : EQI_HAND_R);
		if(!(flag&1) || (flag&2)) { //Normal attacks
			atkmin = status->dex;
			if(sd->equip_index[type] >= 0 && sd->inventory_data[sd->equip_index[type]])
				atkmin = atkmin * (80 + sd->inventory_data[sd->equip_index[type]]->wlv * 20) / 100;
			if(atkmin > atkmax)
				atkmin = atkmax;
			if(flag&2 && !(flag&16)) { //Bows
				atkmin = atkmin * atkmax / 100;
				if (atkmin > atkmax)
					atkmax = atkmin;
			}
		}
	}

	if(sc && sc->data[SC_MAXIMIZEPOWER])
		atkmin = atkmax;

	//Weapon Damage calculation
	if(!(flag&1))
		damage = (atkmax > atkmin ? rnd()%(atkmax - atkmin) : 0) + atkmin;
	else
		damage = atkmax;

	if(sd) {
		//Rodatazone says the range is 0~arrow_atk-1 for non crit
		if(flag&2 && sd->bonus.arrow_atk)
			damage += ((flag&1) ? sd->bonus.arrow_atk : rnd()%sd->bonus.arrow_atk);
		//Size fix only for player
		if(!(sd->special_state.no_sizefix || (flag&8)))
			damage = damage * (type == EQI_HAND_L ? sd->left_weapon.atkmods[t_size] : sd->right_weapon.atkmods[t_size]) / 100;
	} else {
		if(src->type == BL_ELEM) {
			struct status_change *e_sc = status_get_sc(src);
			int e_class = status_get_class(src);

			if(e_sc) {
				switch(e_class) {
					case ELEMENTALID_AGNI_S:
					case ELEMENTALID_AGNI_M:
					case ELEMENTALID_AGNI_L:
						if(e_sc->data[SC_FIRE_INSIGNIA] && e_sc->data[SC_FIRE_INSIGNIA]->val1 == 1)
							damage += damage * 20 / 100;
						break;
					case ELEMENTALID_AQUA_S:
					case ELEMENTALID_AQUA_M:
					case ELEMENTALID_AQUA_L:
						if(e_sc->data[SC_WATER_INSIGNIA] && e_sc->data[SC_WATER_INSIGNIA]->val1 == 1)
							damage += damage * 20 / 100;
						break;
					case ELEMENTALID_VENTUS_S:
					case ELEMENTALID_VENTUS_M:
					case ELEMENTALID_VENTUS_L:
						if(e_sc->data[SC_WIND_INSIGNIA] && e_sc->data[SC_WIND_INSIGNIA]->val1 == 1)
							damage += damage * 20 / 100;
						break;
					case ELEMENTALID_TERA_S:
					case ELEMENTALID_TERA_M:
					case ELEMENTALID_TERA_L:
						if(e_sc->data[SC_EARTH_INSIGNIA] && e_sc->data[SC_EARTH_INSIGNIA]->val1 == 1)
							damage += damage * 20 / 100;
						break;
				}
			}
		}
	}

	//Finally, add base attack
	if(flag&4)
		damage += status->matk_min;
	else
		damage += status->batk;

	//Rodatazone says that overrefine bonuses are part of base atk
	if(sd) {
		switch(type) {
			case EQI_HAND_L:
				if(sd->left_weapon.overrefine)
					damage += rnd()%sd->left_weapon.overrefine + 1;
				break;
			case EQI_HAND_R:
				if(sd->right_weapon.overrefine)
					damage += rnd()%sd->right_weapon.overrefine + 1;
				break;
		}
	}
	return damage;
}

/*==========================================
 * Consumes ammo for the given skill.
 *------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
void battle_consume_ammo(TBL_PC *sd, uint16 skill_id, uint16 skill_lv)
{
	int qty = 1;

	if(!battle_config.ammo_decrement)
		return;

	if(skill_id) {
		qty = skill_get_ammo_qty(skill_id,skill_lv);
		if(!qty)
			qty = 1;
	}

	if(sd->equip_index[EQI_AMMO] >= 0) //Qty check should have been done in skill_check_condition
		pc_delitem(sd,sd->equip_index[EQI_AMMO],qty,0,1,LOG_TYPE_CONSUME);

	sd->state.arrow_atk = 0;
}

static int battle_range_type(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	if(skill_get_inf2(skill_id)&INF2_TRAP || //Traps are always short range [Akinari],[Xynvaroth]
		skill_id == NJ_KIRIKAGE)
		return BF_SHORT;

	if(src->type == BL_MOB && skill_id == AC_SHOWER) //When monsters use Arrow Shower, it is always short range
		return BF_SHORT;

	//Skill range criteria
	if(battle_config.skillrange_by_distance && (src->type&battle_config.skillrange_by_distance)) {
		if(check_distance_bl(src, target, 3)) //Based on distance between src/target [Skotlex]
			return BF_SHORT;
		return BF_LONG;
	}

	if(skill_get_range2(src, skill_id, skill_lv, true) <= 3) //Based on used skill's range
		return BF_SHORT;
	return BF_LONG;
}

static inline int battle_adjust_skill_damage(int m, unsigned short skill_id)
{
	if(map[m].skill_count) {
		int i;

		ARR_FIND(0, map[m].skill_count, i, map[m].skills[i]->skill_id == skill_id);
		if(i < map[m].skill_count)
			return map[m].skills[i]->modifier;
	}
	return 0;
}

static int battle_blewcount_bonus(struct map_session_data *sd, uint16 skill_id)
{
	uint8 i;

	if(!sd->skillblown[0].id)
		return 0;

	//Apply the bonus blewcount [Skotlex]
	for(i = 0; i < ARRAYLENGTH(sd->skillblown) && sd->skillblown[i].id; i++)
		if(sd->skillblown[i].id == skill_id)
			return sd->skillblown[i].val;
	return 0;
}

#ifdef ADJUST_SKILL_DAMAGE
/** Damage calculation for adjusting skill damage
 * @param caster Applied caster type for damage skill
 * @param type BL_Type of attacker
 */
static bool battle_skill_damage_iscaster(uint8 caster, enum bl_type src_type)
{
	if(!caster)
		return false;

	switch(src_type) {
		case BL_PC: if(caster&SDC_PC) return true; break;
		case BL_MOB: if(caster&SDC_MOB) return true; break;
		case BL_PET: if(caster&SDC_PET) return true; break;
		case BL_HOM: if(caster&SDC_HOM) return true; break;
		case BL_MER: if(caster&SDC_MER) return true; break;
		case BL_ELEM: if(caster&SDC_ELEM) return true; break;
	}
	return false;
}

/** Gets skill damage rate from a skill (based on skill_damage_db.txt)
 * @param src
 * @param target
 * @param skill_id
 * @return Skill damage rate
 */
static int battle_skill_damage_skill(struct block_list *src, struct block_list *target, uint16 skill_id)
{
	uint16 idx = skill_get_index(skill_id), m = src->m;
	struct s_skill_damage *damage = NULL;
	struct map_data *mapd = &map[m];

	if(!idx || !skill_db[idx].damage.map)
		return 0;

	damage = &skill_db[idx].damage;

	//Check the adjustment works for specified type
	if(!battle_skill_damage_iscaster(damage->caster, src->type))
		return 0;

	if((damage->map&1 && (!mapd->flag.pvp && !map_flag_gvg2(m) && !mapd->flag.battleground && !mapd->flag.skill_damage && !mapd->flag.restricted)) ||
		(damage->map&2 && mapd->flag.pvp) ||
		(damage->map&4 && map_flag_gvg2(m)) ||
		(damage->map&8 && mapd->flag.battleground) ||
		(damage->map&16 && mapd->flag.skill_damage) ||
		(mapd->flag.restricted && damage->map&(8 * mapd->zone)))
	{
		switch(target->type) {
			case BL_PC:
				return damage->pc;
			case BL_MOB:
				if(is_boss(target))
					return damage->boss;
				else
					return damage->mob;
			default:
				return damage->other;
		}
	}
	return 0;
}

/** Gets skill damage rate from a skill (based on 'skill_damage' mapflag)
 * @param src
 * @param target
 * @param skill_id
 * @return Skill damage rate
 */
static int battle_skill_damage_map(struct block_list *src, struct block_list *target, uint16 skill_id)
{
	int rate = 0;
	uint8 i = 0;
	struct map_data *mapd = &map[src->m];

	if(!mapd || !mapd->flag.skill_damage)
		return 0;

	//Damage rate for all skills at this map
	if(battle_skill_damage_iscaster(mapd->adjust.damage.caster, src->type)) {
		switch(target->type) {
			case BL_PC:
				rate = mapd->adjust.damage.pc;
				break;
			case BL_MOB:
				if(is_boss(target))
					rate = mapd->adjust.damage.boss;
				else
					rate = mapd->adjust.damage.mob;
				break;
			default:
				rate = mapd->adjust.damage.other;
				break;
		}
	}

	if(!mapd->skill_damage.count)
		return rate;

	//Damage rate for specified skill at this map
	for(i = 0; i < mapd->skill_damage.count; i++) {
		if(mapd->skill_damage.entries[i]->skill_id == skill_id &&
			battle_skill_damage_iscaster(mapd->skill_damage.entries[i]->caster, src->type)) {
			switch(target->type) {
				case BL_PC:
					rate += mapd->skill_damage.entries[i]->pc;
					break;
				case BL_MOB:
					if(is_boss(target))
						rate += mapd->skill_damage.entries[i]->boss;
					else
						rate += mapd->skill_damage.entries[i]->mob;
					break;
				default:
					rate += mapd->skill_damage.entries[i]->other;
					break;
			}
		}
	}
	return rate;
}

/** Check skill damage adjustment based on mapflags and skill_damage_db.txt for specified skill
 * @param src
 * @param target
 * @param skill_id
 * @return Total damage rate
 */
static int battle_skill_damage(struct block_list *src, struct block_list *target, uint16 skill_id)
{
	nullpo_ret(src);

	if(!target)
		return 0;
	return battle_skill_damage_skill(src, target, skill_id) + battle_skill_damage_map(src, target, skill_id);
}
#endif

struct Damage battle_calc_magic_attack(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, int mflag);
struct Damage battle_calc_misc_attack(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, int mflag);

/*=======================================================
 * Should infinite defense be applied on target? (plant)
 *-------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
bool target_has_infinite_defense(struct block_list *target, uint16 skill_id, int flag)
{
	struct status_data *tstatus = status_get_status_data(target);

	if(target->type == BL_SKILL) {
		TBL_SKILL *su = ((TBL_SKILL *)target);

		if(su && su->group && su->group->skill_id == WM_REVERBERATION)
			return true;
	}
	if(tstatus->mode&MD_IGNOREMELEE && (flag&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON))
		return true;
	if(tstatus->mode&MD_IGNORERANGED && (flag&(BF_LONG|BF_MAGIC)) == BF_LONG)
		return true;
	return (tstatus->mode&MD_PLANT && skill_id != RA_CLUSTERBOMB
#ifdef RENEWAL
		&& skill_id != HT_BLASTMINE && skill_id != HT_CLAYMORETRAP
#endif
	);
}

/*========================
 * Is attack arrow based?
 *------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool is_skill_using_arrow(struct block_list *src, uint16 skill_id)
{
	if(src) {
		struct status_data *sstatus = status_get_status_data(src);
		struct map_session_data *sd = BL_CAST(BL_PC, src);

		return ((sd && sd->state.arrow_atk) ||
			(!sd && ((skill_id && skill_get_ammotype(skill_id)) || sstatus->rhw.range > 3)) ||
			skill_id == HT_PHANTASMIC || skill_id == GS_GROUNDDRIFT);
	} else
		return false;
}

/*=========================================
 * Is attack right handed? Default: Yes
 *-----------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool is_attack_right_handed(struct block_list *src, uint16 skill_id)
{
	if(src) {
		struct map_session_data *sd = BL_CAST(BL_PC, src);

		//Skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
		if(!skill_id && sd && !sd->weapontype1 && sd->weapontype2 > 0)
			return false;
	}
	return true;
}

/*=======================================
 * Is attack left handed? Default: No
 *---------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool is_attack_left_handed(struct block_list *src, uint16 skill_id)
{
	if(src) {
		struct status_data *sstatus = status_get_status_data(src);
		struct map_session_data *sd = BL_CAST(BL_PC, src);

		if(!skill_id) { //Skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
			if(sd) {
				if(!sd->weapontype1 && sd->weapontype2 > 0)
					return true;
				if(sd->status.weapon == W_KATAR)
					return true;
			}
			if(sstatus->lhw.atk)
				return true;
		}
	}
	return false;
}

/*=============================
 * Do we score a critical hit?
 *-----------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool is_attack_critical(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, bool first_call)
{
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct map_session_data *tsd = BL_CAST(BL_PC, target);

	if(!first_call)
		return (wd.type == DMG_CRITICAL);

	if(skill_id == NPC_CRITICALSLASH || skill_id == LG_PINPOINTATTACK)
		return true; //Always critical

	if(!(wd.type&DMG_MULTI_HIT) && sstatus->cri &&
		(!skill_id || skill_id == KN_AUTOCOUNTER ||
		skill_id == SN_SHARPSHOOTING || skill_id == MA_SHARPSHOOTING ||
		skill_id == NJ_KIRIKAGE || skill_id == GC_COUNTERSLASH))
	{
		short cri = sstatus->cri;

		if(sd) {
			if(!battle_config.show_status_katar_crit && sd->status.weapon == W_KATAR)
				cri <<= 1; //On official double critical bonus from katar won't showed in status display
			cri += sd->critaddrace[tstatus->race] + sd->critaddrace[RC_ALL];
			if(is_skill_using_arrow(src, skill_id))
				cri += sd->bonus.arrow_cri;
		}

		if(sc && sc->data[SC_CAMOUFLAGE])
			cri += 100 * min(10, sc->data[SC_CAMOUFLAGE]->val3); //Max 100% (1K)

		//The official equation is * 2, but that only applies when sd's do critical
		//Therefore, we use the old value 3 on cases when an sd gets attacked by a mob
		cri -= tstatus->luk * (!sd && tsd ? 3 : 2);

		if(tsc && tsc->data[SC_SLEEP])
			cri <<= 1;

		switch(skill_id) {
			case 0:
				if(!(sc && sc->data[SC_AUTOCOUNTER]))
					break;
				clif_specialeffect(src, 131, AREA);
				status_change_end(src, SC_AUTOCOUNTER, INVALID_TIMER);
			//Fall through
			case KN_AUTOCOUNTER:
				if(battle_config.auto_counter_type && (battle_config.auto_counter_type&src->type))
					return true;
				else
					cri <<= 1;
				break;
			case SN_SHARPSHOOTING:
			case MA_SHARPSHOOTING:
				cri += 200;
				break;
			case NJ_KIRIKAGE:
				cri += 250 + 50 * skill_lv;
				break;
		}

		if(tsd && tsd->bonus.critical_def)
			cri = cri * (100 - tsd->bonus.critical_def) / 100;
		return (rnd()%1000 < cri);
	}
	return 0;
}

/*==========================================================
 * Is the attack piercing? (Investigate/Ice Pick in pre-re)
 *----------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool is_attack_piercing(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, short weapon_position)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_data *tstatus = status_get_status_data(target);

	switch(skill_id) {
		case CR_GRANDCROSS:
		case NPC_GRANDDARKNESS:
		case CR_SHIELDBOOMERANG:
#ifdef RENEWAL //Renewal Soul Breaker no longer gains ice pick effect [helvetica]
		case ASC_BREAKER:
#endif
		case PA_SACRIFICE:
		case PA_SHIELDCHAIN:
		case RK_DRAGONBREATH:
		case RK_DRAGONBREATH_WATER:
		case NC_SELFDESTRUCTION:
		case KO_HAPPOKUNAI:
			return false;
		case MO_INVESTIGATE:
		case RL_MASS_SPIRAL:
			return true;
		default:
#ifndef RENEWAL //Renewal critical gains ice pick effect [helvetica]
			if(is_attack_critical(wd, src, target, skill_id, skill_lv, false))
				return false;
#endif
			break;
	}

	if(sd) { //Elemental/Racial adjustments
		if((sd->right_weapon.def_ratio_atk_ele&(1<<tstatus->def_ele)) || (sd->right_weapon.def_ratio_atk_ele&(1<<ELE_ALL)) ||
			(sd->right_weapon.def_ratio_atk_race&(1<<tstatus->race)) || (sd->right_weapon.def_ratio_atk_race&(1<<RC_ALL)) ||
			(sd->right_weapon.def_ratio_atk_class&(1<<tstatus->class_)) || (sd->right_weapon.def_ratio_atk_class&(1<<CLASS_ALL)))
			if(weapon_position == EQI_HAND_R)
				return true;
		if((sd->left_weapon.def_ratio_atk_ele&(1<<tstatus->def_ele)) || (sd->left_weapon.def_ratio_atk_ele&(1<<ELE_ALL)) ||
			(sd->left_weapon.def_ratio_atk_race&(1<<tstatus->race)) || (sd->left_weapon.def_ratio_atk_race&(1<<RC_ALL)) ||
			(sd->left_weapon.def_ratio_atk_class&(1<<tstatus->class_)) || (sd->left_weapon.def_ratio_atk_class&(1<<CLASS_ALL)))
		{ //Pass effect onto right hand if configured so [Skotlex]
			if(battle_config.left_cardfix_to_right && is_attack_right_handed(src, skill_id)) {
				if(weapon_position == EQI_HAND_R)
					return true;
			} else if(weapon_position == EQI_HAND_L)
				return true;
		}
	}
	return false;
}

static bool battle_skill_get_damage_properties(uint16 skill_id, int is_splash)
{
	int nk = skill_get_nk(skill_id);

	if(!skill_id && is_splash) //If flag, this is splash damage from Baphomet Card and it always hits
		nk |= NK_NO_CARDFIX_ATK|NK_IGNORE_FLEE;
	return nk;
}

/*=============================
 * Checks if attack is hitting
 *-----------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool is_attack_hitting(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, bool first_call)
{
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct map_session_data *sd = BL_CAST(BL_PC,src);
	int nk = battle_skill_get_damage_properties(skill_id,wd.miscflag);
	short flee, hitrate;

	if(!first_call)
		return (wd.dmg_lv != ATK_FLEE);

	if(is_attack_critical(wd,src,target,skill_id,skill_lv,false))
		return true;
	else if(sd && sd->bonus.perfect_hit > 0 && rnd()%100 < sd->bonus.perfect_hit)
		return true;
	else if(sc && sc->data[SC_FUSION])
		return true;
	else if(skill_id == AS_SPLASHER && !wd.miscflag)
		return true;
	else if(skill_id == CR_SHIELDBOOMERANG && sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_CRUSADER)
		return true;
	else if(tsc && tsc->opt1 && tsc->opt1 != OPT1_STONEWAIT && tsc->opt1 != OPT1_BURNING)
		return true;
	else if(nk&NK_IGNORE_FLEE)
		return true;

	if(sc && sc->data[SC_NEUTRALBARRIER] && (wd.flag&(BF_LONG|BF_MAGIC)) == BF_LONG)
		return false;

	flee = tstatus->flee;
#ifdef RENEWAL
	hitrate = 0; //Default hitrate
#else
	hitrate = 80; //Default hitrate
#endif

	if(battle_config.agi_penalty_type && battle_config.agi_penalty_target&target->type) {
		unsigned char attacker_count = unit_counttargeted(target); //256 max targets should be a sane max

		if(attacker_count >= battle_config.agi_penalty_count) {
			if(battle_config.agi_penalty_type == 1)
				flee = (flee * (100 - (attacker_count - (battle_config.agi_penalty_count - 1)) * battle_config.agi_penalty_num)) / 100;
			else //Assume type 2 : absolute reduction
				flee -= (attacker_count - (battle_config.agi_penalty_count - 1)) * battle_config.agi_penalty_num;
			flee = max(flee,1);
		}
	}

	hitrate += sstatus->hit - flee;

	//Fogwall's hit penalty is only for normal ranged attacks
	if(tsc && tsc->data[SC_FOGWALL] && !skill_id && (wd.flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON))
		hitrate -= 50;

	if(sd && is_skill_using_arrow(src,skill_id))
		hitrate += sd->bonus.arrow_hit;

#ifdef RENEWAL
	if(sd) //In renewal, hit bonus from Vultures Eye is not anymore shown in status window
		hitrate += pc_checkskill(sd,AC_VULTURE);
#endif

	if(skill_id) {
		switch(skill_id) { //Hit skill modifiers
			//It is proven that bonus is applied on final hitrate, not hit
			case SM_BASH:
			case MS_BASH:
				hitrate += hitrate * 5 * skill_lv / 100;
				break;
			case MS_MAGNUM:
			case SM_MAGNUM:
				hitrate += hitrate * 10 * skill_lv / 100;
				break;
			case KN_AUTOCOUNTER:
			case PA_SHIELDCHAIN:
			case NPC_WATERATTACK:
			case NPC_GROUNDATTACK:
			case NPC_FIREATTACK:
			case NPC_WINDATTACK:
			case NPC_POISONATTACK:
			case NPC_HOLYATTACK:
			case NPC_DARKNESSATTACK:
			case NPC_UNDEADATTACK:
			case NPC_TELEKINESISATTACK:
			case NPC_BLEEDING:
			case NPC_EARTHQUAKE:
			case NPC_FIREBREATH:
			case NPC_ICEBREATH:
			case NPC_THUNDERBREATH:
			case NPC_ACIDBREATH:
			case NPC_DARKNESSBREATH:
				hitrate += hitrate * 20 / 100;
				break;
			case KN_PIERCE:
			case ML_PIERCE:
				hitrate += hitrate * 5 * skill_lv / 100;
				break;
			case AS_SONICBLOW:
				if(sd && pc_checkskill(sd,AS_SONICACCEL) > 0)
					hitrate += hitrate * 50 / 100;
				break;
			case MC_CARTREVOLUTION:
			case GN_CART_TORNADO:
			case GN_CARTCANNON:
				if(sd && pc_checkskill(sd,GN_REMODELING_CART) > 0)
					hitrate += pc_checkskill(sd,GN_REMODELING_CART) * 4;
				break;
			case GC_VENOMPRESSURE:
				hitrate += 10 + 4 * skill_lv;
				break;
			case SC_FATALMENACE:
				hitrate -= 35 - 5 * skill_lv;
				break;
			case LG_BANISHINGPOINT:
				hitrate += 3 * skill_lv;
				break;
			case RL_SLUGSHOT: {
					int8 dist = distance_bl(src,target);

					if(dist > 3) { //Reduce n hitrate for each cell after initial 3 cells, different each level
						dist -= 3;
						hitrate -= (11 - skill_lv) * dist; //-10:-9:-8:-7:-6
					}
				}
				break;
		} //+1 hit per level of Double Attack on a successful double attack (making sure other multi attack skills do not trigger this) [helvetica]
	} else if(sd && (wd.type&DMG_MULTI_HIT) && wd.div_ == 2)
		hitrate += pc_checkskill(sd,TF_DOUBLE);

	if(sd) {
		uint16 lv;

		if((lv = pc_checkskill(sd,BS_WEAPONRESEARCH)) > 0) //Weaponry Research hidden bonus
			hitrate += hitrate * 2 * lv / 100;
		if((sd->status.weapon == W_1HSWORD || sd->status.weapon == W_DAGGER) && (lv = pc_checkskill(sd,GN_TRAINING_SWORD)) > 0)
			hitrate += 3 * lv;
	}

	if(sc) {
		if(sc->data[SC_INCHITRATE])
			hitrate += hitrate * sc->data[SC_INCHITRATE]->val1 / 100;
		if(sc->data[SC_MTF_ASPD])
			hitrate += sc->data[SC_MTF_ASPD]->val2;
		if(sc->data[SC_MTF_ASPD2])
			hitrate += sc->data[SC_MTF_ASPD2]->val2;
	}

	hitrate = cap_value(hitrate,battle_config.min_hitrate,battle_config.max_hitrate);
	return (rnd()%100 < hitrate);
}

/*==========================================
 * If attack ignores def.
 *------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool attack_ignores_def(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, short weapon_position)
{
	struct status_data *tstatus = status_get_status_data(target);
	struct status_change *sc = status_get_sc(src);
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	int nk = battle_skill_get_damage_properties(skill_id, wd.miscflag);

	switch(skill_id) {
		case CR_GRANDCROSS:
		case NPC_GRANDDARKNESS:
			return (nk&NK_IGNORE_DEF);
	}

#ifndef RENEWAL //Renewal critical doesn't ignore defense reduction
	if(is_attack_critical(wd, src, target, skill_id, skill_lv, false))
		return true;
	else
#endif
	if(sc && sc->data[SC_FUSION])
		return true;
	if(sd) { //Ignore Defense
		if((sd->right_weapon.ignore_def_ele&(1<<tstatus->def_ele)) || (sd->right_weapon.ignore_def_ele&(1<<ELE_ALL)) ||
			(sd->right_weapon.ignore_def_race&(1<<tstatus->race)) || (sd->right_weapon.ignore_def_race&(1<<RC_ALL)) ||
			(sd->right_weapon.ignore_def_class&(1<<tstatus->class_)) || (sd->right_weapon.ignore_def_class&(1<<CLASS_ALL)))
			if(weapon_position == EQI_HAND_R)
				return true;
		if((sd->left_weapon.ignore_def_ele&(1<<tstatus->def_ele)) || (sd->left_weapon.ignore_def_ele&(1<<ELE_ALL)) ||
			(sd->left_weapon.ignore_def_race&(1<<tstatus->race)) || (sd->left_weapon.ignore_def_race&(1<<RC_ALL)) ||
			(sd->left_weapon.ignore_def_class&(1<<tstatus->class_)) || (sd->left_weapon.ignore_def_class&(1<<CLASS_ALL)))
		{
			if(battle_config.left_cardfix_to_right && is_attack_right_handed(src, skill_id)) {
				if(weapon_position == EQI_HAND_R)
					return true; //Move effect to right hand [Skotlex]
			} else if(weapon_position == EQI_HAND_L)
				return true;
		}
	}
	return (nk&NK_IGNORE_DEF);
}

/*================================================
 * Should skill attack consider VVS and masteries?
 *------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static bool battle_skill_stacks_masteries_vvs(uint16 skill_id)
{
	switch(skill_id) {
		case MO_INVESTIGATE:
		case MO_EXTREMITYFIST:
		case CR_GRANDCROSS:
		case PA_SACRIFICE:
#ifndef RENEWAL
		case NJ_ISSEN:
		case LK_SPIRALPIERCE:
		case ML_SPIRALPIERCE:
		case CR_SHIELDBOOMERANG:
		case PA_SHIELDCHAIN:
#else
		case AM_ACIDTERROR:
		case CR_ACIDDEMONSTRATION:
		case GN_FIRE_EXPANSION_ACID:
#endif
		case RK_DRAGONBREATH:
		case RK_DRAGONBREATH_WATER:
		case NC_SELFDESTRUCTION:
		case KO_HAPPOKUNAI:
		case RL_MASS_SPIRAL:
			return false;
	}
	return true;
}

#ifdef RENEWAL
/*========================================
 * Calculate equipment ATK for renewal ATK
 *----------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static int battle_calc_equip_attack(struct block_list *src, uint16 skill_id)
{
	int eatk = 0;
	struct status_data *status = status_get_status_data(src);
	struct map_session_data *sd = BL_CAST(BL_PC, src);

	if(sd) //Add arrow ATK if using an applicable skill
		eatk += (is_skill_using_arrow(src, skill_id) ? sd->bonus.arrow_atk : 0);
	return eatk + status->eatk;
}
#endif

/*========================================
 * Returns the element type of attack
 *----------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static int battle_get_weapon_element(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, short weapon_position)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_change *sc = status_get_sc(src);
	struct status_data *sstatus = status_get_status_data(src);
	int element = skill_get_ele(skill_id, skill_lv);

	if(!skill_id || element == -1) { //Take weapon's element
		if(weapon_position == EQI_HAND_R)
			element = sstatus->rhw.ele;
		else
			element = sstatus->lhw.ele;
		if(is_skill_using_arrow(src, skill_id) && sd && sd->bonus.arrow_ele && weapon_position == EQI_HAND_R)
			element = sd->bonus.arrow_ele;
		if(sd && sd->spiritcharm_type != CHARM_TYPE_NONE && sd->spiritcharm >= MAX_SPIRITCHARM)
			element = sd->spiritcharm_type; //Summoning 10 spiritcharm will endow your weapon
		//On official endows override all other elements [helvetica]
		if(sc && sc->data[SC_ENCHANTARMS]) //Check for endows
			element = sc->data[SC_ENCHANTARMS]->val2;
	} else if(element == -2) //Use enchantment's element
		element = status_get_attack_sc_element(src,sc);
	else if(element == -3) //Use random element
		element = rnd()%ELE_ALL;

	switch(skill_id) {
		case LK_SPIRALPIERCE: //Forced neutral for monsters
			if(!sd)
				element = ELE_NEUTRAL;
			break;
		case LG_HESPERUSLIT:
			if(sc && sc->data[SC_BANDING] && sc->data[SC_BANDING]->val2 > 4)
				element = ELE_HOLY;
			break;
		case RL_H_MINE: //Force RL_H_MINE deals fire damage if activated by RL_FLICKER
			if(sd && sd->flicker)
				element = ELE_FIRE;
			break;
	}

	if(sc && sc->data[SC_GOLDENE_FERSE] && ((!skill_id && (rnd()%100 < sc->data[SC_GOLDENE_FERSE]->val4)) || skill_id == MH_STAHL_HORN) )
		element = ELE_HOLY;
	return element;
}

#define ATK_RATE(damage, damage2, a) { damage = damage * (a) / 100; if(is_attack_left_handed(src, skill_id)) damage2 = damage2 * (a) / 100; }
#define ATK_RATE2(damage, damage2, a , b) { damage = damage *(a) / 100; if(is_attack_left_handed(src, skill_id)) damage2 = damage2 * (b) / 100; }
#define ATK_RATER(damage, a) { damage = damage * (a) / 100; }
#define ATK_RATEL(damage2, a) { damage2 = damage2 * (a) / 100; }
//Adds dmg%. 100 = +100% (double) damage. 10 = +10% damage
#define ATK_ADDRATE(damage, damage2, a) { damage += damage * (a) / 100; if(is_attack_left_handed(src, skill_id)) damage2 += damage2 *(a) / 100; }
#define ATK_ADDRATE2(damage, damage2, a , b) { damage += damage * (a) / 100; if(is_attack_left_handed(src, skill_id)) damage2 += damage2 * (b) / 100; }
//Adds an absolute value to damage. 100 = +100 damage
#define ATK_ADD(damage, damage2, a) { damage += a; if(is_attack_left_handed(src, skill_id)) damage2 += a; }
#define ATK_ADD2(damage, damage2, a , b) { damage += a; if(is_attack_left_handed(src, skill_id)) damage2 += b; }

#ifdef RENEWAL
	#define RE_ALLATK_ADD(wd, a) { ATK_ADD(wd.statusAtk, wd.statusAtk2, a); ATK_ADD(wd.weaponAtk, wd.weaponAtk2, a); ATK_ADD(wd.equipAtk, wd.equipAtk2, a); ATK_ADD(wd.masteryAtk, wd.masteryAtk2, a); }
	#define RE_ALLATK_RATE(wd, a) { ATK_RATE(wd.statusAtk, wd.statusAtk2, a); ATK_RATE(wd.weaponAtk, wd.weaponAtk2, a); ATK_RATE(wd.equipAtk, wd.equipAtk2, a); ATK_RATE(wd.masteryAtk, wd.masteryAtk2, a); }
	#define RE_ALLATK_ADDRATE(wd, a) { ATK_ADDRATE(wd.statusAtk, wd.statusAtk2, a); ATK_ADDRATE(wd.weaponAtk, wd.weaponAtk2, a); ATK_ADDRATE(wd.equipAtk, wd.equipAtk2, a); ATK_ADDRATE(wd.masteryAtk, wd.masteryAtk2, a); }
#else
	#define RE_ALLATK_ADD(wd, a) {;}
	#define RE_ALLATK_RATE(wd, a) {;}
	#define RE_ALLATK_ADDRATE(wd, a) {;}
#endif

/*========================================
 * Do element damage modifier calculation
 *----------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static struct Damage battle_calc_element_damage(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_change *sc = status_get_sc(src);
	struct status_data *tstatus = status_get_status_data(target);
	int element = skill_get_ele(skill_id, skill_lv);
	int left_element = battle_get_weapon_element(wd, src, target, skill_id, skill_lv, EQI_HAND_L);
	int right_element = battle_get_weapon_element(wd, src, target, skill_id, skill_lv, EQI_HAND_R);
	int nk = battle_skill_get_damage_properties(skill_id, wd.miscflag);

	if(!(nk&NK_NO_ELEFIX) && (wd.damage > 0 || wd.damage2 > 0)) { //Elemental attribute fix
		//Non-pc physical melee attacks (mob, pet, homun) are "no elemental", they deal 100% to all target elements
		//However the "no elemental" attacks still get reduced by "Neutral resistance"
		//Also non-pc units have only a defending element, but can inflict elemental attacks using skills [exneval]
		if((battle_config.attack_attr_none&src->type) && ((!skill_id && !right_element) ||
			(skill_id && (element == -1 || !right_element))) && (wd.flag&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON))
			return wd;
		switch(skill_id) {
			case SR_TIGERCANNON:
				if(wd.miscflag&16)
					wd.damage = battle_damage_temp[0];
			//Fall through
			case MC_CARTREVOLUTION:
			case RA_CLUSTERBOMB:
			case NC_ARMSCANNON:
			case SR_CRESCENTELBOW_AUTOSPELL:
			case SR_GATEOFHELL:
			case KO_BAKURETSU:
				wd.damage = battle_attr_fix(src, target, wd.damage, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
				break;
			case RA_FIRINGTRAP:
				wd.damage = battle_attr_fix(src, target, wd.damage, ELE_FIRE, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, ELE_FIRE, tstatus->def_ele, tstatus->ele_lv);
				break;
			case RA_ICEBOUNDTRAP:
				wd.damage = battle_attr_fix(src, target, wd.damage, ELE_WATER, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, ELE_WATER, tstatus->def_ele, tstatus->ele_lv);
				break;
			case GN_CARTCANNON:
			case KO_HAPPOKUNAI:
				wd.damage = battle_attr_fix(src, target, wd.damage, (sd && sd->bonus.arrow_ele ? sd->bonus.arrow_ele : ELE_NEUTRAL), tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, (sd && sd->bonus.arrow_ele ? sd->bonus.arrow_ele : ELE_NEUTRAL), tstatus->def_ele, tstatus->ele_lv);
				break;
			default:
#ifndef RENEWAL
				if(skill_id == NPC_EARTHQUAKE)
					break; //Do attribute fix later
#endif
				wd.damage = battle_attr_fix(src, target, wd.damage, right_element, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, left_element, tstatus->def_ele, tstatus->ele_lv);
				break;
		}
		if(sc && sc->data[SC_WATK_ELEMENT] && battle_skill_stacks_masteries_vvs(skill_id)) {
			int64 dmg, dmg2;

			dmg = wd.damage * sc->data[SC_WATK_ELEMENT]->val2 / 100;
			wd.damage += battle_attr_fix(src, target, dmg, sc->data[SC_WATK_ELEMENT]->val1, tstatus->def_ele, tstatus->ele_lv);
			if(is_attack_left_handed(src, skill_id)) {
				dmg2 = wd.damage2 * sc->data[SC_WATK_ELEMENT]->val2 / 100;
				wd.damage2 += battle_attr_fix(src, target, dmg2, sc->data[SC_WATK_ELEMENT]->val1, tstatus->def_ele, tstatus->ele_lv);
			}
		}
	}
	return wd;
}

/*==================================
 * Calculate weapon mastery damages
 *----------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static struct Damage battle_calc_attack_masteries(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_change *sc = status_get_sc(src);
	struct status_data *sstatus = status_get_status_data(src);
	int t_class = status_get_class(target);
	short div_ = max(wd.div_, 1);
	uint16 lv;

	//Add mastery damage
	if(sd && battle_skill_stacks_masteries_vvs(skill_id)) {
		wd.damage = battle_addmastery(sd, target, wd.damage, 0);
#ifdef RENEWAL
		wd.masteryAtk = battle_addmastery(sd, target, wd.weaponAtk, 0);
#endif
		if(is_attack_left_handed(src, skill_id)) {
			wd.damage2 = battle_addmastery(sd, target, wd.damage2, 1);
#ifdef RENEWAL
			wd.masteryAtk2 = battle_addmastery(sd, target, wd.weaponAtk2, 1);
#endif
		}
		//General skill masteries
#ifdef RENEWAL
		if(skill_id != MC_CARTREVOLUTION && pc_checkskill(sd, BS_HILTBINDING) > 0)
			ATK_ADD(wd.masteryAtk, wd.masteryAtk2, 4);
		if(skill_id != CR_SHIELDBOOMERANG)
			ATK_ADD2(wd.masteryAtk, wd.masteryAtk2, div_ * sd->right_weapon.star, div_ * sd->left_weapon.star);
		if(skill_id == MO_FINGEROFFENSIVE) {
			ATK_ADD(wd.masteryAtk, wd.masteryAtk2, div_ * sd->spiritball_old * 3);
		} else
			ATK_ADD(wd.masteryAtk, wd.masteryAtk2, div_ * sd->spiritball * 3);
		if(!skill_id && sd->status.party_id && (lv = pc_checkskill(sd, TK_POWER)) > 0) { //Doesn't increase skill damage in renewal [exneval]
			int members = party_foreachsamemap(party_sub_count, sd, 0);

			if(members > 1)
				ATK_ADDRATE(wd.masteryAtk, wd.masteryAtk2, 2 * lv * (members - 1));
		}
#endif
		switch(skill_id) {
			case TF_POISON:
				ATK_ADD(wd.damage, wd.damage2, 15 * skill_lv);
#ifdef RENEWAL //Additional ATK from Envenom is treated as mastery type damage [helvetica]
				ATK_ADD(wd.masteryAtk, wd.masteryAtk2, 15 * skill_lv);
#endif
				break;
			case GS_GROUNDDRIFT:
				ATK_ADD(wd.damage, wd.damage2, 50 * skill_lv);
				break;
			case NJ_SYURIKEN:
				if((lv = pc_checkskill(sd, NJ_TOBIDOUGU)) > 0) {
					ATK_ADD(wd.damage, wd.damage2, 3 * lv);
#ifdef RENEWAL
					ATK_ADD(wd.masteryAtk, wd.masteryAtk2, 3 * lv);
#endif
				}
				break;
			case RA_WUGDASH:
			case RA_WUGSTRIKE:
			case RA_WUGBITE:
				if((lv = pc_checkskill(sd, RA_TOOTHOFWUG)) > 0) {
					ATK_ADD(wd.damage, wd.damage2, 30 * lv);
#ifdef RENEWAL
					ATK_ADD(wd.masteryAtk, wd.masteryAtk2, 30 * lv);
#endif
				}
				break;
		}
		if(sc) { //Status change considered as masteries
			uint8 i = 0;

#ifdef RENEWAL //The level 4 weapon limitation has been removed
			if(sc->data[SC_NIBELUNGEN])
				ATK_ADD(wd.masteryAtk, wd.masteryAtk2, sc->data[SC_NIBELUNGEN]->val2);
#endif
			if(sc->data[SC_AURABLADE]) {
				ATK_ADD(wd.damage, wd.damage2, 20 * sc->data[SC_AURABLADE]->val1);
#ifdef RENEWAL
				ATK_ADD(wd.masteryAtk, wd.masteryAtk2, 20 * sc->data[SC_AURABLADE]->val1);
#endif
			}
			if(sc->data[SC_CAMOUFLAGE]) {
				ATK_ADD(wd.damage, wd.damage2, 30 * min(10, sc->data[SC_CAMOUFLAGE]->val3));
#ifdef RENEWAL
				ATK_ADD(wd.masteryAtk, wd.masteryAtk2, 30 * min(10, sc->data[SC_CAMOUFLAGE]->val3));
#endif
			}
			if(sc->data[SC_GN_CARTBOOST]) {
				ATK_ADD(wd.damage, wd.damage2, 10 * sc->data[SC_GN_CARTBOOST]->val1);
#ifdef RENEWAL
				ATK_ADD(wd.masteryAtk, wd.masteryAtk2, 10 * sc->data[SC_GN_CARTBOOST]->val1);
#endif
			}
			if(sc->data[SC_RUSHWINDMILL]) {
				ATK_ADD(wd.damage, wd.damage2, sc->data[SC_RUSHWINDMILL]->val4);
#ifdef RENEWAL
				ATK_ADD(wd.masteryAtk, wd.masteryAtk2, sc->data[SC_RUSHWINDMILL]->val4);
#endif
			}
#ifdef RENEWAL
			if(sc->data[SC_PROVOKE])
				ATK_ADDRATE(wd.masteryAtk, wd.masteryAtk2, sc->data[SC_PROVOKE]->val3);
#endif
			if(sc->data[SC_MIRACLE])
				i = 2; //Star anger
			else
				ARR_FIND(0, MAX_PC_FEELHATE, i, t_class == sd->hate_mob[i]);
			if(i < MAX_PC_FEELHATE && (lv = pc_checkskill(sd,sg_info[i].anger_id)) > 0) {
				int skillratio = sd->status.base_level + sstatus->dex + sstatus->luk;

				if(i == 2)
					skillratio += sstatus->str; //Star Anger
				if(lv < 4)
					skillratio /= 12 - 3 * lv;
				ATK_ADDRATE(wd.damage, wd.damage2, skillratio);
#ifdef RENEWAL
				ATK_ADDRATE(wd.masteryAtk, wd.masteryAtk2, skillratio);
#endif
			}
		}
	}

	return wd;
}

#ifdef RENEWAL
/*=========================================
 * Calculate the various Renewal ATK parts
 *-----------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_damage_parts(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct map_session_data *sd = BL_CAST(BL_PC, src);

	int right_element = battle_get_weapon_element(wd, src, target, skill_id, skill_lv, EQI_HAND_R);
	int left_element = battle_get_weapon_element(wd, src, target, skill_id, skill_lv, EQI_HAND_L);

	wd.statusAtk += battle_calc_status_attack(sstatus, EQI_HAND_R);
	wd.statusAtk2 += battle_calc_status_attack(sstatus, EQI_HAND_L);

	if(skill_id || sd->sc.data[SC_SEVENWIND]) { //Mild Wind applies element to status ATK as well as weapon ATK [helvetica]
		wd.statusAtk = battle_attr_fix(src, target, wd.statusAtk, right_element, tstatus->def_ele, tstatus->ele_lv);
		wd.statusAtk2 = battle_attr_fix(src, target, wd.statusAtk, left_element, tstatus->def_ele, tstatus->ele_lv);
	} else { //Status ATK is considered neutral on normal attacks [helvetica]
		wd.statusAtk = battle_attr_fix(src, target, wd.statusAtk, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
		wd.statusAtk2 = battle_attr_fix(src, target, wd.statusAtk, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
	}

	wd.weaponAtk += battle_calc_base_weapon_attack(src, tstatus, &sstatus->rhw, sd, skill_id);
	wd.weaponAtk = battle_attr_fix(src, target, wd.weaponAtk, right_element, tstatus->def_ele, tstatus->ele_lv);

	wd.weaponAtk2 += battle_calc_base_weapon_attack(src, tstatus, &sstatus->lhw, sd, skill_id);
	wd.weaponAtk2 = battle_attr_fix(src, target, wd.weaponAtk2, left_element, tstatus->def_ele, tstatus->ele_lv);

	wd.equipAtk += battle_calc_equip_attack(src, skill_id);
	if(is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_R))
		wd.equipAtk += battle_get_defense(src, target, skill_id, 0) / 2;
	wd.equipAtk = battle_attr_fix(src, target, wd.equipAtk, right_element, tstatus->def_ele, tstatus->ele_lv);

	wd.equipAtk2 += battle_calc_equip_attack(src, skill_id);
	if(is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_L))
		wd.equipAtk2 += battle_get_defense(src, target, skill_id, 0) / 2;
	wd.equipAtk2 = battle_attr_fix(src, target, wd.equipAtk2, left_element, tstatus->def_ele, tstatus->ele_lv);

	//Mastery ATK is a special kind of ATK that has no elemental properties
	//Because masteries are not elemental, they are unaffected by Ghost armors or Raydric Card
	wd = battle_calc_attack_masteries(wd, src, target, skill_id, skill_lv);

	wd.damage = 0;
	wd.damage2 = 0;

	return wd;
}
#endif

/*==========================================================
 * Calculate basic ATK that goes into the skill ATK formula
 *----------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_skill_base_damage(struct Damage wd, struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv)
{
	struct status_change *sc = status_get_sc(src);
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	uint16 i;
	int nk = battle_skill_get_damage_properties(skill_id, wd.miscflag);

	switch(skill_id) { //Calc base damage according to skill
		case PA_SACRIFICE:
			wd.damage = sstatus->max_hp * 9 / 100;
			wd.damage2 = 0;
#ifdef RENEWAL
			wd.weaponAtk = wd.damage;
			wd.weaponAtk2 = wd.damage2;
#endif
			break;
#ifdef RENEWAL
		case LK_SPIRALPIERCE:
		case ML_SPIRALPIERCE:
			if(sd) {
				short index = sd->equip_index[EQI_HAND_R];

				wd = battle_calc_damage_parts(wd, src, target, skill_id, skill_lv);
				//Weight from spear is treated as equipment ATK on official [helvetica]
				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON)
					ATK_ADD(wd.equipAtk, wd.equipAtk2, sd->inventory_data[index]->weight * 5 / 100); //50% of weight
			} else //Monsters have no weight and use ATK instead
				wd.damage = battle_calc_base_damage(src, sstatus, &sstatus->rhw, sc, tstatus->size, sd, 0);
			switch(tstatus->size) { //Size-fix
				case SZ_SMALL: //125%
					RE_ALLATK_RATE(wd, 125);
					break;
				case SZ_BIG: //75%
					RE_ALLATK_RATE(wd, 75);
					break;
			}
			break;
		case CR_SHIELDBOOMERANG:
		case PA_SHIELDCHAIN:
			if(sd) {
				short index = sd->equip_index[EQI_HAND_L];

				wd = battle_calc_damage_parts(wd, src, target, skill_id, skill_lv);
				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
					ATK_ADD(wd.equipAtk, wd.equipAtk2, sd->inventory_data[index]->weight / 10);
			} else
				wd.damage = battle_calc_base_damage(src, sstatus, &sstatus->rhw, sc, tstatus->size, sd, 0);
#else
		case NJ_ISSEN:
			wd.damage = 40 * sstatus->str + sstatus->hp * 8 * skill_lv / 100;
			wd.damage2 = 0;
			break;
		case LK_SPIRALPIERCE:
		case ML_SPIRALPIERCE:
			if(sd) {
				short index = sd->equip_index[EQI_HAND_R];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON)
					wd.damage = sd->inventory_data[index]->weight * 8 / 100; //80% of weight
				ATK_ADDRATE(wd.damage, wd.damage2, 50 * skill_lv); //Skill modifier applies to weight only
			} else
				wd.damage = battle_calc_base_damage(src, sstatus, &sstatus->rhw, sc, tstatus->size, sd, 0);
			i = sstatus->str / 10;
			i *= i;
			ATK_ADD(wd.damage, wd.damage2, i); //Add STR bonus
			switch(tstatus->size) {
				case SZ_SMALL:
					ATK_RATE(wd.damage, wd.damage2, 125);
					break;
				case SZ_BIG:
					ATK_RATE(wd.damage, wd.damage2, 75);
					break;
			}
			break;
		case CR_SHIELDBOOMERANG:
		case PA_SHIELDCHAIN:
			wd.damage = sstatus->batk;
			wd.damage2 = 0;
			if(sd) {
				short index = sd->equip_index[EQI_HAND_L];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
					ATK_ADD(wd.damage, wd.damage2, sd->inventory_data[index]->weight / 10);
			} else
				wd.damage = battle_calc_base_damage(src, sstatus, &sstatus->rhw, sc, tstatus->size, sd, 0);
#endif
			break;
		case RK_DRAGONBREATH:
		case RK_DRAGONBREATH_WATER:
			{
				int damagevalue = (sstatus->hp / 50 + status_get_max_sp(src) / 4) * skill_lv;

				if(status_get_lv(src) > 100)
					damagevalue = damagevalue * status_get_lv(src) / 150;
				if(sd)
					damagevalue = damagevalue * (100 + 5 * (pc_checkskill(sd,RK_DRAGONTRAINING) - 1)) / 100;
				ATK_ADD(wd.damage, wd.damage2, damagevalue);
#ifdef RENEWAL
				ATK_ADD(wd.weaponAtk, wd.weaponAtk2, damagevalue);
#endif
			}
			break;
		case NC_SELFDESTRUCTION: {
				int damagevalue = (skill_lv + 1) * ((sd ? pc_checkskill(sd,NC_MAINFRAME) : 4) + 8) * (status_get_sp(src) + sstatus->vit);

				if(status_get_lv(src) > 100)
					damagevalue = damagevalue * status_get_lv(src) / 100;
				damagevalue = damagevalue + sstatus->hp;
				ATK_ADD(wd.damage, wd.damage2, damagevalue);
#ifdef RENEWAL
				ATK_ADD(wd.weaponAtk, wd.weaponAtk2, damagevalue);
#endif
			}
			break;
		case KO_HAPPOKUNAI:
			if(sd) {
				short index = sd->equip_index[EQI_AMMO];
				int damagevalue = 3 * (
#ifdef RENEWAL
					2 *
#endif
					sstatus->batk + sstatus->rhw.atk + (index >= 0 && sd->inventory_data[index] ?
					sd->inventory_data[index]->atk : 0)) * (skill_lv + 5) / 5;

				ATK_ADD(wd.damage, wd.damage2, damagevalue);
#ifdef RENEWAL
				ATK_ADD(wd.weaponAtk, wd.weaponAtk2, damagevalue);
#endif
			} else
				ATK_ADD(wd.damage, wd.damage2, 5000);
			break;
		case HFLI_SBR44: //[orn]
			if(src->type == BL_HOM)
				wd.damage = ((TBL_HOM *)src)->homunculus.intimacy;
			break;
		default:
#ifdef RENEWAL
			if(sd)
				wd = battle_calc_damage_parts(wd, src, target, skill_id, skill_lv);
			else {
				i = (!skill_id && sc && sc->data[SC_CHANGE] ? 4 : 0);
				wd.damage = battle_calc_base_damage(src, sstatus, &sstatus->rhw, sc, tstatus->size, sd, i);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_calc_base_damage(src, sstatus, &sstatus->lhw, sc, tstatus->size, sd, i);
			}
#else
			i = (is_attack_critical(wd, src, target, skill_id, skill_lv, false) ? 1 : 0)|
				(is_skill_using_arrow(src, skill_id) ? 2 : 0)|
				(skill_id == HW_MAGICCRASHER ? 4 : 0)|
				(!skill_id && sc && sc->data[SC_CHANGE] ? 4 : 0)|
				(skill_id == MO_EXTREMITYFIST ? 8 : 0)|
				(sc && sc->data[SC_WEAPONPERFECTION] ? 8 : 0);
			if(is_skill_using_arrow(src, skill_id) && sd) {
				switch(sd->status.weapon) {
					case W_BOW:	case W_REVOLVER:
					case W_GATLING:	case W_SHOTGUN:
					case W_GRENADE:
						break;
					default:
						i |= 16; //For ex. shuriken must not be influenced by DEX
						break;
				}
			}
			wd.damage = battle_calc_base_damage(src, sstatus, &sstatus->rhw, sc, tstatus->size, sd, i);
			if(is_attack_left_handed(src, skill_id))
				wd.damage2 = battle_calc_base_damage(src, sstatus, &sstatus->lhw, sc, tstatus->size, sd, i);
#endif
			if(nk&NK_SPLASHSPLIT) { //Divide ATK among targets
				if(wd.miscflag > 0) {
					wd.damage /= wd.miscflag;
#ifdef RENEWAL
					wd.statusAtk /= wd.miscflag;
					wd.weaponAtk /= wd.miscflag;
					wd.equipAtk /= wd.miscflag;
					wd.masteryAtk /= wd.miscflag;
#endif
				} else
					ShowError("0 enemies targeted by %d:%s, divide per 0 avoided!\n", skill_id, skill_get_name(skill_id));
			}
			if(sd) { //Add any bonuses that modify the base damage
				if(sd->bonus.atk_rate) {
					ATK_ADDRATE(wd.damage, wd.damage2, sd->bonus.atk_rate);
#ifdef RENEWAL //Renewal: Attack bonus only modify weapon and equip ATK [exneval]
					ATK_ADDRATE(wd.weaponAtk, wd.weaponAtk2, sd->bonus.atk_rate);
					ATK_ADDRATE(wd.equipAtk, wd.equipAtk2, sd->bonus.atk_rate);
#endif
				}
#ifndef RENEWAL
				{
					uint16 lv = 0;

					//Add +crit damage bonuses here in pre-renewal mode [helvetica]
					if(sd->bonus.crit_atk_rate && is_attack_critical(wd, src, target, skill_id, skill_lv, false))
						ATK_ADDRATE(wd.damage, wd.damage2, sd->bonus.crit_atk_rate);
					if(sd->status.party_id && (lv = pc_checkskill(sd, TK_POWER)) > 0 &&
						(i = party_foreachsamemap(party_sub_count, sd, 0)) > 1) //Exclude the player himself [Inkfish]
						ATK_ADDRATE(wd.damage, wd.damage2, 2 * lv * (i - 1));
				}
#endif
			}
			break;
	} //End switch(skill_id)
	return wd;
}

//For quick div adjustment
#define DAMAGE_DIV_FIX(dmg, div) { if(div > 1) (dmg) *= div; else if(div < 0) (div) *= -1; }
#define DAMAGE_DIV_FIX2(dmg, div) { if(div > 1) (dmg) *= div; }

/*=======================================
 * Check for and calculate multi attacks
 *---------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static struct Damage battle_calc_multi_attack(struct Damage wd, struct block_list *src,struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct status_data *tstatus = status_get_status_data(target);

	//If no skill_id passed, check for double attack [helvetica]
	if(sd && !skill_id) {
		short i;
		short dachance = 0; //Success chance of double attacking. If player is in fear breeze status and generated number is within fear breeze's range, this will be ignored
		short hitnumber = 0; //Used for setting how many hits will hit
		short gendetect[] = { 12, 12, 21, 27, 30 }; //If generated number is outside this value while in fear breeze status, it will check if their's a chance for double attacking
		short generate = rnd()%100 + 1; //Generates a random number between 1 - 100 which is then used to determine if fear breeze or double attacking will happen

		//First we go through a number of checks to see if their's any chance of double attacking a target. Only the highest success chance is taken
		if(sd->bonus.double_rate > 0 && sd->weapontype1 != W_FIST)
			dachance = sd->bonus.double_rate;

		if(sc && sc->data[SC_KAGEMUSYA] && sc->data[SC_KAGEMUSYA]->val3 > dachance && sd->weapontype1 != W_FIST)
			dachance = sc->data[SC_KAGEMUSYA]->val3;

		if(sc && sc->data[SC_E_CHAIN] && 5 * sc->data[SC_E_CHAIN]->val1 > dachance)
			dachance = 5 * sc->data[SC_E_CHAIN]->val1;

		if(5 * pc_checkskill(sd,TF_DOUBLE) > dachance && sd->weapontype1 == W_DAGGER)
			dachance = 5 * pc_checkskill(sd,TF_DOUBLE);

		if(5 * pc_checkskill(sd,GS_CHAINACTION) > dachance && sd->weapontype1 == W_REVOLVER)
			dachance = 5 * pc_checkskill(sd,GS_CHAINACTION);

		//This checks if the generated value is within fear breeze's success chance range for the level used as set by gendetect
		if(sc && sc->data[SC_FEARBREEZE] && generate <= gendetect[sc->data[SC_FEARBREEZE]->val1 - 1] &&
			sd->weapontype1 == W_BOW && (i = sd->equip_index[EQI_AMMO]) > 0 && sd->inventory_data[i] &&
			sd->status.inventory[i].amount > 1)
		{
				if(generate >= 1 && generate <= 12) //12% chance to deal 2 hits
					hitnumber = 2;
				else if(generate >= 13 && generate <= 21) //9% chance to deal 3 hits
					hitnumber = 3;
				else if(generate >= 22 && generate <= 27) //6% chance to deal 4 hits
					hitnumber = 4;
				else if(generate >= 28 && generate <= 30) //3% chance to deal 5 hits
					hitnumber = 5;
				hitnumber = min(hitnumber,sd->status.inventory[i].amount);
				sc->data[SC_FEARBREEZE]->val4 = hitnumber - 1;
		}
		//If the generated value is higher then Fear Breeze's success chance range,
		//but not higher then the player's double attack success chance, then allow a double attack to happen
		else if(generate < dachance)
			hitnumber = 2;

		if(hitnumber > 1) { //Needed to allow critical attacks to hit when not hitting more then once
			wd.div_ = hitnumber;
			wd.type = DMG_MULTI_HIT;
			if(sc && sc->data[SC_E_CHAIN] && !sc->data[SC_QD_SHOT_READY])
				sc_start(src,src,SC_QD_SHOT_READY,100,target->id,skill_get_time(RL_QD_SHOT,1) + status_get_dex(src) * 4);
		}
	}

	if(skill_id == RA_AIMEDBOLT && tsc && (tsc->data[SC_ANKLE] || tsc->data[SC_ELECTRICSHOCKER] || tsc->data[SC_BITE]))
		wd.div_ = tstatus->size + 2 + (rnd()%100 < 50 - tstatus->size * 10 ? 1 : 0);

	return wd;
}

/*======================================================
 * Calculate skill level ratios for weapon-based skills
 *------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static int battle_calc_attack_skill_ratio(struct Damage wd,struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC,src);
	struct map_session_data *tsd = BL_CAST(BL_PC,target);
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	int skillratio = 100;
	int i;

	if(sc && battle_skill_stacks_masteries_vvs(skill_id)) { //Skill damage modifiers that stack linearly
		if(sc->data[SC_OVERTHRUST])
			skillratio += sc->data[SC_OVERTHRUST]->val3;
		if(sc->data[SC_MAXOVERTHRUST])
			skillratio += sc->data[SC_MAXOVERTHRUST]->val2;
		if(sc->data[SC_BERSERK])
#ifndef RENEWAL
			skillratio += 100;
#else
			skillratio += 200;
		if(sc->data[SC_TRUESIGHT])
			skillratio += 2 * sc->data[SC_TRUESIGHT]->val1;
		if(sc->data[SC_CONCENTRATION])
			skillratio += sc->data[SC_CONCENTRATION]->val2;
#endif
		if(sc->data[SC_CRUSHSTRIKE] && (!skill_id || skill_id == KN_AUTOCOUNTER)) {
			skillratio += -100 + sc->data[SC_CRUSHSTRIKE]->val2;
			skill_break_equip(src,src,EQP_WEAPON,2000,BCT_SELF);
			status_change_end(src,SC_CRUSHSTRIKE,INVALID_TIMER);
		}
		if(sc->data[SC_HEAT_BARREL])
			skillratio += sc->data[SC_HEAT_BARREL]->val2;
	}

	switch(skill_id) {
		case SM_BASH:
		case MS_BASH:
			skillratio += 30 * skill_lv;
			break;
		case SM_MAGNUM:
		case MS_MAGNUM:
			if(wd.miscflag == 1)
				skillratio += 20 * skill_lv; //Inner 3x3 circle takes 100%+20%*level damage [Playtester]
			else
				skillratio += 10 * skill_lv; //Outer 5x5 circle takes 100%+10%*level damage [Playtester]
			break;
		case MC_MAMMONITE:
			skillratio += 50 * skill_lv;
			break;
		case HT_POWER:
			skillratio += -50 + 8 * sstatus->str;
			break;
		case AC_DOUBLE:
		case MA_DOUBLE:
			skillratio += 10 * (skill_lv - 1);
			break;
		case AC_SHOWER:
		case MA_SHOWER:
#ifdef RENEWAL
			skillratio += 50 + 10 * skill_lv;
#else
			skillratio += -25 + 5 * skill_lv;
#endif
			break;
		case AC_CHARGEARROW:
		case MA_CHARGEARROW:
			skillratio += 50;
			break;
#ifndef RENEWAL
		case HT_FREEZINGTRAP:
		case MA_FREEZINGTRAP:
			skillratio += -50 + 10 * skill_lv;
			break;
#endif
		case KN_PIERCE:
		case ML_PIERCE:
			skillratio += 10 * skill_lv;
			break;
		case MER_CRASH:
			skillratio += 10 * skill_lv;
			break;
		case KN_SPEARSTAB:
			skillratio += 20 * skill_lv;
			break;
		case KN_SPEARBOOMERANG:
			skillratio += 50 * skill_lv;
			break;
		case KN_BRANDISHSPEAR:
		case ML_BRANDISH:
			{
				int ratio = 100 + 20 * skill_lv;

				skillratio += -100 + ratio;
				if(skill_lv > 3 && !wd.miscflag)
					skillratio += ratio / 2;
				if(skill_lv > 6 && !wd.miscflag)
					skillratio += ratio / 4;
				if(skill_lv > 9 && !wd.miscflag)
					skillratio += ratio / 8;
				if(skill_lv > 6 && wd.miscflag == 1)
					skillratio += ratio / 2;
				if(skill_lv > 9 && wd.miscflag == 1)
					skillratio += ratio / 4;
				if(skill_lv > 9 && wd.miscflag == 2)
					skillratio += ratio / 2;
				break;
			}
		case KN_BOWLINGBASH:
		case MS_BOWLINGBASH:
			skillratio+= 40 * skill_lv;
			break;
		case AS_GRIMTOOTH:
			skillratio += 20 * skill_lv;
			break;
		case AS_POISONREACT:
			skillratio += 30 * skill_lv;
			break;
		case AS_SONICBLOW:
			skillratio += 300 + 40 * skill_lv;
			break;
		case TF_SPRINKLESAND:
			skillratio += 30;
			break;
		case MC_CARTREVOLUTION:
			skillratio += 50;
			if(sd && sd->cart_weight)
				skillratio += 100 * sd->cart_weight / sd->cart_weight_max; //+1% every 1% weight
			else if(!sd)
				skillratio += 100; //Max damage for non players
			break;
		case NPC_PIERCINGATT:
			skillratio += -25; //75% base damage
			break;
		case NPC_COMBOATTACK:
			skillratio += 25 * skill_lv;
			break;
		case NPC_RANDOMATTACK:
		case NPC_WATERATTACK:
		case NPC_GROUNDATTACK:
		case NPC_FIREATTACK:
		case NPC_WINDATTACK:
		case NPC_POISONATTACK:
		case NPC_HOLYATTACK:
		case NPC_DARKNESSATTACK:
		case NPC_UNDEADATTACK:
		case NPC_TELEKINESISATTACK:
		case NPC_BLOODDRAIN:
		case NPC_ACIDBREATH:
		case NPC_DARKNESSBREATH:
		case NPC_FIREBREATH:
		case NPC_ICEBREATH:
		case NPC_THUNDERBREATH:
		case NPC_HELLJUDGEMENT:
		case NPC_PULSESTRIKE:
			skillratio += 100 * (skill_lv - 1);
			break;
		case NPC_EARTHQUAKE:
			skillratio += 100 + 100 * skill_lv + 100 * skill_lv / 2;
			break;
		case RG_BACKSTAP:
			if(sd && sd->status.weapon == W_BOW && battle_config.backstab_bow_penalty)
				skillratio += (200 + 40 * skill_lv) / 2;
			else
				skillratio += 200 + 40 * skill_lv;
			break;
		case RG_RAID:
			skillratio += 40 * skill_lv;
			break;
		case RG_INTIMIDATE:
			skillratio += 30 * skill_lv;
			break;
		case CR_SHIELDCHARGE:
			skillratio += 20 * skill_lv;
			break;
		case CR_SHIELDBOOMERANG:
			skillratio += 30 * skill_lv;
			break;
		case NPC_DARKCROSS:
		case CR_HOLYCROSS:
#ifdef RENEWAL
			if(sd && sd->status.weapon == W_2HSPEAR)
				skillratio += 70 * skill_lv;
			else
#endif
				skillratio += 35 * skill_lv;
			break;
#ifndef RENEWAL
		case AM_DEMONSTRATION:
			skillratio += 20 * skill_lv;
			break;
#endif
		case AM_ACIDTERROR:
			skillratio += 40 * skill_lv;
			break;
		case MO_FINGEROFFENSIVE:
			skillratio += 50 * skill_lv;
			break;
		case MO_INVESTIGATE:
			skillratio += 100 + 150 * skill_lv;
			break;
		case MO_TRIPLEATTACK:
			skillratio += 20 * skill_lv;
			break;
		case MO_CHAINCOMBO:
			skillratio += 50 + 50 * skill_lv;
			break;
		case MO_COMBOFINISH:
			skillratio += 140 + 60 * skill_lv;
			break;
		case BA_MUSICALSTRIKE:
		case DC_THROWARROW:
			skillratio += 25 + 25 * skill_lv;
			break;
		case CH_TIGERFIST:
			skillratio += -60 + 100 * skill_lv;
			break;
		case CH_CHAINCRUSH:
			skillratio += 300 + 100 * skill_lv;
			break;
		case CH_PALMSTRIKE:
			skillratio += 100 + 100 * skill_lv;
			break;
		case LK_HEADCRUSH:
			skillratio += 40 * skill_lv;
			break;
		case LK_JOINTBEAT:
			i = -50 + 10 * skill_lv;
			if(wd.miscflag&BREAK_NECK)
				i <<= 1; //Although not clear, it's being assumed that the 2x damage is only for the break neck ailment
			skillratio += i;
			break;
#ifdef RENEWAL //Renewal: Skill ratio applies to entire damage [helvetica]
		case LK_SPIRALPIERCE:
		case ML_SPIRALPIERCE:
			skillratio += 50 * skill_lv;
			break;
#endif
		case ASC_METEORASSAULT:
			skillratio += -60 + 40 * skill_lv;
			break;
		case SN_SHARPSHOOTING:
		case MA_SHARPSHOOTING:
			skillratio += 100 + 50 * skill_lv;
			break;
		case CG_ARROWVULCAN:
			skillratio += 100 + 100 * skill_lv;
			break;
		case AS_SPLASHER:
			skillratio += 400 + 50 * skill_lv;
			if(sd)
				skillratio += 20 * pc_checkskill(sd,AS_POISONREACT);
			break;
#ifndef RENEWAL //Pre-Renewal: Skill ratio for weapon part of damage [helvetica]
		case ASC_BREAKER:
			skillratio += -100 + 100 * skill_lv;
			break;
#endif
		case PA_SACRIFICE:
			skillratio += -10 + 10 * skill_lv;
			break;
		case PA_SHIELDCHAIN:
			skillratio += 30 * skill_lv;
			break;
		case WS_CARTTERMINATION:
			i = max(1,10 * (16 - skill_lv));
			if(sd && sd->cart_weight) //Preserve damage ratio when max cart weight is changed
				skillratio += sd->cart_weight / i * 80000 / battle_config.max_cart_weight - 100;
			else if(!sd)
				skillratio += 80000 / i - 100;
			break;
		case TK_DOWNKICK:
		case TK_STORMKICK:
			skillratio += 60 + 20 * skill_lv + 10 * pc_checkskill(sd,TK_RUN); //+Dmg (to Kick skills, %)
			break;
		case TK_TURNKICK:
		case TK_COUNTER:
			skillratio += 90 + 30 * skill_lv + 10 * pc_checkskill(sd,TK_RUN);
			break;
		case TK_JUMPKICK:
			skillratio += -70 + 10 * skill_lv + 10 * pc_checkskill(sd,TK_RUN);
			if(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == skill_id)
				skillratio += 10 * status_get_lv(src) / 3; //Tumble bonus
			if(wd.miscflag) { //Running bonus
				skillratio += 10 * status_get_lv(src) / 3; //@TODO: Check the real value?
				if(sc && sc->data[SC_STRUP]) //Strup bonus
					skillratio <<= 1;
			}
			break;
		case GS_TRIPLEACTION:
			skillratio += 50 * skill_lv;
			break;
		case GS_BULLSEYE: //Only works well against non boss brute/demi human monster
			if(!(tstatus->mode&MD_BOSS) && (tstatus->race == RC_BRUTE || tstatus->race == RC_DEMIHUMAN))
				skillratio += 400;
			break;
		case GS_TRACKING:
			skillratio += 100 * (skill_lv + 1);
			break;
		case GS_PIERCINGSHOT:
#ifdef RENEWAL
			if(sd && sd->weapontype1 == W_RIFLE)
				skillratio += 50 + 30 * skill_lv;
			else
#endif
				skillratio += 20 * skill_lv;
			break;
		case GS_RAPIDSHOWER:
			skillratio += 400 + 50 * skill_lv;
			break;
		case GS_DESPERADO:
			skillratio += 50 * (skill_lv - 1);
			break;
		case GS_DUST:
			skillratio += 50 * skill_lv;
			break;
		case GS_FULLBUSTER:
			skillratio += 100 * (skill_lv + 2);
			break;
		case GS_SPREADATTACK:
#ifdef RENEWAL
			skillratio += 20 * skill_lv;
#else
			skillratio += 20 * (skill_lv - 1);
#endif
			break;
#ifdef RENEWAL
		case GS_GROUNDDRIFT:
			skillratio += 100 + 20 * skill_lv;
			break;
#endif
		case NJ_HUUMA:
			skillratio += 50 + 150 * skill_lv;
			break;
		case NJ_TATAMIGAESHI:
			skillratio += 10 * skill_lv;
#ifdef RENEWAL
			skillratio <<= 1;
#endif
			break;
		case NJ_KASUMIKIRI:
			skillratio += 10 * skill_lv;
			break;
		case NJ_KIRIKAGE:
			skillratio += 100 * (skill_lv - 1);
			break;
#ifdef RENEWAL
		case NJ_KUNAI:
			skillratio += 200;
			break;
#endif
		case KN_CHARGEATK: { //+100% every 3 cells of distance but hard-limited to 500%
				unsigned int k = (wd.miscflag - 1) / 3;

				if(k < 0)
					k = 0;
				else if(k > 4)
					k = 4;
				skillratio += 100 * k;
			}
			break;
		case HT_PHANTASMIC:
			skillratio += 50;
			break;
		case MO_BALKYOUNG:
			skillratio += 200;
			break;
		case HFLI_MOON: //[orn]
			skillratio += 10 + 110 * skill_lv;
			break;
		case HFLI_SBR44: //[orn]
			skillratio += 100 * (skill_lv - 1);
			break;
		case NPC_VAMPIRE_GIFT:
			skillratio += ((skill_lv - 1)%5 + 1) * 100;
			break;
		case RK_SONICWAVE:
			//ATK = {((Skill Level + 5) x 100) x (1 + [(Caster's Base Level - 100) / 200])} %
			skillratio += -100 + (skill_lv + 5) * 100;
			skillratio = skillratio * (100 + (status_get_lv(src) - 100) / 2) / 100;
			break;
		case RK_HUNDREDSPEAR:
			skillratio += 500 + 80 * skill_lv;
			if(sd) {
				short index = sd->equip_index[EQI_HAND_R];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON)
					skillratio += max(10000 - sd->inventory_data[index]->weight,0) / 10;
				skillratio += 50 * pc_checkskill(sd,LK_SPIRALPIERCE);
			} //(1 + [(Caster's Base Level - 100) / 200])
			skillratio = skillratio * (100 + (status_get_lv(src) - 100) / 2) / 100;
			break;
		case RK_WINDCUTTER:
			skillratio += -100 + (skill_lv + 2) * 50;
			RE_LVL_DMOD(100);
			break;
		case RK_IGNITIONBREAK:
			//3x3 cell Damage = ATK [{(Skill Level x 300) x (1 + [(Caster's Base Level - 100) / 100])}] %
			//7x7 cell Damage = ATK [{(Skill Level x 250) x (1 + [(Caster's Base Level - 100) / 100])}] %
			//11x11 cell Damage = ATK [{(Skill Level x 200) x (1 + [(Caster's Base Level - 100) / 100])}] %
			i = distance_bl(src,target);
			if(i < 2)
				skillratio += -100 + 300 * skill_lv;
			else if(i < 4)
				skillratio += -100 + 250 * skill_lv;
			else
				skillratio += -100 + 200 * skill_lv;
			skillratio = skillratio * status_get_lv(src) / 100;
			//Additional (Skill Level x 100) damage if your weapon element is fire
			if(sstatus->rhw.ele == ELE_FIRE)
				skillratio += 100 * skill_lv;
			break;
		case RK_STORMBLAST:
			//ATK = [{Rune Mastery Skill Level + (Caster's INT / 8)} x 100] %
			skillratio += -100 + 100 * ((sd ? pc_checkskill(sd,RK_RUNEMASTERY) : 10) + sstatus->int_ / 8);
			break;
		case RK_PHANTOMTHRUST:
			//ATK = [{(Skill Level x 50) + (Spear Master Level x 10)} x Caster's Base Level / 150] %
			skillratio += -100 + 50 * skill_lv + 10 * (sd ? pc_checkskill(sd,KN_SPEARMASTERY): 10);
			RE_LVL_DMOD(150); //Base level bonus
			break;
		case GC_CROSSIMPACT:
			skillratio += 900 + 100 * skill_lv;
			RE_LVL_DMOD(120);
			break;
		case GC_COUNTERSLASH:
			//ATK [{(Skill Level x 100) + 300} x Caster's Base Level / 120]% + ATK [(AGI x 2) + (Caster's Job Level x 4)]%
			skillratio += 200 + (100 * skill_lv);
			RE_LVL_DMOD(120);
			break;
		case GC_VENOMPRESSURE:
			skillratio += 900;
			break;
		case GC_PHANTOMMENACE:
			skillratio += 200;
			break;
		case GC_ROLLINGCUTTER:
			skillratio += -50 + 50 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case GC_CROSSRIPPERSLASHER:
			skillratio += 300 + 80 * skill_lv;
			RE_LVL_DMOD(100);
			if(sc && sc->data[SC_ROLLINGCUTTER])
				skillratio += sc->data[SC_ROLLINGCUTTER]->val1 * sstatus->agi;
			break;
		case GC_DARKCROW:
			skillratio += 100 * (skill_lv - 1);
			break;
		case AB_DUPLELIGHT_MELEE:
			skillratio += 10 * skill_lv;
			break;
		case RA_ARROWSTORM:
			skillratio += 900 + 80 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case RA_AIMEDBOLT:
			skillratio += 400 + 50 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case RA_CLUSTERBOMB:
			skillratio += 100 + 100 * skill_lv;
			break;
		case RA_WUGDASH: //ATK 300%
			skillratio += 200;
			break;
		case RA_WUGSTRIKE:
			skillratio += -100 + 200 * skill_lv;
			break;
		case RA_WUGBITE:
			skillratio += 300 + 200 * skill_lv;
			if(skill_lv == 5)
				skillratio += 100;
			break;
		case RA_SENSITIVEKEEN:
			skillratio += 50 * skill_lv;
			break;
		case NC_BOOSTKNUCKLE:
			skillratio += 100 + 100 * skill_lv + sstatus->dex;
			RE_LVL_DMOD(120);
			break;
		case NC_PILEBUNKER:
			skillratio += 200 + 100 * skill_lv + sstatus->str;
			RE_LVL_DMOD(100);
			break;
		case NC_VULCANARM:
			skillratio += -100 + 70 * skill_lv + sstatus->dex;
			RE_LVL_DMOD(120);
			break;
		case NC_FLAMELAUNCHER:
		case NC_COLDSLOWER:
			skillratio += 200 + 300 * skill_lv;
			RE_LVL_DMOD(150);
			break;
		case NC_ARMSCANNON:
			switch(tstatus->size) {
				case SZ_SMALL: skillratio += 200 + 400 * skill_lv; break; //Small
				case SZ_MEDIUM: skillratio += 200 + 350 * skill_lv; break; //Medium
				case SZ_BIG: skillratio += 200 + 300 * skill_lv; break; //Large
			}
			RE_LVL_DMOD(120);
			break;
		case NC_AXEBOOMERANG:
			skillratio += 150 + 50 * skill_lv;
			if(sd) {
				short index = sd->equip_index[EQI_HAND_R];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON)
					skillratio += sd->inventory_data[index]->weight / 10;
			}
			RE_LVL_DMOD(100);
			break;
		case NC_POWERSWING:
			skillratio += -100 + sstatus->str + sstatus->dex;
			RE_LVL_DMOD(100);
			skillratio += 300 + 100 * skill_lv;
			break;
		case NC_AXETORNADO:
			skillratio += 100 + 100 * skill_lv + sstatus->vit;
			RE_LVL_DMOD(100);
			i = distance_bl(src,target);
			if(i > 2)
				skillratio = skillratio * 75 / 100;
			break;
		case NC_MAGMA_ERUPTION_DOTDAMAGE:
			skillratio += 350 + 50 * skill_lv;
			break;
		case SC_FATALMENACE:
			skillratio += 100 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case SC_TRIANGLESHOT:
			skillratio += 200 + (skill_lv - 1) * sstatus->agi / 2;
			RE_LVL_DMOD(120);
			break;
		case SC_FEINTBOMB:
			skillratio += -100 + (1 + skill_lv) * sstatus->dex / 2 * status_get_job_lv(src) / 10;
			RE_LVL_DMOD(120);
			break;
		case LG_CANNONSPEAR:
			skillratio += -100 + skill_lv * (50 + sstatus->str);
			RE_LVL_DMOD(100);
			break;
		case LG_BANISHINGPOINT:
			skillratio += -100 + 50 * skill_lv + 30 * (sd ? pc_checkskill(sd,SM_BASH) : 10);
			RE_LVL_DMOD(100);
			break;
		case LG_SHIELDPRESS:
			skillratio += -100 + 150 * skill_lv + sstatus->str;
			if(sd) {
				short index = sd->equip_index[EQI_HAND_L];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
					skillratio += sd->inventory_data[index]->weight / 10;
			}
			RE_LVL_DMOD(100);
			break;
		case LG_PINPOINTATTACK:
			skillratio += -100 + 100 * skill_lv + 5 * sstatus->agi;
			RE_LVL_DMOD(120);
			break;
		case LG_RAGEBURST:
			if(sd && sd->spiritball_old)
				skillratio += -100 + 200 * sd->spiritball_old + (sstatus->max_hp - sstatus->hp) / 100;
			else
				skillratio += 2900 + (sstatus->max_hp - sstatus->hp) / 100;
			RE_LVL_DMOD(100);
			break;
		case LG_SHIELDSPELL:
			if(sd && skill_lv == 1) { //[(Caster's Base Level x 4) + (Shield DEF x 10) + (Caster's VIT x 2)] %
				short index = sd->equip_index[EQI_HAND_L];

				skillratio += -100 + 4 * status_get_lv(src) + 2 * sstatus->vit;
				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
					skillratio += sd->inventory_data[index]->def * 10;
			} else
				skillratio = 0;
			break;
		case LG_MOONSLASHER:
			skillratio += -100 + 120 * skill_lv + 80 * (sd ? pc_checkskill(sd,LG_OVERBRAND) : 5);
			RE_LVL_DMOD(100);
			break;
		case LG_OVERBRAND:
			skillratio += -100 + 400 * skill_lv + 50 * (sd ? pc_checkskill(sd,CR_SPEARQUICKEN) : 10);
			RE_LVL_DMOD(150);
			break;
		case LG_OVERBRAND_BRANDISH:
			skillratio += -100 + 300 * skill_lv + sstatus->str + sstatus->dex;
			RE_LVL_DMOD(150);
			break;
		case LG_OVERBRAND_PLUSATK:
			skillratio += -100 + 200 * skill_lv;
			break;
		case LG_RAYOFGENESIS:
			skillratio += 200 + 300 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case LG_EARTHDRIVE:
			if(sd) {
				short index = sd->equip_index[EQI_HAND_L];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
					skillratio += -100 + (skill_lv + 1) * sd->inventory_data[index]->weight / 10;
			}
			RE_LVL_DMOD(100);
			break;
		case LG_HESPERUSLIT:
			if(sc) {
				if(sc->data[SC_INSPIRATION])
					skillratio += 1100;
				if(sc->data[SC_BANDING]) {
					skillratio += -100 + 120 * skill_lv + 200 * sc->data[SC_BANDING]->val2;
					if(sc->data[SC_BANDING]->val2 > 5)
						skillratio = skillratio * 150 / 100;
				}
				RE_LVL_DMOD(100);
			}
			break;
		case SR_DRAGONCOMBO:
			skillratio += 40 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case SR_SKYNETBLOW:
			//ATK [{(Skill Level x 100) + (Caster's AGI) + 150} x Caster's Base Level / 100] %
			if(wd.miscflag&8)
				skillratio += 100 * skill_lv + sstatus->agi + 50;
			else //ATK [{(Skill Level x 80) + (Caster's AGI)} x Caster's Base Level / 100] %
				skillratio += -100 + 80 * skill_lv + sstatus->agi;
			RE_LVL_DMOD(100);
			break;
		case SR_EARTHSHAKER:
			//[(Skill Level x 150) x (Caster's Base Level / 100) + (Caster's INT x 3)] %
			if(tsc && ((tsc->option&(OPTION_HIDE|OPTION_CLOAK)) || tsc->data[SC_CAMOUFLAGE])) {
				skillratio += -100 + 150 * skill_lv;
				RE_LVL_DMOD(100);
				skillratio += sstatus->int_ * 3;
			} else { //[(Skill Level x 50) x (Caster's Base Level / 100) + (Caster's INT x 2)] %
				skillratio += -100 + 50 * skill_lv;
				RE_LVL_DMOD(100);
				skillratio += sstatus->int_ * 2;
			}
			break;
		case SR_FALLENEMPIRE: //ATK [(Skill Level x 150 + 100) x Caster's Base Level / 150] %
			skillratio += 150 * skill_lv;
			RE_LVL_DMOD(150);
 			break;
		case SR_TIGERCANNON: {
				int hp = sstatus->max_hp * (10 + 2 * skill_lv) / 100,
					sp = sstatus->max_sp * (5 + skill_lv) / 100;

				if(wd.miscflag&16)
					break;
				//ATK [((Caster's consumed HP + SP) / 2) x Caster's Base Level / 100] %
				if(wd.miscflag&8)
					skillratio += -100 + (hp + sp) / 2;
				else //ATK [((Caster's consumed HP + SP) / 4) x Caster's Base Level / 100] %
					skillratio += -100 + (hp + sp) / 4;
				RE_LVL_DMOD(100);
			}
			break;
		case SR_RAMPAGEBLASTER:
			if(sc && sc->data[SC_EXPLOSIONSPIRITS]) {
				skillratio += -100 + (20 * sc->data[SC_EXPLOSIONSPIRITS]->val1 + 20 * skill_lv) * (sd ? sd->spiritball_old : 1);
				RE_LVL_DMOD(120);
			} else {
				skillratio += -100 + (20 * skill_lv) * (sd ? sd->spiritball_old : 1);
				RE_LVL_DMOD(150);
			}
			break;
		case SR_KNUCKLEARROW:
			if(wd.miscflag&4) {
				//ATK [(Skill Level x 150) + (1000 x Target's current weight / Maximum weight) + (Target's Base Level x 5) x (Caster's Base Level / 150)] %
				skillratio += -100 + 150 * skill_lv + status_get_lv(target) * 5;
				if(tsd && tsd->weight)
					skillratio += 100 * tsd->weight / tsd->max_weight;
				RE_LVL_DMOD(150);
			} else { //ATK [(Skill Level x 100 + 500) x Caster's Base Level / 100] %
				skillratio += 400 + 100 * skill_lv;
				RE_LVL_DMOD(100);
			}
			break;
		case SR_WINDMILL:
			//ATK [(Caster's Base Level + Caster's DEX) x Caster's Base Level / 100] %
			skillratio += -100 + status_get_lv(src) + sstatus->dex;
			RE_LVL_DMOD(100);
			break;
		case SR_CRESCENTELBOW_AUTOSPELL:
			//ATK [{(Target's HP / 100) x Skill Level} x Caster's Base Level / 125] %
			skillratio += -100 + tstatus->hp / 100 * skill_lv;
			RE_LVL_DMOD(125);
			skillratio = min(5000,skillratio); //Maximum of 5000% ATK
			break;
		case SR_GATEOFHELL:
			if(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE)
				skillratio += -100 + 800 * skill_lv;
			else
				skillratio += -100 + 500 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case SR_GENTLETOUCH_QUIET:
			skillratio += -100 + 100 * skill_lv + sstatus->dex;
			RE_LVL_DMOD(100);
			break;
		case SR_HOWLINGOFLION:
			skillratio += -100 + 300 * skill_lv;
			RE_LVL_DMOD(150);
			break;
		case SR_RIDEINLIGHTNING:
			//ATK [{(Skill Level x 200) + Additional Damage} x Caster's Base Level / 100] %
			skillratio += -100 + 200 * skill_lv;
			if((sstatus->rhw.ele) == ELE_WIND || (sstatus->lhw.ele) == ELE_WIND)
				skillratio += skill_lv * 50;
			RE_LVL_DMOD(100);
			break;
		case WM_REVERBERATION_MELEE:
			//ATK [{(Skill Level x 100) + 300} x Caster's Base Level / 100]
			skillratio += 200 + 100 * (sd ? pc_checkskill(sd,WM_REVERBERATION) : 5);
			RE_LVL_DMOD(100);
			break;
		case WM_SEVERE_RAINSTORM_MELEE:
			//ATK [{(Caster's DEX + AGI) x (Skill Level / 5)} x Caster's Base Level / 100] %
			skillratio += -100 + (sstatus->dex + sstatus->agi) * skill_lv / 5;
			RE_LVL_DMOD(100);
			break;
		case WM_GREAT_ECHO: {
				skillratio += 300 + 200 * skill_lv;

				if(party_calc_chorusbonus(sd,0) == 1)
					skillratio += 100;
				else if(party_calc_chorusbonus(sd,0) == 2)
					skillratio += 200;
				else if(party_calc_chorusbonus(sd,0) == 3)
					skillratio += 400;
				else if(party_calc_chorusbonus(sd,0) == 4)
					skillratio += 800;
				else if(party_calc_chorusbonus(sd,0) == 5)
					skillratio += 1600;
			}
			RE_LVL_DMOD(100);
			break;
		case GN_CART_TORNADO: {
				//ATK [( Skill Level x 50 ) + ( Cart Weight / ( 150 - Caster's Base STR ))] + ( Cart Remodeling Skill Level x 50 )] %
				skillratio += -100 + 50 * skill_lv;
				if(sd && sd->cart_weight) {
					short strbonus = sd->status.str; //Only using base STR

					skillratio += sd->cart_weight / 10 / (150 - min(strbonus,120)) + 50 * pc_checkskill(sd,GN_REMODELING_CART);
				}
			}
			break;
		case GN_CARTCANNON: //ATK [{( Cart Remodeling Skill Level x 50 ) x ( INT / 40 )} + ( Cart Cannon Skill Level x 60 )] %
			skillratio += -100 + 60 * skill_lv + 50 * (sd ? pc_checkskill(sd,GN_REMODELING_CART) : 5) * sstatus->int_ / 40;
			break;
		case GN_SPORE_EXPLOSION:
			skillratio += 100 + sstatus->int_ + 100 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case GN_WALLOFTHORN:
			skillratio += 10 * skill_lv;
			break;
		case GN_CRAZYWEED_ATK:
			skillratio += 400 + 100 * skill_lv;
			break;
		case GN_SLINGITEM_RANGEMELEEATK:
			if(sd) {
				switch(sd->itemid) {
					case ITEMID_APPLE_BOMB:
						skillratio += 200 + sstatus->str + sstatus->dex;
						break;
					case ITEMID_COCONUT_BOMB:
					case ITEMID_PINEAPPLE_BOMB:
						skillratio += 700 + sstatus->str + sstatus->dex;
						break;
					case ITEMID_MELON_BOMB:
						skillratio += 400 + sstatus->str + sstatus->dex;
						break;
					case ITEMID_BANANA_BOMB:
						skillratio += 777 + sstatus->str + sstatus->dex;
						break;
					case ITEMID_BLACK_LUMP:
						skillratio += -100 + (sstatus->str + sstatus->agi + sstatus->dex) / 3;
						break;
					case ITEMID_BLACK_HARD_LUMP:
						skillratio += -100 + (sstatus->str + sstatus->agi + sstatus->dex) / 2;
						break;
					case ITEMID_VERY_HARD_LUMP:
						skillratio += -100 + sstatus->str + sstatus->agi + sstatus->dex;
						break;
				}
				RE_LVL_DMOD(100);
			}
			break;
		case SO_VARETYR_SPEAR:
			//ATK [{( Striking Level x 50 ) + ( Varetyr Spear Skill Level x 50 )} x Caster's Base Level / 100 ] %
			skillratio += -100 + 50 * skill_lv + 50 * (sd ? pc_checkskill(sd,SO_STRIKING) : 5);
			RE_LVL_DMOD(100);
			if(sc && sc->data[SC_BLAST_OPTION])
				skillratio += 5 * status_get_job_lv(src);
			break;
		//Physical Elemantal Spirits Attack Skills
		case EL_CIRCLE_OF_FIRE:
		case EL_FIRE_BOMB_ATK:
		case EL_STONE_RAIN:
			skillratio += 200;
			break;
		case EL_FIRE_WAVE_ATK:
			skillratio += 500;
			break;
		case EL_TIDAL_WEAPON:
			skillratio += 1400;
			break;
		case EL_WIND_SLASH:
			skillratio += 100;
			break;
		case EL_HURRICANE:
			skillratio += 600;
			break;
		case EL_TYPOON_MIS:
		case EL_WATER_SCREW_ATK:
			skillratio += 900;
			break;
		case EL_STONE_HAMMER:
			skillratio += 400;
			break;
		case EL_ROCK_CRUSHER:
			skillratio += 700;
			break;
		case KO_JYUMONJIKIRI:
			skillratio += -100 + 150 * skill_lv;
			RE_LVL_DMOD(120);
			if(tsc && tsc->data[SC_JYUMONJIKIRI])
				skillratio += skill_lv * status_get_lv(src);
			break;
		case KO_HUUMARANKA:
			skillratio += -100 + 150 * skill_lv + sstatus->agi + sstatus->dex + 100 * (sd ? pc_checkskill(sd,NJ_HUUMA) : 5);
			break;
		case KO_SETSUDAN:
			skillratio += 100 * (skill_lv - 1);
			RE_LVL_DMOD(100);
			if(tsc && tsc->data[SC_SPIRIT])
				skillratio += 200 * tsc->data[SC_SPIRIT]->val1;
			break;
		case KO_BAKURETSU:
			skillratio += -100 + (50 + sstatus->dex / 4) * (sd ? pc_checkskill(sd,NJ_TOBIDOUGU) : 10) * 4 * skill_lv / 10;
			RE_LVL_DMOD(120);
			skillratio += 10 * status_get_job_lv(src);
			break;
		case KO_MAKIBISHI:
			skillratio += -100 + 20 * skill_lv;
			break;
		case MH_NEEDLE_OF_PARALYZE:
			skillratio += 600 + 100 * skill_lv;
			break;
		case MH_STAHL_HORN:
			skillratio += 400 + 100 * skill_lv * status_get_lv(src) / 150;
			break;
		case MH_LAVA_SLIDE:
			skillratio += -100 + (10 * skill_lv + status_get_lv(src)) * 2 * status_get_lv(src) / 100;
			break;
		case MH_SONIC_CRAW:
			skillratio += -100 + 40 * skill_lv * status_get_lv(src) / 150;
			break;
		case MH_SILVERVEIN_RUSH:
			skillratio += -100 + 150 * skill_lv * status_get_lv(src) / 100;
			break;
		case MH_MIDNIGHT_FRENZY:
			skillratio += -100 + 300 * skill_lv * status_get_lv(src) / 150;
			break;
		case MH_TINDER_BREAKER:
			skillratio += -100 + (100 * skill_lv + 3 * sstatus->str) * status_get_lv(src) / 120;
			break;
		case MH_CBC:
			skillratio += 300 * skill_lv + 4 * status_get_lv(src);
			break;
		case MH_MAGMA_FLOW:
			skillratio += -100 + (100 * skill_lv + 3 * status_get_lv(src)) * status_get_lv(src) / 120;
			break;
		case RL_MASS_SPIRAL:
			skillratio += -100 + 200 * skill_lv;
			break;
		case RL_FIREDANCE:
			skillratio += -100 + 100 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case RL_BANISHING_BUSTER:
			skillratio += 900 + 200 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case RL_S_STORM:
			skillratio += 900 + 100 * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case RL_SLUGSHOT: {
				uint16 w = 50;
				short idx = -1;

				if(sd && (idx = sd->equip_index[EQI_AMMO]) >= 0 && sd->inventory_data[idx])
					w = sd->inventory_data[idx]->weight / 10;
				skillratio += -100 + max(w,1) * 32 * skill_lv;
			}
			break;
		case RL_D_TAIL:
			skillratio += 2400 + 500 * skill_lv;
			break;
		case RL_R_TRIP:
		case RL_R_TRIP_PLUSATK:
			skillratio += -100 + (sstatus->dex / 2) * (10 + 3 * skill_lv);
			if(skill_id == RL_R_TRIP_PLUSATK)
				skillratio >>= 1; //Half damage
			break;
		case RL_H_MINE:
			skillratio += 100 + 200 * skill_lv;
			if(sd && sd->flicker) //Explode bonus damage
				skillratio += 500 + 300 * skill_lv;
			break;
		case RL_HAMMER_OF_GOD:
			skillratio += 1500 + 800 * skill_lv + (((sd ? sd->spiritball_old : 1) + 1) / 2) * 200;
			break;
		case RL_FIRE_RAIN:
			skillratio += 1900 + sstatus->dex * skill_lv;
			RE_LVL_DMOD(100);
			break;
		case RL_AM_BLAST:
			skillratio += 1900 + 200 * skill_lv;
			break;
		case SU_BITE:
			skillratio += 100;
			break;
		case SU_SCRATCH:
			skillratio += -50 + 50 * skill_lv;
			break;
		case SU_SCAROFTAROU:
			skillratio += -100 + 100 * skill_lv;
			if(is_boss(target))
				skillratio <<= 1;
			break;
		case SU_PICKYPECK:
			skillratio += 100 + 100 * skill_lv;
			break;
		case SU_PICKYPECK_DOUBLE_ATK:
			skillratio += 300 + 200 * skill_lv;
			break;
		case SU_LUNATICCARROTBEAT:
			skillratio += 100 + 100 * skill_lv;
			break;
	}
	return skillratio;
}

/*==============================================================================
 * Constant skill damage additions are added after skill level ratio calculation
 *------------------------------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static int64 battle_calc_skill_constant_addition(struct Damage wd,struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC,src);
	struct map_session_data *tsd = BL_CAST(BL_PC,target);
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	int64 atk = 0;

	switch(skill_id) {
		case MO_EXTREMITYFIST: //[malufett]
			atk = 250 * (skill_lv + 1) + 10 * (sstatus->sp + 1) * wd.damage / 100 + 8 * wd.damage;
			break;
#ifndef RENEWAL
		case GS_MAGICALBULLET:
			atk = status_get_matk(src,2);
			break;
#endif
		case NJ_SYURIKEN:
			atk = 4 * skill_lv;
			break;
#ifdef RENEWAL
		case HT_FREEZINGTRAP:
			if(sd)
				atk = 40 * pc_checkskill(sd,RA_RESEARCHTRAP);
			break;
#endif
		case GC_COUNTERSLASH:
			atk = 2 * sstatus->agi + 4 * status_get_job_lv(src);
			break;
		case LG_SHIELDPRESS:
			if(sd) {
				int damagevalue = 0;
				short index = sd->equip_index[EQI_HAND_L];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
					damagevalue = sstatus->vit * sd->status.inventory[index].refine;
				atk = damagevalue;
			}
			break;
		case SR_FALLENEMPIRE:
			atk = ((tstatus->size + 1) * 2 + skill_lv - 1) * sstatus->str;
			if(tsd && tsd->weight)
				atk += tsd->weight / 10 * sstatus->dex / 120;
			else //Mobs
				atk += status_get_lv(target) * 50;
			break;
	}
	return atk;
}

/*==============================================================
 * Stackable SC bonuses added on top of calculated skill damage
 *--------------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_attack_sc_bonus(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_change *sc = status_get_sc(src);
	struct status_data *sstatus = status_get_status_data(src);
#ifdef RENEWAL
	struct status_data *tstatus = status_get_status_data(target);
#endif
	struct status_change_entry *sce;
	int inf3 = skill_get_inf3(skill_id);

	if(sc && sc->count) { //The following are applied on top of current damage and are stackable
		if((sce = sc->data[SC_ATKPOTION])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val1);
#ifdef RENEWAL
			ATK_ADD(wd.statusAtk, wd.statusAtk2, sce->val1);
#endif
		}
		if((sce = sc->data[SC_QUEST_BUFF1])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val1);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val1);
#endif
		}
		if((sce = sc->data[SC_QUEST_BUFF2])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val1);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val1);
#endif
		}
		if((sce = sc->data[SC_QUEST_BUFF3])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val1);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val1);
#endif
		}
		if(sc->data[SC_ALMIGHTY]) {
			ATK_ADD(wd.damage, wd.damage2, 30);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, 30);
#endif
		}
		if((sce = sc->data[SC_FIGHTINGSPIRIT])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val1);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val1);
#endif
		}
		if((sce = sc->data[SC_BANDING]) && sce->val2 > 1) {
			ATK_ADD(wd.damage, wd.damage2, (10 + 10 * sce->val1) * sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, (10 + 10 * sce->val1) * sce->val2);
#endif
		}
		if((sce = sc->data[SC_GT_CHANGE])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
#endif
		}
		if((sce = sc->data[SC_WATER_BARRIER])) {
			ATK_ADD(wd.damage, wd.damage2, -sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, -sce->val2);
#endif
		}
		if((sce = sc->data[SC_PYROTECHNIC_OPTION])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
#endif
		}
		if((sce = sc->data[SC_HEATER_OPTION])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
#endif
		}
		if((sce = sc->data[SC_TROPIC_OPTION])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
#endif
		}
		if((sce = sc->data[SC_SATURDAYNIGHTFEVER])) {
			ATK_ADD(wd.damage, wd.damage2, 100 * sce->val1);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, 100 * sce->val1);
#endif
		}
		if((sce = sc->data[SC_ZENKAI]) && sstatus->rhw.ele == sce->val2) {
			ATK_ADD(wd.damage, wd.damage2, 200);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, 200);
#endif
		}
		if((sce = sc->data[SC_ZANGETSU])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.statusAtk, wd.statusAtk2, sce->val2);
#endif
		}
		if((sce = sc->data[SC_P_ALTER])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
#endif
		}
		if(sc->data[SC_STYLE_CHANGE]) {
			TBL_HOM *hd = BL_CAST(BL_HOM, src);

			if(hd)
				ATK_ADD(wd.damage, wd.damage2, hd->homunculus.spiritball * 3);
		}
		if((sce = sc->data[SC_FLASHCOMBO])) {
			ATK_ADD(wd.damage, wd.damage2, sce->val2);
#ifdef RENEWAL
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
#endif
		}
#ifdef RENEWAL
		if((sce = sc->data[SC_VOLCANO]))
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
		if((sce = sc->data[SC_DRUMBATTLE]))
			ATK_ADD(wd.equipAtk, wd.equipAtk2, sce->val2);
		if(sc->data[SC_MADNESSCANCEL])
			ATK_ADD(wd.equipAtk, wd.equipAtk2, 100);
		if((sce = sc->data[SC_GATLINGFEVER]))
			ATK_ADD(wd.equipAtk, wd.equipAtk2, 20 + 10 * sce->val1);
#else
		if((sce = sc->data[SC_TRUESIGHT]))
			ATK_ADDRATE(wd.damage, wd.damage2, 2 * sce->val1);
#endif
		if((sce = sc->data[SC_EDP])) {
			switch(skill_id) {
				//Pre-Renewal only: Soul Breaker and Meteor Assault ignores EDP 
				//Renewal only: Grimtooth and Venom Knife ignore EDP
				//Both: Venom Splasher ignores EDP [helvetica]
#ifndef RENEWAL_EDP
				case ASC_BREAKER:
				case ASC_METEORASSAULT:
#else
				case AS_GRIMTOOTH:
				case AS_VENOMKNIFE:
#endif
				case AS_SPLASHER:
					break; //Skills above have no effect with edp
#ifdef RENEWAL_EDP
				//Renewal EDP: damage gets a half modifier on top of EDP bonus for skills [helvetica]
				//* Sonic Blow
				//* Soul Breaker
				//* Counter Slash
				//* Cross Impact
				case AS_SONICBLOW:
				case ASC_BREAKER:
				case GC_COUNTERSLASH:
				case GC_CROSSIMPACT:
					//Only modifier is halved but still benefit with the damage bonus
					ATK_RATE(wd.damage, wd.damage2, 50);
	#ifdef RENEWAL
					ATK_RATE(wd.weaponAtk, wd.weaponAtk2, 50);
					ATK_RATE(wd.equipAtk, wd.equipAtk2, 50);
	#endif
				//Fall through to apply EDP bonuses
				default:
					//Renewal EDP formula [helvetica]
					//Weapon atk * (1 + (edp level * .8))
					//Equip atk * (1 + (edp level * .6))
					ATK_RATE(wd.damage, wd.damage2, 100 + sce->val1 * 80);
	#ifdef RENEWAL
					ATK_RATE(wd.weaponAtk, wd.weaponAtk2, 100 + sce->val1 * 80);
					ATK_RATE(wd.equipAtk, wd.equipAtk2, 100 + sce->val1 * 60);
	#endif
#else
				default:
					ATK_ADDRATE(wd.damage, wd.damage2, sce->val3);
	#ifdef RENEWAL
					ATK_ADDRATE(wd.weaponAtk, wd.weaponAtk2, sce->val3);
	#endif
#endif
					break;
			}
		}
		if((sce = sc->data[SC_INCATKRATE]) && sd) {
			ATK_ADDRATE(wd.damage, wd.damage2, sce->val1);
			RE_ALLATK_ADDRATE(wd, sce->val1);
		}
		if(skill_id == AS_SONICBLOW && (sce = sc->data[SC_SPIRIT]) && sce->val2 == SL_ASSASIN) {
			ATK_ADDRATE(wd.damage, wd.damage2, (map_flag_gvg2(src->m) ? 25 : 100));
			RE_ALLATK_ADDRATE(wd, (map_flag_gvg2(src->m) ? 25 : 100));
		}
		if(skill_id == CR_SHIELDBOOMERANG && (sce = sc->data[SC_SPIRIT]) && sce->val2 == SL_CRUSADER) {
			ATK_ADDRATE(wd.damage, wd.damage2, 100);
			RE_ALLATK_ADDRATE(wd, 100);
		}
		if((sce = sc->data[SC_GLOOMYDAY_SK]) && (inf3&INF3_SC_GLOOMYDAY_SK)) {
			ATK_ADDRATE(wd.damage, wd.damage2, sce->val2);
			RE_ALLATK_ADDRATE(wd, sce->val2);
		}
		if((sce = sc->data[SC_DANCEWITHWUG])) {
			if(inf3&INF3_SC_DANCEWITHWUG) {
				ATK_ADDRATE(wd.damage, wd.damage2, sce->val1 * 10 * party_calc_chorusbonus(sd,1));
				RE_ALLATK_ADDRATE(wd, sce->val1 * 10 * party_calc_chorusbonus(sd,1));
			}
			ATK_ADDRATE(wd.damage, wd.damage2, sce->val1 * 2 * party_calc_chorusbonus(sd,1));
#ifdef RENEWAL
			ATK_ADDRATE(wd.equipAtk, wd.equipAtk2, sce->val1 * 2 * party_calc_chorusbonus(sd,1));
#endif
		}
		if((sce = sc->data[SC_EQC])) {
			ATK_ADDRATE(wd.damage, wd.damage2, -sce->val2);
#ifdef RENEWAL
			ATK_ADDRATE(wd.equipAtk, wd.equipAtk2, -sce->val2);
#endif
		}
		if((wd.flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON)) {
			if((sce = sc->data[SC_UNLIMIT])) {
				switch(skill_id) {
					case RA_WUGDASH:
					case RA_WUGSTRIKE:
					case RA_WUGBITE:
						break;
					default:
						ATK_ADDRATE(wd.damage, wd.damage2, sce->val2);
						RE_ALLATK_ADDRATE(wd, sce->val2);
						break;
				}
			}
			if((sce = sc->data[SC_ARCLOUSEDASH]) && sce->val4) {
				ATK_ADDRATE(wd.damage, wd.damage2, sce->val4);
				RE_ALLATK_ADDRATE(wd, sce->val4);
			}
		}
		if(!skill_id) {
			if((sce = sc->data[SC_ENCHANTBLADE])) { //[((Skill Level x 20) + 100) x (Caster's Base Level / 150)] + Caster's INT + MATK - MDEF - MDEF2
				int64 dmg = sce->val2 + status_get_matk(src, 2);
				short totalmdef = tstatus->mdef + tstatus->mdef2;

				if((dmg = dmg - totalmdef) > 0) {
					ATK_ADD(wd.damage, wd.damage2, dmg);
#ifdef RENEWAL
					ATK_ADD(wd.weaponAtk, wd.weaponAtk2, dmg);
#endif
				}
			}
			if((sce = sc->data[SC_GIANTGROWTH]) && rnd()%100 < sce->val2) {
				ATK_ADDRATE(wd.damage, wd.damage2, 200);
				RE_ALLATK_ADDRATE(wd, 200);
			}
			if((sce = sc->data[SC_EXEEDBREAK])) {
				ATK_ADDRATE(wd.damage, wd.damage2, sce->val2);
				RE_ALLATK_ADDRATE(wd, sce->val2);
			}
			if((sce = sc->data[SC_SPELLFIST])) {
				struct Damage ad = battle_calc_magic_attack(src, target, sce->val3, sce->val4, BF_SHORT);

				wd.damage = wd.damage2 = ad.damage;
#ifdef RENEWAL
				wd.statusAtk = wd.statusAtk2 = ad.damage;
				wd.weaponAtk = wd.weaponAtk2 = 0;
				wd.equipAtk = wd.equipAtk2 = 0;
				wd.masteryAtk = wd.masteryAtk2 = 0;
#endif
			}
		}
	}
	if(sd) {
		if((wd.flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON)) {
			if(pc_checkskill(sd, SU_POWEROFLIFE) > 0 && (pc_checkskill(sd, SU_SCAROFTAROU) +
				pc_checkskill(sd, SU_PICKYPECK) + pc_checkskill(sd, SU_ARCLOUSEDASH) +
				pc_checkskill(sd, SU_LUNATICCARROTBEAT)) == 20)
			{
				ATK_ADDRATE(wd.damage, wd.damage2, 20);
				RE_ALLATK_ADDRATE(wd, 20);
			}
		}
		if(sd->spiritcharm_type == CHARM_TYPE_LAND && sd->spiritcharm > 0) { //KO Earth Charm effect +15% wATK
			ATK_ADDRATE(wd.damage, wd.damage2, 15 * sd->spiritcharm);
#ifdef RENEWAL
			ATK_ADDRATE(wd.weaponAtk, wd.weaponAtk2, 15 * sd->spiritcharm);
#endif
		}
	}
	return wd;
}

/**
 * Calculates defense based on src and target
 * @param src: Source object
 * @param target: Target object
 * @param skill_id: Skill used
 * @param flag: 0 - Return armor defense, 1 - Return status defense
 * @return defense
 */
short battle_get_defense(struct block_list *src, struct block_list *target, uint16 skill_id, uint8 flag)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct map_session_data *tsd = BL_CAST(BL_PC, target);
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);

	//Don't use tstatus->def1 due to skill timer reductions
	defType def1 = status_get_def(target); //eDEF
	short def2 = tstatus->def2, vit_def; //sDEF

	def1 = status_calc_def(target, tsc, def1, false);
	def2 = status_calc_def2(target, tsc, def2, false);

	if(sd) {
		int val = sd->ignore_def_by_race[tstatus->race] + sd->ignore_def_by_race[RC_ALL];

		if(sd->spiritcharm_type == CHARM_TYPE_LAND && sd->spiritcharm > 0) {
			short i = 10 * sd->spiritcharm; //KO Earth Charm effect +10% eDEF

			def1 = def1 * (100 + i) / 100;
		}
		val += sd->ignore_def_by_class[tstatus->class_] + sd->ignore_def_by_class[CLASS_ALL];
		if(val) {
			val = min(val, 100); //Cap it to 100 for 0 DEF min
			def1 = def1 * (100 - val) / 100;
#ifndef RENEWAL
			def2 = def2 * (100 - val) / 100;
#endif
		}
	}
	if(sc && sc->data[SC_EXPIATIO]) {
		short i = 5 * sc->data[SC_EXPIATIO]->val1; //5% per level

		i = min(i, 100);
		def1 = def1 * (100 - i) / 100;
#ifndef RENEWAL
		def2 = def2 * (100 - i) / 100;
#endif
	}
	if(battle_config.vit_penalty_type && battle_config.vit_penalty_target&target->type) {
		unsigned char target_count; //256 max targets should be a sane max

		//Official servers limit the count to 22 targets
		target_count = min(unit_counttargeted(target), (100 / battle_config.vit_penalty_num) + (battle_config.vit_penalty_count - 1));
		if(target_count >= battle_config.vit_penalty_count) {
			if(battle_config.vit_penalty_type == 1) {
				if(!tsc || !tsc->data[SC_STEELBODY])
					def1 = def1 * (100 - (target_count - (battle_config.vit_penalty_count - 1)) * battle_config.vit_penalty_num) / 100;
				def2 = def2 * (100 - (target_count - (battle_config.vit_penalty_count - 1)) * battle_config.vit_penalty_num) / 100;
			} else { //Assume type 2
				if(!tsc || !tsc->data[SC_STEELBODY])
					def1 -= (target_count - (battle_config.vit_penalty_count - 1)) * battle_config.vit_penalty_num;
				def2 -= (target_count - (battle_config.vit_penalty_count - 1)) * battle_config.vit_penalty_num;
			}
		}
#ifndef RENEWAL
		if(skill_id == AM_ACIDTERROR)
			def1 = 0; //Ignores eDEF [Skotlex]
#endif
		def2 = max(def2,1);
	}
	//Vitality reduction
	//[rodatazone: http://rodatazone.simgaming.net/mechanics/substats.php#def]
	if(tsd) { //sd vit-eq
		uint16 lv;

#ifndef RENEWAL
		//[VIT*0.5] + rnd([VIT*0.3], max([VIT*0.3], [VIT^2/150]-1))
		vit_def = def2 * (def2 - 15) / 150;
		vit_def = def2 / 2 + (vit_def > 0 ? rnd()%vit_def : 0);
#else
		vit_def = def2;
#endif
		//This bonus already doesn't work vs players
		if(src->type == BL_MOB && (battle_check_undead(sstatus->race, sstatus->def_ele) || sstatus->race == RC_DEMON) &&
			(lv = pc_checkskill(tsd, AL_DP)) > 0)
			vit_def += lv * (int)(3 + ((float)((tsd->status.base_level + 1) * 4) / 100)); //[orn]
		if(src->type == BL_MOB && (lv = pc_checkskill(tsd, RA_RANGERMAIN)) > 0 &&
			(sstatus->race == RC_BRUTE || sstatus->race == RC_FISH || sstatus->race == RC_PLANT))
			vit_def += lv * 5;
		if(src->type == BL_MOB && (lv = pc_checkskill(tsd, NC_RESEARCHFE)) > 0 &&
			(sstatus->def_ele == ELE_FIRE || sstatus->def_ele == ELE_EARTH))
			vit_def += lv * 10;
		if(src->type == BL_MOB && tsc && tsc->count && tsc->data[SC_P_ALTER] && //If the Platinum Alter is activated
			(battle_check_undead(sstatus->race, sstatus->def_ele) || sstatus->race == RC_UNDEAD)) //Undead attacker
			vit_def += tsc->data[SC_P_ALTER]->val3;
	} else { //Mob-Pet vit-eq
#ifndef RENEWAL
		//VIT + rnd(0, [VIT / 20] ^ 2 - 1)
		vit_def = (def2 / 20) * (def2 / 20);
		if(tsc && tsc->data[SC_SKA])
			vit_def += 100; //Eska increases the random part of the formula by 100
		vit_def = def2 + (vit_def > 0 ? rnd()%vit_def : 0);
#else
		//SoftDEF of monsters is floor((BaseLevel+Vit)/2)
		vit_def = def2;
#endif
	}
	if(battle_config.weapon_defense_type) {
		vit_def += def1 * battle_config.weapon_defense_type;
		def1 = 0;
	}

	return (flag ? vit_def : (short)def1);
}

/**
 * Calculate defense damage reduction
 * @param wd: Weapon data
 * @param src: Source object
 * @param target: Target object
 * @param skill_id: Skill used
 * @param skill_lv: Skill level used
 * @return weapon data
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_defense_reduction(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	short def1 = battle_get_defense(src, target, skill_id, 0);
	short vit_def = battle_get_defense(src, target, skill_id, 1);

	if(attack_ignores_def(wd, src, target, skill_id, skill_lv, EQI_HAND_R) || attack_ignores_def(wd, src, target, skill_id, skill_lv, EQI_HAND_L))
		return wd;
#ifdef RENEWAL
	if(is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_R) || is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_L))
		return wd;
	switch(skill_id) {
		case MO_EXTREMITYFIST:
		case NJ_KUNAI:
		case RK_DRAGONBREATH:
		case RK_DRAGONBREATH_WATER:
		case NC_SELFDESTRUCTION:
		case GN_CARTCANNON:
		case KO_HAPPOKUNAI:
			//Total defense reduction
			wd.damage -= (def1 + vit_def);
			if(is_attack_left_handed(src, skill_id))
				wd.damage2 -= (def1 + vit_def);
			break;
		default:
			/**
			 * RE DEF Reduction
			 * Damage = Attack * (4000 + eDEF) / (4000 + eDEF * 10) - sDEF
			 */
			if(def1 < -399) //It stops at -399
				def1 = 399; //In aegis it set to 1 but in our case it may lead to exploitation so limit it to 399
			wd.damage = wd.damage * (4000 + def1) / (4000 + 10 * def1) - vit_def;
			if(is_attack_left_handed(src, skill_id))
				wd.damage2 = wd.damage2 * (4000 + def1) / (4000 + 10 * def1) - vit_def;
			break;
	}
#else
	if(def1 > 100)
		def1 = 100;
	ATK_RATE2(wd.damage, wd.damage2,
		(is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_R) ? (def1 + vit_def) : (100 - def1)),
		(is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_L) ? (def1 + vit_def) : (100 - def1))
	);
	ATK_ADD2(wd.damage, wd.damage2,
		(is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_R) ? 0 : -vit_def),
		(is_attack_piercing(wd, src, target, skill_id, skill_lv, EQI_HAND_L) ? 0 : -vit_def)
	);
#endif

	return wd;
}

/*====================================
 * Modifiers ignoring DEF
 *------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_attack_post_defense(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_data *sstatus = status_get_status_data(src);

#ifndef RENEWAL
	wd = battle_calc_attack_masteries(wd, src, target, skill_id, skill_lv);
	//Refine bonus
	if(sd && battle_skill_stacks_masteries_vvs(skill_id)) {
		if(skill_id == MO_FINGEROFFENSIVE) { //Counts refine bonus multiple times
			ATK_ADD2(wd.damage, wd.damage2, wd.div_ * sstatus->rhw.atk2, wd.div_ * sstatus->lhw.atk2);
		} else
			ATK_ADD2(wd.damage, wd.damage2, sstatus->rhw.atk2, sstatus->lhw.atk2);
	}
#endif
	//Set to min of 1
	if(is_attack_right_handed(src, skill_id) && wd.damage < 1)
		wd.damage = 1;
	if(is_attack_left_handed(src, skill_id) && wd.damage2 < 1)
		wd.damage2 = 1;
	switch(skill_id) {
		case AS_SONICBLOW:
			if(sd && pc_checkskill(sd, AS_SONICACCEL) > 0)
				ATK_ADDRATE(wd.damage, wd.damage2, 10);
			break;
		case NC_AXETORNADO:
			if(sstatus->rhw.ele == ELE_WIND)
				ATK_ADDRATE(wd.damage, wd.damage2, 25);
			break;
	}

	return wd;
}

/*=================================================================================
 * "Plant"-type (mobs that only take 1 damage from all sources) damage calculation
 *---------------------------------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_attack_plant(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct status_change *sc = status_get_sc(src);
	bool attack_hits = is_attack_hitting(wd, src, target, skill_id, skill_lv, false);

	//Plants receive 1 damage when hit
	if(attack_hits || wd.damage > 0) //In some cases, right hand no need to have a weapon to deal a damage
		wd.damage = 1;
	if((attack_hits || wd.damage2 > 0) && is_attack_left_handed(src, skill_id)) {
		wd.damage2 = 0; //No back hand damage on plant unless dual wielding
		if(is_attack_right_handed(src, skill_id) && sd->status.weapon != W_KATAR) {
			wd.damage2 = 1; //Give a damage on left hand while dual wielding, katar weapon type not included
			wd.miscflag |= 1;
		}
	}
	wd.damage = battle_calc_damage(src, target, &wd, wd.damage, skill_id, skill_lv);
	if(map_flag_gvg2(target->m))
		wd.damage = battle_calc_gvg_damage(src, target, wd.damage, skill_id, wd.flag);
#ifndef RENEWAL
	if(skill_id == NJ_ISSEN)
		wd.dmg_lv = ATK_FLEE;
#endif
	if(sc && sc->data[SC_CAMOUFLAGE] && !skill_id)
		status_change_end(src, SC_CAMOUFLAGE, INVALID_TIMER);

	return wd;
}

/*========================================================================================
 * Perform left/right hand weapon damage calculation based on previously calculated damage
 *----------------------------------------------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_attack_left_right_hands(struct Damage wd, struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);

	if(sd) {
		uint16 lv;

		if(!is_attack_right_handed(src, skill_id) && is_attack_left_handed(src, skill_id)) {
			wd.damage = wd.damage2;
			wd.damage2 = 0;
		} else if(sd->status.weapon == W_KATAR && !skill_id) { //Katar (off hand damage only applies to normal attacks, tested on Aegis 10.2)
			lv = pc_checkskill(sd, TF_DOUBLE);
			wd.damage2 = wd.damage * (1 + lv * 2) / 100;
		} else if(is_attack_right_handed(src, skill_id) && is_attack_left_handed(src, skill_id)) { //Dual-wield
			if(wd.damage > 0) {
				if((sd->class_&MAPID_BASEMASK) == MAPID_THIEF) {
					lv = pc_checkskill(sd,AS_RIGHT);
					ATK_RATER(wd.damage, 50 + lv * 10);
				} else if((sd->class_&MAPID_UPPERMASK) == MAPID_KAGEROUOBORO) {
					lv = pc_checkskill(sd,KO_RIGHT);
					ATK_RATER(wd.damage, 70 + lv * 10);
				}
				wd.damage = max(wd.damage, 1);
			}
			if(wd.damage2 > 0) {
				if((sd->class_&MAPID_BASEMASK) == MAPID_THIEF) {
					lv = pc_checkskill(sd,AS_LEFT);
					ATK_RATEL(wd.damage2, 30 + lv * 10);
				} else if((sd->class_&MAPID_UPPERMASK) == MAPID_KAGEROUOBORO) {
					lv = pc_checkskill(sd,KO_LEFT);
					ATK_RATEL(wd.damage2, 50 + lv * 10);
				}
				wd.damage2 = max(wd.damage2, 1);
			}
		}
	}

	if(!is_attack_right_handed(src, skill_id) && !is_attack_left_handed(src, skill_id) && wd.damage > 0)
		wd.damage = 0;

	if(!is_attack_left_handed(src, skill_id) && wd.damage2 > 0)
		wd.damage2 = 0;

	return wd;
}

/**
 * Check if bl is devoted by someone
 * @param bl
 * @return 'd_bl' if devoted or NULL if not devoted
 */
struct block_list *battle_check_devotion(struct block_list *bl) {
	struct block_list *d_bl = NULL;

	if(battle_config.devotion_rdamage && battle_config.devotion_rdamage > rnd()%100) {
		struct status_change *sc = status_get_sc(bl);

		if(sc && sc->data[SC_DEVOTION])
			d_bl = map_id2bl(sc->data[SC_DEVOTION]->val1);
	}
	return d_bl;
}

/*==========================================
 * BG/GvG attack modifiers
 *------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_attack_gvg_bg(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	if( wd.damage + wd.damage2 ) { //There is a total damage value
		if( target->id != src->id && //Don't reflect your own damage (Grand Cross)
			(!skill_id || skill_id ||
			(src->type == BL_SKILL && (skill_id == SG_SUN_WARM || skill_id == SG_MOON_WARM || skill_id == SG_STAR_WARM))) ) {
				int64 damage = wd.damage + wd.damage2, rdamage = 0;
				struct map_session_data *tsd = BL_CAST(BL_PC, target);
				struct status_data *sstatus = status_get_status_data(src);
				int tick = gettick(), rdelay = 0;

				rdamage = battle_calc_return_damage(target, src, &damage, wd.flag, skill_id, false);
				if( rdamage > 0 ) { //Item reflect gets calculated before any mapflag reducing is applicated
					struct block_list *d_bl = battle_check_devotion(src);

					rdelay = clif_damage(src, (!d_bl ? src : d_bl), tick, wd.amotion, sstatus->dmotion, rdamage, 1, DMG_ENDURE, 0, false);
					if( tsd )
						battle_drain(tsd, src, rdamage, rdamage, sstatus->race, sstatus->class_);
					//Use Reflect Shield to signal this kind of skill trigger [Skotlex]
					battle_delay_damage(tick, wd.amotion, target, (!d_bl ? src : d_bl), 0, CR_REFLECTSHIELD, 0, rdamage, ATK_DEF, rdelay, true, false);
					skill_additional_effect(target, (!d_bl ? src : d_bl), CR_REFLECTSHIELD, 1, BF_WEAPON|BF_SHORT|BF_NORMAL, ATK_DEF, tick);
				}
		}
		if( !wd.damage2 ) {
			wd.damage = battle_calc_damage(src, target, &wd, wd.damage, skill_id, skill_lv);
			if( map_flag_gvg2(target->m) )
				wd.damage = battle_calc_gvg_damage(src, target, wd.damage, skill_id, wd.flag);
			else if( map[target->m].flag.battleground )
				wd.damage = battle_calc_bg_damage(src, target, wd.damage, skill_id, wd.flag);
		} else if( !wd.damage ) {
			wd.damage2 = battle_calc_damage(src, target, &wd, wd.damage2, skill_id, skill_lv);
			if( map_flag_gvg2(target->m) )
				wd.damage2 = battle_calc_gvg_damage(src, target, wd.damage2, skill_id, wd.flag);
			else if( map[target->m].flag.battleground )
				wd.damage2 = battle_calc_bg_damage(src, target, wd.damage2, skill_id, wd.flag);
		} else {
			int64 d1 = wd.damage + wd.damage2, d2 = wd.damage2;

			wd.damage = battle_calc_damage(src, target, &wd, d1, skill_id, skill_lv);
			if( map_flag_gvg2(target->m) )
				wd.damage = battle_calc_gvg_damage(src, target, wd.damage, skill_id, wd.flag);
			else if( map[target->m].flag.battleground )
				wd.damage = battle_calc_bg_damage(src, target, wd.damage, skill_id, wd.flag);
			wd.damage2 = d2 * 100 / d1 * wd.damage / 100;
			if( wd.damage > 1 && wd.damage2 < 1 )
				wd.damage2 = 1;
			wd.damage -= wd.damage2;
		}
	}
	return wd;
}

/*==========================================
 * Final ATK modifiers - After BG/GvG calc
 *------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_weapon_final_atk_modifiers(struct Damage wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct map_session_data *tsd = BL_CAST(BL_PC, target);
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct status_data *sstatus = status_get_status_data(src);
	struct status_change_entry *sce;
#ifdef ADJUST_SKILL_DAMAGE
	int skill_damage = 0;
#endif

	if(sc) {
		if(sc->data[SC_FUSION]) { //SC_FUSION HP penalty [Komurka]
			int hp = sstatus->max_hp;

			if(sd && tsd) {
				hp = 8 * hp / 100;
				if(sstatus->hp * 100 <= sstatus->max_hp * 20)
					hp = sstatus->hp;
			} else
				hp = 2 * hp / 100; //2% HP loss per hit
			status_zap(src, hp, 0);
		}
		if(sc->data[SC_CAMOUFLAGE] && !skill_id)
			status_change_end(src, SC_CAMOUFLAGE, INVALID_TIMER);
	}

	if(tsc && wd.damage) {
		if((sce = tsc->data[SC_REJECTSWORD]) && (src->type != BL_PC ||
			(((TBL_PC *)src)->weapontype1 == W_DAGGER ||
			((TBL_PC *)src)->weapontype1 == W_1HSWORD ||
			((TBL_PC *)src)->status.weapon == W_2HSWORD)) && rnd()%100 < sce->val2)
		{ //Reject Sword bugreport:4493 by Daegaladh
			ATK_RATER(wd.damage, 50);
			status_fix_damage(target, src, wd.damage, clif_damage(target, src, gettick(), 0, 0, wd.damage, 0, DMG_NORMAL, 0, false));
			if(--(sce->val3) <= 0)
				status_change_end(target, SC_REJECTSWORD, INVALID_TIMER);
		}
		if(wd.flag&BF_SHORT && !is_boss(src)) {
			if((sce = tsc->data[SC_DEATHBOUND])) {
				uint8 dir = map_calc_dir(target, src->x, src->y), t_dir = unit_getdir(target);
				int64 rdamage = 0;

				if(distance_bl(src, target) <= 0 || !map_check_dir(dir, t_dir)) {
					rdamage = min(wd.damage, status_get_max_hp(target)) * sce->val2 / 100; //Amplify damage
					wd.damage = rdamage * 30 / 100; //Player receives 30% of the amplified damage
					rdamage = rdamage * 70 / 100; //Target receives 70% of the amplified damage [Rytech]
					clif_skill_damage(target, src, gettick(), status_get_amotion(src), 0, -30000, 1, RK_DEATHBOUND, -1, DMG_SKILL);
					skill_blown(target, src, skill_get_blewcount(RK_DEATHBOUND, sce->val1), unit_getdir(src), 0);
					status_fix_damage(target, src, rdamage, clif_damage(target, src, gettick(), 0, 0, rdamage, 0, DMG_NORMAL, 0, false));
					status_change_end(target, SC_DEATHBOUND, INVALID_TIMER);
					status_change_end(target, SC_TELEPORT_FIXEDCASTINGDELAY, INVALID_TIMER);
				}
			}
			if((sce = tsc->data[SC_CRESCENTELBOW]) && rnd()%100 < sce->val2) {
				battle_damage_temp[0] = wd.damage; //Will be used for bonus part formula [exneval]
				clif_skill_nodamage(target, src, SR_CRESCENTELBOW_AUTOSPELL, sce->val1, 1);
				skill_attack(BF_WEAPON, target, target, src, SR_CRESCENTELBOW_AUTOSPELL, sce->val1, gettick(), 0);
				ATK_ADD(wd.damage, wd.damage2, battle_damage_temp[1] / 10);
				status_change_end(target, SC_CRESCENTELBOW, INVALID_TIMER);
			}
		}
	}

	switch(skill_id) {
#ifndef RENEWAL
		case ASC_BREAKER: { //Breaker int-based damage
				struct Damage md = battle_calc_misc_attack(src, target, skill_id, skill_lv, wd.miscflag);

				wd.damage += md.damage;
			}
			break;
#endif
		case LG_RAYOFGENESIS: {
				struct Damage ad = battle_calc_magic_attack(src, target, skill_id, skill_lv, wd.miscflag);

				wd.damage += ad.damage;
			}
			break;
	}

	//Skill damage adjustment
#ifdef ADJUST_SKILL_DAMAGE
	if((skill_damage = battle_skill_damage(src, target, skill_id)))
		ATK_ADDRATE(wd.damage, wd.damage2, skill_damage);
#endif
	return wd;
}

/*====================================================
 * Basic wd init - not influenced by HIT/MISS/DEF/etc.
 *----------------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
static struct Damage initialize_weapon_data(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, int wflag)
{
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct Damage wd;

	wd.type = DMG_NORMAL; //Normal attack
	wd.div_ = (skill_id ? skill_get_num(skill_id, skill_lv) : 1);
	wd.amotion = (skill_id && skill_get_inf(skill_id)&INF_GROUND_SKILL) ? 0 : sstatus->amotion; //Amotion should be 0 for ground skills
	//if(skill_id == KN_AUTOCOUNTER)
		//wd.amotion >>= 1; //Counter attack DOES obey ASPD delay on official, uncomment if you want the old (bad) behavior [helvetica]
	wd.dmotion = tstatus->dmotion;
	wd.blewcount = skill_get_blewcount(skill_id, skill_lv);
	wd.miscflag = wflag;
	wd.flag = BF_WEAPON; //Initial Flag
	wd.flag |= (skill_id || wd.miscflag) ? BF_SKILL : BF_NORMAL; //Baphomet card's splash damage is counted as a skill [Inkfish]
	wd.isvanishdamage = false;
	wd.isspdamage = false;
	wd.damage = wd.damage2 = 
#ifdef RENEWAL	
	wd.statusAtk = wd.statusAtk2 = wd.equipAtk = wd.equipAtk2 = wd.weaponAtk = wd.weaponAtk2 = wd.masteryAtk = wd.masteryAtk2 =
#endif
	0;
	wd.dmg_lv = ATK_DEF; //This assumption simplifies the assignation later

	if(sd)
		wd.blewcount += battle_blewcount_bonus(sd, skill_id);

	if(skill_id) {
		wd.flag |= battle_range_type(src, target, skill_id, skill_lv);
		switch(skill_id) {
			case TF_DOUBLE: //For NPC used skill
			case GS_CHAINACTION:
				wd.type = DMG_MULTI_HIT;
				break;
			case GS_GROUNDDRIFT:
				wd.amotion = sstatus->amotion;
			//Fall through
			case KN_SPEARSTAB:
			case KN_BOWLINGBASH:
			case MS_BOWLINGBASH:
			case MO_BALKYOUNG:
			case TK_TURNKICK:
				wd.blewcount = 0;
				break;
			case KN_PIERCE:
			case ML_PIERCE:
				wd.div_ = (wd.div_ > 0 ? tstatus->size + 1 : -(tstatus->size + 1));
				break;
			case KN_AUTOCOUNTER:
				wd.flag = (wd.flag&~BF_SKILLMASK)|BF_NORMAL;
				break;
			case MO_FINGEROFFENSIVE:
				if(sd)
					wd.div_ = (battle_config.finger_offensive_type ? 1 : max(sd->spiritball_old,1));
				break;
			case LK_SPIRALPIERCE:
				if(!sd)
					wd.flag = ((wd.flag&~(BF_RANGEMASK|BF_WEAPONMASK))|BF_LONG|BF_MISC);
				break;
			case LG_HESPERUSLIT: {
					struct status_change *sc = status_get_sc(src);

					//The number of hits is set to 3 by default for use in Inspiration status
					//When in banding, the number of hits is equal to the number of Royal Guards in banding
					if(sc && sc->data[SC_BANDING] && sc->data[SC_BANDING]->val2 > 3)
						wd.div_ = sc->data[SC_BANDING]->val2;
				}
				break;
			case RL_R_TRIP: //Knock's back target out of skill range
				wd.blewcount -= distance_bl(src, target);
				break;
			case MH_SONIC_CRAW: {
					TBL_HOM *hd = BL_CAST(BL_HOM, src);

					wd.div_ = (hd ? hd->homunculus.spiritball : skill_get_maxcount(skill_id, skill_lv));
				}
				break;
			case EL_STONE_RAIN:
				if(!(wd.miscflag&1))
					wd.div_ = 1;
				break;
		}
	} else
		wd.flag |= (is_skill_using_arrow(src, skill_id) ? BF_LONG : BF_SHORT);
	return wd;
}

/**
 * Check if we should reflect the damage and calculate it if so
 * @param attack_type : BL_WEAPON, BL_MAGIC or BL_MISC
 * @param wd : weapon damage
 * @param src : bl who did the attack
 * @param target : target of the attack
 * @param skill_id : id of casted skill, 0 = basic atk
 * @param skill_lv : lvl of skill casted
 */
void battle_do_reflect(int attack_type, struct Damage *wd, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	//Don't reflect your own damage (Grand Cross)
	if(wd->damage + wd->damage2 > 0 && src && target && target->id != src->id && (src->type != BL_SKILL ||
		(src->type == BL_SKILL && (skill_id == SG_SUN_WARM || skill_id == SG_MOON_WARM || skill_id == SG_STAR_WARM))))
	{
		int64 damage = wd->damage + wd->damage2, rdamage = 0;
		struct map_session_data *tsd = BL_CAST(BL_PC, target);
		struct status_change *tsc = status_get_sc(target);
		struct status_data *sstatus = status_get_status_data(src);
		struct status_data *tstatus = status_get_status_data(target);
		struct block_list *tbl = target;
		int tick = gettick(), rdelay = 0;

		if(!tsc)
			return;
		if(tsc->data[SC__SHADOWFORM] && !battle_check_devotion(target)) {
			struct map_session_data *s_tsd = map_id2sd(tsc->data[SC__SHADOWFORM]->val2);

			if(battle_check_shadowform(target,SC_REFLECTDAMAGE) && s_tsd && s_tsd->shadowform_id == target->id)
				target = &s_tsd->bl;
		}
		rdamage = battle_calc_return_damage(target, src, &damage, wd->flag, skill_id, true);
		if(rdamage > 0) {
			struct block_list *d_bl = battle_check_devotion(src);

			if(attack_type == BF_WEAPON || attack_type == BF_MISC) {
				if(tsc->data[SC_REFLECTDAMAGE] || battle_check_shadowform(tbl,SC_REFLECTDAMAGE))
					map_foreachinshootrange(battle_damage_area, target, skill_get_splash(LG_REFLECTDAMAGE, 1), BL_CHAR, tick, target, wd->amotion, sstatus->dmotion, rdamage, tstatus->race);
				else {
					rdelay = clif_damage(src, (!d_bl) ? src : d_bl, tick, wd->amotion, sstatus->dmotion, rdamage, 1, DMG_ENDURE, 0, false);
					if(tsd)
						battle_drain(tsd, src, rdamage, rdamage, sstatus->race, sstatus->class_);
					//It appears that official servers give skill reflect damage a longer delay
					battle_delay_damage(tick, wd->amotion, target, (!d_bl) ? src : d_bl, 0, CR_REFLECTSHIELD, 0, rdamage, ATK_DEF, rdelay, true, false);
					skill_additional_effect(target, (!d_bl) ? src : d_bl, CR_REFLECTSHIELD, 1, BF_WEAPON|BF_SHORT|BF_NORMAL, ATK_DEF, tick);
				}
			}
		}
	}
}

/*============================================
 * Calculate "weapon"-type attacks and skills
 *--------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_weapon_attack(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, int wflag)
{
	struct map_session_data *sd, *tsd;
	struct Damage wd;
	struct status_change *sc, *tsc;
	struct status_data *tstatus;
	int right_element, left_element, nk;
	uint16 id;
	uint16 lv;
	int i;

	memset(&wd, 0, sizeof(wd));

	if(!src || !target) {
		nullpo_info(NLP_MARK);
		return wd;
	}

	sc = status_get_sc(src);
	tsc = status_get_sc(target);
	tstatus = status_get_status_data(target);

	wd = initialize_weapon_data(src, target, skill_id, skill_lv, wflag);

	right_element = battle_get_weapon_element(wd, src, target, skill_id, skill_lv, EQI_HAND_R);
	left_element = battle_get_weapon_element(wd, src, target, skill_id, skill_lv, EQI_HAND_L);

	nk = battle_skill_get_damage_properties(skill_id, wd.miscflag);

	if(sc && !sc->count)
		sc = NULL; //Skip checking as there are no status changes active

	if(tsc && !tsc->count)
		tsc = NULL; //Skip checking as there are no status changes active

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);

	//Check for Lucky Dodge
	if((!skill_id || skill_id == PA_SACRIFICE) && tstatus->flee2 && rnd()%1000 < tstatus->flee2) {
		wd.type = DMG_LUCY_DODGE;
		wd.dmg_lv = ATK_LUCKY;
		if(wd.div_ < 0)
			wd.div_ *= -1;
		return wd;
	}

	//On official check for multi hit first so we can override crit on double attack [helvetica]
	wd = battle_calc_multi_attack(wd, src, target, skill_id, skill_lv);

	//Crit check is next since crits always hit on official [helvetica]
	if(is_attack_critical(wd, src, target, skill_id, skill_lv, true))
		wd.type = DMG_CRITICAL;

	//Check if we're landing a hit
	if(!is_attack_hitting(wd, src, target, skill_id, skill_lv, true))
		wd.dmg_lv = ATK_FLEE;
	else if(!target_has_infinite_defense(target, skill_id, wd.flag)) { //No need for math against plants
		int ratio = 0;
		int64 const_val = 0;

		wd = battle_calc_skill_base_damage(wd, src, target, skill_id, skill_lv); //Base skill damage

#ifdef RENEWAL
		if(sd) { //Card Fix for attacker (sd), 2 is added to the "left" flag meaning "attacker cards only"
			wd.weaponAtk += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.weaponAtk, 2, wd.flag);
			wd.equipAtk += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.equipAtk, 2, wd.flag);
			if(is_attack_left_handed(src, skill_id)) {
				wd.weaponAtk2 += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.weaponAtk2, 3, wd.flag);
				wd.equipAtk2 += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.equipAtk2, 3, wd.flag);
			}
		}

		//Final attack bonuses that aren't affected by cards
		wd = battle_attack_sc_bonus(wd, src, target, skill_id, skill_lv);

		if(tsd) { //Card Fix for target (tsd), 2 is not added to the "left" flag meaning "target cards only"
			switch(skill_id) {
				case AM_DEMONSTRATION:
				case AM_ACIDTERROR:
				case NJ_ISSEN:
				case GS_MAGICALBULLET:
				case HW_MAGICCRASHER:
				case ASC_BREAKER:
				case CR_ACIDDEMONSTRATION:
				case GN_FIRE_EXPANSION_ACID:
				case NPC_EARTHQUAKE:
				case SO_VARETYR_SPEAR:
					break; //Do card fix later
				default:
					if(sd) {
						wd.statusAtk += battle_calc_cardfix(BF_WEAPON, src, target, nk|NK_NO_ELEFIX, right_element, left_element, wd.statusAtk, 0, wd.flag);
						wd.weaponAtk += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.weaponAtk, 0, wd.flag);
						wd.equipAtk += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.equipAtk, 0, wd.flag);
						wd.masteryAtk += battle_calc_cardfix(BF_WEAPON, src, target, nk|NK_NO_ELEFIX, right_element, left_element, wd.masteryAtk, 0, wd.flag);
						if(is_attack_left_handed(src, skill_id)) {
							wd.statusAtk2 += battle_calc_cardfix(BF_WEAPON, src, target, nk|NK_NO_ELEFIX, right_element, left_element, wd.statusAtk2, 1, wd.flag);
							wd.weaponAtk2 += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.weaponAtk2, 1, wd.flag);
							wd.equipAtk2 += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.equipAtk2, 1, wd.flag);
							wd.masteryAtk2 += battle_calc_cardfix(BF_WEAPON, src, target, nk|NK_NO_ELEFIX, right_element, left_element, wd.masteryAtk2, 1, wd.flag);
						}
					}
					break;
			}
		}

		if(sd) { //Monsters, homuns and pets have their damage computed directly
			wd.damage = wd.statusAtk + wd.weaponAtk + wd.equipAtk + wd.masteryAtk;
			wd.damage2 = wd.statusAtk2 + wd.weaponAtk2 + wd.equipAtk2 + wd.masteryAtk2;
			if(wd.flag&BF_LONG) { //Affects the entirety of the damage
				switch(skill_id) {
					case RA_WUGSTRIKE:
					case RA_WUGBITE:
						break; //Ignore % modifiers
					default:
						ATK_ADDRATE(wd.damage, wd.damage2, sd->bonus.long_attack_atk_rate);
						break;
				}
			}
		}
#else
		wd = battle_attack_sc_bonus(wd, src, target, skill_id, skill_lv);
#endif

		ratio = battle_calc_attack_skill_ratio(wd, src, target, skill_id, skill_lv); //Skill level ratio
		ATK_RATE(wd.damage, wd.damage2, ratio);

		const_val = battle_calc_skill_constant_addition(wd, src, target, skill_id, skill_lv); //Other skill bonuses
		ATK_ADD(wd.damage, wd.damage2, const_val);

		switch(skill_id) {
			case AB_DUPLELIGHT_MELEE:
				id = AB_DUPLELIGHT;
				break;
			case NC_MAGMA_ERUPTION_DOTDAMAGE:
				id = NC_MAGMA_ERUPTION;
				break;
			case LG_OVERBRAND_BRANDISH:
			case LG_OVERBRAND_PLUSATK:
				id = LG_OVERBRAND;
				break;
			case WM_SEVERE_RAINSTORM_MELEE:
				id = WM_SEVERE_RAINSTORM;
				break;
			case WM_REVERBERATION_MELEE:
				id = WM_REVERBERATION;
				break;
			case GN_CRAZYWEED_ATK:
				id = GN_CRAZYWEED;
				break;
			case GN_SLINGITEM_RANGEMELEEATK:
				id = GN_SLINGITEM;
				break;
			case RL_R_TRIP_PLUSATK:
				id = RL_R_TRIP;
				break;
			case RL_B_FLICKER_ATK:
				id = RL_FLICKER;
				break;
			case RL_GLITTERING_GREED_ATK:
				id = RL_GLITTERING_GREED;
				break;
			case SU_PICKYPECK_DOUBLE_ATK:
				id = SU_PICKYPECK;
				break;
			default:
				id = skill_id;
				break;
		}

		//Add any miscellaneous player skill ATK rate bonuses
		if(sd) {
			if((i = pc_skillatk_bonus(sd, id)))
				ATK_ADDRATE(wd.damage, wd.damage2, i);
			if((i = battle_adjust_skill_damage(src->m, id)))
				ATK_RATE(wd.damage, wd.damage2, i);
		}
		if(tsd && (i = pc_sub_skillatk_bonus(tsd, id)))
			ATK_ADDRATE(wd.damage, wd.damage2, -i);
	} else if(wd.div_ < 0)
		wd.div_ *= -1;

#ifdef RENEWAL //Critical hit ignores flee but not perfect dodge nor defense [exneval]
	if(is_attack_critical(wd, src, target, skill_id, skill_lv, false)) {
		if(sd) { //Check for player so we don't crash out, monsters don't have bonus crit rates [helvetica]
			ATK_ADDRATE(wd.damage, wd.damage2, 40 + sd->bonus.crit_atk_rate);
		} else
			ATK_ADDRATE(wd.damage, wd.damage2, 40);
	}
#endif

	if(wd.damage + wd.damage2 > 0) { //Check if attack ignores DEF
		if(!target_has_infinite_defense(target, skill_id, wd.flag))
			wd = battle_calc_defense_reduction(wd, src, target, skill_id, skill_lv);
		if(wd.dmg_lv != ATK_FLEE)
			wd = battle_calc_attack_post_defense(wd, src, target, skill_id, skill_lv);
	}

	//Damage disregard acurracy and defense check
	switch(skill_id) {
		case TK_DOWNKICK:
		case TK_STORMKICK:
		case TK_TURNKICK:
		case TK_COUNTER:
		case TK_JUMPKICK:
			if(sd && (lv = pc_checkskill(sd, TK_RUN)) > 0) {
				switch(lv) {
					case 1: case 4: case 7: case 10: i = 1; break;
					case 2: case 5: case 8: i = 2; break;
					default: i = 0; break;
				}
				ATK_ADD(wd.damage, wd.damage2, 10 * lv - i); //No miss damage (Kick skills)
			}
			break;
		case HW_MAGICCRASHER:
			if(sd) {
				short index = sd->equip_index[EQI_HAND_R];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON)
					ATK_ADD(wd.damage, wd.damage2, sd->status.inventory[index].refine);
			}
			break;
	}

#ifndef RENEWAL
	if(sd) {
		short div_ = max(wd.div_, 1);

		if((lv = pc_checkskill(sd, BS_WEAPONRESEARCH)) > 0)
			ATK_ADD(wd.damage, wd.damage2, lv * 2);
		if(skill_id != CR_SHIELDBOOMERANG) //Only Shield Boomerang doesn't takes the star crumb bonus
			ATK_ADD2(wd.damage, wd.damage2, div_ * sd->right_weapon.star, div_ * sd->left_weapon.star);
		if(skill_id == MO_FINGEROFFENSIVE) { //The finger offensive spheres on moment of attack do count [Skotlex]
			ATK_ADD(wd.damage, wd.damage2, div_ * sd->spiritball_old * 3);
		} else
			ATK_ADD(wd.damage, wd.damage2, div_ * sd->spiritball * 3);
	}
#else
	if(!sd) //Only monsters have a single ATK for element, in pre-renewal we also apply element to entire ATK on players [helvetica]
#endif
		wd = battle_calc_element_damage(wd, src, target, skill_id, skill_lv);

	if(skill_id == CR_GRANDCROSS || skill_id == NPC_GRANDDARKNESS)
		return wd; //Enough, rest is not needed

#ifndef RENEWAL
	if(sd) {
		switch(skill_id) {
			case RK_DRAGONBREATH:
			case RK_DRAGONBREATH_WATER:
				if(wd.flag&BF_LONG) { //Add check here, because we want to apply the same behavior in pre-renewal [exneval]
					wd.damage = wd.damage * (100 + sd->bonus.long_attack_atk_rate) / 100;
					if(is_attack_left_handed(src, skill_id))
						wd.damage2 = wd.damage2 * (100 + sd->bonus.long_attack_atk_rate) / 100;
				}
				break;
			default:
				wd.damage += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.damage, 2, wd.flag);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.damage2, 3, wd.flag);
				break;
		}
	}
#endif

	if(tsd) {
		switch(skill_id) {
			case NPC_EARTHQUAKE:
			case SO_VARETYR_SPEAR:
				break;
			default:
#ifdef RENEWAL
				if(sd)
					break;
#endif
				wd.damage += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.damage, 0, wd.flag);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 += battle_calc_cardfix(BF_WEAPON, src, target, nk, right_element, left_element, wd.damage2, 1, wd.flag);
				break;
		}
	}

#ifdef RENEWAL
	//Renewal elemental attribute fix [helvetica]
	//Skills gain benefits from the weapon element
	//But final damage is considered to "the forced" and resistances are applied again
	if(sd && !(nk&NK_NO_ELEFIX) && (wd.damage > 0 || wd.damage2 > 0)) {
		switch(skill_id) {
			case SR_TIGERCANNON:
				if(wd.miscflag&16)
					wd.damage = battle_damage_temp[0];
			//Fall through
			case MC_CARTREVOLUTION:
			case MO_INVESTIGATE:
			case AM_ACIDTERROR:
			case CR_SHIELDBOOMERANG:
			case PA_SHIELDCHAIN:
			case CR_ACIDDEMONSTRATION:
			case RA_CLUSTERBOMB:
			case NC_ARMSCANNON:
			case SR_CRESCENTELBOW_AUTOSPELL:
			case SR_GATEOFHELL:
			case GN_FIRE_EXPANSION_ACID:
			case KO_BAKURETSU:
				wd.damage = battle_attr_fix(src, target, wd.damage, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
				break;
			case AM_DEMONSTRATION:
			case RA_FIRINGTRAP:
				wd.damage = battle_attr_fix(src, target, wd.damage, ELE_FIRE, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, ELE_FIRE, tstatus->def_ele, tstatus->ele_lv);
				break;
			case RA_ICEBOUNDTRAP:
				wd.damage = battle_attr_fix(src, target, wd.damage, ELE_WATER, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, ELE_WATER, tstatus->def_ele, tstatus->ele_lv);
				break;
			case GN_CARTCANNON:
			case KO_HAPPOKUNAI:
				wd.damage = battle_attr_fix(src, target, wd.damage, (sd->bonus.arrow_ele ? sd->bonus.arrow_ele : ELE_NEUTRAL), tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, (sd->bonus.arrow_ele ? sd->bonus.arrow_ele : ELE_NEUTRAL), tstatus->def_ele, tstatus->ele_lv);
				break;
			case PA_SACRIFICE:
			case GS_GROUNDDRIFT:
			case RK_DRAGONBREATH:
			case RK_DRAGONBREATH_WATER:
			case NC_SELFDESTRUCTION:
				wd.damage = battle_attr_fix(src, target, wd.damage, right_element, tstatus->def_ele, tstatus->ele_lv);
				if(is_attack_left_handed(src, skill_id))
					wd.damage2 = battle_attr_fix(src, target, wd.damage2, left_element, tstatus->def_ele, tstatus->ele_lv);
				break;
		}
		if(sc && sc->data[SC_WATK_ELEMENT] && battle_skill_stacks_masteries_vvs(skill_id) && skill_id != ASC_METEORASSAULT) {
			int64 dmg, dmg2;

			dmg = wd.damage * sc->data[SC_WATK_ELEMENT]->val2 / 100;
			wd.damage += battle_attr_fix(src, target, dmg, sc->data[SC_WATK_ELEMENT]->val1, tstatus->def_ele, tstatus->ele_lv);
			if(is_attack_left_handed(src, skill_id)) {
				dmg2 = wd.damage2 * sc->data[SC_WATK_ELEMENT]->val2 / 100;
				wd.damage2 += battle_attr_fix(src, target, dmg2, sc->data[SC_WATK_ELEMENT]->val1, tstatus->def_ele, tstatus->ele_lv);
			}
		}
	}
#endif

	//Fixed damage and no elemental [exneval]
	switch(skill_id) {
		case CR_SHIELDBOOMERANG:
		case PA_SHIELDCHAIN:
			if(sd) {
				short index = sd->equip_index[EQI_HAND_L];

				if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
					ATK_ADD(wd.damage, wd.damage2, 10 * sd->status.inventory[index].refine);
			}
			break;
#ifndef RENEWAL
		case NJ_KUNAI:
			ATK_ADD(wd.damage, wd.damage2, 90);
			break;
#endif
		case SR_TIGERCANNON:
			if(wd.miscflag&16 && wd.damage)
				break;
			if(wd.miscflag&8) {
				ATK_ADD(wd.damage, wd.damage2, skill_lv * 500 + status_get_lv(target) * 40);
			} else
				ATK_ADD(wd.damage, wd.damage2, skill_lv * 240 + status_get_lv(target) * 40);
			break;
		case SR_CRESCENTELBOW_AUTOSPELL:
			//[Received damage x {1 + (Skill Level x 0.2)}]
			ATK_ADD(wd.damage, wd.damage2, battle_damage_temp[0] * (1 + skill_lv * 2 / 10));
			break;
		case SR_GATEOFHELL: {
				struct status_data *sstatus = status_get_status_data(src);

				ATK_ADD(wd.damage, wd.damage2, sstatus->max_hp - status_get_hp(src));
				if(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE) {
					ATK_ADD(wd.damage, wd.damage2, sstatus->max_sp * (1 + skill_lv * 2 / 10) + 40 * status_get_lv(src));
				} else
					ATK_ADD(wd.damage, wd.damage2, sstatus->sp * (1 + skill_lv * 2 / 10) + 10 * status_get_lv(src));
			}
			break;
	}

	//Perform multihit calculations
	DAMAGE_DIV_FIX(wd.damage, wd.div_);

	//Only do 1 dmg to plant, no need to calculate rest
	if(target_has_infinite_defense(target, skill_id, wd.flag))
		return battle_calc_attack_plant(wd, src, target, skill_id, skill_lv);

	wd = battle_calc_attack_left_right_hands(wd, src, target, skill_id, skill_lv);

	switch(skill_id) {
#ifdef RENEWAL
		case AM_DEMONSTRATION:
		case AM_ACIDTERROR:
		case NJ_ISSEN:
		case GS_MAGICALBULLET:
		case HW_MAGICCRASHER:
		case ASC_BREAKER:
		case CR_ACIDDEMONSTRATION:
		case GN_FIRE_EXPANSION_ACID:
#endif
		case NPC_EARTHQUAKE:
		case RA_CLUSTERBOMB:
		case RA_FIRINGTRAP:
 		case RA_ICEBOUNDTRAP:
		case SO_VARETYR_SPEAR:
			return wd; //Do GVG fix later
		default:
			if(sd && !skill_id)
				battle_vanish(sd, target, &wd);
			wd = battle_calc_attack_gvg_bg(wd, src, target, skill_id, skill_lv);
			break;
	}

	if(skill_id == SR_CRESCENTELBOW_AUTOSPELL)
		battle_damage_temp[1] = wd.damage; //Will be used for additional damage to the caster [exneval]

	wd = battle_calc_weapon_final_atk_modifiers(wd, src, target, skill_id, skill_lv);

	battle_absorb_damage(target, &wd);

	//Skill reflect gets calculated after all attack modifier
	battle_do_reflect(BF_WEAPON, &wd, src, target, skill_id, skill_lv); //WIP [lighta]

	return wd;
}

/*==========================================
 * Calculate "magic"-type attacks and skills
 *------------------------------------------
 * Credits:
 *	Original coder DracoRPG
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_magic_attack(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, int mflag)
{
	int i, nk;
#ifdef ADJUST_SKILL_DAMAGE
	int skill_damage = 0;
#endif
	short s_ele = 0;
	uint16 id;
	TBL_PC *sd;
	TBL_PC *tsd;
	struct status_change *sc, *tsc;
	struct Damage ad;
	struct status_data *sstatus, *tstatus;
	struct {
		unsigned imdef : 1;
		unsigned infdef : 1;
	} flag;

	memset(&ad, 0, sizeof(ad));
	memset(&flag, 0, sizeof(flag));

	if(!src || !target) {
		nullpo_info(NLP_MARK);
		return ad;
	}

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(target);

	//Initial Values
	ad.damage = 1;
	ad.div_ = skill_get_num(skill_id, skill_lv);
	//Amotion should be 0 for ground skills.
	ad.amotion = (skill_get_inf(skill_id)&INF_GROUND_SKILL ? 0 : sstatus->amotion);
	ad.dmotion = tstatus->dmotion;
	ad.blewcount = skill_get_blewcount(skill_id, skill_lv);
	ad.miscflag = mflag;
	ad.flag = BF_MAGIC|BF_SKILL;
	ad.dmg_lv = ATK_DEF;
	nk = skill_get_nk(skill_id);
	flag.imdef = (nk&NK_IGNORE_DEF ? 1 : 0);

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);
	sc = status_get_sc(src);
	tsc = status_get_sc(target);

	//Initialize variables that will be used afterwards
	s_ele = skill_get_ele(skill_id, skill_lv);

	if(s_ele == -1) { //Skill takes the weapon's element
		s_ele = sstatus->rhw.ele;
		if(sd && sd->spiritcharm_type != CHARM_TYPE_NONE && sd->spiritcharm >= MAX_SPIRITCHARM)
			s_ele = sd->spiritcharm_type; //Summoning 10 spiritcharm will endow your weapon
	} else if(s_ele == -2) //Use status element
		s_ele = status_get_attack_sc_element(src, status_get_sc(src));
	else if(s_ele == -3) //Use random element
		s_ele = rnd()%ELE_ALL;

	switch(skill_id) {
		case LG_SHIELDSPELL:
			if(skill_lv == 2)
				s_ele = ELE_HOLY;
			break;
		case WL_HELLINFERNO:
			if(ad.miscflag&ELE_DARK)
				s_ele = ELE_DARK;
			break;
		case SO_PSYCHIC_WAVE:
			if(sc && sc->count) {
				if (sc->data[SC_HEATER_OPTION])
					s_ele = sc->data[SC_HEATER_OPTION]->val4;
				else if (sc->data[SC_COOLER_OPTION])
					s_ele = sc->data[SC_COOLER_OPTION]->val4;
				else if (sc->data[SC_BLAST_OPTION])
					s_ele = sc->data[SC_BLAST_OPTION]->val3;
				else if (sc->data[SC_CURSED_SOIL_OPTION])
					s_ele = sc->data[SC_CURSED_SOIL_OPTION]->val4;
			}
			break;
		case KO_KAIHOU:
			if(sd && sd->spiritcharm_type != CHARM_TYPE_NONE && sd->spiritcharm > 0)
				s_ele = sd->spiritcharm_type;
			break;
	}

	//Set miscellaneous data that needs be filled
	if(sd) {
		sd->state.arrow_atk = 0;
		ad.blewcount += battle_blewcount_bonus(sd, skill_id);
	}

	//Skill Range Criteria
	ad.flag |= battle_range_type(src, target, skill_id, skill_lv);
	flag.infdef = (tstatus->mode&MD_PLANT ? 1 : 0);

	if(target->type == BL_SKILL) {
		TBL_SKILL *su = ((TBL_SKILL *)target);

		if(su && su->group && su->group->skill_id == WM_REVERBERATION)
			flag.infdef = 1;
	}

	switch(skill_id) {
		case MG_FIREWALL:
		case EL_FIRE_MANTLE:
			if(tstatus->def_ele == ELE_FIRE || battle_check_undead(tstatus->race, tstatus->def_ele))
				ad.blewcount = 0; //No knockback
		//Fall through
		case NJ_KAENSIN:
		case PR_SANCTUARY:
			ad.dmotion = 1; //No flinch animation
			break;
	}

	if(!flag.infdef && (tstatus->mode&MD_IGNOREMAGIC) && (ad.flag&BF_MAGIC)) //Magic
		flag.infdef = 1;

	if(!flag.infdef) { //No need to do the math for plants
		unsigned int skillratio = 100; //Skill dmg modifiers

#ifdef RENEWAL
		ad.damage = 0; //Reinitialize
#endif
//MATK_RATE scales the damage. 100 = no change. 50 is halved, 200 is doubled, etc
#define MATK_RATE(a) { ad.damage = ad.damage * (a) / 100; }
//Adds dmg%. 100 = +100% (double) damage. 10 = +10% damage
#define MATK_ADDRATE(a) { ad.damage += ad.damage * (a) / 100; }
//Adds an absolute value to damage. 100 = +100 damage
#define MATK_ADD(a) { ad.damage += a; }

		switch(skill_id) { //Calc base damage according to skill
			case AL_HEAL:
			case PR_BENEDICTIO:
			case PR_SANCTUARY:
			case AB_HIGHNESSHEAL:
				ad.damage = skill_calc_heal(src, target, skill_id, skill_lv, false);
				break;
			case PR_ASPERSIO:
				ad.damage = 40;
				break;
			case ALL_RESURRECTION:
			case PR_TURNUNDEAD:
				//Undead check is on skill_castend_damage_id code
#ifdef RENEWAL
				i = 10 * skill_lv + sstatus->luk + sstatus->int_ + status_get_lv(src) +
					300 - 300 * tstatus->hp / tstatus->max_hp;
#else
				i = 20 * skill_lv + sstatus->luk + sstatus->int_ + status_get_lv(src) +
					200 - 200 * tstatus->hp / tstatus->max_hp;
#endif
				if(i > 700)
					i = 700;
				if(rnd()%1000 < i && !(tstatus->mode&MD_BOSS))
					ad.damage = tstatus->hp;
				else {
#ifdef RENEWAL
					MATK_ADD(status_get_matk(src, 2));
#else
					ad.damage = status_get_lv(src) + sstatus->int_ + skill_lv * 10;
#endif
				}
				break;
			case PF_SOULBURN:
				ad.damage = tstatus->sp * 2;
				break;
			case NPC_EARTHQUAKE: {
					struct Damage wd = battle_calc_weapon_attack(src, target, skill_id, skill_lv, ad.miscflag);

					ad.damage = wd.damage;
				}
				break;
			case AB_RENOVATIO:
				ad.damage = status_get_lv(src) * 10 + sstatus->int_;
				break;
			case OB_OBOROGENSOU_TRANSITION_ATK:
				ad.damage = battle_damage_temp[0]; //Recieved magic damage * skill_lv / 10
				break;
			case SU_SV_ROOTTWIST_ATK:
				ad.damage = 100;
				break;
			default:
				MATK_ADD(status_get_matk(src, 2));

#ifdef RENEWAL
				switch(skill_id) {
					case AM_DEMONSTRATION:
					case AM_ACIDTERROR:
					case HW_MAGICCRASHER:
					case ASC_BREAKER:
					case CR_ACIDDEMONSTRATION:
					case GN_FIRE_EXPANSION_ACID:
						break; //Do card fix later
					default:
						ad.damage += battle_calc_cardfix(BF_MAGIC, src, target, nk, s_ele, 0, ad.damage, 0, ad.flag);
						break;
				}
#endif

				//Divide MATK in case of multiple targets skill
				if(nk&NK_SPLASHSPLIT) {
					if(ad.miscflag > 0)
						ad.damage /= ad.miscflag;
					else
						ShowError("0 enemies targeted by %d:%s, divide per 0 avoided!\n", skill_id, skill_get_name(skill_id));
				}

				switch(skill_id) {
					case MG_NAPALMBEAT:
						skillratio += -30 + 10 * skill_lv;
						if(tsc && tsc->data[SC_WHITEIMPRISON])
							skillratio <<= 1;
						if(sc && sc->data[SC_TELEKINESIS_INTENSE])
							skillratio = skillratio * sc->data[SC_TELEKINESIS_INTENSE]->val3 / 100;
						break;
					case MG_FIREBALL:
#ifdef RENEWAL
						skillratio += 40 + 20 * skill_lv;
						if(ad.miscflag == 2) //Enemies at the edge of the area will take 75% of the damage
							skillratio = skillratio * 3 / 4;
#else
						skillratio += -30 + 10 * skill_lv;
#endif
						break;
					case MG_SOULSTRIKE:
						if(battle_check_undead(tstatus->race,tstatus->def_ele))
							skillratio += 5 * skill_lv;
						if(tsc && tsc->data[SC_WHITEIMPRISON])
							skillratio <<= 1;
						if(sc && sc->data[SC_TELEKINESIS_INTENSE])
							skillratio = skillratio * sc->data[SC_TELEKINESIS_INTENSE]->val3 / 100;
						break;
					case MG_FIREWALL:
						skillratio -= 50;
						break;
					case MG_FIREBOLT:
					case MG_COLDBOLT:
					case MG_LIGHTNINGBOLT:
						if(sc && sc->data[SC_SPELLFIST] && ad.miscflag&BF_SHORT) {
							//val1 = used spellfist level, val4 = used bolt level [Rytech]
							skillratio += -100 + 50 * sc->data[SC_SPELLFIST]->val1 + sc->data[SC_SPELLFIST]->val4 * 100;
							ad.div_ = 1; //ad mods, to make it work similar to regular hits [Xazax]
							ad.flag = BF_SHORT|BF_WEAPON;
							ad.type = DMG_NORMAL;
						}
						break;
					case MG_THUNDERSTORM:
						//In renewal, Thunder Storm boost is 100% (in pre-re, 80%)
#ifndef RENEWAL
						skillratio -= 20;
#endif
						break;
					case MG_FROSTDIVER:
						skillratio += 10 * skill_lv;
						break;
					case AL_HOLYLIGHT:
						skillratio += 25;
						if(sd && sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_PRIEST)
							skillratio *= 5; //Does 5x damage include bonuses from other skills?
						break;
					case AL_RUWACH:
						skillratio += 45;
						break;
					case WZ_FROSTNOVA:
						skillratio += -100 + (100 + 10 * skill_lv) * 2 / 3;
						break;
					case WZ_FIREPILLAR:
						if(sd && ad.div_ > 0)
							ad.div_ *= -1; //For players, damage is divided by number of hits
						skillratio += -60 + 20 * skill_lv; //20% MATK each hit
						break;
					case WZ_SIGHTRASHER:
						skillratio += 20 * skill_lv;
						break;
					case WZ_WATERBALL:
						skillratio += 30 * skill_lv;
						break;
					case WZ_STORMGUST:
						skillratio += 40 * skill_lv;
						break;
					case HW_NAPALMVULCAN:
						skillratio += 25;
						if(tsc && tsc->data[SC_WHITEIMPRISON])
							skillratio <<= 1;
						if(sc && sc->data[SC_TELEKINESIS_INTENSE])
							skillratio = skillratio * sc->data[SC_TELEKINESIS_INTENSE]->val3 / 100;
						break;
					case SL_STIN: //Target size must be small (0) for full damage
						skillratio += (tstatus->size != SZ_SMALL ? -99 : 10 * skill_lv);
						break;
					case SL_STUN: //Full damage is dealt on small/medium targets
						skillratio += (tstatus->size != SZ_BIG ? 5 * skill_lv : -99);
						break;
					case SL_SMA: //Base damage is 40% + lv%
						skillratio += -60 + status_get_lv(src);
						break;
					case NJ_KOUENKA:
						skillratio -= 10;
						if(sd && sd->spiritcharm_type == CHARM_TYPE_FIRE && sd->spiritcharm > 0)
							skillratio += 20 * sd->spiritcharm;
						break;
					case NJ_KAENSIN:
						skillratio -= 50;
						if(sd && sd->spiritcharm_type == CHARM_TYPE_FIRE && sd->spiritcharm > 0)
							skillratio += 5 * sd->spiritcharm;
						break;
					case NJ_BAKUENRYU:
						skillratio += 50 + 150 * skill_lv;
						if(sd && sd->spiritcharm_type == CHARM_TYPE_FIRE && sd->spiritcharm > 0)
							skillratio += 15 * sd->spiritcharm;
						break;
					case NJ_HYOUSENSOU:
#ifdef RENEWAL
						skillratio -= 30;
#endif
						if(sd && sd->spiritcharm_type == CHARM_TYPE_WATER && sd->spiritcharm > 0)
							skillratio += 5 * sd->spiritcharm;
						break;
					case NJ_HYOUSYOURAKU:
						skillratio += 50 * skill_lv;
						if(sd && sd->spiritcharm_type == CHARM_TYPE_WATER && sd->spiritcharm > 0)
							skillratio += 25 * sd->spiritcharm;
						break;
					case NJ_HUUJIN:
#ifdef RENEWAL
						skillratio += 50;
#endif
						if(sd && sd->spiritcharm_type == CHARM_TYPE_WIND && sd->spiritcharm > 0)
							skillratio += 20 * sd->spiritcharm;
						break;
					case NJ_RAIGEKISAI:
						skillratio += 60 + 40 * skill_lv;
						if(sd && sd->spiritcharm_type == CHARM_TYPE_WIND && sd->spiritcharm > 0)
							skillratio += 15 * sd->spiritcharm;
						break;
					case NJ_KAMAITACHI:
						skillratio += 100 * skill_lv;
						if(sd && sd->spiritcharm_type == CHARM_TYPE_WIND && sd->spiritcharm > 0)
							skillratio += 10 * sd->spiritcharm;
						break;
					case NPC_ENERGYDRAIN:
						skillratio += 100 * skill_lv;
						break;
					case NPC_COMET:
						i = (sc ? distance_xy(target->x, target->y, sc->pos_x, sc->pos_y) : 8);
						if(i <= 3)
							skillratio += 2400 + 500 * skill_lv;
						else if(i <= 5)
							skillratio += 1900 + 500 * skill_lv;
						else if(i <= 7)
							skillratio += 1400 + 500 * skill_lv;
						else
							skillratio += 900 + 500 * skill_lv; 
						break;
					case NPC_VENOMFOG:
						skillratio += 600 + 100 * skill_lv;
						break;
					case NPC_HELLBURNING:
						skillratio += 900;
						break;
#ifdef RENEWAL
					case WZ_HEAVENDRIVE:
					case WZ_METEOR:
						skillratio += 25;
						break;
					case WZ_VERMILION:
						if(sd) {
							int per = 0;

							while((++per) < skill_lv)
								skillratio += per * 5; //100% 105% 115% 130% 150% 175% 205% 240% 280% 325%
						} else
							skillratio += 20 * skill_lv - 20; //Monsters use old formula
						break;
					case AM_ACIDTERROR:
						skillratio += 40 * skill_lv;
						break;
#else
					case WZ_VERMILION:
						skillratio += 20 * skill_lv - 20;
						break;
#endif
					case AB_JUDEX:
						skillratio += 200 + 20 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case AB_ADORAMUS:
						skillratio += 400 + 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case AB_DUPLELIGHT_MAGIC:
						skillratio += 100 + 20 * skill_lv;
						break;
					case WL_SOULEXPANSION:
						skillratio += -100 + (skill_lv + 4) * 100 + sstatus->int_;
						RE_LVL_DMOD(100);
						if(tsc && tsc->data[SC_WHITEIMPRISON])
							skillratio <<= 1;
						if(sc && sc->data[SC_TELEKINESIS_INTENSE])
							skillratio = skillratio * sc->data[SC_TELEKINESIS_INTENSE]->val3 / 100;
						break;
					case WL_FROSTMISTY:
						skillratio += 100 + 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case WL_JACKFROST:
						if(tsc && tsc->data[SC_FREEZING]) {
							skillratio += 900 + 300 * skill_lv;
							RE_LVL_DMOD(100);
						} else {
							skillratio += 400 + 100 * skill_lv;
							RE_LVL_DMOD(150);
						}
						break;
					case WL_DRAINLIFE:
						skillratio += -100 + 200 * skill_lv + sstatus->int_;
						RE_LVL_DMOD(100);
						break;
					case WL_CRIMSONROCK:
						skillratio += 1200 + 300 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case WL_HELLINFERNO:
						skillratio += -100 + 300 * skill_lv;
						RE_LVL_DMOD(100);
						//Shadow: MATK [{( Skill Level x 300 ) x ( Caster's Base Level / 100 ) x 4/5 }] %
						//Fire : MATK [{( Skill Level x 300 ) x ( Caster's Base Level / 100 ) /5 }] %
						if(ad.miscflag&ELE_DARK)
							skillratio *= 4;
						skillratio /= 5;
						break;
					case WL_COMET:
						i = (sc ? distance_xy(target->x, target->y, sc->pos_x, sc->pos_y) : 8);
						if(i <= 3)
							skillratio += 2400 + 500 * skill_lv; //7 x 7 cell
						else if(i <= 5)
							skillratio += 1900 + 500 * skill_lv; //11 x 11 cell
						else if(i <= 7)
							skillratio += 1400 + 500 * skill_lv; //15 x 15 cell
						else
							skillratio += 900 + 500 * skill_lv; //19 x 19 cell
						//MATK [{( Skill Level x 400 ) x ( Caster's Base Level / 120 )} + 2500 ] %
						if(skill_check_pc_partner(sd,skill_id,&skill_lv,skill_get_splash(skill_id,skill_lv),0)) {
							skillratio = skill_lv * 400;
							RE_LVL_DMOD(120);
							skillratio += 2500;
						}
						break;
					case WL_CHAINLIGHTNING_ATK:
						skillratio += 400 + 100 * skill_lv;
						RE_LVL_DMOD(100);
						if(ad.miscflag > 0)
							skillratio += 100 * ad.miscflag;
						break;
					case WL_EARTHSTRAIN:
						skillratio += 1900 + 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case WL_TETRAVORTEX_FIRE:
					case WL_TETRAVORTEX_WATER:
					case WL_TETRAVORTEX_WIND:
					case WL_TETRAVORTEX_GROUND:
						skillratio += 400 + 500 * skill_lv;
						break;
					case WL_SUMMON_ATK_FIRE:
					case WL_SUMMON_ATK_WATER:
					case WL_SUMMON_ATK_WIND:
					case WL_SUMMON_ATK_GROUND:
						skillratio += -100 + (1 + skill_lv) / 2 * (status_get_lv(src) + status_get_job_lv(src));
						RE_LVL_DMOD(100);
						break;
					case LG_RAYOFGENESIS:
						if(sc) {
							if(sc->data[SC_INSPIRATION])
								skillratio += 1400;
							if(sc->data[SC_BANDING])
								skillratio += -100 + 300 * skill_lv + 200 * sc->data[SC_BANDING]->val2;
							RE_LVL_DMOD(25);
						}
						break;
					case LG_SHIELDSPELL: //[(Caster's Base Level x 4) + (Shield MDEF x 100) + (Caster's INT x 2)] %
						if(sd && skill_lv == 2)
							skillratio += -100 + 4 * status_get_lv(src) + 100 * sd->bonus.shieldmdef + 2 * status_get_int(src);
						else
							skillratio = 0;
						break;
					case WM_METALICSOUND:
						skillratio += -100 + 120 * skill_lv + 60 * (sd ? pc_checkskill(sd, WM_LESSON) : 10);
						RE_LVL_DMOD(100);
						if(tsc && (tsc->data[SC_SLEEP] || tsc->data[SC_DEEPSLEEP]))
							skillratio += skillratio / 2;
						break;
					case WM_REVERBERATION_MAGIC:
						//MATK [{(Skill Level x 100) + 100} x Caster's Base Level / 100] %
						skillratio += 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case SO_FIREWALK:
						skillratio += -100 + 60 * skill_lv;
						RE_LVL_DMOD(100);
						if(sc && sc->data[SC_HEATER_OPTION])
							skillratio += sc->data[SC_HEATER_OPTION]->val3 / 2;
						break;
					case SO_ELECTRICWALK:
						skillratio += -100 + 60 * skill_lv;
						RE_LVL_DMOD(100);
						if(sc && sc->data[SC_BLAST_OPTION])
							skillratio += sc->data[SC_BLAST_OPTION]->val2 / 2;
						break;
					case SO_EARTHGRAVE:
						skillratio += -100 + sstatus->int_ * skill_lv + 200 * (sd ? pc_checkskill(sd, SA_SEISMICWEAPON) : 5);
						RE_LVL_DMOD(100);
						if(sc && sc->data[SC_CURSED_SOIL_OPTION])
							skillratio += 5 * sc->data[SC_CURSED_SOIL_OPTION]->val3;
						break;
					case SO_DIAMONDDUST:
						skillratio += -100 + sstatus->int_ * skill_lv + 200 * (sd ? pc_checkskill(sd, SA_FROSTWEAPON) : 5);
						RE_LVL_DMOD(100);
						if(sc && sc->data[SC_COOLER_OPTION])
							skillratio += 5 * sc->data[SC_COOLER_OPTION]->val3;
						break;
					case SO_POISON_BUSTER:
						skillratio += 900 + 300 * skill_lv;
						RE_LVL_DMOD(120);
						if(sc && sc->data[SC_CURSED_SOIL_OPTION])
							skillratio += 5 * sc->data[SC_CURSED_SOIL_OPTION]->val3;
						break;
					case SO_PSYCHIC_WAVE:
						skillratio += -100 + 70 * skill_lv + 3 * sstatus->int_;
						RE_LVL_DMOD(100);
						if(sc && (sc->data[SC_HEATER_OPTION] || sc->data[SC_COOLER_OPTION] ||
							sc->data[SC_BLAST_OPTION] || sc->data[SC_CURSED_SOIL_OPTION]))
							skillratio += 20;
						break;
					case SO_CLOUD_KILL:
						skillratio += -100 + 40 * skill_lv;
						RE_LVL_DMOD(100);
						if(sc && sc->data[SC_CURSED_SOIL_OPTION])
							skillratio += sc->data[SC_CURSED_SOIL_OPTION]->val3;
						break;
					case SO_VARETYR_SPEAR:
						//MATK [{( Endow Tornado skill level x 50 ) + ( Caster's INT x Varetyr Spear Skill level )} x Caster's Base Level / 100 ] %
						skillratio += -100 + status_get_int(src) * skill_lv + 50 * (sd ? pc_checkskill(sd, SA_LIGHTNINGLOADER) : 5);
						RE_LVL_DMOD(100);
						if(sc && sc->data[SC_BLAST_OPTION])
							skillratio += sc->data[SC_BLAST_OPTION]->val2 * 5;
						break;
					case GN_DEMONIC_FIRE:
						if(skill_lv > 20) //Fire Expansion Level 2
							skillratio += 10 + 20 * (skill_lv - 20) + 10 * sstatus->int_;
						else if(skill_lv > 10) { //Fire Expansion Level 1
							skillratio += 10 + 20 * (skill_lv - 10) + status_get_job_lv(src) + sstatus->int_;
							RE_LVL_DMOD(100);
						} else //Normal Demonic Fire Damage
							skillratio += 10 + 20 * skill_lv;
						break;
					case KO_KAIHOU:
						if(sd && sd->spiritcharm_type != CHARM_TYPE_NONE && sd->spiritcharm > 0) {
							skillratio += -100 + 200 * sd->spiritcharm;
							RE_LVL_DMOD(100);
							pc_delspiritcharm(sd, sd->spiritcharm, sd->spiritcharm_type);
						}
						break;
					//Magical Elemental Spirits Attack Skills
					case EL_FIRE_MANTLE:
					case EL_WATER_SCREW:
						skillratio += 900;
						break;
					case EL_FIRE_ARROW:
					case EL_ROCK_CRUSHER_ATK:
						skillratio += 200;
						break;
					case EL_FIRE_BOMB:
					case EL_ICE_NEEDLE:
					case EL_HURRICANE_ATK:
						skillratio += 400;
						break;
					case EL_FIRE_WAVE:
					case EL_TYPOON_MIS_ATK:
						skillratio += 1100;
						break;
					case MH_ERASER_CUTTER:
						skillratio += 400 + 100 * skill_lv + (skill_lv%2 > 0 ? 0 : 300);
						break;
					case MH_XENO_SLASHER:
						if(skill_lv%2)
							skillratio += 350 + 50 * skill_lv; //500:600:700
						else
							skillratio += 400 + 100 * skill_lv; //700:900
						break;
					case MH_HEILIGE_STANGE:
						skillratio += 400 + 250 * skill_lv * status_get_lv(src) / 150;
						break;
					case MH_POISON_MIST:
						skillratio += -100 + 40 * skill_lv * status_get_lv(src) / 100;
						break;
					case SU_SV_STEMSPEAR:
						skillratio += 600;
						break;
					case SU_CN_METEOR:
						skillratio += 100 + 100 * skill_lv;
						break;
				}

				if(sc && ((sc->data[SC_FIRE_INSIGNIA] && sc->data[SC_FIRE_INSIGNIA]->val1 == 3 && s_ele == ELE_FIRE) ||
					(sc->data[SC_WATER_INSIGNIA] && sc->data[SC_WATER_INSIGNIA]->val1 == 3 && s_ele == ELE_WATER) ||
					(sc->data[SC_WIND_INSIGNIA] && sc->data[SC_WIND_INSIGNIA]->val1 == 3 && s_ele == ELE_WIND) ||
					(sc->data[SC_EARTH_INSIGNIA] && sc->data[SC_EARTH_INSIGNIA]->val1 == 3 && s_ele == ELE_EARTH)))
					skillratio += 25;

				MATK_RATE(skillratio);

				//Constant/misc additions from skills
				if(skill_id == WZ_FIREPILLAR)
					MATK_ADD(100 + 50 * skill_lv);
				break;
		}

		switch(skill_id) {
			case WL_CHAINLIGHTNING_ATK:
				id = WL_CHAINLIGHTNING;
				break;
			case AB_DUPLELIGHT_MAGIC:
				id = AB_DUPLELIGHT;
				break;
			case WL_TETRAVORTEX_FIRE:
			case WL_TETRAVORTEX_WATER:
			case WL_TETRAVORTEX_WIND:
			case WL_TETRAVORTEX_GROUND:
				id = WL_TETRAVORTEX;
				break;
			case WL_SUMMON_ATK_FIRE:
			case WL_SUMMON_ATK_WIND:
			case WL_SUMMON_ATK_WATER:
			case WL_SUMMON_ATK_GROUND:
				id = WL_RELEASE;
				break;
			case WM_REVERBERATION_MAGIC:
				id = WM_REVERBERATION;
				break;
			default:
				id = skill_id;
				break;
		}

		if(sd) {
			if((i = pc_skillatk_bonus(sd, id)))
				MATK_ADDRATE(i); //Damage rate bonuses
			if((i = battle_adjust_skill_damage(src->m, id)))
				MATK_RATE(i);
			if(!flag.imdef &&
				((sd->bonus.ignore_mdef_ele&(1<<tstatus->def_ele)) || (sd->bonus.ignore_mdef_ele&(1<<ELE_ALL)) ||
				(sd->bonus.ignore_mdef_race&(1<<tstatus->race)) || (sd->bonus.ignore_mdef_race&(1<<RC_ALL)) ||
				(sd->bonus.ignore_mdef_class&(1<<tstatus->class_)) || (sd->bonus.ignore_mdef_class&(1<<CLASS_ALL))))
				flag.imdef = 1; //Ignore MDEF
		}

		if(tsd && (i = pc_sub_skillatk_bonus(tsd, id)))
			MATK_ADDRATE(-i);

		if(!flag.imdef) {
			defType mdef = tstatus->mdef; //eMDEF
			short mdef2 = tstatus->mdef2; //sMDEF

			mdef = status_calc_mdef(target, tsc, mdef, false);
			mdef2 = status_calc_mdef2(target, tsc, mdef2, false);

			if(sd) {
				i = sd->ignore_mdef_by_race[tstatus->race] + sd->ignore_mdef_by_race[RC_ALL];
				i += sd->ignore_mdef_by_class[tstatus->class_] + sd->ignore_mdef_by_class[CLASS_ALL];
				if(i) {
					i = min(i, 100);
					mdef -= mdef * i / 100;
					//mdef2 -= mdef2 * i / 100;
				}
			}

#ifdef RENEWAL
			/**
			 * RE MDEF Reduction
			 * Damage = Magic Attack * (1000 + eMDEF) / (1000 + eMDEF) - sMDEF
			 */
			if(mdef < -99) //It stops at -99
				mdef = 99; //In aegis it set to 1 but in our case it may lead to exploitation so limit it to 99
			ad.damage = ad.damage * (1000 + mdef) / (1000 + mdef * 10) - mdef2;
#else
			if(battle_config.magic_defense_type)
				ad.damage = ad.damage - mdef * battle_config.magic_defense_type - mdef2;
			else
				ad.damage = ad.damage * (100 - mdef) / 100 - mdef2;
#endif
		}

		if(ad.damage < 1)
			ad.damage = 1;
		else if(sc) { //Only applies when hit
			switch(skill_id) { //@TODO: There is another factor that contribute with the damage and need to be formulated [malufett]
				case MG_LIGHTNINGBOLT:
				case MG_THUNDERSTORM:
					if(sc->data[SC_GUST_OPTION])
						ad.damage += (6 + sstatus->int_ / 4) + max(sstatus->dex - 10, 0) / 30;
					break;
				case MG_FIREBOLT:
				case MG_FIREWALL:
					if(sc->data[SC_PYROTECHNIC_OPTION])
						ad.damage += (6 + sstatus->int_ / 4) + max(sstatus->dex - 10, 0) / 30;
					break;
				case MG_COLDBOLT:
				case MG_FROSTDIVER:
					if(sc->data[SC_AQUAPLAY_OPTION])
						ad.damage += (6 + sstatus->int_ / 4) + max(sstatus->dex - 10, 0) / 30;
					break;
				case WZ_EARTHSPIKE:
				case WZ_HEAVENDRIVE:
					if(sc->data[SC_PETROLOGY_OPTION])
						ad.damage += (6 + sstatus->int_ / 4) + max(sstatus->dex - 10, 0) / 30;
					break;
			}
		}

		if(!(nk&NK_NO_ELEFIX)
#ifdef RENEWAL //Keep neutral reduction from ghost element armor
			|| skill_id == NPC_EARTHQUAKE
#endif
			)
		{
			switch(skill_id) {
#ifdef RENEWAL
				case AM_ACIDTERROR:
				case CR_ACIDDEMONSTRATION:
				case GN_FIRE_EXPANSION_ACID:
					ad.damage = battle_attr_fix(src, target, ad.damage, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
					break;
				case AM_DEMONSTRATION:
					ad.damage = battle_attr_fix(src, target, ad.damage, ELE_FIRE, tstatus->def_ele, tstatus->ele_lv);
					break;
#endif
				default:
					ad.damage = battle_attr_fix(src, target, ad.damage, s_ele, tstatus->def_ele, tstatus->ele_lv);
					break;
			}
		}

		switch(skill_id) { //Apply the physical part of the skill's damage [Skotlex]
			case CR_GRANDCROSS:
			case NPC_GRANDDARKNESS:
				{
					struct Damage wd = battle_calc_weapon_attack(src, target, skill_id, skill_lv, ad.miscflag);

					ad.damage = battle_attr_fix(src, target, wd.damage + ad.damage, s_ele, tstatus->def_ele, tstatus->ele_lv) * (100 + 40 * skill_lv) / 100;
					if(target->id == src->id) {
						if(sd)
							ad.damage >>= 1;
						else
							ad.damage = 0;
					}
				}
				break;
			case SO_VARETYR_SPEAR: {
					struct Damage wd = battle_calc_weapon_attack(src, target, skill_id, skill_lv, ad.miscflag);

					ad.damage += wd.damage;
				}
				break;
		}

#ifndef RENEWAL
		ad.damage += battle_calc_cardfix(BF_MAGIC, src, target, nk, s_ele, 0, ad.damage, 0, ad.flag);
#endif
	}

	DAMAGE_DIV_FIX(ad.damage, ad.div_);

	if(flag.infdef && ad.damage > 0)
		ad.damage = 1;

	switch(skill_id) {
#ifdef RENEWAL
		case AM_DEMONSTRATION:
		case AM_ACIDTERROR:
		case HW_MAGICCRASHER:
		case ASC_BREAKER:
		case CR_ACIDDEMONSTRATION:
		case GN_FIRE_EXPANSION_ACID:
			return ad; //Do GVG fix later
#endif
		default:
			ad.damage = battle_calc_damage(src, target, &ad, ad.damage, skill_id, skill_lv);
			if(map_flag_gvg2(target->m))
				ad.damage = battle_calc_gvg_damage(src, target, ad.damage, skill_id, ad.flag);
			else if(map[target->m].flag.battleground)
				ad.damage = battle_calc_bg_damage(src, target, ad.damage, skill_id, ad.flag);
			break;
	}

	//Skill damage adjustment
#ifdef ADJUST_SKILL_DAMAGE
	if((skill_damage = battle_skill_damage(src, target, skill_id)) != 0)
		MATK_ADDRATE(skill_damage);
#endif

	battle_absorb_damage(target, &ad);

	//Skill reflect gets calculated after all attack modifier
	//NOTE: Magic skill has own handler at skill_attack
	//battle_do_reflect(BF_MAGIC, &ad, src, target, skill_id, skill_lv); //WIP [lighta]

	return ad;
}

/*==========================================
 * Calculate "misc"-type attacks and skills
 *------------------------------------------
 * Credits:
 *	Original coder Skotlex
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_misc_attack(struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, int mflag)
{
#ifdef ADJUST_SKILL_DAMAGE
	int skill_damage = 0;
#endif
	short i, nk;
	short s_ele;
	uint16 id;

	struct map_session_data *sd, *tsd;
	struct Damage md; //DO NOT CONFUSE with md of mob_data!
	struct status_data *sstatus, *tstatus;

	memset(&md, 0, sizeof(md));

	if(!src || !target) {
		nullpo_info(NLP_MARK);
		return md;
	}

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(target);

	//Some initial values
	md.amotion = (skill_get_inf(skill_id)&INF_GROUND_SKILL ? 0 : sstatus->amotion);
	md.dmotion = tstatus->dmotion;
	md.div_ = skill_get_num(skill_id, skill_lv);
	md.blewcount = skill_get_blewcount(skill_id, skill_lv);
	md.miscflag = mflag;
	md.flag = BF_MISC|BF_SKILL;
	md.dmg_lv = ATK_DEF;
	nk = skill_get_nk(skill_id);

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);

	if(sd) {
		sd->state.arrow_atk = 0;
		md.blewcount += battle_blewcount_bonus(sd, skill_id);
	}

	s_ele = skill_get_ele(skill_id, skill_lv);
	if(s_ele < 0 && s_ele != -3) //Attack that takes weapon's element for misc attacks? Make it neutral [Skotlex]
		s_ele = ELE_NEUTRAL;
	else if(s_ele == -3) //Use random element
		s_ele = rnd()%ELE_ALL;

	//Skill Range Criteria
	md.flag |= battle_range_type(src, target, skill_id, skill_lv);

	switch(skill_id) {
		case TF_THROWSTONE:
			md.damage = 50;
			md.flag |= BF_WEAPON;
			break;
#ifdef RENEWAL
		case HT_LANDMINE:
		case MA_LANDMINE:
		case HT_BLASTMINE:
		case HT_CLAYMORETRAP:
			md.damage = skill_lv * sstatus->dex * (3 + status_get_lv(src) / 100) * (1 + sstatus->int_ / 35);
			md.damage += md.damage * (rnd()%20 - 10) / 100;
			md.damage += 40 * (sd ? pc_checkskill(sd, RA_RESEARCHTRAP) : 5);
			break;
#else
		case HT_LANDMINE:
		case MA_LANDMINE:
			md.damage = skill_lv * (sstatus->dex + 75) * (100 + sstatus->int_) / 100;
			break;
		case HT_BLASTMINE:
			md.damage = skill_lv * (sstatus->dex / 2 + 50) * (100 + sstatus->int_) / 100;
			break;
		case HT_CLAYMORETRAP:
			md.damage = skill_lv * (sstatus->dex / 2 + 75) * (100 + sstatus->int_) / 100;
			break;
#endif
		case HT_BLITZBEAT:
		case SN_FALCONASSAULT:
			{
				uint16 lv;

				//Blitz-beat Damage
				if(!sd || !(lv = pc_checkskill(sd, HT_STEELCROW)))
					lv = 0;
				md.damage = (sstatus->dex / 10 + sstatus->int_ / 2 + lv * 3 + 40) * 2;
				if(md.miscflag > 1) //Autocasted Blitz
					nk |= NK_SPLASHSPLIT;
				if(skill_id == SN_FALCONASSAULT) {
					//Div fix of Blitzbeat
					DAMAGE_DIV_FIX2(md.damage, skill_get_num(HT_BLITZBEAT, 5));
					//Falcon Assault Modifier
					md.damage = md.damage * (150 + 70 * skill_lv) / 100;
				}
			}
			break;
#ifdef RENEWAL
		case AM_DEMONSTRATION:
		case AM_ACIDTERROR:
		case GS_MAGICALBULLET:
			{
				//Official renewal formula [exneval]
				//Damage = (Final ATK + Final MATK) * Skill modifiers - (eDEF + sDEF + eMDEF + sMDEF)
				short totaldef, totalmdef;
				struct Damage atk, matk;

				atk = battle_calc_weapon_attack(src, target, skill_id, skill_lv, md.miscflag);
				matk = battle_calc_magic_attack(src, target, skill_id, skill_lv, md.miscflag);
				md.damage = atk.damage + matk.damage;
				totaldef = (short)status_get_def(target) + tstatus->def2;
				totalmdef = tstatus->mdef + tstatus->mdef2;
				md.damage -= totaldef + totalmdef;
				if(skill_id == AM_ACIDTERROR && tstatus->mode&MD_BOSS)
					md.damage >>= 1;
				md.flag |= BF_WEAPON;
				if(skill_id == GS_MAGICALBULLET)
					nk |= NK_IGNORE_FLEE; //Flee already checked in battle_calc_weapon_attack, so don't do it again here [exneval]
			}
			break;
#endif
		case BA_DISSONANCE:
			md.damage = 30 + 10 * skill_lv + 3 * (sd ? pc_checkskill(sd, BA_MUSICALLESSON) : 10);
			break;
		case NPC_SELFDESTRUCTION:
			md.damage = sstatus->hp;
			break;
		case NPC_SMOKING:
			md.damage = 3;
			break;
		case NPC_DARKBREATH:
			md.damage = tstatus->max_hp * skill_lv * 10 / 100;
			break;
		case NPC_EVILLAND:
			md.damage = skill_calc_heal(src, target, skill_id, skill_lv, false);
			break;
		case NPC_MAXPAIN_ATK:
			md.damage = battle_damage_temp[0] * skill_lv * 10 / 100;
			break;
#ifdef RENEWAL
		case HW_MAGICCRASHER: {
				//Official renewal formula [exneval]
				//Damage = (Physical Damage + Magical Damage + Refine) * Attribute modifiers - (eDEF + sDEF)
				//Magical Damage = (MATK - (eMDEF + sMDEF)) / 5
				short totaldef, totalmdef;
				struct Damage atk, matk;

				atk = battle_calc_weapon_attack(src, target, skill_id, skill_lv, md.miscflag);
				matk = battle_calc_magic_attack(src, target, skill_id, skill_lv, md.miscflag);
				totaldef = (short)status_get_def(target) + tstatus->def2;
				totalmdef = tstatus->mdef + tstatus->mdef2;
				matk.damage = (matk.damage - totalmdef) / 5;
				md.damage = atk.damage + matk.damage;
				md.damage -= totaldef;
				if(md.damage <= 0)
					md.damage = atk.damage;
				md.flag |= BF_WEAPON;
				nk |= NK_IGNORE_FLEE;
			}
			break;
#endif
		case ASC_BREAKER:
#ifdef RENEWAL
			{
				//Official renewal formula [helvetica]
				//Damage = ((ATK + MATK) * (3 + (.5 * Skill level))) - (eDEF + sDEF + eMDEF + sMDEF)
				short totaldef, totalmdef;
				struct Damage atk, matk;

				atk = battle_calc_weapon_attack(src, target, skill_id, skill_lv, md.miscflag);
				nk |= NK_NO_ELEFIX; //ATK part takes on weapon element, MATK part is non-elemental
				matk = battle_calc_magic_attack(src, target, skill_id, skill_lv, md.miscflag);
				//(ATK + MATK) * (3 + (.5 * Skill level))
				md.damage = (30 + 5 * skill_lv) * (atk.damage + matk.damage) / 10;
				//Modified defense reduction
				//Final damage = Base damage - (eDEF + sDEF + eMDEF + sMDEF)
				totaldef = (short)status_get_def(target) + tstatus->def2;
				totalmdef = tstatus->mdef + tstatus->mdef2;
				md.damage -= totaldef + totalmdef;
			}
#else
			md.damage = rnd_value(500, 1000) + 5 * skill_lv * sstatus->int_;
			nk |= NK_NO_ELEFIX|NK_IGNORE_FLEE; //These two are not properties of the weapon based part
#endif
			break;
		case HW_GRAVITATION:
#ifdef RENEWAL
			md.damage = 500 + 100 * skill_lv;
#else
			md.damage = 200 + 200 * skill_lv;
#endif
			md.dmotion = 0; //No flinch animation
			break;
		case PA_PRESSURE:
			md.damage = 500 + 300 * skill_lv;
			break;
		case PA_GOSPEL:
			if(mflag)
				md.damage = (rnd()%4000) + 1500;
			else {
				md.damage = (rnd()%5000) + 3000;
#ifdef RENEWAL
				md.damage -= (int64)status_get_def(target);
#else
				md.damage -= (md.damage * (int64)status_get_def(target)) / 100;
#endif
				md.damage -= tstatus->def2;
				md.damage = max(md.damage, 0);
			}
			break;
		case CR_ACIDDEMONSTRATION:
		case GN_FIRE_EXPANSION_ACID:
#ifdef RENEWAL
			{
				//Official renewal formula [exneval]
				//Damage = [Skill level * (([(ATK + MATK) * .7 * target's VIT] - [(eDEF + sDEF) / 2 + (eMDEF + sMDEF) / 2]) / 10)]
				short totaldef, totalmdef;
				short targetVit = min(tstatus->vit, 120);
				struct Damage atk, matk;

				atk = battle_calc_weapon_attack(src, target, skill_id, skill_lv, md.miscflag);
				matk = battle_calc_magic_attack(src, target, skill_id, skill_lv, md.miscflag);
				totaldef = (short)status_get_def(target) + tstatus->def2;
				totalmdef = tstatus->mdef + tstatus->mdef2;
				md.damage = (int64)(skill_lv * ((((atk.damage + matk.damage) * 7 / 10 * targetVit) - (totaldef / 2 + totalmdef / 2)) / 10));
			}
#else
			if(tstatus->vit + sstatus->int_) //Crash fix
				md.damage = (int64)(7 * tstatus->vit * sstatus->int_ * sstatus->int_ / (10 * (tstatus->vit + sstatus->int_)));
			else
				md.damage = 0;
#endif
			if(skill_id == CR_ACIDDEMONSTRATION && tsd)
				md.damage >>= 1;
			break;
		case NJ_ZENYNAGE:
		case KO_MUCHANAGE:
				md.damage = skill_get_zeny(skill_id, skill_lv);
				if(!md.damage)
					md.damage = (skill_id == NJ_ZENYNAGE ? 2 : 10);
				md.damage = (skill_id == NJ_ZENYNAGE ? rnd()%md.damage + md.damage :
					md.damage * rnd_value(50, 100)) / (skill_id == NJ_ZENYNAGE ? 1 : 100);
				if(sd && skill_id == KO_MUCHANAGE && !pc_checkskill(sd, NJ_TOBIDOUGU))
					md.damage = md.damage / 2;
				if(is_boss(target))
					md.damage = md.damage / (skill_id == NJ_ZENYNAGE ? 3 : 2);
				else if(tsd && skill_id == NJ_ZENYNAGE)
					md.damage = md.damage / 2;
			break;
#ifdef RENEWAL
		case NJ_ISSEN: {
				//Official renewal formula [helvetica]
				//Base damage = CurrentHP + ((ATK * CurrentHP * Skill level) / MaxHP)
				//Final damage = Base damage + ((Mirror Image count + 1) / 5 * Base damage) - (eDEF + sDEF)
				short totaldef;
				struct status_change *sc = status_get_sc(src);
				struct Damage atk = battle_calc_weapon_attack(src, target, skill_id, skill_lv, md.miscflag);

				md.damage = sstatus->hp + atk.damage * sstatus->hp * skill_lv / sstatus->max_hp;
				//Mirror Image bonus only occurs if active
				if(sc && sc->data[SC_BUNSINJYUTSU] && (i = sc->data[SC_BUNSINJYUTSU]->val2) > 0) {
					md.div_ = -(i + 2); //Mirror image count + 2
					md.damage += (md.damage * (((i + 1) * 10) / 5)) / 10;
				}
				//Modified defense reduction
				//Final damage = Base damage - (eDEF + sDEF)
				totaldef = (short)status_get_def(target) + tstatus->def2;
				md.damage -= totaldef;
				md.flag |= BF_WEAPON;
			}
			break;
#endif
		case GS_FLING:
			md.damage = status_get_job_lv(src);
			break;
		case HVAN_EXPLOSION: //[orn]
			md.damage = sstatus->max_hp * (50 + 50 * skill_lv) / 100;
			break;
		case RA_CLUSTERBOMB:
		case RA_FIRINGTRAP:
		case RA_ICEBOUNDTRAP:
			md.damage = skill_lv * sstatus->dex + sstatus->int_ * 5 ;
			RE_LVL_TMDMOD();
			if(sd) {
				int researchskill_lv = pc_checkskill(sd, RA_RESEARCHTRAP);

				if(researchskill_lv)
					md.damage = md.damage * 20 * researchskill_lv / (skill_id == RA_CLUSTERBOMB ? 50 : 100);
				else
					md.damage = 0;
			} else
				md.damage = md.damage * 200 / (skill_id == RA_CLUSTERBOMB ? 50 : 100);
			nk |= NK_NO_ELEFIX|NK_IGNORE_FLEE|NK_NO_CARDFIX_DEF;
			break;
		case NC_MAGMA_ERUPTION:
			md.damage = 800 + 200 * skill_lv;
			break;
		case WM_SOUND_OF_DESTRUCTION:
			md.damage = 1000 * skill_lv + sstatus->int_ * (sd ? pc_checkskill(sd, WM_LESSON) : 10);
			md.damage += md.damage * 10 * party_calc_chorusbonus(sd, 0) / 100;
			break;
		case GN_THORNS_TRAP:
			md.damage = 100 + 200 * skill_lv + sstatus->int_;
			break;
		case GN_BLOOD_SUCKER:
			md.damage = 200 + 100 * skill_lv + sstatus->int_;
			break;
		case GN_HELLS_PLANT_ATK:
			md.damage = 10 * skill_lv * status_get_lv(src) + 7 * sstatus->int_ / 2 *
				(18 + status_get_job_lv(src) / 4) * 5 / (10 - (sd ? pc_checkskill(sd, AM_CANNIBALIZE) : 5));
			if(map_flag_gvg2(src->m))
				md.damage >>= 1;
			md.flag |= BF_WEAPON;
			break;
		case RL_B_TRAP:
			md.damage = 3 * skill_lv * tstatus->hp / 100 + 10 * sstatus->dex;
			break;
		case MH_EQC:
			md.damage = max((int)(tstatus->hp - sstatus->hp), 0);
			break;
	}

	if(nk&NK_SPLASHSPLIT) { //Divide ATK among targets
		if(md.miscflag > 0)
			md.damage /= md.miscflag;
		else
			ShowError("0 enemies targeted by %d:%s, divide per 0 avoided!\n", skill_id, skill_get_name(skill_id));
	}

	DAMAGE_DIV_FIX(md.damage, md.div_);

	if(!(nk&NK_IGNORE_FLEE)) {
		struct status_change *sc = status_get_sc(target);

		i = 0; //Temp for "hit or no hit"
		if(sc && sc->opt1 && sc->opt1 != OPT1_STONEWAIT && sc->opt1 != OPT1_BURNING)
			i = 1;
		else {
			short flee = tstatus->flee,
#ifdef RENEWAL
				hitrate = 0; //Default hitrate
#else
				hitrate = 80; //Default hitrate
#endif

			if(battle_config.agi_penalty_type && battle_config.agi_penalty_target&target->type) {
				unsigned char attacker_count = unit_counttargeted(target); //256 max targets should be a sane max

				if(attacker_count >= battle_config.agi_penalty_count) {
					if(battle_config.agi_penalty_type == 1)
						flee = (flee * (100 - (attacker_count - (battle_config.agi_penalty_count - 1)) * battle_config.agi_penalty_num)) / 100;
					else //Assume type 2: absolute reduction
						flee -= (attacker_count - (battle_config.agi_penalty_count - 1)) * battle_config.agi_penalty_num;
					if(flee < 1)
						flee = 1;
				}
			}
			hitrate += sstatus->hit - flee;
#ifdef RENEWAL
			if(sd) //In renewal, hit bonus from Vultures Eye is not shown anymore in status window
				hitrate += pc_checkskill(sd, AC_VULTURE);
#endif
			hitrate = cap_value(hitrate, battle_config.min_hitrate, battle_config.max_hitrate);
			if(rnd()%100 < hitrate)
				i = 1;
		}
		if(!i) {
			md.damage = 0;
			md.dmg_lv = ATK_FLEE;
		}
	}

	switch(skill_id) {
#ifdef RENEWAL
		case GS_MAGICALBULLET:
			break; //Card fix already done
#endif
		default:
			md.damage += battle_calc_cardfix(BF_MISC, src, target, nk, s_ele, 0, md.damage, 0, md.flag);
			break;
	}

	switch(skill_id) {
		case GN_HELLS_PLANT_ATK:
			id = GN_HELLS_PLANT;
			break;
		default:
			id = skill_id;
			break;
	}

	if(sd) {
		if((i = pc_skillatk_bonus(sd, id)))
			md.damage += md.damage * i / 100;
		if((i = battle_adjust_skill_damage(src->m, id)))
			md.damage = md.damage * i / 100;
	}

	if(tsd && (i = pc_sub_skillatk_bonus(tsd, id)))
		md.damage -= md.damage * i / 100;

	if(md.damage > 0) {
		if(tstatus->mode&MD_PLANT) {
			switch(skill_id) {
#ifdef RENEWAL
				case NJ_ISSEN: //Final Strike will show "miss" on plants [helvetica]
					md.damage = 1;
					md.dmg_lv = ATK_FLEE;
					break;
				case HT_BLASTMINE:
				case HT_CLAYMORETRAP:
				case RA_CLUSTERBOMB:
					break; //This trap will do full damage to plants
#endif
				default:
					md.damage = 1;
					break;
			}
		}
		if(target->type == BL_SKILL) {
			TBL_SKILL *su = ((TBL_SKILL *)target);

			if(su && su->group && su->group->skill_id == WM_REVERBERATION)
				md.damage = 1;
		}
		if((tstatus->mode&MD_IGNOREMISC) && (md.flag&BF_MISC))
			md.damage = 1;
	} else
		md.damage = 0;

	if(!(nk&NK_NO_ELEFIX) && md.damage > 0) {
		md.damage = battle_attr_fix(src, target, md.damage, s_ele, tstatus->def_ele, tstatus->ele_lv);
		switch(skill_id) {
			case NC_MAGMA_ERUPTION:
				//Forced neutral [exneval]
				md.damage = battle_attr_fix(src, target, md.damage, ELE_NEUTRAL, tstatus->def_ele, tstatus->ele_lv);
				break;
		}
	}

	switch(skill_id) {
		case RA_FIRINGTRAP:
 		case RA_ICEBOUNDTRAP:
			if(md.damage == 1)
				break; //Keep damage to 1 against "plant"-type mobs
		//Fall through
		case RA_CLUSTERBOMB:
			{
				struct Damage wd = battle_calc_weapon_attack(src, target, skill_id, skill_lv, md.miscflag);

				md.damage += wd.damage;
			}
			break;
		case NJ_ZENYNAGE:
			if(sd) {
				if(md.damage > sd->status.zeny)
					md.damage = sd->status.zeny;
				pc_payzeny(sd, (int)cap_value(md.damage, INT_MIN, INT_MAX), LOG_TYPE_STEAL, NULL);
			}
			break;
	}

	switch(skill_id) {
#ifdef RENEWAL
		case GS_MAGICALBULLET:
			return md; //GVG fix already done
#endif
		default:
			md.damage = battle_calc_damage(src, target, &md, md.damage, skill_id, skill_lv);
			if(map_flag_gvg2(target->m))
				md.damage = battle_calc_gvg_damage(src, target, md.damage, skill_id, md.flag);
			else if(map[target->m].flag.battleground)
				md.damage = battle_calc_bg_damage(src, target, md.damage, skill_id, md.flag);
			break;
	}

	//Skill damage adjustment
#ifdef ADJUST_SKILL_DAMAGE
	if((skill_damage = battle_skill_damage(src, target, skill_id)) != 0)
		md.damage += (int64)md.damage * skill_damage / 100;
#endif

	battle_absorb_damage(target, &md);

	//Skill reflect gets calculated after all attack modifier
	battle_do_reflect(BF_MISC, &md, src, target, skill_id, skill_lv); //WIP [lighta]

	return md;
}

/*==========================================
 * Battle main entry, from skill_attack
 *------------------------------------------
 * Credits:
 *	Original coder unknown
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
struct Damage battle_calc_attack(int attack_type, struct block_list *src, struct block_list *target, uint16 skill_id, uint16 skill_lv, int flag)
{
	struct Damage d;

	switch(attack_type) {
		case BF_WEAPON: d = battle_calc_weapon_attack(src,target,skill_id,skill_lv,flag); break;
		case BF_MAGIC:  d = battle_calc_magic_attack(src,target,skill_id,skill_lv,flag);  break;
		case BF_MISC:   d = battle_calc_misc_attack(src,target,skill_id,skill_lv,flag);   break;
		default:
			ShowError("battle_calc_attack: unknown attack: attack_type=%d skill_id=%d\n",attack_type,skill_id);
			memset(&d,0,sizeof(d));
			break;
	}

	if(d.damage + d.damage2 < 1) { //Miss/Absorbed
		//Weapon attacks should go through to cause additional effects
		if(d.dmg_lv == ATK_DEF /*&& attack_type&(BF_MAGIC|BF_MISC)*/) //Isn't it that additional effects don't apply if miss?
			d.dmg_lv = ATK_MISS;
		d.dmotion = 0;
	} else //Some skills like Weaponry Research will cause damage even if attack is dodged
		d.dmg_lv = ATK_DEF;

	return d;
}

/*==========================================
 * Final damage return function
 *------------------------------------------
 * Credits:
 *	Original coder unknown
 *	Initial refactoring by Baalberith
 *	Refined and optimized by helvetica
 */
int64 battle_calc_return_damage(struct block_list *bl, struct block_list *src, int64 *dmg, int flag, uint16 skill_id, bool status_reflect) {
	struct map_session_data *sd = BL_CAST(BL_PC,bl);
	struct status_change *sc = status_get_sc(bl);
	struct status_change *ssc = status_get_sc(src);
	int64 rdamage = 0, damage = *dmg;
#ifdef RENEWAL
	int max_rdamage = status_get_max_hp(bl);
#endif

#ifdef RENEWAL
	#define CAP_RDAMAGE(d) ( (d) = cap_value((d),1,max_rdamage) )
#else
	#define CAP_RDAMAGE(d) ( (d) = max((d),1) )
#endif

	if( (flag&(BF_SHORT|BF_MAGIC)) == BF_SHORT ) { //Bounces back part of the damage
		if( !status_reflect && sd && sd->bonus.short_weapon_damage_return ) {
			rdamage += damage * sd->bonus.short_weapon_damage_return / 100;
			CAP_RDAMAGE(rdamage);
		} else if( status_reflect && sc && sc->count ) {
			struct status_change_entry *sce;

			if( (sce = sc->data[SC_REFLECTSHIELD]) || (sce = battle_check_shadowform(bl,SC_REFLECTSHIELD)) ) {
				struct status_change_entry *sce_d = sc->data[SC_DEVOTION];
				struct status_change_entry *sce_s = sc->data[SC__SHADOWFORM];
				struct map_session_data *s_sd = NULL;
				struct block_list *d_bl = NULL;

				if( sce_d && (d_bl = map_id2bl(sce_d->val1)) &&
					((d_bl->type == BL_MER && ((TBL_MER *)d_bl)->master && ((TBL_MER *)d_bl)->master->bl.id == bl->id) ||
					(d_bl->type == BL_PC && ((TBL_PC *)d_bl)->devotion[sce_d->val2] == bl->id)) )
				{ //Don't reflect non-skill attack if has SC_REFLECTSHIELD from Devotion bonus inheritance
					if( (!skill_id && battle_config.devotion_rdamage_skill_only && sce->val4) ||
						!check_distance_bl(bl,d_bl,sce_d->val3) )
						return 0;
				} else if( sce_s && (s_sd = map_id2sd(sce_s->val2)) && s_sd->shadowform_id == bl->id && !skill_id )
					return 0;
#ifndef RENEWAL
				if( !(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK) )
					return 0;
				switch( skill_id ) {
					case KN_PIERCE:
					case ML_PIERCE:
						{
							struct status_data *sstatus = status_get_status_data(bl);
							short count = sstatus->size + 1;

							damage = damage / count;
						}
						break;
				}
#endif
				rdamage += damage * sce->val2 / 100;
				CAP_RDAMAGE(rdamage);
			}
			if( (sce = sc->data[SC_REFLECTDAMAGE]) && rnd()%100 < 30 + 10 * sce->val1 &&
#ifdef RENEWAL
				skill_id != WS_CARTTERMINATION &&
#endif
				!(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK) &&
				!(skill_get_inf2(skill_id)&INF2_TRAP) )
			{
				rdamage += damage * sce->val2 / 100;
#ifdef RENEWAL
				max_rdamage = max_rdamage * status_get_lv(bl) / 100;
#endif
				CAP_RDAMAGE(rdamage);
				if( --(sce->val3) <= 0 )
					status_change_end(bl,SC_REFLECTDAMAGE,INVALID_TIMER);
			}
			if( ((sce = sc->data[SC_SHIELDSPELL_DEF]) || (sce = battle_check_shadowform(bl,SC_SHIELDSPELL_DEF))) &&
				sce->val1 == 2 && !is_boss(src) ) {
				rdamage += damage * sce->val2 / 100;
				CAP_RDAMAGE(rdamage);
			}
		}
	} else {
		if( !status_reflect && sd && sd->bonus.long_weapon_damage_return ) {
			rdamage += damage * sd->bonus.long_weapon_damage_return / 100;
			CAP_RDAMAGE(rdamage);
		}
	}

	if( ssc && ssc->data[SC_INSPIRATION] ) {
		rdamage += damage / 100;
		CAP_RDAMAGE(rdamage);
	}

	if( sc && sc->data[SC_KYOMU] && !sc->data[SC_SHIELDSPELL_DEF] )
		return 0; //Nullify reflecting ability except for Shield Spell DEF

	return rdamage;
#undef CAP_RDAMAGE
}

/**
 * Calculate vanish from target
 * @param sd: Player with vanish item
 * @param target: Target to vanish HP/SP
 * @param wd: Reference to Damage struct
 */
void battle_vanish(struct map_session_data *sd, struct block_list *target, struct Damage *wd)
{
	struct status_data *tstatus;
	int race;

	nullpo_retv(sd);
	nullpo_retv(target);
	nullpo_retv(wd);

	tstatus = status_get_status_data(target);
	race = status_get_race(target);
	wd->isvanishdamage = false;
	wd->isspdamage = false;

	if( wd->flag ) {
		short vellum_rate_hp = cap_value(sd->hp_vanish_race[race].rate + sd->hp_vanish_race[RC_ALL].rate, 0, SHRT_MAX);
		short vellum_hp = cap_value(sd->hp_vanish_race[race].per + sd->hp_vanish_race[RC_ALL].per, SHRT_MIN, SHRT_MAX);
		short vellum_rate_sp = cap_value(sd->sp_vanish_race[race].rate + sd->sp_vanish_race[RC_ALL].rate, 0, SHRT_MAX);
		short vellum_sp = cap_value(sd->sp_vanish_race[race].per + sd->sp_vanish_race[RC_ALL].per, SHRT_MIN, SHRT_MAX);

		//The HP and SP vanish bonus from these items can't stack because of the special damage display
		if( vellum_hp && vellum_rate_hp && (vellum_rate_hp >= 1000 || rnd()%1000 < vellum_rate_hp) ) {
			wd->damage = apply_rate(tstatus->max_hp, vellum_hp);
			wd->damage2 = 0;
			wd->isvanishdamage = true;
		} else if( vellum_sp && vellum_rate_sp && (vellum_rate_sp >= 1000 || rnd()%1000 < vellum_rate_sp) ) {
			wd->damage = apply_rate(tstatus->max_sp, vellum_sp);
			wd->damage2 = 0;
			wd->isvanishdamage = true;
			wd->isspdamage = true;
		}
		if( wd->type == DMG_CRITICAL && wd->isvanishdamage )
			wd->type = DMG_NORMAL;
	} else {
		short vrate_hp = cap_value(sd->bonus.hp_vanish_rate, 0, SHRT_MAX);
		short v_hp = cap_value(sd->bonus.hp_vanish_per, SHRT_MIN, SHRT_MAX);
		short vrate_sp = cap_value(sd->bonus.sp_vanish_rate, 0, SHRT_MAX);
		short v_sp = cap_value(sd->bonus.sp_vanish_per, SHRT_MIN, SHRT_MAX);

		if( v_hp && vrate_hp && (vrate_hp >= 1000 || rnd()%1000 < vrate_hp) )
			v_hp = -v_hp;
		else
			v_hp = 0;
		if( v_sp && vrate_sp && (vrate_sp >= 1000 || rnd()%1000 < vrate_sp) )
			v_sp = -v_sp;
		else
			v_sp = 0;
		if( v_hp < 0 || v_sp < 0 )
			status_percent_damage(&sd->bl, target, (int8)v_hp, (int8)v_sp, false);
	}
}

/*===========================================
 * Perform battle drain effects (HP/SP loss)
 *-------------------------------------------*/
void battle_drain(struct map_session_data *sd, struct block_list *tbl, int64 rdamage, int64 ldamage, int race, int class_)
{
	struct weapon_data *wd;
	int64 *damage;
	struct Damage d;
	int thp = 0, //HP gained by attacked
		tsp = 0, //SP gained by attacked
		hp = 0, sp = 0;
	uint8 i = 0;

	if( !CHK_RACE(race) && !CHK_CLASS(class_) )
		return;

	memset(&d, 0, sizeof(d));

	//Check for vanish HP/SP
	battle_vanish(sd, tbl, &d);

	//Check for drain HP/SP
	hp = sp = i = 0;
	for( i = 0; i < 4; i++ ) {
		if( i < 2 ) { //First two iterations: Right hand
			wd = &sd->right_weapon;
			damage = &rdamage;
		} else {
			wd = &sd->left_weapon;
			damage = &ldamage;
		}
		if( *damage <= 0 )
			continue;
		if( i == 1 || i == 3 ) {
			hp = wd->hp_drain_class[class_] + wd->hp_drain_class[CLASS_ALL];
			hp += battle_calc_drain(*damage, wd->hp_drain_rate.rate, wd->hp_drain_rate.per);
			sp = wd->sp_drain_class[class_] + wd->sp_drain_class[CLASS_ALL];
			sp += battle_calc_drain(*damage, wd->sp_drain_rate.rate, wd->sp_drain_rate.per);
			if( hp )
				thp += hp;
			if( sp )
				tsp += sp;
		} else {
			hp = wd->hp_drain_race[race] + wd->hp_drain_race[RC_ALL];
			sp = wd->sp_drain_race[race] + wd->sp_drain_race[RC_ALL];
			if( hp )
				thp += hp;
			if( sp )
				tsp += sp;
		}
	}

	if( !thp && !tsp )
		return;

	status_heal(&sd->bl, thp, tsp, (battle_config.show_hp_sp_drain ? 2 : 0));
}

/*===========================================
 * Deals the same damage to targets in area.
 *-------------------------------------------
 * Credits:
 *	Original coder pakpil
 */
int battle_damage_area(struct block_list *bl, va_list ap) {
	unsigned int tick;
	int64 damage;
	int amotion, dmotion;
	struct block_list *src;

	nullpo_ret(bl);

	tick = va_arg(ap, unsigned int);
	src = va_arg(ap, struct block_list *);
	amotion = va_arg(ap, int);
	dmotion = va_arg(ap, int);
	damage = va_arg(ap, int);

	if( bl->type == BL_MOB && ((TBL_MOB *)bl)->mob_id == MOBID_EMPERIUM )
		return 0;
	if( bl->id != src->id && battle_check_target(src, bl, BCT_ENEMY) > 0 ) {
		map_freeblock_lock();
		if( src->type == BL_PC )
			battle_drain((TBL_PC *)src, bl, damage, damage, status_get_race(bl), status_get_class_(bl));
		if( amotion )
			battle_delay_damage(tick, amotion, src, bl, 0, CR_REFLECTSHIELD, 0, damage, ATK_DEF, 0, true, false);
		else
			status_fix_damage(src, bl, damage, 0);
		clif_damage(bl, bl, tick, amotion, dmotion, damage, 1, DMG_ENDURE, 0, false);
		if( !(src->type == BL_PC && ((TBL_PC *)src)->state.autocast) )
			skill_additional_effect(src, bl, CR_REFLECTSHIELD, 1, BF_WEAPON|BF_SHORT|BF_NORMAL, ATK_DEF, tick);
		map_freeblock_unlock();
	}

	return 0;
}
/*==========================================
 * Do a basic physical attack (call through unit_attack_timer)
 *------------------------------------------*/
enum damage_lv battle_weapon_attack(struct block_list *src, struct block_list *target, unsigned int tick, int flag) {
	struct map_session_data *sd = NULL, *tsd = NULL;
	struct status_data *sstatus, *tstatus;
	struct status_change *sc, *tsc;
	int64 damage;
	uint16 lv;
	struct Damage wd;

	nullpo_retr(ATK_NONE,src);
	nullpo_retr(ATK_NONE,target);

	if (!src->prev || !target->prev)
		return ATK_NONE;

	sd = BL_CAST(BL_PC,src);
	tsd = BL_CAST(BL_PC,target);

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(target);

	sc = status_get_sc(src);
	tsc = status_get_sc(target);

	if (sc && !sc->count) //Avoid sc checks when there's none to check for [Skotlex]
		sc = NULL;

	if (tsc && !tsc->count)
		tsc = NULL;

	if (sd) {
		sd->state.arrow_atk = (sd->status.weapon == W_BOW || (sd->status.weapon >= W_REVOLVER && sd->status.weapon <= W_GRENADE));
		if (sd->state.arrow_atk) {
			short index = sd->equip_index[EQI_AMMO];

			if (index < 0 || !sd->inventory_data[index])
				return ATK_NONE;
		}
	}

	if (sc && sc->count) {
		if (sc->data[SC_CLOAKING] && !(sc->data[SC_CLOAKING]->val4&2))
			status_change_end(src,SC_CLOAKING,INVALID_TIMER);
		else if (sc->data[SC_CLOAKINGEXCEED] && !(sc->data[SC_CLOAKINGEXCEED]->val4&2))
			status_change_end(src,SC_CLOAKINGEXCEED,INVALID_TIMER);
	}

	if (tsc && tsc->data[SC_AUTOCOUNTER] && status_check_skilluse(target,src,KN_AUTOCOUNTER,1)) {
		uint8 dir = map_calc_dir(target,src->x,src->y);
		int t_dir = unit_getdir(target);
		int dist = distance_bl(src,target);

		if (dist <= 0 || (!map_check_dir(dir,t_dir) && dist <= tstatus->rhw.range + 1)) {
			uint16 skill_lv = tsc->data[SC_AUTOCOUNTER]->val1;

			clif_skillcastcancel(target); //Remove the casting bar [Skotlex]
			clif_damage(src,target,tick,sstatus->amotion,1,0,1,DMG_NORMAL,0,false); //Display miss
			status_change_end(target,SC_AUTOCOUNTER,INVALID_TIMER);
			skill_attack(BF_WEAPON,target,target,src,KN_AUTOCOUNTER,skill_lv,tick,0);
			return ATK_BLOCK;
		}
	}

	if (tsc && tsc->data[SC_BLADESTOP_WAIT] && !is_boss(src) && (src->type == BL_PC || !tsd ||
		distance_bl(src,target) <= (tsd->status.weapon == W_FIST ? 1 : 2))) {
		uint16 skill_lv = tsc->data[SC_BLADESTOP_WAIT]->val1;
		int duration = skill_get_time2(MO_BLADESTOP,skill_lv);

		status_change_end(target,SC_BLADESTOP_WAIT,INVALID_TIMER);
		//Target locked
		if (sc_start4(src,src,SC_BLADESTOP,100,(sd ? pc_checkskill(sd,MO_BLADESTOP) : 5),0,0,target->id,duration)) {
			clif_damage(src,target,tick,sstatus->amotion,1,0,1,DMG_NORMAL,0,false);
			clif_bladestop(target,src->id,1);
			sc_start4(src,target,SC_BLADESTOP,100,skill_lv,0,0,src->id,duration);
			return ATK_BLOCK;
		}
	}

	if (sd && (lv = pc_checkskill(sd,MO_TRIPLEATTACK)) > 0) {
		int rate = 30 - lv; //Base rate

		if (sc && sc->data[SC_SKILLRATE_UP] && sc->data[SC_SKILLRATE_UP]->val1 == MO_TRIPLEATTACK) {
			rate += rate * sc->data[SC_SKILLRATE_UP]->val2 / 100;
			status_change_end(src,SC_SKILLRATE_UP,INVALID_TIMER);
		}
		if (rnd()%100 < rate) { //Need to apply canact_tick here because it doesn't go through skill_castend_id
			sd->ud.canact_tick = max(tick + skill_delayfix(src,MO_TRIPLEATTACK,lv),sd->ud.canact_tick);
			if (skill_attack(BF_WEAPON,src,src,target,MO_TRIPLEATTACK,lv,tick,0))
				return ATK_DEF;
			return ATK_MISS;
		}
	}

	if (sc) {
		if (sc->data[SC_SACRIFICE]) {
			uint16 skill_lv = sc->data[SC_SACRIFICE]->val1;
			damage_lv ret_val;

			if (--(sc->data[SC_SACRIFICE]->val2) <= 0)
				status_change_end(src,SC_SACRIFICE,INVALID_TIMER);
			//We need to calculate the DMG before the hp reduction,because it can kill the source
			//For further information: bugreport:4950
			ret_val = (damage_lv)skill_attack(BF_WEAPON,src,src,target,PA_SACRIFICE,skill_lv,tick,0);
			status_zap(src,sstatus->max_hp * 9 / 100,0); //Damage to self is always 9%
			if (ret_val == ATK_NONE)
				return ATK_MISS;
			return ret_val;
		}
		if (sc->data[SC_MAGICALATTACK]) {
			if (skill_attack(BF_MAGIC,src,src,target,NPC_MAGICALATTACK,sc->data[SC_MAGICALATTACK]->val1,tick,0))
				return ATK_DEF;
			return ATK_MISS;
		}
	}

	if (tsc) {
		if (tsc->data[SC_MTF_MLEATKED] && rnd()%100 < tsc->data[SC_MTF_MLEATKED]->val3)
			clif_skill_nodamage(target,target,SM_ENDURE,tsc->data[SC_MTF_MLEATKED]->val2,
				sc_start(target,target,SC_ENDURE,100,tsc->data[SC_MTF_MLEATKED]->val2,skill_get_time(SM_ENDURE,tsc->data[SC_MTF_MLEATKED]->val2)));
		if (tsc->data[SC_KAAHI] && tstatus->hp < tstatus->max_hp && status_charge(target,0,tsc->data[SC_KAAHI]->val3)) {
			int hp_heal = tstatus->max_hp - tstatus->hp;

			if (hp_heal > tsc->data[SC_KAAHI]->val2)
				hp_heal = tsc->data[SC_KAAHI]->val2;
			if (hp_heal)
				status_heal(target,hp_heal,0,2);
		}
	}

	wd = battle_calc_attack(BF_WEAPON,src,target,0,0,flag);

	if (sc && sc->count) { //Consume the status even if missed
		if (sc->data[SC_EXEEDBREAK])
			status_change_end(src,SC_EXEEDBREAK,INVALID_TIMER);
		if (sc->data[SC_SPELLFIST] && --(sc->data[SC_SPELLFIST]->val2) < 0)
			status_change_end(src,SC_SPELLFIST,INVALID_TIMER);
	}

	if (sd) {
		if (battle_config.ammo_decrement && sc && sc->data[SC_FEARBREEZE] && sc->data[SC_FEARBREEZE]->val4 > 0) {
			short idx = sd->equip_index[EQI_AMMO];

			if (idx >= 0 && sd->status.inventory[idx].amount >= sc->data[SC_FEARBREEZE]->val4) {
				pc_delitem(sd,idx,sc->data[SC_FEARBREEZE]->val4,0,1,LOG_TYPE_CONSUME);
				sc->data[SC_FEARBREEZE]->val4 = 0;
			}
		}
		if (sd->state.arrow_atk) //Consume arrow
			battle_consume_ammo(sd,0,0);
	}

	damage = wd.damage + wd.damage2;

	if (damage > 0 && target->id != src->id) {
		if (sc && sc->data[SC_DUPLELIGHT] && wd.flag&BF_SHORT &&
			rnd()%100 <= 10 + 2 * sc->data[SC_DUPLELIGHT]->val1) { //Activates it only from melee damage
			uint16 skill_id;

			if (rnd()%2 == 1)
				skill_id = AB_DUPLELIGHT_MELEE;
			else
				skill_id = AB_DUPLELIGHT_MAGIC;
			skill_attack(skill_get_type(skill_id),src,src,target,skill_id,sc->data[SC_DUPLELIGHT]->val1,tick,flag);
		}
	}

	wd.dmotion = clif_damage(src,target,tick,wd.amotion,wd.dmotion,wd.damage,wd.div_,(enum e_damage_type)wd.type,wd.damage2,wd.isspdamage);

	if (damage > 0) {
		if (sd && sd->bonus.splash_range)
			skill_castend_damage_id(src,target,0,1,tick,0);
		if (target->type == BL_SKILL) {
			struct skill_unit *su = (struct skill_unit *)target;
			struct skill_unit_group *sg;

			if (su && (sg = su->group)) {
				if (sg->skill_id == HT_BLASTMINE)
					skill_blown(src,target,3,-1,0);
				if (sg->skill_id == GN_WALLOFTHORN) {
					int right_element = battle_get_weapon_element(wd,src,target,0,0,EQI_HAND_R);

					if (--su->val2 <= 0)
						skill_delunit(su); //Max hits reached
					if (right_element == ELE_FIRE) {
						struct block_list *ssrc = map_id2bl(sg->src_id);

						if(ssrc) {
							sg->unit_id = UNT_USED_TRAPS;
							sg->limit = 0;
							ssrc->val1 = skill_get_time(sg->skill_id,sg->skill_lv) - DIFF_TICK(tick,sg->tick); //Fire Wall duration [exneval]
							skill_unitsetting(ssrc,sg->skill_id,sg->skill_lv,sg->val3>>16,sg->val3&0xffff,1);
						}
					}
				}
			}
		}
	}

	map_freeblock_lock();

	//Ignore shadow form status if get devoted [exneval]
	if (!(tsc && tsc->data[SC_DEVOTION]) && !wd.isvanishdamage && skill_check_shadowform(target,0,damage,wd.div_)) {
		if (!status_isdead(target))
			skill_additional_effect(src,target,0,0,wd.flag,wd.dmg_lv,tick);
		if (wd.dmg_lv > ATK_BLOCK)
			skill_counter_additional_effect(src,target,0,0,wd.flag,tick);
	} else
		battle_delay_damage(tick,wd.amotion,src,target,wd.flag,0,0,damage,wd.dmg_lv,wd.dmotion,true,wd.isspdamage);

	if (tsc) {
		if (tsc->data[SC_DEVOTION]) {
			struct status_change_entry *sce_d = tsc->data[SC_DEVOTION];
			struct block_list *d_bl = map_id2bl(sce_d->val1);

			if (d_bl &&
				((d_bl->type == BL_MER && ((TBL_MER *)d_bl)->master && ((TBL_MER *)d_bl)->master->bl.id == target->id) ||
				(d_bl->type == BL_PC && ((TBL_PC *)d_bl)->devotion[sce_d->val2] == target->id)) &&
				check_distance_bl(target,d_bl,sce_d->val3))
			{
				clif_damage(d_bl,d_bl,tick,0,0,damage,0,DMG_NORMAL,0,false);
				status_fix_damage(NULL,d_bl,damage,0);
			} else
				status_change_end(target,SC_DEVOTION,INVALID_TIMER);
		}
		if (tsc->data[SC_CIRCLE_OF_FIRE_OPTION] && wd.flag&BF_SHORT && target->type == BL_PC) {
			struct status_change_entry *sce_e = tsc->data[SC_CIRCLE_OF_FIRE_OPTION];
			struct elemental_data *ed = ((TBL_PC *)target)->ed;

			if (ed) {
				clif_skill_damage(&ed->bl,target,tick,status_get_amotion(src),0,-30000,1,EL_CIRCLE_OF_FIRE,sce_e->val1,DMG_SKILL);
				skill_attack(BF_WEAPON,&ed->bl,&ed->bl,src,EL_CIRCLE_OF_FIRE,sce_e->val1,tick,flag);
			}
		}
		if (tsc->data[SC_WATER_SCREEN_OPTION]) {
			struct status_change_entry *sce_e = tsc->data[SC_WATER_SCREEN_OPTION];
			struct block_list *e_bl = map_id2bl(sce_e->val1);

			if (e_bl) {
				clif_damage(e_bl,e_bl,tick,0,0,damage,wd.div_,DMG_NORMAL,0,false);
				status_fix_damage(NULL,e_bl,damage,0);
			}
		}
		if (tsc->data[SC_MAXPAIN]) {
			battle_damage_temp[0] = damage;
			skill_castend_damage_id(target,src,NPC_MAXPAIN_ATK,tsc->data[SC_MAXPAIN]->val1,tick,flag);
		}
	}

	if (sc && sc->data[SC_AUTOSPELL] && rnd()%100 < sc->data[SC_AUTOSPELL]->val4) {
		int sp = 0, i = rnd()%100;
		uint16 skill_id = sc->data[SC_AUTOSPELL]->val2,
			skill_lv = sc->data[SC_AUTOSPELL]->val3;

		if (sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_SAGE)
			i = 0; //Max chance, no skill_lv reduction [Skotlex]
		//Reduction only for skill_lv > 1
		if (skill_lv > 1) {
			if (i >= 50)
				skill_lv /= 2;
			else if (i >= 15)
				skill_lv--;
		}
		sp = skill_get_sp(skill_id,skill_lv) * 2 / 3;
		if (status_charge(src,0,sp)) {
			switch (skill_get_casttype(skill_id)) {
				case CAST_GROUND:
					skill_castend_pos2(src,target->x,target->y,skill_id,skill_lv,tick,flag);
					break;
				case CAST_NODAMAGE:
					skill_castend_nodamage_id(src,target,skill_id,skill_lv,tick,flag);
					break;
				case CAST_DAMAGE:
					skill_castend_damage_id(src,target,skill_id,skill_lv,tick,flag);
					break;
			}
		}
	}

	if (sd) {
		if (sc && sc->data[SC__AUTOSHADOWSPELL] && wd.flag&BF_SHORT && rnd()%100 < sc->data[SC__AUTOSHADOWSPELL]->val3 &&
			sd->status.skill[sc->data[SC__AUTOSHADOWSPELL]->val1].id != 0 &&
			sd->status.skill[sc->data[SC__AUTOSHADOWSPELL]->val1].flag == SKILL_FLAG_PLAGIARIZED)
		{
			int r_skill = sd->status.skill[sc->data[SC__AUTOSHADOWSPELL]->val1].id,
				r_lv = sc->data[SC__AUTOSHADOWSPELL]->val2, type;

				if ((type = skill_get_casttype(r_skill)) == CAST_GROUND) {
					int maxcount = 0;

					if (!(BL_PC&battle_config.skill_reiteration) &&
						skill_get_unit_flag(r_skill)&UF_NOREITERATION)
						type = -1;
					if (BL_PC&battle_config.skill_nofootset &&
						skill_get_unit_flag(r_skill)&UF_NOFOOTSET)
						type = -1;
					if (BL_PC&battle_config.land_skill_limit &&
						(maxcount = skill_get_maxcount(r_skill,r_lv)) > 0) {
						int v;

						for (v = 0; v < MAX_SKILLUNITGROUP && sd->ud.skillunit[v] && maxcount; v++) {
							if (sd->ud.skillunit[v]->skill_id == r_skill)
								maxcount--;
						}
						if (maxcount == 0)
							type = -1;
					}
					if (type != CAST_GROUND) {
						clif_skill_fail(sd,r_skill,USESKILL_FAIL_LEVEL,0,0);
						map_freeblock_unlock();
						return wd.dmg_lv;
					}
				}
				sd->state.autocast = 1;
				skill_consume_requirement(sd,r_skill,r_lv,1);
				switch (type) {
					case CAST_GROUND:
						skill_castend_pos2(src,target->x,target->y,r_skill,r_lv,tick,flag);
						break;
					case CAST_DAMAGE:
						skill_castend_damage_id(src,target,r_skill,r_lv,tick,flag);
						break;
					case CAST_NODAMAGE:
						skill_castend_nodamage_id(src,target,r_skill,r_lv,tick,flag);
						break;
				}
				sd->state.autocast = 0;
				sd->ud.canact_tick = max(tick + skill_delayfix(src,r_skill,r_lv),sd->ud.canact_tick);
				clif_status_change(src,SI_POSTDELAY,1,skill_delayfix(src,r_skill,r_lv),0,0,1);
		}
		if (wd.flag&BF_WEAPON && target->id != src->id && damage > 0) {
			if (battle_config.left_cardfix_to_right)
				battle_drain(sd,target,wd.damage,wd.damage,tstatus->race,tstatus->class_);
			else
				battle_drain(sd,target,wd.damage,wd.damage2,tstatus->race,tstatus->class_);
		}
	}

	if (tsc) {
		if (damage > 0 && tsc->data[SC_POISONREACT] &&
			(rnd()%100 < tsc->data[SC_POISONREACT]->val3 ||
			sstatus->def_ele == ELE_POISON) &&
			//check_distance_bl(src,target,tstatus->rhw.range + 1) && //Doesn't checks range!
			status_check_skilluse(target,src,TF_POISON,0))
		{ //Poison React
			struct status_change_entry *sce = tsc->data[SC_POISONREACT];

			if (sstatus->def_ele == ELE_POISON) {
				sce->val2 = 0;
				skill_attack(BF_WEAPON,target,target,src,AS_POISONREACT,sce->val1,tick,0);
			} else {
				skill_attack(BF_WEAPON,target,target,src,TF_POISON,5,tick,0);
				--sce->val2;
			}
			if (sce->val2 <= 0)
				status_change_end(target,SC_POISONREACT,INVALID_TIMER);
		}
	}

	map_freeblock_unlock();
	return wd.dmg_lv;
}

/*=========================
 * Check for undead status
 *-------------------------
 * Credits:
 *	Original coder Skotlex
 *  Refactored by Baalberith
 */
int battle_check_undead(int race,int element)
{
	if(!battle_config.undead_detect_type) {
		if(element == ELE_UNDEAD)
			return 1;
	} else if(battle_config.undead_detect_type == 1) {
		if(race == RC_UNDEAD)
			return 1;
	} else {
		if(element == ELE_UNDEAD || race == RC_UNDEAD)
			return 1;
	}
	return 0;
}

/*================================================================
 * Returns the upmost level master starting with the given object
 *----------------------------------------------------------------*/
struct block_list *battle_get_master(struct block_list *src)
{
	struct block_list *prev; //Used for infinite loop check (master of yourself?)

	do {
		prev = src;
		switch (src->type) {
			case BL_PET:
				if (((TBL_PET *)src)->master)
					src = (struct block_list *)((TBL_PET *)src)->master;
				break;
			case BL_MOB:
				if (((TBL_MOB *)src)->master_id)
					src = map_id2bl(((TBL_MOB *)src)->master_id);
				break;
			case BL_HOM:
				if (((TBL_HOM *)src)->master)
					src = (struct block_list *)((TBL_HOM *)src)->master;
				break;
			case BL_MER:
				if (((TBL_MER *)src)->master)
					src = (struct block_list *)((TBL_MER *)src)->master;
				break;
			case BL_ELEM:
				if (((TBL_ELEM *)src)->master)
					src = (struct block_list *)((TBL_ELEM *)src)->master;
				break;
			case BL_SKILL:
				if (((TBL_SKILL *)src)->group && ((TBL_SKILL *)src)->group->src_id)
					src = map_id2bl(((TBL_SKILL *)src)->group->src_id);
				break;
		}
	} while (src && src != prev);
	return prev;
}

/*==========================================
 * Checks the state between two targets
 * (enemy, friend, party, guild, etc)
 *------------------------------------------
 * Usage:
 * See battle.h for possible values/combinations
 * to be used here (BCT_* constants)
 * Return value is:
 * 1: flag holds true (is enemy, party, etc)
 * -1: flag fails
 * 0: Invalid target (non-targetable ever)
 *
 * Credits:
 *	Original coder unknown
 *	Rewritten by Skotlex
 */
int battle_check_target(struct block_list *src, struct block_list *target, int flag)
{
	int16 m; //Map
	int state = 0; //Initial state none
	int strip_enemy = 1; //Flag which marks whether to remove the BCT_ENEMY status if it's also friend/ally
	struct block_list *s_bl = NULL;
	struct block_list *t_bl = NULL;
	struct unit_data *ud = NULL;

	nullpo_ret(src);
	nullpo_ret(target);

	s_bl = src;
	t_bl = target;

	ud = unit_bl2ud(target);
	m = target->m;

	//s_bl/t_bl hold the 'master' of the attack, while src/target are the actual objects involved
	if( !(s_bl = battle_get_master(src)) )
		s_bl = src;

	if( !(t_bl = battle_get_master(target)) )
		t_bl = target;

	if( s_bl->type == BL_PC ) {
		switch( t_bl->type ) {
			case BL_MOB: //Source => PC, Target => MOB
				if( pc_has_permission((TBL_PC *)s_bl, PC_PERM_DISABLE_PVM) )
					return 0;
				break;
			case BL_PC:
				if( pc_has_permission((TBL_PC *)s_bl, PC_PERM_DISABLE_PVP) )
					return 0;
				break;
			default: //Anything else goes
				break;
		}
	}

	switch( target->type ) { //Checks on actual target
		case BL_PC: {
				struct status_change *sc = status_get_sc(src);

				if( (((TBL_PC *)target)->invincible_timer != INVALID_TIMER || pc_isinvisible((TBL_PC *)target)) && 
					!(flag&BCT_NOENEMY) )
					return -1; //Cannot be targeted yet
				if( sc && sc->count && sc->data[SC_VOICEOFSIREN] && sc->data[SC_VOICEOFSIREN]->val2 == target->id )
					return -1;
			}
			break;
		case BL_MOB: {
				struct mob_data *md = ((TBL_MOB *)target);

				if( ud ) {
					if( ud->immune_attack )
						return 0;
					if( ud->immune_attack2 && !battle_getcurrentskill(s_bl) && battle_check_range(s_bl, target, 2) )
						return 0;
				}
				if( ((md->special_state.ai == AI_SPHERE || //Marine Sphere
					(md->special_state.ai == AI_FLORA && battle_config.summon_flora&1) || //Flora
					(md->special_state.ai == AI_ZANZOU && map_flag_vs(md->bl.m)) || //Zanzou
					md->special_state.ai == AI_FAW) && //FAW
					s_bl->type == BL_PC && src->type != BL_MOB) )
				{ //Targettable by players
					state |= BCT_ENEMY;
					strip_enemy = 0;
				}
			}
			break;
		case BL_SKILL: {
				TBL_SKILL *su = ((TBL_SKILL *)target);
				uint16 skill_id = battle_getcurrentskill(src);

				if( !su || !su->group )
					return 0;
				if( (skill_get_inf2(su->group->skill_id)&INF2_TRAP) && su->group->unit_id != UNT_USED_TRAPS ) {
					if( !skill_id ) {
						;
					} else if( skill_get_inf2(skill_id)&INF2_HIT_TRAP ) { //Only a few skills can target traps
						switch( skill_id ) {
							case RK_DRAGONBREATH:
							case RK_DRAGONBREATH_WATER:
							case NC_SELFDESTRUCTION:
							case NC_AXETORNADO:
							case SR_SKYNETBLOW:
								if( !map[m].flag.pvp && !map[m].flag.gvg )
									return 0; //Can only hit traps in PVP/GVG maps
								break;
						}
					} else
						return 0;
					state |= BCT_ENEMY;
					strip_enemy = 0;
				} else if( su->group->skill_id == WZ_ICEWALL || (su->group->skill_id == GN_WALLOFTHORN && su->group->unit_id != UNT_FIREWALL) ) {
					switch( skill_id ) {
						case RK_DRAGONBREATH:
						case RK_DRAGONBREATH_WATER:
						case NC_SELFDESTRUCTION:
						case NC_AXETORNADO:
						case SR_SKYNETBLOW:
							if( !map[m].flag.pvp && !map[m].flag.gvg )
								return 0;
							break;
						case HT_CLAYMORETRAP:
							return 0; //Can't hit icewall
						default:
							if( (flag&BCT_ALL) == BCT_ALL && !(skill_get_inf2(skill_id)&INF2_HIT_TRAP) )
								return -1; //Usually BCT_ALL stands for only hitting chars, but skills specifically set to hit traps also hit icewall
							break;
					}
					state |= BCT_ENEMY;
					strip_enemy = 0;
				} else //Excepting traps, icewall, wall of thornes, you should not be able to target skills
					return 0;
			}
			break;
		case BL_MER:
		case BL_HOM:
		case BL_ELEM:
			if( ud && ud->immune_attack )
				return 0;
			break;
		default: //All else not specified is an invalid target
			return 0;
	}

	switch( t_bl->type ) { //Checks on target master
		case BL_PC: {
				struct map_session_data *sd = NULL;
				struct status_change *sc = NULL;

				if( t_bl == s_bl )
					break;
				sd = BL_CAST(BL_PC, t_bl);
				sc = status_get_sc(t_bl);
				if( (sd->state.monster_ignore || (sc->data[SC_KINGS_GRACE] && (src->type != BL_PC || !battle_getcurrentskill(s_bl)))) && (flag&BCT_ENEMY) )
					return 0; //Global immunity only to attacks
				if( sd->status.karma && s_bl->type == BL_PC && ((TBL_PC *)s_bl)->status.karma )
					state |= BCT_ENEMY; //Characters with bad karma may fight amongst them
				if( sd->state.killable ) {
					state |= BCT_ENEMY; //Everything can kill it
					strip_enemy = 0;
				}
			}
			break;
		case BL_MOB: {
				struct mob_data *md = BL_CAST(BL_MOB, t_bl);

				if( !((agit_flag || agit2_flag) && map[m].flag.gvg_castle) &&
					md->guardian_data && (md->guardian_data->g || md->guardian_data->castle->guild_id) )
					return 0; //Disable guardians/emperium owned by Guilds on non-woe times
			}
			break;
	}

	switch( src->type ) { //Checks on actual src type
		case BL_PET:
			if( flag&BCT_ENEMY ) {
				if( t_bl->type != BL_MOB )
					return 0; //Pet may not attack non-mobs
				if( t_bl->type == BL_MOB && ((TBL_MOB *)t_bl)->guardian_data )
					return 0; //Pet may not attack Guardians/Emperium
			}
			break;
		case BL_SKILL: {
				struct skill_unit *su = (struct skill_unit *)src;
				struct status_change *tsc = status_get_sc(target);
				int inf2;

				if( !su || !su->group )
					return 0;
				inf2 = skill_get_inf2(su->group->skill_id);
				if( su->group->src_id == target->id ) {
					if( inf2&INF2_NO_TARGET_SELF )
						return -1;
					if( inf2&INF2_TARGET_SELF )
						return 1;
				}
				//Status changes that prevent traps from triggering
				if( (inf2&INF2_TRAP) && tsc && tsc->count && tsc->data[SC_SIGHTBLASTER] &&
					tsc->data[SC_SIGHTBLASTER]->val2 > 0 && !(tsc->data[SC_SIGHTBLASTER]->val4%2) )
					return -1;
			}
			break;
		case BL_MER:
			if( t_bl->type == BL_MOB && ((TBL_MOB *)t_bl)->mob_id == MOBID_EMPERIUM && (flag&BCT_ENEMY) )
				return 0; //Mercenary may not attack Emperium
			break;
	}

	switch( s_bl->type ) { //Checks on source master
		case BL_PC: {
				struct map_session_data *sd = BL_CAST(BL_PC, s_bl);

				if( t_bl->id != s_bl->id ) {
					if( sd->state.killer ) {
						state |= BCT_ENEMY; //Can kill anything
						strip_enemy = 0;
					} else if( sd->duel_group && !((!battle_config.duel_allow_pvp && map[m].flag.pvp) ||
						(!battle_config.duel_allow_gvg && map_flag_gvg2(m))) ) {
						if( t_bl->type == BL_PC && (sd->duel_group == ((TBL_PC *)t_bl)->duel_group) )
							return (flag&BCT_ENEMY) ? 1 : -1; //Duel targets can ONLY be your enemy, nothing else
						else if( src->type != BL_SKILL || (flag&BCT_ALL) != BCT_ALL )
							return 0;
					}
				}
				if( map_flag_gvg2(m) && !sd->status.guild_id && t_bl->type == BL_MOB && ((TBL_MOB *)t_bl)->mob_id == MOBID_EMPERIUM )
					return 0; //If you don't belong to a guild, can't target emperium
				if( t_bl->type != BL_PC )
					state |= BCT_ENEMY; //Natural enemy
			}
			break;
		case BL_MOB: {
				struct mob_data *md = BL_CAST(BL_MOB, s_bl);

				if( !((agit_flag || agit2_flag) && map[m].flag.gvg_castle) &&
					md->guardian_data && (md->guardian_data->g || md->guardian_data->castle->guild_id) )
					return 0; //Disable guardians/emperium owned by Guilds on non-woe times
				if( !md->special_state.ai ) { //Normal mobs
					if( (target->type == BL_MOB && t_bl->type == BL_PC &&
						(((TBL_MOB *)target)->special_state.ai != AI_ATTACK && //Clone
						((TBL_MOB *)target)->special_state.ai != AI_ZANZOU && //Zanzou
						((TBL_MOB *)target)->special_state.ai != AI_FAW)) || //FAW
						(t_bl->type == BL_MOB && !((TBL_MOB *)t_bl)->special_state.ai) )
						state |= BCT_PARTY; //Normal mobs with no ai are friends
					else
						state |= BCT_ENEMY; //However, all else are enemies
				} else {
					if( t_bl->type == BL_MOB && !((TBL_MOB *)t_bl)->special_state.ai )
						state |= BCT_ENEMY; //Natural enemy for AI mobs are normal mobs
				}
			}
			break;
		default: //Need some sort of default behaviour for unhandled types
			if( t_bl->type != s_bl->type )
				state |= BCT_ENEMY;
			break;
	}

	if( (flag&BCT_ALL) == BCT_ALL ) { //All actually stands for all attackable chars, icewall and traps
		if( target->type&(BL_CHAR|BL_SKILL) )
			return 1;
		else
			return -1;
	}

	if( flag == BCT_NOONE ) //Why would someone use this? no clue
		return -1;

	if( t_bl == s_bl ) { //No need for further testing
		state |= (BCT_SELF|BCT_PARTY|BCT_GUILD);
		if( (state&BCT_ENEMY) && strip_enemy )
			state &= ~BCT_ENEMY;
		return (flag&state) ? 1 : -1;
	}

	if( map_flag_vs(m) ) { //Check rivalry settings
		int sbg_id = 0, tbg_id = 0;

		if( map[m].flag.battleground ) {
			sbg_id = bg_team_get_id(s_bl);
			tbg_id = bg_team_get_id(t_bl);
		}

		if( (flag&(BCT_PARTY|BCT_ENEMY)) ) {
			int s_party = status_get_party_id(s_bl);
			int s_guild = status_get_guild_id(s_bl);

			if( s_party && s_party == status_get_party_id(t_bl) && !(map[m].flag.pvp && map[m].flag.pvp_noparty) &&
				!(map_flag_gvg2(m) && map[m].flag.gvg_noparty && !(s_guild && s_guild == status_get_guild_id(t_bl))) &&
				(!map[m].flag.battleground || sbg_id == tbg_id) )
				state |= BCT_PARTY;
			else
				state |= BCT_ENEMY;
		}

		if( (flag&(BCT_GUILD|BCT_ENEMY)) ) {
			int s_guild = status_get_guild_id(s_bl);
			int t_guild = status_get_guild_id(t_bl);

			if( !(map[m].flag.pvp && map[m].flag.pvp_noguild) && s_guild &&
				t_guild && (s_guild == t_guild || (!(flag&BCT_SAMEGUILD) && guild_isallied(s_guild, t_guild))) &&
				(!map[m].flag.battleground || sbg_id == tbg_id) )
				state |= BCT_GUILD;
			else
				state |= BCT_ENEMY;
		}

		if( state&BCT_ENEMY && map[m].flag.battleground && sbg_id && sbg_id == tbg_id )
			state &= ~BCT_ENEMY;

		if( state&BCT_ENEMY && battle_config.pk_mode && !map_flag_gvg2(m) && s_bl->type == BL_PC && t_bl->type == BL_PC ) {
			TBL_PC *sd = (TBL_PC *)s_bl, *tsd = (TBL_PC *)t_bl;

			//Prevent novice engagement on pk_mode (feature by Valaris)
			if( (sd->class_&MAPID_UPPERMASK) == MAPID_NOVICE ||
				(tsd->class_&MAPID_UPPERMASK) == MAPID_NOVICE ||
				(int)sd->status.base_level < battle_config.pk_min_level ||
			  	(int)tsd->status.base_level < battle_config.pk_min_level ||
				(battle_config.pk_level_range &&
				abs((int)sd->status.base_level - (int)tsd->status.base_level) > battle_config.pk_level_range) )
				state &= ~BCT_ENEMY;
		}
	} else { //Non pvp/gvg, check party/guild settings
		if( (flag&BCT_PARTY) || (state&BCT_ENEMY) ) {
			int s_party = status_get_party_id(s_bl);

			if( s_party && s_party == status_get_party_id(t_bl) )
				state |= BCT_PARTY;
		}

		if( (flag&BCT_GUILD) || (state&BCT_ENEMY) ) {
			int s_guild = status_get_guild_id(s_bl);
			int t_guild = status_get_guild_id(t_bl);

			if( s_guild && t_guild && (s_guild == t_guild || (!(flag&BCT_SAMEGUILD) && guild_isallied(s_guild, t_guild))) )
				state |= BCT_GUILD;
		}
	}

	if( !state ) //If not an enemy, nor a guild, nor party, nor yourself, it's neutral
		state = BCT_NEUTRAL;
	else if( (state&BCT_ENEMY) && strip_enemy && (state&(BCT_SELF|BCT_PARTY|BCT_GUILD)) )
		state &= ~BCT_ENEMY; //Alliance state takes precedence over enemy one

	return (flag&state) ? 1 : -1;
}

/*==========================================
 * Check if can attack from this range
 * Basic check then calling path_search for obstacle etc
 *------------------------------------------
 */
bool battle_check_range(struct block_list *src, struct block_list *bl, int range)
{
	int d;

	nullpo_retr(false, src);
	nullpo_retr(false, bl);

	if( src->m != bl->m )
		return false;

#ifndef CIRCULAR_AREA
	if( src->type == BL_PC ) { //Range for players' attacks and skills should always have a circular check [Angezerus]
		if( !check_distance_client_bl(src, bl, range) )
			return false;
	} else
#endif
	if( !check_distance_bl(src, bl, range) )
		return false;

	if( (d = distance_bl(src, bl)) < 2 )
		return true; //No need for path checking

	if( d > AREA_SIZE )
		return false; //Avoid targetting objects beyond your range of sight

	return path_search_long(NULL,src->m,src->x,src->y,bl->x,bl->y,CELL_CHKWALL);
}

/*=============================================
 * Battle.conf settings and default/max values
 *---------------------------------------------
 */
static const struct _battle_data {
	const char *str;
	int *val;
	int defval;
	int min;
	int max;
} battle_data[] = {
	{ "warp_point_debug",                   &battle_config.warp_point_debug,                0,      0,      1,              },
	{ "enable_critical",                    &battle_config.enable_critical,                 BL_PC,  BL_NUL, BL_ALL,         },
	{ "mob_critical_rate",                  &battle_config.mob_critical_rate,               100,    0,      INT_MAX,        },
	{ "critical_rate",                      &battle_config.critical_rate,                   100,    0,      INT_MAX,        },
	{ "enable_baseatk",                     &battle_config.enable_baseatk,                  BL_PC|BL_HOM, BL_NUL, BL_ALL,   },
	{ "enable_perfect_flee",                &battle_config.enable_perfect_flee,             BL_PC|BL_PET, BL_NUL, BL_ALL,   },
	{ "casting_rate",                       &battle_config.cast_rate,                       100,    0,      INT_MAX,        },
	{ "delay_rate",                         &battle_config.delay_rate,                      100,    0,      INT_MAX,        },
	{ "delay_dependon_dex",                 &battle_config.delay_dependon_dex,              0,      0,      1,              },
	{ "delay_dependon_agi",                 &battle_config.delay_dependon_agi,              0,      0,      1,              },
	{ "skill_delay_attack_enable",          &battle_config.sdelay_attack_enable,            0,      0,      1,              },
	{ "left_cardfix_to_right",              &battle_config.left_cardfix_to_right,           0,      0,      1,              },
	{ "skill_add_range",                    &battle_config.skill_add_range,                 0,      0,      INT_MAX,        },
	{ "skill_out_range_consume",            &battle_config.skill_out_range_consume,         1,      0,      1,              },
	{ "skillrange_by_distance",             &battle_config.skillrange_by_distance,          ~BL_PC, BL_NUL, BL_ALL,         },
	{ "skillrange_from_weapon",             &battle_config.use_weapon_skill_range,          BL_NUL, BL_NUL, BL_ALL,         },
	{ "player_damage_delay_rate",           &battle_config.pc_damage_delay_rate,            100,    0,      INT_MAX,        },
	{ "defunit_not_enemy",                  &battle_config.defnotenemy,                     0,      0,      1,              },
	{ "gvg_traps_target_all",               &battle_config.vs_traps_bctall,                 BL_PC,  BL_NUL, BL_ALL,         },
	{ "traps_setting",                      &battle_config.traps_setting,                   0,      0,      2,              },
	{ "summon_flora_setting",               &battle_config.summon_flora,                    1|2,    0,      1|2,            },
	{ "clear_skills_on_death",              &battle_config.clear_unit_ondeath,              BL_NUL, BL_NUL, BL_ALL,         },
	{ "clear_skills_on_warp",               &battle_config.clear_unit_onwarp,               BL_ALL, BL_NUL, BL_ALL,         },
	{ "random_monster_checklv",             &battle_config.random_monster_checklv,          0,      0,      1,              },
	{ "attribute_recover",                  &battle_config.attr_recover,                    1,      0,      1,              },
	{ "flooritem_lifetime",                 &battle_config.flooritem_lifetime,              60000,  1000,   INT_MAX,        },
	{ "item_auto_get",                      &battle_config.item_auto_get,                   0,      0,      1,              },
	{ "item_first_get_time",                &battle_config.item_first_get_time,             3000,   0,      INT_MAX,        },
	{ "item_second_get_time",               &battle_config.item_second_get_time,            1000,   0,      INT_MAX,        },
	{ "item_third_get_time",                &battle_config.item_third_get_time,             1000,   0,      INT_MAX,        },
	{ "mvp_item_first_get_time",            &battle_config.mvp_item_first_get_time,         10000,  0,      INT_MAX,        },
	{ "mvp_item_second_get_time",           &battle_config.mvp_item_second_get_time,        10000,  0,      INT_MAX,        },
	{ "mvp_item_third_get_time",            &battle_config.mvp_item_third_get_time,         2000,   0,      INT_MAX,        },
	{ "drop_rate0item",                     &battle_config.drop_rate0item,                  0,      0,      1,              },
	{ "base_exp_rate",                      &battle_config.base_exp_rate,                   100,    0,      INT_MAX,        },
	{ "job_exp_rate",                       &battle_config.job_exp_rate,                    100,    0,      INT_MAX,        },
	{ "pvp_exp",                            &battle_config.pvp_exp,                         1,      0,      1,              },
	{ "death_penalty_type",                 &battle_config.death_penalty_type,              0,      0,      2,              },
	{ "death_penalty_base",                 &battle_config.death_penalty_base,              0,      0,      INT_MAX,        },
	{ "death_penalty_job",                  &battle_config.death_penalty_job,               0,      0,      INT_MAX,        },
	{ "zeny_penalty",                       &battle_config.zeny_penalty,                    0,      0,      INT_MAX,        },
	{ "hp_rate",                            &battle_config.hp_rate,                         100,    1,      INT_MAX,        },
	{ "sp_rate",                            &battle_config.sp_rate,                         100,    1,      INT_MAX,        },
	{ "restart_hp_rate",                    &battle_config.restart_hp_rate,                 0,      0,      100,            },
	{ "restart_sp_rate",                    &battle_config.restart_sp_rate,                 0,      0,      100,            },
	{ "guild_aura",                         &battle_config.guild_aura,                      31,     0,      31,             },
	{ "mvp_hp_rate",                        &battle_config.mvp_hp_rate,                     100,    1,      INT_MAX,        },
	{ "mvp_exp_rate",                       &battle_config.mvp_exp_rate,                    100,    0,      INT_MAX,        },
	{ "monster_hp_rate",                    &battle_config.monster_hp_rate,                 100,    1,      INT_MAX,        },
	{ "monster_max_aspd",                   &battle_config.monster_max_aspd,                199,    100,    199,            },
	{ "view_range_rate",                    &battle_config.view_range_rate,                 100,    0,      INT_MAX,        },
	{ "chase_range_rate",                   &battle_config.chase_range_rate,                100,    0,      INT_MAX,        },
	{ "gtb_sc_immunity",                    &battle_config.gtb_sc_immunity,                 50,     0,      INT_MAX,        },
	{ "guild_max_castles",                  &battle_config.guild_max_castles,               0,      0,      INT_MAX,        },
	{ "guild_skill_relog_delay",            &battle_config.guild_skill_relog_delay,         300000, 0,      INT_MAX,        },
	{ "emergency_call",                     &battle_config.emergency_call,                  11,     0,      31,             },
	{ "atcommand_spawn_quantity_limit",     &battle_config.atc_spawn_quantity_limit,        100,    0,      INT_MAX,        },
	{ "atcommand_slave_clone_limit",        &battle_config.atc_slave_clone_limit,           25,     0,      INT_MAX,        },
	{ "partial_name_scan",                  &battle_config.partial_name_scan,               0,      0,      1,              },
	{ "player_skillfree",                   &battle_config.skillfree,                       0,      0,      1,              },
	{ "player_skillup_limit",               &battle_config.skillup_limit,                   1,      0,      1,              },
	{ "weapon_produce_rate",                &battle_config.wp_rate,                         100,    0,      INT_MAX,        },
	{ "potion_produce_rate",                &battle_config.pp_rate,                         100,    0,      INT_MAX,        },
	{ "monster_active_enable",              &battle_config.monster_active_enable,           1,      0,      1,              },
	{ "monster_damage_delay_rate",          &battle_config.monster_damage_delay_rate,       100,    0,      INT_MAX,        },
	{ "monster_loot_type",                  &battle_config.monster_loot_type,               0,      0,      1,              },
//	{ "mob_skill_use",                      &battle_config.mob_skill_use,                   1,      0,      1,              }, //Deprecated
	{ "mob_skill_rate",                     &battle_config.mob_skill_rate,                  100,    0,      INT_MAX,        },
	{ "mob_skill_delay",                    &battle_config.mob_skill_delay,                 100,    0,      INT_MAX,        },
	{ "mob_count_rate",                     &battle_config.mob_count_rate,                  100,    0,      INT_MAX,        },
	{ "mob_spawn_delay",                    &battle_config.mob_spawn_delay,                 100,    0,      INT_MAX,        },
	{ "plant_spawn_delay",                  &battle_config.plant_spawn_delay,               100,    0,      INT_MAX,        },
	{ "boss_spawn_delay",                   &battle_config.boss_spawn_delay,                100,    0,      INT_MAX,        },
	{ "no_spawn_on_player",                 &battle_config.no_spawn_on_player,              0,      0,      100,            },
	{ "force_random_spawn",                 &battle_config.force_random_spawn,              0,      0,      1,              },
	{ "slaves_inherit_mode",                &battle_config.slaves_inherit_mode,             2,      0,      3,              },
	{ "slaves_inherit_speed",               &battle_config.slaves_inherit_speed,            3,      0,      3,              },
	{ "summons_trigger_autospells",         &battle_config.summons_trigger_autospells,      1,      0,      1,              },
	{ "pc_damage_walk_delay_rate",          &battle_config.pc_walk_delay_rate,              20,     0,      INT_MAX,        },
	{ "damage_walk_delay_rate",             &battle_config.walk_delay_rate,                 100,    0,      INT_MAX,        },
	{ "multihit_delay",                     &battle_config.multihit_delay,                  80,     0,      INT_MAX,        },
	{ "quest_skill_learn",                  &battle_config.quest_skill_learn,               0,      0,      1,              },
	{ "quest_skill_reset",                  &battle_config.quest_skill_reset,               0,      0,      1,              },
	{ "basic_skill_check",                  &battle_config.basic_skill_check,               1,      0,      1,              },
	{ "guild_emperium_check",               &battle_config.guild_emperium_check,            1,      0,      1,              },
	{ "guild_exp_limit",                    &battle_config.guild_exp_limit,                 50,     0,      99,             },
	{ "player_invincible_time",             &battle_config.pc_invincible_time,              5000,   0,      INT_MAX,        },
	{ "pet_catch_rate",                     &battle_config.pet_catch_rate,                  100,    0,      INT_MAX,        },
	{ "pet_rename",                         &battle_config.pet_rename,                      0,      0,      1,              },
	{ "pet_friendly_rate",                  &battle_config.pet_friendly_rate,               100,    0,      INT_MAX,        },
	{ "pet_hungry_delay_rate",              &battle_config.pet_hungry_delay_rate,           100,    10,     INT_MAX,        },
	{ "pet_hungry_friendly_decrease",       &battle_config.pet_hungry_friendly_decrease,    5,      0,      INT_MAX,        },
	{ "pet_status_support",                 &battle_config.pet_status_support,              0,      0,      1,              },
	{ "pet_attack_support",                 &battle_config.pet_attack_support,              0,      0,      1,              },
	{ "pet_damage_support",                 &battle_config.pet_damage_support,              0,      0,      1,              },
	{ "pet_support_min_friendly",           &battle_config.pet_support_min_friendly,        900,    0,      950,            },
	{ "pet_bonus_min_friendly",             &battle_config.pet_bonus_min_friendly,          750,    0,      950,            },
	{ "pet_support_rate",                   &battle_config.pet_support_rate,                100,    0,      INT_MAX,        },
	{ "pet_attack_exp_to_master",           &battle_config.pet_attack_exp_to_master,        0,      0,      1,              },
	{ "pet_attack_exp_rate",                &battle_config.pet_attack_exp_rate,             100,    0,      INT_MAX,        },
	{ "pet_lv_rate",                        &battle_config.pet_lv_rate,                     0,      0,      INT_MAX,        },
	{ "pet_max_stats",                      &battle_config.pet_max_stats,                   99,     0,      INT_MAX,        },
	{ "pet_max_atk1",                       &battle_config.pet_max_atk1,                    750,    0,      INT_MAX,        },
	{ "pet_max_atk2",                       &battle_config.pet_max_atk2,                    1000,   0,      INT_MAX,        },
	{ "pet_disable_in_gvg",                 &battle_config.pet_no_gvg,                      0,      0,      1,              },
	{ "skill_min_damage",                   &battle_config.skill_min_damage,                2|4,    0,      1|2|4,          },
	{ "finger_offensive_type",              &battle_config.finger_offensive_type,           0,      0,      1,              },
	{ "heal_exp",                           &battle_config.heal_exp,                        0,      0,      INT_MAX,        },
	{ "resurrection_exp",                   &battle_config.resurrection_exp,                0,      0,      INT_MAX,        },
	{ "shop_exp",                           &battle_config.shop_exp,                        0,      0,      INT_MAX,        },
	{ "max_heal_lv",                        &battle_config.max_heal_lv,                     11,     1,      INT_MAX,        },
	{ "max_heal",                           &battle_config.max_heal,                        9999,   0,      INT_MAX,        },
	{ "combo_delay_rate",                   &battle_config.combo_delay_rate,                100,    0,      INT_MAX,        },
	{ "item_check",                         &battle_config.item_check,                      0x0,    0x0,    0x7,            },
	{ "item_use_interval",                  &battle_config.item_use_interval,               100,    0,      INT_MAX,        },
	{ "cashfood_use_interval",              &battle_config.cashfood_use_interval,           60000,  0,      INT_MAX,        },
	{ "wedding_modifydisplay",              &battle_config.wedding_modifydisplay,           0,      0,      1,              },
	{ "wedding_ignorepalette",              &battle_config.wedding_ignorepalette,           0,      0,      1,              },
	{ "xmas_ignorepalette",                 &battle_config.xmas_ignorepalette,              0,      0,      1,              },
	{ "summer_ignorepalette",               &battle_config.summer_ignorepalette,            0,      0,      1,              },
	{ "hanbok_ignorepalette",               &battle_config.hanbok_ignorepalette,            0,      0,      1,              },
	{ "natural_healhp_interval",            &battle_config.natural_healhp_interval,         6000,   NATURAL_HEAL_INTERVAL, INT_MAX, },
	{ "natural_healsp_interval",            &battle_config.natural_healsp_interval,         8000,   NATURAL_HEAL_INTERVAL, INT_MAX, },
	{ "natural_heal_skill_interval",        &battle_config.natural_heal_skill_interval,     10000,  NATURAL_HEAL_INTERVAL, INT_MAX, },
	{ "natural_heal_weight_rate",           &battle_config.natural_heal_weight_rate,        50,     50,     101             },
	{ "ammo_decrement",                     &battle_config.ammo_decrement,                  1,      0,      2,              },
	{ "ammo_unequip",                       &battle_config.ammo_unequip,                    1,      0,      1,              },
	{ "ammo_check_weapon",                  &battle_config.ammo_check_weapon,               1,      0,      1,              },
	{ "max_aspd",                           &battle_config.max_aspd,                        190,    100,    199,            },
	{ "max_third_aspd",                     &battle_config.max_third_aspd,                  193,    100,    199,            },
	{ "max_walk_speed",                     &battle_config.max_walk_speed,                  300,    100,    100*DEFAULT_WALK_SPEED, },
	{ "max_lv",                             &battle_config.max_lv,                          99,     0,      MAX_LEVEL,      },
	{ "aura_lv",                            &battle_config.aura_lv,                         99,     0,      INT_MAX,        },
	{ "max_hp",                             &battle_config.max_hp,                          32500,  100,    1000000000,     },
	{ "max_sp",                             &battle_config.max_sp,                          32500,  100,    1000000000,     },
	{ "max_cart_weight",                    &battle_config.max_cart_weight,                 8000,   100,    1000000,        },
	{ "max_parameter",                      &battle_config.max_parameter,                   99,     10,     SHRT_MAX,       },
	{ "max_baby_parameter",                 &battle_config.max_baby_parameter,              80,     10,     SHRT_MAX,       },
	{ "max_def",                            &battle_config.max_def,                         99,     0,      INT_MAX,        },
	{ "over_def_bonus",                     &battle_config.over_def_bonus,                  0,      0,      1000,           },
	{ "skill_log",                          &battle_config.skill_log,                       BL_NUL, BL_NUL, BL_ALL,         },
	{ "battle_log",                         &battle_config.battle_log,                      0,      0,      1,              },
	{ "etc_log",                            &battle_config.etc_log,                         1,      0,      1,              },
	{ "save_clothcolor",                    &battle_config.save_clothcolor,                 1,      0,      1,              },
	{ "undead_detect_type",                 &battle_config.undead_detect_type,              0,      0,      2,              },
	{ "auto_counter_type",                  &battle_config.auto_counter_type,               BL_ALL, BL_NUL, BL_ALL,         },
	{ "min_hitrate",                        &battle_config.min_hitrate,                     5,      0,      100,            },
	{ "max_hitrate",                        &battle_config.max_hitrate,                     100,    0,      100,            },
	{ "agi_penalty_target",                 &battle_config.agi_penalty_target,              BL_PC,  BL_NUL, BL_ALL,         },
	{ "agi_penalty_type",                   &battle_config.agi_penalty_type,                1,      0,      2,              },
	{ "agi_penalty_count",                  &battle_config.agi_penalty_count,               3,      2,      INT_MAX,        },
	{ "agi_penalty_num",                    &battle_config.agi_penalty_num,                 10,     0,      INT_MAX,        },
	{ "vit_penalty_target",                 &battle_config.vit_penalty_target,              BL_PC,  BL_NUL, BL_ALL,         },
	{ "vit_penalty_type",                   &battle_config.vit_penalty_type,                1,      0,      2,              },
	{ "vit_penalty_count",                  &battle_config.vit_penalty_count,               3,      2,      INT_MAX,        },
	{ "vit_penalty_num",                    &battle_config.vit_penalty_num,                 5,      1,      INT_MAX,        },
	{ "weapon_defense_type",                &battle_config.weapon_defense_type,             0,      0,      INT_MAX,        },
	{ "magic_defense_type",                 &battle_config.magic_defense_type,              0,      0,      INT_MAX,        },
	{ "skill_reiteration",                  &battle_config.skill_reiteration,               BL_NUL, BL_NUL, BL_ALL,         },
	{ "skill_nofootset",                    &battle_config.skill_nofootset,                 BL_PC,  BL_NUL, BL_ALL,         },
	{ "player_cloak_check_type",            &battle_config.pc_cloak_check_type,             1,      0,      1|2|4,          },
	{ "monster_cloak_check_type",           &battle_config.monster_cloak_check_type,        4,      0,      1|2|4,          },
	{ "sense_type",                         &battle_config.estimation_type,                 1|2,    0,      1|2,            },
	{ "gvg_short_attack_damage_rate",       &battle_config.gvg_short_damage_rate,           80,     0,      INT_MAX,        },
	{ "gvg_long_attack_damage_rate",        &battle_config.gvg_long_damage_rate,            80,     0,      INT_MAX,        },
	{ "gvg_weapon_attack_damage_rate",      &battle_config.gvg_weapon_damage_rate,          60,     0,      INT_MAX,        },
	{ "gvg_magic_attack_damage_rate",       &battle_config.gvg_magic_damage_rate,           60,     0,      INT_MAX,        },
	{ "gvg_misc_attack_damage_rate",        &battle_config.gvg_misc_damage_rate,            60,     0,      INT_MAX,        },
	{ "gvg_flee_penalty",                   &battle_config.gvg_flee_penalty,                20,     0,      INT_MAX,        },
	{ "pk_short_attack_damage_rate",        &battle_config.pk_short_damage_rate,            80,     0,      INT_MAX,        },
	{ "pk_long_attack_damage_rate",         &battle_config.pk_long_damage_rate,             70,     0,      INT_MAX,        },
	{ "pk_weapon_attack_damage_rate",       &battle_config.pk_weapon_damage_rate,           60,     0,      INT_MAX,        },
	{ "pk_magic_attack_damage_rate",        &battle_config.pk_magic_damage_rate,            60,     0,      INT_MAX,        },
	{ "pk_misc_attack_damage_rate",         &battle_config.pk_misc_damage_rate,             60,     0,      INT_MAX,        },
	{ "mob_changetarget_byskill",           &battle_config.mob_changetarget_byskill,        0,      0,      1,              },
	{ "attack_direction_change",            &battle_config.attack_direction_change,         BL_ALL, BL_NUL, BL_ALL,         },
	{ "land_skill_limit",                   &battle_config.land_skill_limit,                BL_ALL, BL_NUL, BL_ALL,         },
	{ "monster_class_change_full_recover",  &battle_config.monster_class_change_recover,    1,      0,      1,              },
	{ "produce_item_name_input",            &battle_config.produce_item_name_input,         0x1|0x2, 0,     0x9F,           },
	{ "display_skill_fail",                 &battle_config.display_skill_fail,              2,      0,      1|2|4|8,        },
	{ "chat_warpportal",                    &battle_config.chat_warpportal,                 0,      0,      1,              },
	{ "mob_warp",                           &battle_config.mob_warp,                        0,      0,      1|2|4,          },
	{ "dead_branch_active",                 &battle_config.dead_branch_active,              1,      0,      1,              },
	{ "vending_max_value",                  &battle_config.vending_max_value,               10000000, 1,    MAX_ZENY,       },
	{ "vending_over_max",                   &battle_config.vending_over_max,                1,      0,      1,              },
	{ "show_steal_in_same_party",           &battle_config.show_steal_in_same_party,        0,      0,      1,              },
	{ "party_hp_mode",                      &battle_config.party_hp_mode,                   0,      0,      1,              },
	{ "show_party_share_picker",            &battle_config.party_show_share_picker,         1,      0,      1,              },
	{ "show_picker.item_type",              &battle_config.show_picker_item_type,           112,    0,      INT_MAX,        },
	{ "party_update_interval",              &battle_config.party_update_interval,           1000,   100,    INT_MAX,        },
	{ "party_item_share_type",              &battle_config.party_share_type,                0,      0,      1|2|3,          },
	{ "attack_attr_none",                   &battle_config.attack_attr_none,                ~BL_PC, BL_NUL, BL_ALL,         },
	{ "gx_allhit",                          &battle_config.gx_allhit,                       0,      0,      1,              },
	{ "gx_disptype",                        &battle_config.gx_disptype,                     1,      0,      1,              },
	{ "devotion_level_difference",          &battle_config.devotion_level_difference,       10,     0,      INT_MAX,        },
	{ "player_skill_partner_check",         &battle_config.player_skill_partner_check,      1,      0,      1,              },
	{ "invite_request_check",               &battle_config.invite_request_check,            1,      0,      1,              },
	{ "skill_removetrap_type",              &battle_config.skill_removetrap_type,           0,      0,      1,              },
	{ "disp_experience",                    &battle_config.disp_experience,                 0,      0,      1,              },
	{ "disp_zeny",                          &battle_config.disp_zeny,                       0,      0,      1,              },
	{ "castle_defense_rate",                &battle_config.castle_defense_rate,             100,    0,      100,            },
	{ "bone_drop",                          &battle_config.bone_drop,                       0,      0,      2,              },
	{ "buyer_name",                         &battle_config.buyer_name,                      1,      0,      1,              },
	{ "skill_wall_check",                   &battle_config.skill_wall_check,                1,      0,      1,              },
	{ "official_cell_stack_limit",          &battle_config.official_cell_stack_limit,       1,      0,      255,            },
	{ "custom_cell_stack_limit",            &battle_config.custom_cell_stack_limit,         1,      1,      255,            },
	{ "dancing_weaponswitch_fix",           &battle_config.dancing_weaponswitch_fix,        1,      0,      1,              },
	{ "check_occupied_cells",               &battle_config.check_occupied_cells,            1,      0,      1,              },
	
	//eAthena additions
	{ "item_logarithmic_drops",             &battle_config.logarithmic_drops,               0,      0,      1,              },
	{ "item_drop_common_min",               &battle_config.item_drop_common_min,            1,      1,      10000,          },
	{ "item_drop_common_max",               &battle_config.item_drop_common_max,            10000,  1,      10000,          },
	{ "item_drop_equip_min",                &battle_config.item_drop_equip_min,             1,      1,      10000,          },
	{ "item_drop_equip_max",                &battle_config.item_drop_equip_max,             10000,  1,      10000,          },
	{ "item_drop_card_min",                 &battle_config.item_drop_card_min,              1,      1,      10000,          },
	{ "item_drop_card_max",                 &battle_config.item_drop_card_max,              10000,  1,      10000,          },
	{ "item_drop_mvp_min",                  &battle_config.item_drop_mvp_min,               1,      1,      10000,          },
	{ "item_drop_mvp_max",                  &battle_config.item_drop_mvp_max,               10000,  1,      10000,          },
	{ "item_drop_mvp_mode",                 &battle_config.item_drop_mvp_mode,              0,      0,      2,              },
	{ "item_drop_heal_min",                 &battle_config.item_drop_heal_min,              1,      1,      10000,          },
	{ "item_drop_heal_max",                 &battle_config.item_drop_heal_max,              10000,  1,      10000,          },
	{ "item_drop_use_min",                  &battle_config.item_drop_use_min,               1,      1,      10000,          },
	{ "item_drop_use_max",                  &battle_config.item_drop_use_max,               10000,  1,      10000,          },
	{ "item_drop_add_min",                  &battle_config.item_drop_adddrop_min,           1,      1,      10000,          },
	{ "item_drop_add_max",                  &battle_config.item_drop_adddrop_max,           10000,  1,      10000,          },
	{ "item_drop_treasure_min",             &battle_config.item_drop_treasure_min,          1,      1,      10000,          },
	{ "item_drop_treasure_max",             &battle_config.item_drop_treasure_max,          10000,  1,      10000,          },
	{ "item_rate_mvp",                      &battle_config.item_rate_mvp,                   100,    0,      1000000,        },
	{ "item_rate_common",                   &battle_config.item_rate_common,                100,    0,      1000000,        },
	{ "item_rate_common_boss",              &battle_config.item_rate_common_boss,           100,    0,      1000000,        },
	{ "item_rate_equip",                    &battle_config.item_rate_equip,                 100,    0,      1000000,        },
	{ "item_rate_equip_boss",               &battle_config.item_rate_equip_boss,            100,    0,      1000000,        },
	{ "item_rate_card",                     &battle_config.item_rate_card,                  100,    0,      1000000,        },
	{ "item_rate_card_boss",                &battle_config.item_rate_card_boss,             100,    0,      1000000,        },
	{ "item_rate_heal",                     &battle_config.item_rate_heal,                  100,    0,      1000000,        },
	{ "item_rate_heal_boss",                &battle_config.item_rate_heal_boss,             100,    0,      1000000,        },
	{ "item_rate_use",                      &battle_config.item_rate_use,                   100,    0,      1000000,        },
	{ "item_rate_use_boss",                 &battle_config.item_rate_use_boss,              100,    0,      1000000,        },
	{ "item_rate_adddrop",                  &battle_config.item_rate_adddrop,               100,    0,      1000000,        },
	{ "item_rate_treasure",                 &battle_config.item_rate_treasure,              100,    0,      1000000,        },
	{ "prevent_logout",                     &battle_config.prevent_logout,                  10000,  0,      60000,          },
	{ "prevent_logout_trigger",             &battle_config.prevent_logout_trigger,          0xE,    0,      0xF,            },
	{ "alchemist_summon_reward",            &battle_config.alchemist_summon_reward,         1,      0,      2,              },
	{ "drops_by_luk",                       &battle_config.drops_by_luk,                    0,      0,      INT_MAX,        },
	{ "drops_by_luk2",                      &battle_config.drops_by_luk2,                   0,      0,      INT_MAX,        },
	{ "equip_natural_break_rate",           &battle_config.equip_natural_break_rate,        0,      0,      INT_MAX,        },
	{ "equip_self_break_rate",              &battle_config.equip_self_break_rate,           100,    0,      INT_MAX,        },
	{ "equip_skill_break_rate",             &battle_config.equip_skill_break_rate,          100,    0,      INT_MAX,        },
	{ "pk_mode",                            &battle_config.pk_mode,                         0,      0,      2,              },
	{ "pk_level_range",                     &battle_config.pk_level_range,                  0,      0,      INT_MAX,        },
	{ "manner_system",                      &battle_config.manner_system,                   0xFFF,  0,      0xFFF,          },
	{ "pet_equip_required",                 &battle_config.pet_equip_required,              0,      0,      1,              },
	{ "multi_level_up",                     &battle_config.multi_level_up,                  0,      0,      1,              },
	{ "max_exp_gain_rate",                  &battle_config.max_exp_gain_rate,               0,      0,      INT_MAX,        },
	{ "backstab_bow_penalty",               &battle_config.backstab_bow_penalty,            0,      0,      1,              },
	{ "night_at_start",                     &battle_config.night_at_start,                  0,      0,      1,              },
	{ "show_mob_info",                      &battle_config.show_mob_info,                   0,      0,      1|2|4,          },
	{ "ban_hack_trade",                     &battle_config.ban_hack_trade,                  0,      0,      INT_MAX,        },
	{ "packet_ver_flag",                    &battle_config.packet_ver_flag,                 0x7FFFFFFF,0,   INT_MAX,        },
	{ "packet_ver_flag2",                   &battle_config.packet_ver_flag2,                0x7FFFFFFF,0,   INT_MAX,        },
	{ "min_hair_style",                     &battle_config.min_hair_style,                  0,      0,      INT_MAX,        },
	{ "max_hair_style",                     &battle_config.max_hair_style,                  23,     0,      INT_MAX,        },
	{ "min_hair_color",                     &battle_config.min_hair_color,                  0,      0,      INT_MAX,        },
	{ "max_hair_color",                     &battle_config.max_hair_color,                  9,      0,      INT_MAX,        },
	{ "min_cloth_color",                    &battle_config.min_cloth_color,                 0,      0,      INT_MAX,        },
	{ "max_cloth_color",                    &battle_config.max_cloth_color,                 4,      0,      INT_MAX,        },
	{ "pet_hair_style",                     &battle_config.pet_hair_style,                  100,    0,      INT_MAX,        },
	{ "castrate_dex_scale",                 &battle_config.castrate_dex_scale,              150,    1,      INT_MAX,        },
	{ "vcast_stat_scale",                   &battle_config.vcast_stat_scale,                530,    1,      INT_MAX,        },
	{ "area_size",                          &battle_config.area_size,                       14,     0,      INT_MAX,        },
	{ "zeny_from_mobs",                     &battle_config.zeny_from_mobs,                  0,      0,      1,              },
	{ "mobs_level_up",                      &battle_config.mobs_level_up,                   0,      0,      1,              },
	{ "mobs_level_up_exp_rate",             &battle_config.mobs_level_up_exp_rate,          1,      1,      INT_MAX,        },
	{ "pk_min_level",                       &battle_config.pk_min_level,                    55,     1,      INT_MAX,        },
	{ "skill_steal_max_tries",              &battle_config.skill_steal_max_tries,           0,      0,      UCHAR_MAX,      },
	{ "motd_type",                          &battle_config.motd_type,                       0,      0,      1,              },
	{ "finding_ore_rate",                   &battle_config.finding_ore_rate,                100,    0,      INT_MAX,        },
	{ "exp_calc_type",                      &battle_config.exp_calc_type,                   0,      0,      1,              },
	{ "exp_bonus_attacker",                 &battle_config.exp_bonus_attacker,              25,     0,      INT_MAX,        },
	{ "exp_bonus_max_attacker",             &battle_config.exp_bonus_max_attacker,          12,     2,      INT_MAX,        },
	{ "min_skill_delay_limit",              &battle_config.min_skill_delay_limit,           100,    10,     INT_MAX,        },
	{ "default_walk_delay",                 &battle_config.default_walk_delay,              300,    0,      INT_MAX,        },
	{ "no_skill_delay",                     &battle_config.no_skill_delay,                  BL_MOB, BL_NUL, BL_ALL,         },
	{ "attack_walk_delay",                  &battle_config.attack_walk_delay,               BL_ALL, BL_NUL, BL_ALL,         },
	{ "require_glory_guild",                &battle_config.require_glory_guild,             0,      0,      1,              },
	{ "idle_no_share",                      &battle_config.idle_no_share,                   0,      0,      INT_MAX,        },
	{ "party_even_share_bonus",             &battle_config.party_even_share_bonus,          0,      0,      INT_MAX,        },
	{ "delay_battle_damage",                &battle_config.delay_battle_damage,             1,      0,      1,              },
	{ "hide_woe_damage",                    &battle_config.hide_woe_damage,                 0,      0,      1,              },
	{ "display_version",                    &battle_config.display_version,                 1,      0,      1,              },
	{ "display_hallucination",              &battle_config.display_hallucination,           1,      0,      1,              },
	{ "use_statpoint_table",                &battle_config.use_statpoint_table,             1,      0,      1,              },
	{ "ignore_items_gender",                &battle_config.ignore_items_gender,             1,      0,      1,              },
	{ "berserk_cancels_buffs",              &battle_config.berserk_cancels_buffs,           0,      0,      1,              },
	{ "debuff_on_logout",                   &battle_config.debuff_on_logout,                1|2,    0,      1|2,            },
	{ "monster_ai",                         &battle_config.mob_ai,                          0x000,  0x000,  0xFFF,          },
	{ "hom_setting",                        &battle_config.hom_setting,                     0xFFFF, 0x0000, 0xFFFF,         },
	{ "dynamic_mobs",                       &battle_config.dynamic_mobs,                    1,      0,      1,              },
	{ "mob_remove_damaged",                 &battle_config.mob_remove_damaged,              1,      0,      1,              },
	{ "show_hp_sp_drain",                   &battle_config.show_hp_sp_drain,                0,      0,      1,              },
	{ "show_hp_sp_gain",                    &battle_config.show_hp_sp_gain,                 1,      0,      1,              },
	{ "mob_npc_event_type",                 &battle_config.mob_npc_event_type,              1,      0,      1,              },
	{ "character_size",                     &battle_config.character_size,                  1|2,    0,      1|2,            },
	{ "mob_max_skilllvl",                   &battle_config.mob_max_skilllvl,                MAX_SKILL_LEVEL, 1, MAX_SKILL_LEVEL, },
	{ "retaliate_to_master",                &battle_config.retaliate_to_master,             1,      0,      1,              },
	{ "rare_drop_announce",                 &battle_config.rare_drop_announce,              0,      0,      10000,          },
	{ "duel_allow_pvp",                     &battle_config.duel_allow_pvp,                  0,      0,      1,              },
	{ "duel_allow_gvg",                     &battle_config.duel_allow_gvg,                  0,      0,      1,              },
	{ "duel_allow_teleport",                &battle_config.duel_allow_teleport,             0,      0,      1,              },
	{ "duel_autoleave_when_die",            &battle_config.duel_autoleave_when_die,         1,      0,      1,              },
	{ "duel_time_interval",                 &battle_config.duel_time_interval,              60,     0,      INT_MAX,        },
	{ "duel_only_on_same_map",              &battle_config.duel_only_on_same_map,           0,      0,      1,              },
	{ "skip_teleport_lv1_menu",             &battle_config.skip_teleport_lv1_menu,          0,      0,      1,              },
	{ "allow_skill_without_day",            &battle_config.allow_skill_without_day,         0,      0,      1,              },
	{ "allow_es_magic_player",              &battle_config.allow_es_magic_pc,               0,      0,      1,              },
	{ "skill_caster_check",                 &battle_config.skill_caster_check,              1,      0,      1,              },
	{ "status_cast_cancel",                 &battle_config.sc_castcancel,                   BL_NUL, BL_NUL, BL_ALL,         },
	{ "pc_status_def_rate",                 &battle_config.pc_sc_def_rate,                  100,    0,      INT_MAX,        },
	{ "mob_status_def_rate",                &battle_config.mob_sc_def_rate,                 100,    0,      INT_MAX,        },
	{ "pc_max_status_def",                  &battle_config.pc_max_sc_def,                   100,    0,      INT_MAX,        },
	{ "mob_max_status_def",                 &battle_config.mob_max_sc_def,                  100,    0,      INT_MAX,        },
	{ "sg_miracle_skill_ratio",             &battle_config.sg_miracle_skill_ratio,          1,      0,      10000,          },
	{ "sg_angel_skill_ratio",               &battle_config.sg_angel_skill_ratio,            10,     0,      10000,          },
	{ "autospell_stacking",                 &battle_config.autospell_stacking,              0,      0,      1,              },
	{ "override_mob_names",                 &battle_config.override_mob_names,              0,      0,      2,              },
	{ "min_chat_delay",                     &battle_config.min_chat_delay,                  0,      0,      INT_MAX,        },
	{ "friend_auto_add",                    &battle_config.friend_auto_add,                 1,      0,      1,              },
	{ "hom_rename",                         &battle_config.hom_rename,                      0,      0,      1,              },
	{ "homunculus_show_growth",             &battle_config.homunculus_show_growth,          0,      0,      1,              },
	{ "homunculus_friendly_rate",           &battle_config.homunculus_friendly_rate,        100,    0,      INT_MAX,        },
	{ "vending_tax",                        &battle_config.vending_tax,                     0,      0,      10000,          },
	{ "day_duration",                       &battle_config.day_duration,                    0,      0,      INT_MAX,        },
	{ "night_duration",                     &battle_config.night_duration,                  0,      0,      INT_MAX,        },
	{ "mob_remove_delay",                   &battle_config.mob_remove_delay,                60000,  1000,   INT_MAX,        },
	{ "mob_active_time",                    &battle_config.mob_active_time,                 0,      0,      INT_MAX,        },
	{ "boss_active_time",                   &battle_config.boss_active_time,                0,      0,      INT_MAX,        },
	{ "sg_miracle_skill_duration",          &battle_config.sg_miracle_skill_duration,       3600000, 0,     INT_MAX,        },
	{ "hvan_explosion_intimate",            &battle_config.hvan_explosion_intimate,         45000,  0,      100000,         },
	{ "quest_exp_rate",                     &battle_config.quest_exp_rate,                  100,    0,      INT_MAX,        },
	{ "at_mapflag",                         &battle_config.autotrade_mapflag,               0,      0,      1,              },
	{ "at_timeout",                         &battle_config.at_timeout,                      0,      0,      INT_MAX,        },
	{ "homunculus_autoloot",                &battle_config.homunculus_autoloot,             0,      0,      1,              },
	{ "idle_no_autoloot",                   &battle_config.idle_no_autoloot,                0,      0,      INT_MAX,        },
	{ "max_guild_alliance",                 &battle_config.max_guild_alliance,              3,      0,      3,              },
	{ "ksprotection",                       &battle_config.ksprotection,                    5000,   0,      INT_MAX,        },
	{ "auction_feeperhour",                 &battle_config.auction_feeperhour,              12000,  0,      INT_MAX,        },
	{ "auction_maximumprice",               &battle_config.auction_maximumprice,            500000000, 0,   MAX_ZENY,       },
	{ "homunculus_auto_vapor",              &battle_config.homunculus_auto_vapor,           1,      0,      1,              },
	{ "display_status_timers",              &battle_config.display_status_timers,           1,      0,      1,              },
	{ "skill_add_heal_rate",                &battle_config.skill_add_heal_rate,             39,      0,      INT_MAX,        },
	{ "eq_single_target_reflectable",       &battle_config.eq_single_target_reflectable,    1,      0,      1,              },
	{ "invincible.nodamage",                &battle_config.invincible_nodamage,             0,      0,      1,              },
	{ "mob_slave_keep_target",              &battle_config.mob_slave_keep_target,           0,      0,      1,              },
	{ "autospell_check_range",              &battle_config.autospell_check_range,           0,      0,      1,              },
	{ "client_reshuffle_dice",              &battle_config.client_reshuffle_dice,           0,      0,      1,              },
	{ "client_sort_storage",                &battle_config.client_sort_storage,             0,      0,      1,              },
	{ "feature.buying_store",               &battle_config.feature_buying_store,            1,      0,      1,              },
	{ "feature.search_stores",              &battle_config.feature_search_stores,           1,      0,      1,              },
	{ "searchstore_querydelay",             &battle_config.searchstore_querydelay,         10,      0,      INT_MAX,        },
	{ "searchstore_maxresults",             &battle_config.searchstore_maxresults,         30,      1,      INT_MAX,        },
	{ "display_party_name",                 &battle_config.display_party_name,              0,      0,      1,              },
	{ "cashshop_show_points",               &battle_config.cashshop_show_points,            0,      0,      1,              },
	{ "mail_show_status",                   &battle_config.mail_show_status,                0,      0,      2,              },
	{ "client_limit_unit_lv",               &battle_config.client_limit_unit_lv,            0,      0,      BL_ALL,         },
	//BattleGround Settings
	{ "bg_update_interval",                 &battle_config.bg_update_interval,              1000,   100,    INT_MAX,        },
	{ "bg_short_attack_damage_rate",        &battle_config.bg_short_damage_rate,            80,     0,      INT_MAX,        },
	{ "bg_long_attack_damage_rate",         &battle_config.bg_long_damage_rate,             80,     0,      INT_MAX,        },
	{ "bg_weapon_attack_damage_rate",       &battle_config.bg_weapon_damage_rate,           60,     0,      INT_MAX,        },
	{ "bg_magic_attack_damage_rate",        &battle_config.bg_magic_damage_rate,            60,     0,      INT_MAX,        },
	{ "bg_misc_attack_damage_rate",         &battle_config.bg_misc_damage_rate,             60,     0,      INT_MAX,        },
	{ "bg_flee_penalty",                    &battle_config.bg_flee_penalty,                 20,     0,      INT_MAX,        },
	{ "max_third_parameter",                &battle_config.max_third_parameter,             130,    10,     SHRT_MAX,       },
	{ "max_baby_third_parameter",           &battle_config.max_baby_third_parameter,        117,    10,     SHRT_MAX,       },
	{ "max_trans_parameter",                &battle_config.max_trans_parameter,             99,     10,     SHRT_MAX,       },
	{ "max_third_trans_parameter",          &battle_config.max_third_trans_parameter,       130,    10,     SHRT_MAX,       },
	{ "max_extended_parameter",             &battle_config.max_extended_parameter,          125,    10,     SHRT_MAX,       },
	{ "skill_amotion_leniency",             &battle_config.skill_amotion_leniency,          0,      0,      300,            },
	{ "mvp_tomb_enabled",                   &battle_config.mvp_tomb_enabled,                1,      0,      1,              },
	{ "mvp_tomb_delay",                     &battle_config.mvp_tomb_delay,               9000,      0,      INT_MAX,        },
	{ "feature.atcommand_suggestions",      &battle_config.atcommand_suggestions_enabled,   0,      0,      1,              },
	{ "min_npc_vendchat_distance",          &battle_config.min_npc_vendchat_distance,       3,      0,      100,            },
	{ "atcommand_mobinfo_type",             &battle_config.atcommand_mobinfo_type,          0,      0,      1,              },
	{ "homunculus_max_level",               &battle_config.hom_max_level,                   99,     0,      MAX_LEVEL,      },
	{ "homunculus_S_max_level",             &battle_config.hom_S_max_level,                 150,    0,      MAX_LEVEL,      },
	{ "mob_size_influence",                 &battle_config.mob_size_influence,              0,      0,      1,              },
	{ "skill_trap_type",                    &battle_config.skill_trap_type,                 0,      0,      3,              },
	{ "allow_consume_restricted_item",      &battle_config.allow_consume_restricted_item,   1,      0,      1,              },
	{ "allow_equip_restricted_item",        &battle_config.allow_equip_restricted_item,     1,      0,      1,              },
	{ "max_walk_path",                      &battle_config.max_walk_path,                  17,      1,      MAX_WALKPATH,   },
	{ "item_enabled_npc",                   &battle_config.item_enabled_npc,                1,      0,      1,              },
	{ "item_flooritem_check",               &battle_config.item_onfloor,                    1,      0,      1,              },
	{ "bowling_bash_area",                  &battle_config.bowling_bash_area,               0,      0,      20,             },
	{ "gm_ignore_warpable_area",            &battle_config.gm_ignore_warpable_area,         0,      2,      100,            },
	{ "snovice_call_type",                  &battle_config.snovice_call_type,               0,      0,      1,              },
	{ "guild_notice_changemap",             &battle_config.guild_notice_changemap,          2,      0,      2,              },
	{ "drop_rateincrease",                  &battle_config.drop_rateincrease,               0,      0,      1,              },
	{ "feature.auction",                    &battle_config.feature_auction,                 0,      0,      2,              },
	{ "mon_trans_disable_in_gvg",           &battle_config.mon_trans_disable_in_gvg,        0,      0,      1,              },
	{ "transform_end_on_death",             &battle_config.transform_end_on_death,          1,      0,      1,              },
	{ "feature.banking",                    &battle_config.feature_banking,                 1,      0,      1,              },
	{ "homunculus_S_growth_level",          &battle_config.hom_S_growth_level,             99,      0,      MAX_LEVEL,      },
	{ "emblem_woe_change",                  &battle_config.emblem_woe_change,               0,      0,      1,              },
	{ "emblem_transparency_limit",          &battle_config.emblem_transparency_limit,      80,      0,    100,              },
#ifdef VIP_ENABLE
	{ "vip_storage_increase",               &battle_config.vip_storage_increase,            0,      0,      MAX_STORAGE - MIN_STORAGE, },
#else
	{ "vip_storage_increase",               &battle_config.vip_storage_increase,            0,      0,      MAX_STORAGE, },
#endif
	{ "vip_base_exp_increase",              &battle_config.vip_base_exp_increase,           0,      0,      INT_MAX,        },
	{ "vip_job_exp_increase",               &battle_config.vip_job_exp_increase,            0,      0,      INT_MAX,        },
	{ "vip_exp_penalty_base_normal",        &battle_config.vip_exp_penalty_base_normal,     0,      0,      INT_MAX,        },
	{ "vip_exp_penalty_job_normal",         &battle_config.vip_exp_penalty_job_normal,      0,      0,      INT_MAX,        },
	{ "vip_exp_penalty_base",               &battle_config.vip_exp_penalty_base,            0,      0,      INT_MAX,        },
	{ "vip_exp_penalty_job",                &battle_config.vip_exp_penalty_job,             0,      0,      INT_MAX,        },
	{ "vip_bm_increase",                    &battle_config.vip_bm_increase,                 0,      0,      INT_MAX,        },
	{ "vip_drop_increase",                  &battle_config.vip_drop_increase,               0,      0,      INT_MAX,        },
	{ "vip_gemstone",                       &battle_config.vip_gemstone,                    0,      0,      1,              },
	{ "vip_disp_rate",                      &battle_config.vip_disp_rate,                   1,      0,      1,              },
	{ "discount_item_point_shop",           &battle_config.discount_item_point_shop,        0,      0,      3,              },
	{ "oktoberfest_ignorepalette",          &battle_config.oktoberfest_ignorepalette,       0,      0,      1,              },
	{ "update_enemy_position",              &battle_config.update_enemy_position,           0,      0,      1,              },
	{ "devotion_rdamage",                   &battle_config.devotion_rdamage,                0,      0,    100,              },
	{ "feature.autotrade",                  &battle_config.feature_autotrade,               1,      0,      1,              },
	{ "feature.autotrade_direction",        &battle_config.feature_autotrade_direction,     4,      -1,     7,              },
	{ "feature.autotrade_head_direction",   &battle_config.feature_autotrade_head_direction,0,      -1,     2,              },
	{ "feature.autotrade_sit",              &battle_config.feature_autotrade_sit,           1,      -1,     1,              },
	{ "feature.autotrade_open_delay",       &battle_config.feature_autotrade_open_delay,    5000,   1000,   INT_MAX,        },
	{ "feature.autotrade_move",             &battle_config.feature_autotrade_move,          0,      0,      1,              },
	{ "disp_serverbank_msg",                &battle_config.disp_serverbank_msg,             0,      0,      1,              },
	{ "disp_servervip_msg",                 &battle_config.disp_servervip_msg,              0,      0,      1,              },
	{ "warg_can_falcon",                    &battle_config.warg_can_falcon,                 0,      0,      1,              },
	{ "path_blown_halt",                    &battle_config.path_blown_halt,                 1,      0,      1,              },
	{ "rental_mount_speed_boost",           &battle_config.rental_mount_speed_boost,        25,     0,      100,            },
	{ "feature.warp_suggestions",           &battle_config.warp_suggestions_enabled,        0,      0,      1,              },
	{ "taekwon_mission_mobname",            &battle_config.taekwon_mission_mobname,         0,      0,      2,              },
	{ "teleport_on_portal",                 &battle_config.teleport_on_portal,              0,      0,      1,              },
	{ "cart_revo_knockback",                &battle_config.cart_revo_knockback,             1,      0,      1,              },
	{ "guild_castle_invite",                &battle_config.guild_castle_invite,             0,      0,      1,              },
	{ "guild_castle_expulsion",             &battle_config.guild_castle_expulsion,          0,      0,      1,              },
	{ "transcendent_status_points",         &battle_config.transcendent_status_points,     52,      1,      INT_MAX,        },
	{ "taekwon_ranker_min_lv",              &battle_config.taekwon_ranker_min_lv,          90,      1,      MAX_LEVEL,      },
	{ "revive_onwarp",                      &battle_config.revive_onwarp,                   1,      0,      1,              },
	{ "fame_taekwon_mission",               &battle_config.fame_taekwon_mission,            1,      0,      INT_MAX,        },
	{ "fame_refine_lv1",                    &battle_config.fame_refine_lv1,                 1,      0,      INT_MAX,        },
	{ "fame_refine_lv1",                    &battle_config.fame_refine_lv1,                 1,      0,      INT_MAX,        },
	{ "fame_refine_lv2",                    &battle_config.fame_refine_lv2,                 25,     0,      INT_MAX,        },
	{ "fame_refine_lv3",                    &battle_config.fame_refine_lv3,                 1000,   0,      INT_MAX,        },
	{ "fame_forge",                         &battle_config.fame_forge,                      10,     0,      INT_MAX,        },
	{ "fame_pharmacy_3",                    &battle_config.fame_pharmacy_3,                 1,      0,      INT_MAX,        },
	{ "fame_pharmacy_5",                    &battle_config.fame_pharmacy_5,                 3,      0,      INT_MAX,        },
	{ "fame_pharmacy_7",                    &battle_config.fame_pharmacy_7,                 10,     0,      INT_MAX,        },
	{ "fame_pharmacy_10",                   &battle_config.fame_pharmacy_10,                50,     0,      INT_MAX,        },
	{ "mail_delay",                         &battle_config.mail_delay,                      1000,   1000,   INT_MAX,        },
	{ "at_monsterignore",                   &battle_config.autotrade_monsterignore,         0,      0,      1,              },
	{ "spawn_direction",                    &battle_config.spawn_direction,                 0,      0,      1,              },
	{ "arrow_shower_knockback",             &battle_config.arrow_shower_knockback,          1,      0,      1,              },
	{ "devotion_rdamage_skill_only",        &battle_config.devotion_rdamage_skill_only,     1,      0,      1,              },
	{ "max_extended_aspd",                  &battle_config.max_extended_aspd,               193,    100,    199,            },
	{ "knockback_left",                     &battle_config.knockback_left,                  1,      0,      1,              },
	{ "song_timer_reset",                   &battle_config.song_timer_reset,                0,      0,      1,              },
	{ "cursed_circle_in_gvg",               &battle_config.cursed_circle_in_gvg,            1,      0,      1,              },
	{ "snap_dodge",                         &battle_config.snap_dodge,                      0,      0,      1,              },
	{ "monster_chase_refresh",              &battle_config.mob_chase_refresh,               3,      0,      30,             },
	{ "mob_icewall_walk_block",             &battle_config.mob_icewall_walk_block,          75,     0,      255,            },
	{ "boss_icewall_walk_block",            &battle_config.boss_icewall_walk_block,         0,      0,      255,            },
	{ "stormgust_knockback",                &battle_config.stormgust_knockback,             1,      0,      1,              },
	{ "default_fixed_castrate",             &battle_config.default_fixed_castrate,          20,     0,      100,            },
	{ "default_bind_on_equip",              &battle_config.default_bind_on_equip,    BOUND_CHAR, BOUND_NONE, BOUND_MAX - 1, },
	{ "pet_ignore_infinite_def",            &battle_config.pet_ignore_infinite_def,         0,      0,      1,              },
	{ "homunculus_evo_intimacy_need",       &battle_config.homunculus_evo_intimacy_need,    91100,  0,      INT_MAX,        },
	{ "homunculus_evo_intimacy_reset",      &battle_config.homunculus_evo_intimacy_reset,   1000,   0,      INT_MAX,        },
	{ "monster_loot_search_type",           &battle_config.monster_loot_search_type,        1,      0,      1,              },
	{ "max_homunculus_hp",                  &battle_config.max_homunculus_hp,               32767,  100,    INT_MAX,        },
	{ "max_homunculus_sp",                  &battle_config.max_homunculus_sp,               32767,  100,    INT_MAX,        },
	{ "max_homunculus_parameter",           &battle_config.max_homunculus_parameter,        150,    10,     SHRT_MAX,       },
	{ "feature.roulette",                   &battle_config.feature_roulette,                1,      0,      1,              },
	{ "monster_hp_bars_info",               &battle_config.monster_hp_bars_info,            1,      0,      1,              },
	{ "min_body_style",                     &battle_config.min_body_style,                  0,      0,      SHRT_MAX,       },
	{ "max_body_style",                     &battle_config.max_body_style,                  1,      0,      SHRT_MAX,       },
	{ "save_body_style",                    &battle_config.save_body_style,                 0,      0,      1,              },
	{ "mvp_exp_reward_message",             &battle_config.mvp_exp_reward_message,          0,      0,      1,              },
	{ "max_summoner_parameter",             &battle_config.max_summoner_parameter,          120,    10,     SHRT_MAX,       },
	{ "monster_eye_range_bonus",            &battle_config.mob_eye_range_bonus,             0,      0,      10,             },
	{ "crimsonrock_knockback",              &battle_config.crimsonrock_knockback,           1,      0,      1,              },
	{ "tarotcard_equal_chance",             &battle_config.tarotcard_equal_chance,          0,      0,      1,              },
	{ "show_status_katar_crit",             &battle_config.show_status_katar_crit,          0,      0,      1,              },
	{ "dispel_song",                        &battle_config.dispel_song,                     0,      0,      1,              },
	{ "monster_stuck_warning",              &battle_config.mob_stuck_warning,               0,      0,      1,              },
	{ "guild_maprespawn_clones",            &battle_config.guild_maprespawn_clones,         0,      0,      1,              },
	{ "skill_eightpath_algorithm",          &battle_config.skill_eightpath_algorithm,       1,      0,      1,              },
	{ "can_damage_skill",                   &battle_config.can_damage_skill,                1,      0,      BL_ALL,         },
	{ "atcommand_levelup_events",           &battle_config.atcommand_levelup_events,        0,      0,      1,              },
};

#ifndef STATS_OPT_OUT
/**
 * rAthena anonymous statistic usage report -- packet is built here, and sent to char server to report.
 */
void rAthena_report(char *date, char *time_c) {
	int i, rev = 0, bd_size = ARRAYLENGTH(battle_data);
	unsigned int config = 0;
	const char *rev_str;
	char timestring[25];
	time_t curtime;
	char *buf;

	enum config_table {
		C_CIRCULAR_AREA         = 0x0001,
		C_CELLNOSTACK           = 0x0002,
		C_BETA_THREAD_TEST      = 0x0004,
		C_SCRIPT_CALLFUNC_CHECK = 0x0008,
		C_OFFICIAL_WALKPATH     = 0x0010,
		C_RENEWAL               = 0x0020,
		C_RENEWAL_CAST          = 0x0040,
		C_RENEWAL_DROP          = 0x0080,
		C_RENEWAL_EXP           = 0x0100,
		C_RENEWAL_LVDMG         = 0x0200,
		C_RENEWAL_EDP           = 0x0400,
		C_RENEWAL_ASPD          = 0x0800,
		C_SECURE_NPCTIMEOUT     = 0x1000,
		C_SQL_DBS               = 0x2000,
		C_SQL_LOGS              = 0x4000,
	};

	if( (rev_str = get_git_hash()) != 0 )
		rev = atoi(rev_str);

	//We get the current time
	time(&curtime);
	strftime(timestring, 24, "%Y-%m-%d %H:%M:%S", localtime(&curtime));

//Various compile-time options
#ifdef CIRCULAR_AREA
	config |= C_CIRCULAR_AREA;
#endif
	
#ifdef CELL_NOSTACK
	config |= C_CELLNOSTACK;
#endif
	
#ifdef BETA_THREAD_TEST
	config |= C_BETA_THREAD_TEST;
#endif

#ifdef SCRIPT_CALLFUNC_CHECK
	config |= C_SCRIPT_CALLFUNC_CHECK;
#endif

#ifdef OFFICIAL_WALKPATH
	config |= C_OFFICIAL_WALKPATH;
#endif

#ifdef RENEWAL
	config |= C_RENEWAL;
#endif
	
#ifdef RENEWAL_CAST
	config |= C_RENEWAL_CAST;
#endif

#ifdef RENEWAL_DROP
	config |= C_RENEWAL_DROP;
#endif

#ifdef RENEWAL_EXP
	config |= C_RENEWAL_EXP;
#endif
	
#ifdef RENEWAL_LVDMG
	config |= C_RENEWAL_LVDMG;
#endif

#ifdef RENEWAL_EDP
	config |= C_RENEWAL_EDP;
#endif
	
#ifdef RENEWAL_ASPD
	config |= C_RENEWAL_ASPD;
#endif
	
#ifdef SECURE_NPCTIMEOUT
	config |= C_SECURE_NPCTIMEOUT;
#endif

	//Non-define part
	if( db_use_sqldbs )
		config |= C_SQL_DBS;

	if( log_config.sql_logs )
		config |= C_SQL_LOGS;

#define BFLAG_LENGTH 35

	CREATE(buf, char, 6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + ( bd_size * ( BFLAG_LENGTH + 4 ) ) + 1 );

	//Build packet
	WBUFW(buf,0) = 0x3000;
	WBUFW(buf,2) = 6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + ( bd_size * ( BFLAG_LENGTH + 4 ) );
	WBUFW(buf,4) = 0x9c;

	safestrncpy((char *)WBUFP(buf,6), date, 12);
	safestrncpy((char *)WBUFP(buf,6 + 12), time_c, 9);
	safestrncpy((char *)WBUFP(buf,6 + 12 + 9), timestring, 24);

	WBUFL(buf,6 + 12 + 9 + 24)         = rev;
	WBUFL(buf,6 + 12 + 9 + 24 + 4)     = map_getusers();

	WBUFL(buf,6 + 12 + 9 + 24 + 4 + 4) = config;
	WBUFL(buf,6 + 12 + 9 + 24 + 4 + 4 + 4) = bd_size;

	for( i = 0; i < bd_size; i++ ) {
		safestrncpy((char *)WBUFP(buf,6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + ( i * ( BFLAG_LENGTH + 4 ) ) ), battle_data[i].str, 35);
		WBUFL(buf,6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + BFLAG_LENGTH + ( i * ( BFLAG_LENGTH + 4 )  )  ) = *battle_data[i].val;
	}

	chrif_send_report(buf, 6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + ( bd_size * ( BFLAG_LENGTH + 4 ) ) );

	aFree(buf);
	
#undef BFLAG_LENGTH
}
static int rAthena_report_timer(int tid, unsigned int tick, int id, intptr_t data) {
	if( chrif_isconnected() ) //Char server relays it, so it must be online
		rAthena_report(__DATE__,__TIME__);
	return 0;
}
#endif

/*==========================
 * Set battle settings
 *--------------------------*/
int battle_set_value(const char *w1, const char *w2)
{
	int val = config_switch(w2);

	int i;
	ARR_FIND(0, ARRAYLENGTH(battle_data), i, strcmpi(w1, battle_data[i].str) == 0);
	if (i == ARRAYLENGTH(battle_data))
		return 0; //Not found

	if (val < battle_data[i].min || val > battle_data[i].max) {
		ShowWarning("Value for setting '%s': %s is invalid (min:%i max:%i)! Defaulting to %i...\n", w1, w2, battle_data[i].min, battle_data[i].max, battle_data[i].defval);
		val = battle_data[i].defval;
	}

	*battle_data[i].val = val;
	return 1;
}

/*===========================
 * Get battle settings
 *---------------------------*/
int battle_get_value(const char *w1)
{
	int i;
	ARR_FIND(0, ARRAYLENGTH(battle_data), i, strcmpi(w1, battle_data[i].str) == 0);
	if (i == ARRAYLENGTH(battle_data))
		return 0; //Not found
	else
		return *battle_data[i].val;
}

/*======================
 * Set default settings
 *----------------------*/
void battle_set_defaults()
{
	int i;
	for (i = 0; i < ARRAYLENGTH(battle_data); i++)
		*battle_data[i].val = battle_data[i].defval;
}

/*==================================
 * Cap certain battle.conf settings
 *----------------------------------*/
void battle_adjust_conf()
{
	battle_config.monster_max_aspd = 2000 - battle_config.monster_max_aspd * 10;
	battle_config.max_aspd = 2000 - battle_config.max_aspd * 10;
	battle_config.max_third_aspd = 2000 - battle_config.max_third_aspd * 10;
	battle_config.max_extended_aspd = 2000 - battle_config.max_extended_aspd * 10;
	battle_config.max_walk_speed = 100 * DEFAULT_WALK_SPEED / battle_config.max_walk_speed;
	battle_config.max_cart_weight *= 10;

	if (battle_config.max_def > 100 && !battle_config.weapon_defense_type) //Added by [Skotlex]
		battle_config.max_def = 100;

	if (battle_config.min_hitrate > battle_config.max_hitrate)
		battle_config.min_hitrate = battle_config.max_hitrate;

	if (battle_config.pet_max_atk1 > battle_config.pet_max_atk2) //Skotlex
		battle_config.pet_max_atk1 = battle_config.pet_max_atk2;

	if (battle_config.day_duration && battle_config.day_duration < 60000) //Added by [Yor]
		battle_config.day_duration = 60000;
	if (battle_config.night_duration && battle_config.night_duration < 60000) //Added by [Yor]
		battle_config.night_duration = 60000;

#if PACKETVER < 20100427
	if (battle_config.feature_buying_store) {
		ShowWarning("conf/battle/feature.conf:buying_store is enabled but it requires PACKETVER 2010-04-27 or newer, disabling...\n");
		battle_config.feature_buying_store = 0;
	}
#endif

#if PACKETVER < 20100803
	if (battle_config.feature_search_stores) {
		ShowWarning("conf/battle/feature.conf:search_stores is enabled but it requires PACKETVER 2010-08-03 or newer, disabling...\n");
		battle_config.feature_search_stores = 0;
	}
#endif

#if PACKETVER > 20120000 && PACKETVER < 20130515 //Exact date (when it started) not known
	if (battle_config.feature_auction) {
		ShowWarning("conf/battle/feature.conf:feature.auction is enabled but it is not stable on PACKETVER "EXPAND_AND_QUOTE(PACKETVER)", disabling...\n");
		ShowWarning("conf/battle/feature.conf:feature.auction change value to '2' to silence this warning and maintain it enabled\n");
		battle_config.feature_auction = 0;
	}
#elif PACKETVER >= 20141112
	if (battle_config.feature_auction) {
		ShowWarning("conf/battle/feature.conf:feature.auction is enabled but it is not available for clients from 2014-11-12 on, disabling...\n");
		ShowWarning("conf/battle/feature.conf:feature.auction change value to '2' to silence this warning and maintain it enabled\n");
		battle_config.feature_auction = 0;
	}
#endif

#if PACKETVER < 20130724
	if (battle_config.feature_banking) {
		ShowWarning("conf/battle/feature.conf banking is enabled but it requires PACKETVER 2013-07-24 or newer, disabling...\n");
		battle_config.feature_banking = 0;
	}
#endif

#if PACKETVER < 20131223
	if (battle_config.mvp_exp_reward_message) {
		ShowWarning("conf/battle/client.conf MVP EXP reward message is enabled but it requires PACKETVER 2013-12-23 or newer, disabling...\n");
		battle_config.mvp_exp_reward_message = 0;
	}
#endif

#if PACKETVER < 20141022
	if (battle_config.feature_roulette) {
		ShowWarning("conf/battle/feature.conf roulette is enabled but it requires PACKETVER 2014-10-22 or newer, disabling...\n");
		battle_config.feature_roulette = 0;
	}
#elif PACKETVER >= 20150401
	if (battle_config.feature_auction) {
		ShowWarning("conf/battle/feature.conf roulette is enabled but it is not available for clients from 2015-04-01 on, disabling...\n");
		battle_config.feature_roulette = 0;
	}
#endif

#ifndef CELL_NOSTACK
	if (battle_config.custom_cell_stack_limit != 1)
		ShowWarning("Battle setting 'custom_cell_stack_limit' takes no effect as this server was compiled without Cell Stack Limit support.\n");
#endif
}

/*=====================================
 * Read battle.conf settings from file
 *-------------------------------------*/
int battle_config_read(const char *cfgName)
{
	FILE *fp;
	static int count = 0;

	if (count == 0)
		battle_set_defaults();

	count++;

	fp = fopen(cfgName,"r");
	if (fp == NULL)
		ShowError("File not found: %s\n", cfgName);
	else {
		char line[1024], w1[1024], w2[1024];

		while(fgets(line, sizeof(line), fp)) {
			if (line[0] == '/' && line[1] == '/')
				continue;
			if (sscanf(line, "%1023[^:]:%1023s", w1, w2) != 2)
				continue;
			if (strcmpi(w1, "import") == 0)
				battle_config_read(w2);
			else if
				(battle_set_value(w1, w2) == 0)
				ShowWarning("Unknown setting '%s' in file %s\n", w1, cfgName);
		}

		fclose(fp);
	}

	count--;

	if (count == 0)
		battle_adjust_conf();

	return 0;
}

/*==========================
 * Initialize battle timer
 *--------------------------*/
void do_init_battle(void)
{
	delay_damage_ers = ers_new(sizeof(struct delay_damage),"battle.c::delay_damage_ers",ERS_OPT_CLEAR);
	add_timer_func_list(battle_delay_damage_sub, "battle_delay_damage_sub");
	
#ifndef STATS_OPT_OUT
	add_timer_func_list(rAthena_report_timer, "rAthena_report_timer");
	add_timer_interval(gettick() + 30000, rAthena_report_timer, 0, 0, 60000 * 30);
#endif

}

/*==================
 * End battle timer
 *------------------*/
void do_final_battle(void)
{
	ers_destroy(delay_damage_ers);
}
