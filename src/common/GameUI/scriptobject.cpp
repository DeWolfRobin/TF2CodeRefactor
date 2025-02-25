//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//
// CScriptObject and CDescription class definitions
// 
#include "scriptobject.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <vgui_controls/Label.h>
#include "filesystem.h"
#include "tier1/convar.h"
#include "cdll_int.h"
#include "vgui/IVGui.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

using namespace vgui;
static char token[1024];

extern IVEngineClient* engine;

//
// Helper: StripFloatTrailingZeros
// Scans for a '.' and removes trailing '0's, also removing the dot if necessary.
//
void StripFloatTrailingZeros(char* str)
{
    char* period = strchr(str, '.');
    if (!period)
        return;

    char* end = str + strlen(str) - 1;
    while (end > period && *end == '0')
    {
        *end = '\0';
        --end;
    }
    if (*end == '.')
    {
        *end = '\0';
    }
}

/////////////////////
// Constant object type descriptions
objtypedesc_t objtypes[] =
{
    { O_BOOL,     "BOOL" },
    { O_NUMBER,   "NUMBER" },
    { O_LIST,     "LIST" },
    { O_STRING,   "STRING" },
    { O_OBSOLETE, "OBSOLETE" },
    { O_SLIDER,   "SLIDER" },
    { O_CATEGORY, "CATEGORY" },
    { O_BUTTON,   "BUTTON" },
};

//
// mpcontrol_t methods
//
mpcontrol_t::mpcontrol_t(Panel* parent, char const* panelName)
    : Panel(parent, panelName), type(O_BADTYPE), pControl(NULL), pPrompt(NULL),
    pScrObj(NULL), next(NULL)
{
    SetPaintBackgroundEnabled(false);
}

void mpcontrol_t::OnSizeChanged(int wide, int tall)
{
    int inset = 4;
    if (pPrompt)
    {
        int w = wide / 2;
        if (pControl)
        {
            pControl->SetBounds(w + 20, inset, w - 20, tall - 2 * inset);
        }
        pPrompt->SetBounds(0, inset, w + 20, tall - 2 * inset);
    }
    else
    {
        if (pControl)
        {
            pControl->SetBounds(0, inset, wide, tall - 2 * inset);
        }
    }
}

//
// CScriptListItem methods
//
CScriptListItem::CScriptListItem()
{
    pNext = NULL;
    memset(szItemText, 0, sizeof(szItemText));
    memset(szValue, 0, sizeof(szValue));
}

CScriptListItem::CScriptListItem(char const* strItem, char const* strValue)
{
    pNext = NULL;
    Q_strncpy(szItemText, strItem, sizeof(szItemText));
    Q_strncpy(szValue, strValue, sizeof(szValue));
}

//
// CScriptObject methods
//
CScriptObject::CScriptObject(void)
    : vgui::Panel(NULL, ""), m_pLastItem(NULL)
{
    type = O_BOOL;
    bSetInfo = false;  // Prepend "Setinfo" to keyvalue pair in config?
    pNext = NULL;
    pListItems = NULL;
    tooltip[0] = '\0';
}

CScriptObject::~CScriptObject()
{
    RemoveAndDeleteAllItems();
}

void CScriptObject::RemoveAndDeleteAllItems(void)
{
    CScriptListItem* p = pListItems, * n;
    while (p)
    {
        n = p->pNext;
        delete p;
        p = n;
    }
    pListItems = NULL;
    m_pLastItem = NULL;
}

void CScriptObject::SetCurValue(char const* strValue)
{
    Q_strncpy(curValue, strValue, sizeof(curValue));
    fcurValue = (float)atof(curValue);

    if (type == O_NUMBER || type == O_BOOL || type == O_SLIDER)
    {
        StripFloatTrailingZeros(curValue);
    }
}

//
// Optimized AddItem using a tail pointer for O(1) insertion.
//
void CScriptObject::AddItem(CScriptListItem* pItem)
{
    if (!pListItems)
    {
        pListItems = pItem;
        m_pLastItem = pItem;
        pItem->pNext = NULL;
    }
    else
    {
        m_pLastItem->pNext = pItem;
        m_pLastItem = pItem;
        pItem->pNext = NULL;
    }
}

