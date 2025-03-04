﻿/*
===========================================================================

  Copyright (c) 2010-2015 Darkstar Dev Teams

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

  This file is part of DarkStar-server source code.

===========================================================================
*/

#include "../common/showmsg.h"
#include "../common/timer.h"

#include <vector>
#include <string.h>
#include "alliance.h"
#include "entities/battleentity.h"
#include "utils/charutils.h"
#include "conquest_system.h"
#include "utils/battleutils.h"
#include "utils/blueutils.h"
#include "utils/jailutils.h"
#include "utils/petutils.h"
#include "utils/zoneutils.h"
#include "map.h"
#include "party.h"
#include "treasure_pool.h"
#include "message.h"

#include "packets/char_sync.h"
#include "packets/char_update.h"
#include "packets/char_abilities.h"
#include "packets/menu_config.h"
#include "packets/message_standard.h"
#include "packets/party_define.h"
#include "packets/party_member_update.h"


/************************************************************************
*																		*
*  Конструктор   														*
*																		*
************************************************************************/

CParty::CParty(CBattleEntity* PEntity)
{
	DSP_DEBUG_BREAK_IF(PEntity == nullptr);
	DSP_DEBUG_BREAK_IF(PEntity->PParty != nullptr);

    m_PartyID   = PEntity->id;
    m_PartyType = PEntity->objtype == TYPE_PC ? PARTY_PCS : PARTY_MOBS;
    m_PartyNumber = 0;

    m_PLeader       = nullptr;
	m_PAlliance		= nullptr;
	m_PSyncTarget 	= nullptr;
	m_PQuaterMaster = nullptr;


	AddMember(PEntity);
	SetLeader((int8*)PEntity->name.c_str());
}

CParty::CParty(uint32 id)
{
	m_PartyID = id;
	m_PartyType = PARTY_PCS;
    m_PartyNumber = 0;

	m_PLeader       = nullptr;
	m_PAlliance     = nullptr;
	m_PSyncTarget   = nullptr;
	m_PQuaterMaster = nullptr;
}

/************************************************************************
*																		*
*  Распускаем группу													*
*																		*
************************************************************************/

void CParty::DisbandParty(bool playerInitiated)
{
    if (m_PAlliance)
    {
        m_PAlliance->delParty(this);
    }
	m_PSyncTarget = nullptr;
	SetQuarterMaster(nullptr);

	m_PLeader = nullptr;
	m_PAlliance	= nullptr;

    if (m_PartyType == PARTY_PCS)
    {
        PushPacket(0, 0, new CPartyDefinePacket(nullptr));

	    for (uint8 i = 0; i < members.size(); ++i)
	    {
		    CCharEntity* PChar = (CCharEntity*)members.at(i);

		    PChar->PParty = nullptr;
			PChar->PLatentEffectContainer->CheckLatentsPartyJobs();
			PChar->PLatentEffectContainer->CheckLatentsPartyMembers(members.size());
			PChar->PLatentEffectContainer->CheckLatentsPartyAvatar();
		    PChar->pushPacket(new CPartyMemberUpdatePacket(PChar, 0, PChar->getZone()));

		    // TODO: TreasurePool должен оставаться у последнего персонажа, но сейчас это не критично

            if (PChar->PTreasurePool != nullptr &&
                PChar->PTreasurePool->GetPoolType() != TREASUREPOOL_ZONE)
		    {
			    PChar->PTreasurePool->DelMember(PChar);
			    PChar->PTreasurePool = new CTreasurePool(TREASUREPOOL_SOLO);
			    PChar->PTreasurePool->AddMember(PChar);
                PChar->PTreasurePool->UpdatePool(PChar);
		    }
            CStatusEffect* sync = PChar->StatusEffectContainer->GetStatusEffect(EFFECT_LEVEL_SYNC);
            if (sync && sync->GetDuration() == 0)
            {
    			PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, 0, 30, 553));
                sync->SetStartTime(gettick());
                sync->SetDuration(30000);
            }
            Sql_Query(SqlHandle, "DELETE FROM accounts_parties WHERE charid = %u;", PChar->id);
	    }

        // make sure chat server isn't notified of a disband if this came from the chat server already
        if (playerInitiated)
        {
            uint8 data[8];
            WBUFL(data, 0) = m_PartyID;
            WBUFL(data, 4) = m_PartyID;
            message::send(MSG_PT_DISBAND, data, sizeof data, nullptr);
        }
    }
	delete this;
}

/************************************************************************
*                                                                       *
*  Назначаем роли участникам группы	(только для персонажей)             *
*                                                                       *
************************************************************************/

