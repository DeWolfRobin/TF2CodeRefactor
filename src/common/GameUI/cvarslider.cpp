//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================

#include "cvarslider.h"
#include <stdio.h>
#include "tier1/KeyValues.h"
#include "tier1/convar.h"
#include <vgui/IVGui.h>
#include <vgui_controls/PropertyPage.h>

#define CVARSLIDER_SCALE_FACTOR 100.0f

// memdbgon must be the last include file in a .cpp file!!!
#include <tier0/memdbgon.h>

using namespace vgui;

DECLARE_BUILD_FACTORY(CCvarSlider);

//-----------------------------------------------------------------------------
// Constructor: Default slider with no preset cvar.
//-----------------------------------------------------------------------------
CCvarSlider::CCvarSlider(Panel* parent, const char* name)
    : Slider(parent, name),
    m_bAllowOutOfRange(false),
    m_bModifiedOnce(false),
    m_fStartValue(0.0f),
    m_iStartValue(0),
    m_iLastSliderValue(0),
    m_fCurrentValue(0.0f),
    m_bCreatedInCode(false),
    m_flMinValue(0.0f),
    m_flMaxValue(1.0f)
{
    SetupSlider(0, 1, "", false);
    AddActionSignalTarget(this);
}

//-----------------------------------------------------------------------------
// Constructor: Slider created with parameters.
//-----------------------------------------------------------------------------
CCvarSlider::CCvarSlider(Panel* parent, const char* panelName, const char* caption,
    float minValue, float maxValue, const char* cvarname, bool bAllowOutOfRange)
    : Slider(parent, panelName),
    m_bAllowOutOfRange(bAllowOutOfRange),
    m_bModifiedOnce(false),
    m_fStartValue(0.0f),
    m_iStartValue(0),
    m_iLastSliderValue(0),
    m_fCurrentValue(0.0f),
    m_bCreatedInCode(true),
    m_flMinValue(minValue),
    m_flMaxValue(maxValue)
{
    AddActionSignalTarget(this);
    SetupSlider(minValue, maxValue, cvarname, bAllowOutOfRange);
}

//-----------------------------------------------------------------------------
// Destructor.
//-----------------------------------------------------------------------------
CCvarSlider::~CCvarSlider()
{
}

//-----------------------------------------------------------------------------
// SetupSlider: Configures the slider range, tick captions, and initial value.
//-----------------------------------------------------------------------------
void CCvarSlider::SetupSlider(float minValue, float maxValue, const char* cvarname, bool bAllowOutOfRange)
{
    // Adjust min/max based on the associated cvar, if valid.
    UIConVarRef var(g_pVGui->GetVGUIEngine(), cvarname, true);
    if (var.IsValid())
    {
        float flCVarMin;
        if (var.GetMin(flCVarMin))
        {
            minValue = m_bUseConVarMinMax ? flCVarMin : MAX(minValue, flCVarMin);
        }
        float flCVarMax;
        if (var.GetMax(flCVarMax))
        {
            maxValue = m_bUseConVarMinMax ? flCVarMax : MIN(maxValue, flCVarMax);
        }
    }

    m_flMinValue = minValue;
    m_flMaxValue = maxValue;

    // Set the slider range using a scale factor.
    SetRange(static_cast<int>(CVARSLIDER_SCALE_FACTOR * minValue), static_cast<int>(CVARSLIDER_SCALE_FACTOR * maxValue));

    char szMin[32], szMax[32];
    Q_snprintf(szMin, sizeof(szMin), "%.2f", minValue);
    Q_snprintf(szMax, sizeof(szMax), "%.2f", maxValue);
    SetTickCaptions(szMin, szMax);

    Q_strncpy(m_szCvarName, cvarname, sizeof(m_szCvarName));

    m_bModifiedOnce = false;
    m_bAllowOutOfRange = bAllowOutOfRange;

    // Initialize slider value based on current cvar value.
    Reset();
}

//-----------------------------------------------------------------------------
// ApplySettings: Applies resource settings from the provided KeyValues.
//-----------------------------------------------------------------------------
void CCvarSlider::ApplySettings(KeyValues* inResourceData)
{
    BaseClass::ApplySettings(inResourceData);

    if (!m_bCreatedInCode)
    {
        float minValue = inResourceData->GetFloat("minvalue", 0);
        float maxValue = inResourceData->GetFloat("maxvalue", 1);
        const char* cvarname = inResourceData->GetString("cvar_name", "");
        bool bAllowOutOfRange = (inResourceData->GetInt("allowoutofrange", 0) != 0);
        SetupSlider(minValue, maxValue, cvarname, bAllowOutOfRange);

        if (GetParent())
        {
            // If our parent is a property page, add the dialog as the signal target.
            if (dynamic_cast<vgui::PropertyPage*>(GetParent()) && GetParent()->GetParent())
            {
                GetParent()->GetParent()->AddActionSignalTarget(this);
            }
            else
            {
                GetParent()->AddActionSignalTarget(this);
            }
        }
    }
}

//-----------------------------------------------------------------------------
// GetSettings: Saves the current control settings into KeyValues.
//-----------------------------------------------------------------------------
void CCvarSlider::GetSettings(KeyValues* outResourceData)
{
    BaseClass::GetSettings(outResourceData);

    if (!m_bCreatedInCode)
    {
        outResourceData->SetFloat("minvalue", m_flMinValue);
        outResourceData->SetFloat("maxvalue", m_flMaxValue);
        outResourceData->SetString("cvar_name", m_szCvarName);
        outResourceData->SetInt("allowoutofrange", m_bAllowOutOfRange);
    }
}