/*
===================
UTIL_StripInvalidCharacters

Removes any formatting codes and double quote characters from the input string.
===================
*/
void UTIL_StripInvalidCharacters(char* pszInput, int maxlen)
{
    char szOutput[4096];
    char* pIn = pszInput, * pOut = szOutput;

    while (*pIn)
    {
        if (*pIn != '"' && *pIn != '%')
        {
            *pOut++ = *pIn;
        }
        pIn++;
    }
    *pOut = '\0';
    Q_strncpy(pszInput, szOutput, maxlen);
}

void FixupString(char* inString, int maxlen)
{
    char szBuffer[4096];
    Q_strncpy(szBuffer, inString, sizeof(szBuffer));
    UTIL_StripInvalidCharacters(szBuffer, sizeof(szBuffer));
    Q_strncpy(inString, szBuffer, maxlen);
}

/*
===================
CleanFloat

Removes any trailing ".000" from the float string.
===================
*/
char* CleanFloat(float val)
{
    static int curstring = 0;
    static char string[2][32];
    curstring = (curstring + 1) % 2;
    Q_snprintf(string[curstring], sizeof(string[curstring]), "%f", val);
    char* str = string[curstring];

    if (!str || !*str || !strchr(str, '.'))
        return str;

    char* tmp = str;
    while (*tmp) ++tmp;
    --tmp;
    while (tmp > str && *tmp == '0')
    {
        *tmp = '\0';
        --tmp;
    }
    if (*tmp == '.')
        *tmp = '\0';
    return str;
}

//
// CScriptObject::WriteToScriptFile: Writes the object out as a script.
//
void CScriptObject::WriteToScriptFile(FileHandle_t fp)
{
    if (type == O_OBSOLETE)
        return;

    FixupString(cvarname, sizeof(cvarname));
    g_pFullFileSystem->FPrintf(fp, "\t\"%s\"\r\n", cvarname);
    g_pFullFileSystem->FPrintf(fp, "\t{\r\n");

    FixupString(prompt, sizeof(prompt));
    FixupString(tooltip, sizeof(tooltip));

    CScriptListItem* pItem = NULL;

    switch (type)
    {
    case O_BOOL:
        g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", prompt);
        if (tooltip[0])
            g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", tooltip);
        g_pFullFileSystem->FPrintf(fp, "\t\t{ BOOL }\r\n");
        g_pFullFileSystem->FPrintf(fp, "\t\t{ \"%i\" }\r\n", (int)fcurValue ? 1 : 0);
        break;
    case O_NUMBER:
        g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", prompt);
        if (tooltip[0])
            g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", tooltip);
        g_pFullFileSystem->FPrintf(fp, "\t\t{ NUMBER %s %s }\r\n", CleanFloat(fMin), CleanFloat(fMax));
        g_pFullFileSystem->FPrintf(fp, "\t\t{ \"%s\" }\r\n", CleanFloat(fcurValue));
        break;
    case O_STRING:
        g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", prompt);
        if (tooltip[0])
            g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", tooltip);
        g_pFullFileSystem->FPrintf(fp, "\t\t{ STRING }\r\n");
        FixupString(curValue, sizeof(curValue));
        g_pFullFileSystem->FPrintf(fp, "\t\t{ \"%s\" }\r\n", curValue);
        break;
    case O_LIST:
        g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", prompt);
        if (tooltip[0])
            g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", tooltip);
        g_pFullFileSystem->FPrintf(fp, "\t\t{\r\n\t\t\tLIST\r\n");
        pItem = pListItems;
        while (pItem)
        {
            UTIL_StripInvalidCharacters(pItem->szItemText, sizeof(pItem->szItemText));
            UTIL_StripInvalidCharacters(pItem->szValue, sizeof(pItem->szValue));
            g_pFullFileSystem->FPrintf(fp, "\t\t\t\"%s\" \"%s\"\r\n", pItem->szItemText, pItem->szValue);
            pItem = pItem->pNext;
        }
        g_pFullFileSystem->FPrintf(fp, "\t\t}\r\n");
        g_pFullFileSystem->FPrintf(fp, "\t\t{ \"%s\" }\r\n", CleanFloat(fcurValue));
        break;
    case O_SLIDER:
        g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", prompt);
        if (tooltip[0])
            g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", tooltip);
        g_pFullFileSystem->FPrintf(fp, "\t\t{ SLIDER %s %s }\r\n", CleanFloat(fMin), CleanFloat(fMax));
        g_pFullFileSystem->FPrintf(fp, "\t\t{ \"%s\" }\r\n", CleanFloat(fcurValue));
        break;
    case O_CATEGORY:
        g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", prompt);
        if (tooltip[0])
            g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", tooltip);
        g_pFullFileSystem->FPrintf(fp, "\t\t{ CATEGORY }\r\n");
        break;
    case O_BUTTON:
        g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", prompt);
        if (tooltip[0])
            g_pFullFileSystem->FPrintf(fp, "\t\t\"%s\"\r\n", tooltip);
        g_pFullFileSystem->FPrintf(fp, "\t\t{ BUTTON }\r\n");
        FixupString(curValue, sizeof(curValue));
        g_pFullFileSystem->FPrintf(fp, "\t\t{ \"%s\" }\r\n", curValue);
        break;
    }

    if (bSetInfo)
        g_pFullFileSystem->FPrintf(fp, "\t\tSetInfo\r\n");

    g_pFullFileSystem->FPrintf(fp, "\t}\r\n\r\n");
}