void CParty::AssignPartyRole(int8* MemberName, uint8 role)
{
    DSP_DEBUG_BREAK_IF (m_PartyType != PARTY_PCS);

	switch(role)
	{
		case 0: SetLeader(MemberName);		    break;
        case 4: SetQuarterMaster(MemberName);   break;
		case 5: SetQuarterMaster(nullptr);	        break;
        case 6: SetSyncTarget(MemberName, 238);	break;
        case 7: SetSyncTarget(nullptr, 553);       break;
	}
	uint8 data[4];
	WBUFL(data, 0) = m_PartyID;
    message::send(MSG_PT_RELOAD, data, sizeof data, nullptr);
	return;
}

/************************************************************************
*																		*
*  Узнаем количество участников группы в указанной зоне					*
*																		*
************************************************************************/

uint8 CParty::MemberCount(uint16 ZoneID)
{
	uint8 count = 0;

	for (uint32 i = 0; i < members.size(); ++i)
	{
		if (members.at(i)->getZone() == ZoneID)
		{
			count++;
		}
	}
	return count;
}

/************************************************************************
*                                                                       *
*  Returns Entity Pointer to Party Member by Name (used for kick)       *
*                                                                       *
************************************************************************/

CBattleEntity* CParty::GetMemberByName(const int8* MemberName)
{
    DSP_DEBUG_BREAK_IF(m_PartyType != PARTY_PCS);

    for (uint32 i = 0; i < members.size(); ++i)
        if (strcmp(MemberName, members.at(i)->GetName()) == 0)
            return members.at(i);

    return nullptr;
}

/************************************************************************
*																		*
*  Удаляем персонажа из группы				  							*
*																		*
************************************************************************/

void CParty::RemoveMember(CBattleEntity* PEntity)
{
	DSP_DEBUG_BREAK_IF(PEntity == nullptr);
	DSP_DEBUG_BREAK_IF(PEntity->PParty != this);

	if (m_PLeader == PEntity)
	{
		RemovePartyLeader(PEntity);
	}
	else
	{
		for (uint32 i = 0; i < members.size(); ++i)
		{
			if (PEntity == members.at(i))
			{
				members.erase(members.begin()+i);

                if (m_PartyType == PARTY_PCS)
                {
                    CCharEntity* PChar = (CCharEntity*)PEntity;

				    if (m_PQuaterMaster == PChar)
				    {
					    SetQuarterMaster(nullptr);
				    }
				    if (m_PSyncTarget == PChar)
				    {
					    SetSyncTarget(nullptr, 553);
                        CStatusEffect* sync = PChar->StatusEffectContainer->GetStatusEffect(EFFECT_LEVEL_SYNC);
                        if (sync && sync->GetDuration() == 0)
                        {
			                PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, 0, 30, 553));
                            sync->SetStartTime(gettick());
                            sync->SetDuration(30000);
                        }
                        DisableSync();
				    }
                    if (m_PSyncTarget != nullptr && m_PSyncTarget != PChar)
                    {
                        if (PChar->status != STATUS_DISAPPEAR)
		                {
                            CStatusEffect* sync = PChar->StatusEffectContainer->GetStatusEffect(EFFECT_LEVEL_SYNC);
                            if (sync && sync->GetDuration() == 0)
                            {
			                    PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, 0, 30, 553));
                                sync->SetStartTime(gettick());
                                sync->SetDuration(30000);
                            }
		                }
                    }
                    PChar->PLatentEffectContainer->CheckLatentsPartyMembers(members.size());

				    PChar->pushPacket(new CPartyDefinePacket(nullptr));
				    PChar->pushPacket(new CPartyMemberUpdatePacket(PChar, 0, PChar->getZone()));
				    PChar->pushPacket(new CCharUpdatePacket(PChar));
				    PChar->PParty = nullptr;

					Sql_Query(SqlHandle, "DELETE FROM accounts_parties WHERE charid = %u;", PChar->id);

					uint8 data[4];
					WBUFL(data, 0) = m_PartyID;
                    message::send(MSG_PT_RELOAD, data, sizeof data, nullptr);

				    if (PChar->PTreasurePool != nullptr &&
					    PChar->PTreasurePool->GetPoolType() != TREASUREPOOL_ZONE)
				    {
					    PChar->PTreasurePool->DelMember(PChar);
					    PChar->PTreasurePool = new CTreasurePool(TREASUREPOOL_SOLO);
					    PChar->PTreasurePool->AddMember(PChar);
                        PChar->PTreasurePool->UpdatePool(PChar);
				    }
                }
				break;
			}
		}
	}
}