//-----------------------------------------------------------------------------
// SetCVarName: Sets the cvar name and resets the slider to match its current value.
//-----------------------------------------------------------------------------
void CCvarSlider::SetCVarName(const char* cvarname)
{
    Q_strncpy(m_szCvarName, cvarname, sizeof(m_szCvarName));
    m_bModifiedOnce = false;
    Reset();
}

//-----------------------------------------------------------------------------
// SetMinMaxValues: Updates the slider range and tick captions (if requested), then resets.
//-----------------------------------------------------------------------------
void CCvarSlider::SetMinMaxValues(float minValue, float maxValue, bool bSetTickDisplay)
{
    SetRange(static_cast<int>(CVARSLIDER_SCALE_FACTOR * minValue), static_cast<int>(CVARSLIDER_SCALE_FACTOR * maxValue));

    if (bSetTickDisplay)
    {
        char szMin[32], szMax[32];
        Q_snprintf(szMin, sizeof(szMin), "%.2f", minValue);
        Q_snprintf(szMax, sizeof(szMax), "%.2f", maxValue);
        SetTickCaptions(szMin, szMax);
    }
    Reset();
}

//-----------------------------------------------------------------------------
// SetTickColor: Sets the color for slider tick marks.
//-----------------------------------------------------------------------------
void CCvarSlider::SetTickColor(Color color)
{
    m_TickColor = color;
}

//-----------------------------------------------------------------------------
// Paint: Updates the slider value if the external cvar has changed, then paints.
//-----------------------------------------------------------------------------
void CCvarSlider::Paint()
{
    UIConVarRef var(g_pVGui->GetVGUIEngine(), m_szCvarName, true);
    if (!var.IsValid())
        return;

    float curvalue = var.GetFloat();

    // Update slider if the cvar value has changed.
    if (curvalue != m_fStartValue)
    {
        int val = static_cast<int>(CVARSLIDER_SCALE_FACTOR * curvalue);
        m_fStartValue = curvalue;
        m_fCurrentValue = curvalue;
        SetValue(val);
        m_iStartValue = GetValue();
    }
    BaseClass::Paint();
}

//-----------------------------------------------------------------------------
// ApplyChanges: Applies slider modifications to the associated cvar.
//-----------------------------------------------------------------------------
void CCvarSlider::ApplyChanges()
{
    if (m_bModifiedOnce)
    {
        m_iStartValue = GetValue();
        m_fStartValue = m_bAllowOutOfRange ? m_fCurrentValue : static_cast<float>(m_iStartValue) / CVARSLIDER_SCALE_FACTOR;
        UIConVarRef var(g_pVGui->GetVGUIEngine(), m_szCvarName, true);
        if (!var.IsValid())
            return;
        var.SetValue(m_fStartValue);
    }
}

//-----------------------------------------------------------------------------
// GetSliderValue: Returns the slider’s current value as a float.
//-----------------------------------------------------------------------------
float CCvarSlider::GetSliderValue()
{
    return m_bAllowOutOfRange ? m_fCurrentValue : static_cast<float>(GetValue()) / CVARSLIDER_SCALE_FACTOR;
}

//-----------------------------------------------------------------------------
// SetSliderValue: Sets the slider’s value (and marks it modified if changed).
//-----------------------------------------------------------------------------
void CCvarSlider::SetSliderValue(float fValue)
{
    int nVal = static_cast<int>(CVARSLIDER_SCALE_FACTOR * fValue);
    SetValue(nVal, false);
    m_iLastSliderValue = GetValue();
    if (m_fCurrentValue != fValue)
    {
        m_fCurrentValue = fValue;
        m_bModifiedOnce = true;
    }
}

//-----------------------------------------------------------------------------
// Reset: Resets the slider to the current value of its associated cvar.
//-----------------------------------------------------------------------------
void CCvarSlider::Reset()
{
    UIConVarRef var(g_pVGui->GetVGUIEngine(), m_szCvarName, true);
    if (!var.IsValid())
    {
        m_fCurrentValue = m_fStartValue = 0.0f;
        SetValue(0, false);
        m_iStartValue = GetValue();
        m_iLastSliderValue = m_iStartValue;
        return;
    }
    m_fStartValue = var.GetFloat();
    m_fCurrentValue = m_fStartValue;
    int value = static_cast<int>(CVARSLIDER_SCALE_FACTOR * m_fStartValue);
    SetValue(value, false);
    m_iStartValue = GetValue();
    m_iLastSliderValue = m_iStartValue;
}

//-----------------------------------------------------------------------------
// HasBeenModified: Returns true if the slider value has been changed.
//-----------------------------------------------------------------------------
bool CCvarSlider::HasBeenModified()
{
    int currentValue = GetValue();
    if (currentValue != m_iStartValue)
    {
        m_bModifiedOnce = true;
    }
    return m_bModifiedOnce;
}

//-----------------------------------------------------------------------------
// OnSliderMoved: Handles slider movement events and signals if modified.
//-----------------------------------------------------------------------------
void CCvarSlider::OnSliderMoved()
{
    if (HasBeenModified())
    {
        int currentValue = GetValue();
        if (m_iLastSliderValue != currentValue)
        {
            m_iLastSliderValue = currentValue;
            m_fCurrentValue = static_cast<float>(m_iLastSliderValue) / CVARSLIDER_SCALE_FACTOR;
        }
        PostActionSignal(new KeyValues("ControlModified"));
    }
}

//-----------------------------------------------------------------------------
// OnSliderDragEnd: Applies changes when slider dragging ends (if not created in code).
//-----------------------------------------------------------------------------
void CCvarSlider::OnSliderDragEnd()
{
    if (!m_bCreatedInCode)
    {
        ApplyChanges();
    }
}