void CScriptObject::WriteToFile(FileHandle_t fp)
{
    if (type == O_OBSOLETE || type == O_CATEGORY || type == O_BUTTON)
        return;

    FixupString(cvarname, sizeof(cvarname));
    g_pFullFileSystem->FPrintf(fp, "\"%s\"\t\t", cvarname);

    CScriptListItem* pItem;
    float fVal;

    switch (type)
    {
    case O_BOOL:
        g_pFullFileSystem->FPrintf(fp, "\"%s\"\r\n", fcurValue != 0.0f ? "1" : "0");
        break;
    case O_NUMBER:
    case O_SLIDER:
        fVal = fcurValue;
        if (fMin != -1.0f)
            fVal = max(fVal, fMin);
        if (fMax != -1.0f)
            fVal = min(fVal, fMax);
        g_pFullFileSystem->FPrintf(fp, "\"%f\"\r\n", fVal);
        break;
    case O_STRING:
        FixupString(curValue, sizeof(curValue));
        g_pFullFileSystem->FPrintf(fp, "\"%s\"\r\n", curValue);
        break;
    case O_LIST:
        pItem = pListItems;
        while (pItem)
        {
            if (!Q_stricmp(pItem->szValue, curValue))
                break;
            pItem = pItem->pNext;
        }
        if (pItem)
        {
            UTIL_StripInvalidCharacters(pItem->szValue, sizeof(pItem->szValue));
            g_pFullFileSystem->FPrintf(fp, "\"%s\"\r\n", pItem->szValue);
        }
        else  // Couldn't find index
        {
            g_pFullFileSystem->FPrintf(fp, "\"0\"\r\n");
        }
        break;
    }
}

void CScriptObject::WriteToConfig(void)
{
    if (type == O_OBSOLETE || type == O_CATEGORY || type == O_BUTTON)
        return;

    char* pszKey = cvarname;
    char szValue[2048];

    CScriptListItem* pItem;
    float fVal;

    switch (type)
    {
    case O_BOOL:
        Q_snprintf(szValue, sizeof(szValue), "%s", fcurValue != 0.0f ? "1" : "0");
        break;
    case O_NUMBER:
    case O_SLIDER:
        fVal = fcurValue;
        if (fMin != -1.0f)
            fVal = max(fVal, fMin);
        if (fMax != -1.0f)
            fVal = min(fVal, fMax);
        Q_snprintf(szValue, sizeof(szValue), "%f", fVal);
        break;
    case O_STRING:
        Q_snprintf(szValue, sizeof(szValue), "\"%s\"", curValue);
        UTIL_StripInvalidCharacters(szValue, sizeof(szValue));
        break;
    case O_LIST:
        pItem = pListItems;
        while (pItem)
        {
            if (!Q_stricmp(pItem->szValue, curValue))
                break;
            pItem = pItem->pNext;
        }
        if (pItem)
        {
            Q_snprintf(szValue, sizeof(szValue), "%s", pItem->szValue);
            UTIL_StripInvalidCharacters(szValue, sizeof(szValue));
        }
        else  // Couldn't find index
        {
            Q_strncpy(szValue, "0", sizeof(szValue));
        }
        break;
    }

    char command[256];
    if (bSetInfo)
    {
        Q_snprintf(command, sizeof(command), "setinfo %s \"%s\"\n", pszKey, szValue);
    }
    else
    {
        Q_snprintf(command, sizeof(command), "%s \"%s\"\n", pszKey, szValue);
    }
    engine->ClientCmd_Unrestricted(command);
}