void CParty::DelMember(CBattleEntity* PEntity)
{
	DSP_DEBUG_BREAK_IF(PEntity == nullptr);
	DSP_DEBUG_BREAK_IF(PEntity->PParty != this);

	if (m_PLeader == PEntity)
	{
		RemovePartyLeader(PEntity);
	}
	else
	{
		for (uint32 i = 0; i < members.size(); ++i)
		{
			if (PEntity == members.at(i))
			{
				members.erase(members.begin() + i);

				if (m_PartyType == PARTY_PCS)
				{
					CCharEntity* PChar = (CCharEntity*)PEntity;

					if (m_PQuaterMaster == PChar)
					{
						SetQuarterMaster(nullptr);
					}
					if (m_PSyncTarget == PChar)
					{
						SetSyncTarget(nullptr, 553);
						CStatusEffect* sync = PChar->StatusEffectContainer->GetStatusEffect(EFFECT_LEVEL_SYNC);
						if (sync && sync->GetDuration() == 0)
						{
							PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, 0, 30, 553));
							sync->SetStartTime(gettick());
							sync->SetDuration(30000);
						}
						DisableSync();
					}
					if (m_PSyncTarget != nullptr && m_PSyncTarget != PChar)
					{
						if (PChar->status != STATUS_DISAPPEAR)
						{
							CStatusEffect* sync = PChar->StatusEffectContainer->GetStatusEffect(EFFECT_LEVEL_SYNC);
							if (sync && sync->GetDuration() == 0)
							{
								PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, 0, 30, 553));
								sync->SetStartTime(gettick());
								sync->SetDuration(30000);
							}
						}
					}
					PChar->PLatentEffectContainer->CheckLatentsPartyMembers(members.size());

					PChar->pushPacket(new CPartyDefinePacket(nullptr));
					PChar->pushPacket(new CPartyMemberUpdatePacket(PChar, 0, PChar->getZone()));
					PChar->pushPacket(new CCharUpdatePacket(PChar));
					PChar->PParty = nullptr;

					if (PChar->PTreasurePool != nullptr &&
						PChar->PTreasurePool->GetPoolType() != TREASUREPOOL_ZONE)
					{
						PChar->PTreasurePool->DelMember(PChar);
						PChar->PTreasurePool = new CTreasurePool(TREASUREPOOL_SOLO);
						PChar->PTreasurePool->AddMember(PChar);
						PChar->PTreasurePool->UpdatePool(PChar);
					}
				}
				break;
			}
		}
	}
}

void CParty::PopMember(CBattleEntity* PEntity)
{
    for (uint32 i = 0; i < members.size(); ++i)
    {
        if (PEntity == members.at(i))
        {
            members.erase(members.begin() + i);
        }
    }
    //free memory, party will re reinsatiated when they zone back in
    if (members.empty())
    {
        if (m_PAlliance)
        {
            if (m_PAlliance->getMainParty() == this)
            {
                m_PAlliance->setMainParty(nullptr);
            }
            for (uint8 i = 0; i < m_PAlliance->partyList.size(); ++i)
            {
                if (this == m_PAlliance->partyList.at(i))
                    m_PAlliance->partyList.erase(m_PAlliance->partyList.begin() + i);
            }
        }
        delete this;
    }
    PEntity->PParty = nullptr;
}

/************************************************************************
*																		*
*  Лидер покидает группу												*
*																		*
************************************************************************/

void CParty::RemovePartyLeader(CBattleEntity* PEntity)
{
	DSP_DEBUG_BREAK_IF(members.empty());

    int ret = Sql_Query(SqlHandle, "SELECT charname FROM accounts_sessions JOIN chars ON accounts_sessions.charid = chars.charid \
                                    JOIN accounts_parties ON accounts_parties.charid = chars.charid WHERE partyid = %u AND NOT partyflag & %d \
                                    ORDER BY timestamp ASC LIMIT 1;", m_PartyID, PARTY_LEADER);
    if (ret != SQL_ERROR && Sql_NumRows(SqlHandle) != 0 && Sql_NextRow(SqlHandle) == SQL_SUCCESS)
    {
        std::string newLeader(Sql_GetData(SqlHandle, 0));
        SetLeader(newLeader.c_str());
    }
    if (m_PLeader == PEntity)
    {
        DisbandParty();
    }
    else
    {
        RemoveMember(PEntity);
    }
}

/************************************************************************
*																		*
*  Добавляем персонажа в группу											*
*																		*
************************************************************************/

