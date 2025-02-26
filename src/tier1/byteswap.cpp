//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Low level byte swapping routines.
// 
// $NoKeywords: $
//=============================================================================

#include "byteswap.h"

//-----------------------------------------------------------------------------
// Copy a single field from the input buffer to the output buffer, swapping the bytes if necessary
//-----------------------------------------------------------------------------
void CByteswap::SwapFieldToTargetEndian(void* pOutputBuffer, void* pData, typedescription_t* pField)
{
	switch (pField->fieldType)
	{
	case FIELD_CHARACTER:
		SwapBufferToTargetEndian<char>(static_cast<char*>(pOutputBuffer), static_cast<char*>(pData), pField->fieldSize);
		break;

	case FIELD_BOOLEAN:
		SwapBufferToTargetEndian<bool>(static_cast<bool*>(pOutputBuffer), static_cast<bool*>(pData), pField->fieldSize);
		break;

	case FIELD_SHORT:
		SwapBufferToTargetEndian<short>(static_cast<short*>(pOutputBuffer), static_cast<short*>(pData), pField->fieldSize);
		break;

	case FIELD_FLOAT:
		// Swap floats by treating them as uints
		SwapBufferToTargetEndian<uint>(static_cast<uint*>(pOutputBuffer), static_cast<uint*>(pData), pField->fieldSize);
		break;

	case FIELD_INTEGER:
		SwapBufferToTargetEndian<int>(static_cast<int*>(pOutputBuffer), static_cast<int*>(pData), pField->fieldSize);
		break;

	case FIELD_VECTOR:
		SwapBufferToTargetEndian<uint>(static_cast<uint*>(pOutputBuffer), static_cast<uint*>(pData), pField->fieldSize * 3);
		break;

	case FIELD_VECTOR2D:
		SwapBufferToTargetEndian<uint>(static_cast<uint*>(pOutputBuffer), static_cast<uint*>(pData), pField->fieldSize * 2);
		break;

	case FIELD_QUATERNION:
		SwapBufferToTargetEndian<uint>(static_cast<uint*>(pOutputBuffer), static_cast<uint*>(pData), pField->fieldSize * 4);
		break;

	case FIELD_EMBEDDED:
	{
		// Use local pointers so we don't modify the caller's pointer.
		typedescription_t* pEmbed = pField->td->dataDesc;
		BYTE* pOut = static_cast<BYTE*>(pOutputBuffer);
		BYTE* pIn = static_cast<BYTE*>(pData);
		for (int i = 0; i < pField->fieldSize; ++i)
		{
			SwapFieldsToTargetEndian(pOut + pEmbed->fieldOffset[TD_OFFSET_NORMAL],
				pIn + pEmbed->fieldOffset[TD_OFFSET_NORMAL],
				pField->td);
			pOut += pField->fieldSizeInBytes;
			pIn += pField->fieldSizeInBytes;
		}
	}
	break;

	default:
		assert(0);
	}
}

//-----------------------------------------------------------------------------
// Write a block of fields. Works a bit like the saverestore code.
//-----------------------------------------------------------------------------
void CByteswap::SwapFieldsToTargetEndian(void* pOutputBuffer, void* pBaseData, datamap_t* pDataMap)
{
	// Deal with base class first.
	if (pDataMap->baseMap)
	{
		SwapFieldsToTargetEndian(pOutputBuffer, pBaseData, pDataMap->baseMap);
	}

	typedescription_t* pFields = pDataMap->dataDesc;
	int fieldCount = pDataMap->dataNumFields;
	for (int i = 0; i < fieldCount; ++i)
	{
		typedescription_t* pField = &pFields[i];
		SwapFieldToTargetEndian(static_cast<BYTE*>(pOutputBuffer) + pField->fieldOffset[TD_OFFSET_NORMAL],
			static_cast<BYTE*>(pBaseData) + pField->fieldOffset[TD_OFFSET_NORMAL],
			pField);
	}
}