objtype_t CScriptObject::GetType(char* pszType)
{
    int nTypes = sizeof(objtypes) / sizeof(objtypedesc_t);
    for (int i = 0; i < nTypes; i++)
    {
        if (!stricmp(objtypes[i].szDescription, pszType))
            return objtypes[i].type;
    }
    return O_BADTYPE;
}

bool CScriptObject::ReadFromBuffer(const char** pBuffer, bool isNewObject)
{
    // Get the first token.
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;

    if (isNewObject)
    {
        Q_strncpy(cvarname, token, sizeof(cvarname));
    }

    // Parse the {
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    if (strcmp(token, "{"))
    {
        Msg("Expecting '{', got '%s'", token);
        return false;
    }

    // Parse the Prompt
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    if (isNewObject)
    {
        Q_strncpy(prompt, token, sizeof(prompt));
    }

    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;

    // If it's not a {, consider it the optional tooltip
    if (strcmp(token, "{"))
    {
        Q_strncpy(tooltip, token, sizeof(tooltip));
        // Parse the next {
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
    }

    if (strcmp(token, "{"))
    {
        Msg("Expecting '{', got '%s'", token);
        return false;
    }

    // Now parse the type:
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    objtype_t newType = GetType(token);
    if (isNewObject)
    {
        type = newType;
    }
    if (newType == O_BADTYPE)
    {
        Msg("Type '%s' unknown", token);
        return false;
    }

    // If it's a category, we're done.
    if (newType == O_CATEGORY)
    {
        // Parse the }
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
        if (strcmp(token, "}"))
        {
            Msg("Expecting '{', got '%s'", token);
            return false;
        }
        // Parse the final }
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
        if (strcmp(token, "}"))
        {
            Msg("Expecting '{', got '%s'", token);
            return false;
        }
        return true;
    }

    switch (newType)
    {
    case O_OBSOLETE:
    case O_BOOL:
    case O_STRING:
    case O_BUTTON:
        // Parse the next {
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
        if (strcmp(token, "}"))
        {
            Msg("Expecting '{', got '%s'", token);
            return false;
        }
        break;
    case O_NUMBER:
    case O_SLIDER:
        // Parse the Min
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
        if (isNewObject)
        {
            fMin = (float)atof(token);
        }
        // Parse the Max
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
        if (isNewObject)
        {
            fMax = (float)atof(token);
        }
        // Parse the next {
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
        if (strcmp(token, "{"))
        {
            Msg("Expecting '{', got '%s'", token);
            return false;
        }
        break;
    case O_LIST:
        // Parse items until we get the }
        while (1)
        {
            *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
            if (!token[0])
                return false;
            if (!strcmp(token, "}"))
                break;
            char strItem[128];
            char strValue[128];
            Q_strncpy(strItem, token, sizeof(strItem));
            *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
            if (!token[0])
                return false;
            Q_strncpy(strValue, token, sizeof(strValue));
            if (isNewObject)
            {
                CScriptListItem* pItem = new CScriptListItem(strItem, strValue);
                AddItem(pItem);
            }
        }
        break;
    }

    // Now read in the default value
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    if (strcmp(token, "{"))
    {
        Msg("Expecting '{', got '%s'", token);
        return false;
    }
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    Q_strncpy(defValue, token, sizeof(defValue));
    fdefValue = (float)atof(token);
    if (type == O_NUMBER || type == O_SLIDER)
    {
        StripFloatTrailingZeros(defValue);
    }
    SetCurValue(defValue);
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    if (strcmp(token, "}"))
    {
        Msg("Expecting '{', got '%s'", token);
        return false;
    }
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    if (!stricmp(token, "SetInfo"))
    {
        bSetInfo = true;
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
    }
    if (strcmp(token, "}"))
    {
        Msg("Expecting '{', got '%s'", token);
        return false;
    }
    return true;
}

/////////////////////////
// CDescription methods
/////////////////////////
CDescription::CDescription(void)
    : pObjList(NULL), m_pLastObj(NULL), m_pszHintText(NULL), m_pszDescriptionType(NULL)
{
}