void CParty::AddMember(CBattleEntity* PEntity)
{
	DSP_DEBUG_BREAK_IF(PEntity == nullptr);
	DSP_DEBUG_BREAK_IF(PEntity->PParty != nullptr);

	PEntity->PParty = this;
	members.push_back(PEntity);

    if (m_PartyType == PARTY_PCS)
    {
        DSP_DEBUG_BREAK_IF(PEntity->objtype != TYPE_PC);

        CCharEntity* PChar = (CCharEntity*)PEntity;

        uint32 allianceid = 0;
        if (m_PAlliance)
        {
            allianceid = m_PAlliance->m_AllianceID;
        }

        Sql_Query(SqlHandle, "INSERT INTO accounts_parties (charid, partyid, allianceid, partyflag) VALUES (%u, %u, %u, %u);", PChar->id, m_PartyID, allianceid, GetMemberFlags(PChar));
		uint8 data[4];
		WBUFL(data, 0) = m_PartyID;
        message::send(MSG_PT_RELOAD, data, sizeof data, nullptr);
		ReloadTreasurePool(PChar);

	    if (PChar->nameflags.flags & FLAG_INVITE)
	    {
            PChar->nameflags.flags ^= FLAG_INVITE;
            PChar->updatemask |= UPDATE_HP;

            charutils::SaveCharStats(PChar);

		    PChar->pushPacket(new CMenuConfigPacket(PChar));
		    PChar->pushPacket(new CCharUpdatePacket(PChar));
		    PChar->pushPacket(new CCharSyncPacket(PChar));
	    }
	    PChar->PTreasurePool->UpdatePool(PChar);

        //Apply level sync if the party is level synced 
        if (m_PSyncTarget != nullptr)
        {
            if (PChar->getZone() == m_PSyncTarget->getZone() )
		    {
			    PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, 0, m_PSyncTarget->GetMLevel(), 540));
                PChar->StatusEffectContainer->AddStatusEffect(new CStatusEffect(
                    EFFECT_LEVEL_SYNC,
                    EFFECT_LEVEL_SYNC,
                    m_PSyncTarget->GetMLevel(),
                    0,
                    0), true);
                PChar->StatusEffectContainer->DelStatusEffectsByFlag(EFFECTFLAG_DISPELABLE | EFFECTFLAG_ON_ZONE);
                PChar->loc.zone->PushPacket(PChar, CHAR_INRANGE, new CCharSyncPacket(PChar));
            }
        }
    }
}

void CParty::AddMember(uint32 id)
{
	if (m_PartyType == PARTY_PCS)
	{
        uint32 allianceid = 0;
        if (m_PAlliance)
        {
            allianceid = m_PAlliance->m_AllianceID;
        }
		Sql_Query(SqlHandle, "INSERT INTO accounts_parties (charid, partyid, allianceid, partyflag) VALUES (%u, %u, %u, %u);", id, m_PartyID, allianceid, 0);
		uint8 data[4];
		WBUFL(data, 0) = m_PartyID;
        message::send(MSG_PT_RELOAD, data, sizeof data, nullptr);

		/*if (PChar->nameflags.flags & FLAG_INVITE)
		{
			PChar->nameflags.flags ^= FLAG_INVITE;
            PChar->updatemask |= UPDATE_HP;

			charutils::SaveCharStats(PChar);

			PChar->status = STATUS_UPDATE;
			PChar->pushPacket(new CMenuConfigPacket(PChar));
			PChar->pushPacket(new CCharUpdatePacket(PChar));
			PChar->pushPacket(new CCharSyncPacket(PChar));
		}
		PChar->PTreasurePool->UpdatePool(PChar);*/
	}
}

void CParty::PushMember(CBattleEntity* PEntity)
{
    DSP_DEBUG_BREAK_IF(PEntity == nullptr);
    DSP_DEBUG_BREAK_IF(PEntity->PParty != nullptr);

    PEntity->PParty = this;
    members.push_back(PEntity);

    //TODO: get flags from db, set leader/qm/etc appropriately

    ReloadTreasurePool((CCharEntity*)PEntity);
}

void CParty::SetPartyID(uint32 id)
{
    m_PartyID = id;
}

/************************************************************************
*																		*
*  Получаем уникальный ID группы										*
*																		*
************************************************************************/

uint32 CParty::GetPartyID()
{
	return m_PartyID;
}

/************************************************************************
*																		*
*  Получаем указатель на лидера группы									*
*																		*
************************************************************************/

CBattleEntity* CParty::GetLeader()
{
	return m_PLeader;
}

/************************************************************************
*																		*
*  Получаем указатель на цель синхронизации уровней						*
*																		*
************************************************************************/

CBattleEntity* CParty::GetSyncTarget()
{
	return m_PSyncTarget;
}

/************************************************************************
*																		*
*  Получаем указатель на владельца сокровищ								*
*																		*
************************************************************************/

CBattleEntity* CParty::GetQuaterMaster()
{
	return m_PQuaterMaster;
}



/************************************************************************
*                                                                       *
*  Получаем список флагов персонажа                                     *
*                                                                       *
************************************************************************/

uint16 CParty::GetMemberFlags(CBattleEntity* PEntity)
{
    DSP_DEBUG_BREAK_IF(PEntity == nullptr);
	DSP_DEBUG_BREAK_IF(PEntity->PParty != this);

    uint16 Flags = 0;

	if (PEntity->PParty->m_PAlliance != nullptr)
	{
		if (PEntity == m_PLeader && PEntity->PParty->m_PAlliance->getMainParty() == PEntity->PParty)
			Flags |= ALLIANCE_LEADER;
	}

    if (PEntity->PParty->m_PartyNumber == 1)
        Flags += PARTY_SECOND;
    else if (PEntity->PParty->m_PartyNumber == 2)
        Flags += PARTY_THIRD;

    if (PEntity == m_PLeader)       Flags |= PARTY_LEADER;
	if (PEntity == m_PQuaterMaster) Flags |= PARTY_QM;
    if (PEntity == m_PSyncTarget)   Flags |= PARTY_SYNC;

    return Flags;
}

