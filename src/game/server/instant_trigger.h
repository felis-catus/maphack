//========= Copyright Felis, Licensed under 0BSD. =============================//
//
// Purpose: Alternative to Source's triggers, mainly used by MapHack
//
//=============================================================================//

#ifndef INSTANT_TRIGGER_H
#define INSTANT_TRIGGER_H

//-----------------------------------------------------------------------------
class CInstantTrigger : public CPointEntity
{
public:
	DECLARE_CLASS( CInstantTrigger, CPointEntity );
	DECLARE_DATADESC();

	CInstantTrigger();

	void Spawn() OVERRIDE;
	void Activate() OVERRIDE;

	void TriggerThink();

	bool CanTrigger( CBaseEntity *pEnt ) const;

	void InputEnable( inputdata_t &inputdata );
	void InputDisable( inputdata_t &inputdata );
	void InputToggle( inputdata_t &inputdata );

	void SetActive( bool bActive );

protected:
	string_t m_iszFilterName;
	CHandle<CBaseFilter> m_hFilter;

	string_t m_iszMapHackEvent;
	float m_flRadius;

	COutputEvent m_OnTrigger;

	// Keyfield for "Start Disabled", old name and behavior is kept for backward compatibility
	bool m_bDisabled;

	bool m_bAllowPlayers;
	bool m_bAllowNPCs;
	bool m_bAllowPhysics;
	bool m_bAllowAll;

	bool m_bNoClear;

private:
	bool m_bActive;
};

#endif