CDescription::~CDescription()
{
    CScriptObject* p = pObjList, * n;
    while (p)
    {
        n = p->pNext;
        p->pNext = NULL;
        p->MarkForDeletion();
        p = n;
    }
    pObjList = NULL;
    m_pLastObj = NULL;

    if (m_pszHintText)
        free(m_pszHintText);
    if (m_pszDescriptionType)
        free(m_pszDescriptionType);
}

CScriptObject* CDescription::FindObject(const char* pszObjectName)
{
    if (!pszObjectName)
        return NULL;
    CScriptObject* p = pObjList;
    while (p)
    {
        if (!stricmp(pszObjectName, p->cvarname))
            return p;
        p = p->pNext;
    }
    return NULL;
}

//
// Optimized AddObject using a tail pointer for O(1) insertion.
//
void CDescription::AddObject(CScriptObject* pObj)
{
    if (!pObjList)
    {
        pObjList = pObj;
        m_pLastObj = pObj;
        pObj->pNext = NULL;
    }
    else
    {
        m_pLastObj->pNext = pObj;
        m_pLastObj = pObj;
        pObj->pNext = NULL;
    }
}

bool CDescription::ReadFromBuffer(const char** pBuffer, bool bAllowNewObject)
{
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;

    if (stricmp(token, "VERSION"))
    {
        Msg("Expecting 'VERSION', got '%s'", token);
        return false;
    }
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
    {
        Msg("Expecting version #");
        return false;
    }
    float fVer = (float)atof(token);
    if (fVer != SCRIPT_VERSION)
    {
        Msg("Version mismatch, expecting %f, got %f", SCRIPT_VERSION, fVer);
        return false;
    }
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    if (stricmp(token, "DESCRIPTION"))
    {
        Msg("Expecting 'DESCRIPTION', got '%s'", token);
        return false;
    }
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
    {
        Msg("Expecting '%s'", m_pszDescriptionType);
        return false;
    }
    if (stricmp(token, m_pszDescriptionType))
    {
        Msg("Expecting %s, got %s", m_pszDescriptionType, token);
        return false;
    }
    *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
    if (!token[0])
        return false;
    if (strcmp(token, "{"))
    {
        Msg("Expecting '{', got '%s'", token);
        return false;
    }
    const char* pStart;
    CScriptObject* pObj;
    while (1)
    {
        pStart = *pBuffer;
        *pBuffer = engine->ParseFile(*pBuffer, token, sizeof(token));
        if (!token[0])
            return false;
        if (!stricmp(token, "}"))
            break;
        *pBuffer = pStart;
        bool mustAdd = bAllowNewObject;
        pObj = FindObject(token);
        if (pObj)
        {
            pObj->ReadFromBuffer(&pStart, false);
            mustAdd = false;
        }
        else
        {
            pObj = new CScriptObject();
            if (!pObj)
            {
                Msg("Couldn't create script object");
                return false;
            }
            if (!pObj->ReadFromBuffer(&pStart, true))
            {
                delete pObj;
                return false;
            }
            if (!mustAdd)
            {
                delete pObj;
            }
        }
        *pBuffer = pStart;
        if (mustAdd)
        {
            AddObject(pObj);
        }
    }
    return true;
}

bool CDescription::InitFromFile(const char* pszFileName, bool bAllowNewObject)
{
    FileHandle_t file = g_pFullFileSystem->Open(pszFileName, "rb");
    if (!file)
        return false;
    int len = g_pFullFileSystem->Size(file);
    byte* buffer = new unsigned char[len];
    Assert(buffer);
    g_pFullFileSystem->Read(buffer, len, file);
    g_pFullFileSystem->Close(file);
    const char* pBuffer = (const char*)buffer;
    ReadFromBuffer(&pBuffer, bAllowNewObject);
    delete[] buffer;
    return true;
}

void CDescription::WriteToFile(FileHandle_t fp)
{
    CScriptObject* pObj = pObjList;
    WriteFileHeader(fp);
    while (pObj)
    {
        pObj->WriteToFile(fp);
        pObj = pObj->pNext;
    }
}

void CDescription::WriteToConfig(void)
{
    CScriptObject* pObj = pObjList;
    while (pObj)
    {
        pObj->WriteToConfig();
        pObj = pObj->pNext;
    }
}