/************************************************************************
*                                                                       *
*  Обновляем карту группы для всех членов группы                        *
*                                                                       *
************************************************************************/

void CParty::ReloadParty()
{
    //check if partyflags have changed
    int ret = Sql_Query(SqlHandle, "SELECT charid, partyflag FROM accounts_parties WHERE partyid = %d;", m_PartyID);

    if (ret != SQL_ERROR && Sql_NumRows(SqlHandle) > 0)
    {
        uint8 j = 0;
        while (Sql_NextRow(SqlHandle) == SQL_SUCCESS)
        {
            uint32 charid = Sql_GetUIntData(SqlHandle, 0);
            uint16 memberflags = Sql_GetUIntData(SqlHandle, 1);

            if (memberflags & PARTY_LEADER)
            {
                bool found = false;
                for (auto member : members)
                {
                    if (member->id == charid)
                    {
                        m_PLeader = member;
                        found = true;
                    }
                }
                if (!found)
                {
                    m_PLeader = nullptr;
                }
            }
            if (memberflags & PARTY_QM)
            {
                bool found = false;
                for (auto member : members)
                {
                    if (member->id == charid)
                    {
                        m_PQuaterMaster = member;
                        found = true;
                    }
                }
                if (!found)
                {
                    m_PQuaterMaster = nullptr;
                }
            }
            if (memberflags & ALLIANCE_LEADER && m_PAlliance)
            {
                bool found = false;
                for (auto member : members)
                {
                    if (member->id == charid)
                    {
                        m_PAlliance->setMainParty(this);
                        found = true;
                    }
                }
                if (!found)
                {
                    m_PAlliance->setMainParty(nullptr);
                }
            }
        }
    }
	//alliance
	if (this->m_PAlliance != nullptr)
	{
		for (uint8 a = 0; a < m_PAlliance->partyList.size(); ++a)
		{
			for (uint8 i = 0; i < m_PAlliance->partyList.at(a)->members.size(); ++i)
			{
				CCharEntity* PChar = (CCharEntity*)m_PAlliance->partyList.at(a)->members.at(i);
                PChar->ReloadPartyDec();
				uint16 alliance = 0;
				PChar->pushPacket(new CPartyDefinePacket(m_PAlliance->partyList.at(a)));
				int ret = Sql_Query(SqlHandle, "SELECT chars.charid, chars.charname, partyflag, pos_zone, pos_prevzone FROM accounts_parties \
												LEFT JOIN chars ON accounts_parties.charid = chars.charid WHERE \
												allianceid = %d ORDER BY partyflag & %u, timestamp;",
												m_PAlliance->m_AllianceID, PARTY_SECOND | PARTY_THIRD);
				if (ret != SQL_ERROR && Sql_NumRows(SqlHandle) > 0)
				{
					uint8 j = 0;
					while (Sql_NextRow(SqlHandle) == SQL_SUCCESS)
					{
						if ((Sql_GetUIntData(SqlHandle, 2) & (PARTY_SECOND | PARTY_THIRD)) != alliance)
						{
							alliance = Sql_GetUIntData(SqlHandle, 2) & (PARTY_SECOND | PARTY_THIRD);
							j = 0;
						}
						CCharEntity* PPartyMember = zoneutils::GetChar(Sql_GetUIntData(SqlHandle, 0));
						if (PPartyMember)
						{
							PChar->pushPacket(new CPartyMemberUpdatePacket(PPartyMember, j, PChar->getZone()));
						}
						else
						{
                            uint16 zoneid = Sql_GetUIntData(SqlHandle, 3) == 0 ? Sql_GetUIntData(SqlHandle, 4) : Sql_GetUIntData(SqlHandle, 3);
							PChar->pushPacket(new CPartyMemberUpdatePacket(
								Sql_GetUIntData(SqlHandle, 0), Sql_GetData(SqlHandle, 1),
								Sql_GetUIntData(SqlHandle, 2), zoneid));
						}
						j++;
					}
				}
			}
		}
	}
	else

	//regular party
	for (uint8 i = 0; i < members.size(); ++i)
	{
		CCharEntity* PChar = (CCharEntity*)members.at(i);

		PChar->PLatentEffectContainer->CheckLatentsPartyJobs();
		PChar->PLatentEffectContainer->CheckLatentsPartyMembers(members.size());
		PChar->PLatentEffectContainer->CheckLatentsPartyAvatar();
        PChar->ReloadPartyDec();
		PChar->pushPacket(new CPartyDefinePacket(this));
		int ret = Sql_Query(SqlHandle, "SELECT chars.charid, chars.charname, partyflag, pos_zone, pos_prevzone FROM accounts_parties \
									   	LEFT JOIN chars ON accounts_parties.charid = chars.charid WHERE \
										partyid = %d ORDER BY timestamp;",
										m_PartyID);
		if (ret != SQL_ERROR && Sql_NumRows(SqlHandle) > 0)
		{
			uint8 j = 0;
			while (Sql_NextRow(SqlHandle) == SQL_SUCCESS)
			{
				CCharEntity* PPartyMember = zoneutils::GetChar(Sql_GetUIntData(SqlHandle, 0));
				if (PPartyMember)
				{
					PChar->pushPacket(new CPartyMemberUpdatePacket(PPartyMember, j, PChar->getZone()));
				}
				else
                {
                    uint16 zoneid = Sql_GetUIntData(SqlHandle, 3) == 0 ? Sql_GetUIntData(SqlHandle, 4) : Sql_GetUIntData(SqlHandle, 3);
                    PChar->pushPacket(new CPartyMemberUpdatePacket(
                        Sql_GetUIntData(SqlHandle, 0), Sql_GetData(SqlHandle, 1),
                        Sql_GetUIntData(SqlHandle, 2), zoneid));
                }
				j++;
			}
		}
	}
}


