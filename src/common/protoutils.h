//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================

#ifndef VALVE_PROTO_UTILS_H
#define VALVE_PROTO_UTILS_H
#ifdef _WIN32
#pragma once
#endif

#include <initializer_list>

namespace google { namespace protobuf { class Message; class Descriptor; }; };

namespace ValveProtoUtils {
    // Asserts that the message descriptor contains exactly the specified field numbers.
    bool MessageHasExactFields(const google::protobuf::Descriptor& desc, std::initializer_list<int> fields);
    bool MessageHasExactFields(const google::protobuf::Message& msg, std::initializer_list<int> fields);
};

#endif // VALVE_PROTO_UTILS_H