void CDescription::WriteToScriptFile(FileHandle_t fp)
{
    CScriptObject* pObj = pObjList;
    WriteScriptHeader(fp);
    while (pObj)
    {
        pObj->WriteToScriptFile(fp);
        pObj = pObj->pNext;
    }
    g_pFullFileSystem->FPrintf(fp, "}\r\n");
}

void CDescription::TransferCurrentValues(const char* pszConfigFile)
{
    char szValue[1024];
    CScriptObject* pObj = pObjList;
    while (pObj)
    {
        UIConVarRef var(g_pVGui->GetVGUIEngine(), pObj->cvarname, true);
        if (!var.IsValid())
        {
            if (pObj->type != O_CATEGORY && pObj->type != O_BUTTON)
            {
                DevMsg("Could not find '%s'\n", pObj->cvarname);
            }
            pObj = pObj->pNext;
            continue;
        }
        const char* value = var.GetString();
        if (value && value[0])
        {
            Q_strncpy(szValue, value, sizeof(szValue));
            Q_strncpy(pObj->curValue, szValue, sizeof(pObj->curValue));
            pObj->fcurValue = (float)atof(szValue);
            Q_strncpy(pObj->defValue, szValue, sizeof(pObj->defValue));
            pObj->fdefValue = (float)atof(szValue);
        }
        pObj = pObj->pNext;
    }
}

void CDescription::setDescription(const char* pszDesc)
{
    m_pszDescriptionType = strdup(pszDesc);
}

void CDescription::setHint(const char* pszHint)
{
    m_pszHintText = strdup(pszHint);
}

//
// CInfoDescription
//
CInfoDescription::CInfoDescription(void)
    : CDescription()
{
    setHint("// NOTE:  THIS FILE IS AUTOMATICALLY REGENERATED, \r\n"
        //DO NOT EDIT THIS HEADER, YOUR COMMENTS WILL BE LOST IF YOU DO\r\n
        "// User options script\r\n\r\n"
        // Format:\r\n
        "//  Version [float]\r\n"
        //  Options description followed by \r\n
        "//  Options defaults\r\n\r\n"
        // Option description syntax:\r\n\r\n
        "//  \"cvar\" { \"Prompt\" { type [ type info ] } { default } }\r\n\r\n"
        //  type = \r\n
        "//   BOOL   (a yes/no toggle)\r\n"
        "//   STRING\r\n"
        "//   NUMBER\r\n"
        "//   LIST\r\n\r\n"
        // type info:\r\n
        "// BOOL                 no type info\r\n"
        "// NUMBER       min max range, use -1 -1 for no limits\r\n"
        "// STRING       no type info\r\n"
        "// LIST         \"\" delimited list of options value pairs\r\n\r\n\r\n");
    setDescription("INFO_OPTIONS");
}

void CInfoDescription::WriteScriptHeader(FileHandle_t fp)
{
    char am_pm[] = "AM";
    tm newtime;
    VCRHook_LocalTime(&newtime);
    g_pFullFileSystem->FPrintf(fp, (char*)getHint());
    g_pFullFileSystem->FPrintf(fp, "// Half-Life User Info Configuration Layout Script (stores last settings chosen, too)\r\n");
    g_pFullFileSystem->FPrintf(fp, "// File generated:  %.19s %s\r\n", asctime(&newtime), am_pm);
    g_pFullFileSystem->FPrintf(fp, "//\r\n//\r\n// Cvar\t-\tSetting\r\n\r\n");
    g_pFullFileSystem->FPrintf(fp, "VERSION %.1f\r\n\r\n", SCRIPT_VERSION);
    g_pFullFileSystem->FPrintf(fp, "DESCRIPTION INFO_OPTIONS\r\n{\r\n");
}

void CInfoDescription::WriteFileHeader(FileHandle_t fp)
{
    char am_pm[] = "AM";
    tm newtime;
    VCRHook_LocalTime(&newtime);
    g_pFullFileSystem->FPrintf(fp, "// Half-Life User Info Configuration Settings\r\n");
    g_pFullFileSystem->FPrintf(fp, "// DO NOT EDIT, GENERATED BY HALF-LIFE\r\n");
    g_pFullFileSystem->FPrintf(fp, "// File generated:  %.19s %s\r\n", asctime(&newtime), am_pm);
    g_pFullFileSystem->FPrintf(fp, "//\r\n//\r\n// Cvar\t-\tSetting\r\n\r\n");
}