/************************************************************************
*																		*
*  Обновляем статусы членов группы для выбранного персонажа				*
*  Возвращаем номер персонажа в группе									*
*																		*
************************************************************************/

void CParty::ReloadPartyMembers(CCharEntity* PChar)
{
    PChar->ReloadPartyDec();
    PChar->pushPacket(new CPartyDefinePacket(this));
    
    int ret;
    int alliance = 0;

    if (m_PAlliance)
    {
        ret = Sql_Query(SqlHandle, "SELECT chars.charid, chars.charname, partyflag, pos_zone, pos_prevzone FROM accounts_parties \
                                    LEFT JOIN chars ON accounts_parties.charid = chars.charid WHERE \
                                    allianceid = %d ORDER BY partyflag & %u, timestamp;",
                                    m_PAlliance->m_AllianceID, PARTY_SECOND | PARTY_THIRD);
    }
    else
    {
        ret = Sql_Query(SqlHandle, "SELECT chars.charid, chars.charname, partyflag, pos_zone, pos_prevzone FROM accounts_parties \
                                    LEFT JOIN chars ON accounts_parties.charid = chars.charid WHERE \
                                    partyid = %d ORDER BY timestamp;",
                                    m_PartyID);
    }

    if (ret != SQL_ERROR && Sql_NumRows(SqlHandle) > 0)
    {
        uint8 j = 0;
        while (Sql_NextRow(SqlHandle) == SQL_SUCCESS)
        {
            if ((Sql_GetUIntData(SqlHandle, 2) & (PARTY_SECOND | PARTY_THIRD)) != alliance)
            {
                alliance = Sql_GetUIntData(SqlHandle, 2) & (PARTY_SECOND | PARTY_THIRD);
                j = 0;
            }
            CCharEntity* PPartyMember = zoneutils::GetChar(Sql_GetUIntData(SqlHandle, 0));
            if (PPartyMember)
            {
                PChar->pushPacket(new CPartyMemberUpdatePacket(PPartyMember, j, PChar->getZone()));
            }
            else
            {
                uint16 zoneid = Sql_GetUIntData(SqlHandle, 3) == 0 ? Sql_GetUIntData(SqlHandle, 4) : Sql_GetUIntData(SqlHandle, 3);
                PChar->pushPacket(new CPartyMemberUpdatePacket(
                    Sql_GetUIntData(SqlHandle, 0), Sql_GetData(SqlHandle, 1),
                    Sql_GetUIntData(SqlHandle, 2), zoneid));
            }
            j++;
        }
    }
}

/************************************************************************
*																		*
*  Обновляем TreasurePool для указанного персонажа						*
*																		*
************************************************************************/

void CParty::ReloadTreasurePool(CCharEntity* PChar)
{
	DSP_DEBUG_BREAK_IF(PChar == nullptr);

	if (PChar->PTreasurePool != nullptr && PChar->PTreasurePool->GetPoolType() == TREASUREPOOL_ZONE){
		return;
	}


	//alliance
	if(PChar->PParty != nullptr)
	{
		if (PChar->PParty->m_PAlliance != nullptr)
		{
			for (uint8 a = 0; a < PChar->PParty->m_PAlliance->partyList.size(); ++a)
			{
				for (uint8 i = 0; i < PChar->PParty->m_PAlliance->partyList.at(a)->members.size(); ++i)
				{
					CCharEntity* PPartyMember = (CCharEntity*)PChar->PParty->m_PAlliance->partyList.at(a)->members.at(i);

					if (PPartyMember != PChar && PPartyMember->PTreasurePool != nullptr &&	PPartyMember->getZone() == PChar->getZone())
					{
						if (PChar->PTreasurePool != nullptr)
						{
							PChar->PTreasurePool->DelMember(PChar);
						}
						PChar->PTreasurePool = PPartyMember->PTreasurePool;
						PChar->PTreasurePool->AddMember(PChar);
						return;
					}
				}

			}//regular party
		}else if (PChar->PParty->m_PAlliance == nullptr){
					for (uint8 i = 0; i < members.size(); ++i){
						CCharEntity* PPartyMember = (CCharEntity*)members.at(i);

						if (PPartyMember != PChar &&
							PPartyMember->PTreasurePool != nullptr &&
							PPartyMember->getZone() == PChar->getZone())
						{
							if (PChar->PTreasurePool != nullptr)
							{
								PChar->PTreasurePool->DelMember(PChar);
							}
							PChar->PTreasurePool = PPartyMember->PTreasurePool;
							PChar->PTreasurePool->AddMember(PChar);
							return;
						}
					}
				}
	}

	if (PChar->PTreasurePool == nullptr)
	{
		PChar->PTreasurePool = new CTreasurePool(TREASUREPOOL_SOLO);
		PChar->PTreasurePool->AddMember(PChar);
	}
}

/************************************************************************
*																		*
*  Устанавливаем лидера группы											*
*																		*
************************************************************************/

void CParty::SetLeader(const char* MemberName)
{
    if (m_PartyType == PARTY_PCS)
    {
        uint32 newId = 0;
        int ret = Sql_Query(SqlHandle, "SELECT chars.charid from accounts_sessions JOIN chars ON chars.charid = accounts_sessions.charid WHERE charname = ('%s')", MemberName);

        if (ret != SQL_ERROR && Sql_NumRows(SqlHandle) != 0 && Sql_NextRow(SqlHandle) == SQL_SUCCESS)
        {
            newId = Sql_GetUIntData(SqlHandle, 0);
        }
        else
        {
            return;
        }

        Sql_Query(SqlHandle, "UPDATE accounts_parties SET partyflag = partyflag & ~%d WHERE partyid = %u AND partyflag & %d", ALLIANCE_LEADER | PARTY_LEADER, m_PartyID, PARTY_LEADER);
        Sql_Query(SqlHandle, "UPDATE accounts_parties SET partyid = %u WHERE partyid = %u", newId, m_PartyID);
        Sql_Query(SqlHandle, "UPDATE accounts_parties SET allianceid = %u WHERE allianceid = %u", newId, m_PartyID);

        m_PLeader = GetMemberByName(MemberName);
        if (this->m_PAlliance && this->m_PAlliance->m_AllianceID == m_PartyID)
            m_PAlliance->m_AllianceID = newId;

        m_PartyID = newId;
		Sql_Query(SqlHandle, "UPDATE accounts_parties SET partyflag = partyflag | IF(allianceid = partyid, %d, %d) WHERE charid = %u", ALLIANCE_LEADER | PARTY_LEADER, PARTY_LEADER, newId);
    }
    else
    {
        m_PLeader = members.at(0);
    }
}

/************************************************************************
*																		*
*  Устанавливаем цель синхронизации уровней								*
*																		*
************************************************************************/

void CParty::SetSyncTarget(int8* MemberName, uint16 message)
{
    CBattleEntity* PEntity = nullptr;
    if (MemberName)
    {
        PEntity = GetMemberByName(MemberName);
    }

    if (map_config.level_sync_enable)
    {
        if (PEntity && PEntity->objtype == TYPE_PC)
        {
            CCharEntity* PChar = (CCharEntity*)PEntity;
            //enable level sync
            if (PChar->GetMLevel() < 10 )
            {
                ((CCharEntity*)GetLeader())->pushPacket(new CMessageBasicPacket((CCharEntity*)GetLeader(), (CCharEntity*)GetLeader(), 0, 10, 541));
                return;
            }
            else if (PChar->getZone() != GetLeader()->getZone())
            {
                ((CCharEntity*)GetLeader())->pushPacket(new CMessageBasicPacket((CCharEntity*)GetLeader(), (CCharEntity*)GetLeader(), 0, 0, 542));
                return;
            }
            else
            {
                for (uint8 i = 0; i < members.size(); ++i)
                {
                    if (members.at(i)->StatusEffectContainer->HasStatusEffect(EFFECT_LEVEL_RESTRICTION) || members.at(i)->StatusEffectContainer->HasStatusEffect(EFFECT_LEVEL_SYNC))
                    {
                        ((CCharEntity*)GetLeader())->pushPacket(new CMessageBasicPacket((CCharEntity*)GetLeader(), (CCharEntity*)GetLeader(), 0, 0, 543));
                        return;
                    }
                }
                m_PSyncTarget = PChar;
	            for (uint8 i = 0; i < members.size(); ++i)
	            {
		            if(members.at(i)->objtype != TYPE_PC) continue;

		            CCharEntity* member = (CCharEntity*)members.at(i);

                    if (member->status != STATUS_DISAPPEAR &&
                         member->getZone() == PChar->getZone() )
		            {
			            member->pushPacket(new CMessageStandardPacket(PChar->GetMLevel(), 0, 0, 0, message));
                        member->StatusEffectContainer->AddStatusEffect(new CStatusEffect(
                            EFFECT_LEVEL_SYNC,
                            EFFECT_LEVEL_SYNC,
                            PChar->GetMLevel(),
                            0,
                            0), true);
                        member->StatusEffectContainer->DelStatusEffectsByFlag(EFFECTFLAG_DISPELABLE | EFFECTFLAG_ON_ZONE);
                        member->loc.zone->PushPacket(member, CHAR_INRANGE, new CCharSyncPacket(member));
		            }
	            }
            }
        }
        else
        {
            if (m_PSyncTarget != nullptr)
            {
                //disable level sync
                for (uint8 i = 0; i < members.size(); ++i)
	            {
		            if(members.at(i)->objtype != TYPE_PC) continue;

		            CCharEntity* member = (CCharEntity*)members.at(i);

                    if (member->status != STATUS_DISAPPEAR)
		            {
                        CStatusEffect* sync = member->StatusEffectContainer->GetStatusEffect(EFFECT_LEVEL_SYNC);
                        if (sync && sync->GetDuration() == 0)
                        {
			                member->pushPacket(new CMessageBasicPacket(member, member, 10, 30, message));
                            sync->SetStartTime(gettick());
                            sync->SetDuration(30000);
                        }
		            }
	            }
            }
        }
    }
}

/************************************************************************
*																		*
*  Усранавливаем владельца сокровищ										*
*																		*
************************************************************************/

void CParty::SetQuarterMaster(const char* MemberName)
{
    CBattleEntity* PEntity = MemberName ? GetMemberByName(MemberName) : nullptr;
	m_PQuaterMaster = PEntity;
	Sql_Query(SqlHandle, "UPDATE accounts_parties SET partyflag = partyflag & ~%d WHERE partyid = %u AND partyflag & %d", PARTY_QM, m_PartyID, PARTY_QM);
    if (MemberName != nullptr)
    {
        Sql_Query(SqlHandle, "UPDATE accounts_parties JOIN chars ON accounts_parties.charid = chars.charid \
                              SET partyflag = partyflag | %d WHERE partyid = %u AND charname = '%s';", PARTY_QM, m_PartyID, MemberName);
    }
}

/************************************************************************
*																		*
*  Отправляем пакет всем членам группы, если зона указана как 0 или		*
*  членам группы в указанной зоне.										*
*  Пакет для PPartyMember не отправляется в обоих случаях.				*
*																		*
************************************************************************/

void CParty::PushPacket(uint32 senderID, uint16 ZoneID, CBasicPacket* packet)
{
	for (uint32 i = 0; i < members.size(); ++i)
	{
		if (members.at(i) == nullptr || members.at(i)->objtype != TYPE_PC)
		{
			continue;
		}

		CCharEntity* member = (CCharEntity*)members.at(i);

        if (member->id != senderID &&
            member->status != STATUS_DISAPPEAR &&
             !jailutils::InPrison(member) )
		{
			if (ZoneID == 0 || member->getZone() == ZoneID)
			{
				member->pushPacket(new CBasicPacket(*packet));
			}
		}
	}
	delete packet;
}

void CParty::DisableSync()
{
    m_PSyncTarget = nullptr;
    ReloadParty();
}

void CParty::RefreshSync()
{
    CCharEntity* sync = (CCharEntity*)m_PSyncTarget;
    uint8 syncLevel = sync->jobs.job[sync->GetMJob()];
    if (syncLevel < 10)
    {
        SetSyncTarget(nullptr, 554);
    }
    for (uint8 i = 0; i < members.size(); ++i)
	{
		if(members.at(i)->objtype != TYPE_PC || members.at(i)->getZone() != sync->getZone()) continue;

		CCharEntity* member = (CCharEntity*)members.at(i);

        uint8 NewMLevel = 0;

		if (syncLevel < member->jobs.job[member->GetMJob()])
		{
			NewMLevel = syncLevel;
		}else{
			NewMLevel = member->jobs.job[member->GetMJob()];
		}

		if (member->GetMLevel() != NewMLevel)
		{
            charutils::RemoveAllEquipMods(member);
			member->SetMLevel(NewMLevel);
			member->SetSLevel(member->jobs.job[member->GetSJob()]);
            charutils::ApplyAllEquipMods(member);

            blueutils::ValidateBlueSpells(member);
			charutils::BuildingCharSkillsTable(member);
			charutils::CalculateStats(member);
			charutils::BuildingCharTraitsTable(member);
			charutils::BuildingCharAbilityTable(member);
			charutils::CheckValidEquipment(member);
            member->pushPacket(new CCharAbilitiesPacket(member));
		}
        member->pushPacket(new CMessageBasicPacket(member, member, 0, syncLevel, 540));
	}
    m_PSyncTarget = sync;
}

void CParty::SetPartyNumber(uint8 number)
{
    m_PartyNumber = number;
}