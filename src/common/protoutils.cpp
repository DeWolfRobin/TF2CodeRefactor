//========= Copyright Valve Corporation, All rights reserved. ============//

#include "cbase.h"
#include "protoutils.h"

#include "google/protobuf/message.h"

//-----------------------------------------------------------------------------
// Checks if the given protobuf Message has exactly the specified field numbers.
bool ValveProtoUtils::MessageHasExactFields(const google::protobuf::Message& msg,
    std::initializer_list<int> fields)
{
    // Simply forward to the descriptor overload.
    return ValveProtoUtils::MessageHasExactFields(*msg.GetDescriptor(), fields);
}

//-----------------------------------------------------------------------------
// Checks if the given protobuf Descriptor has exactly the specified field numbers.
bool ValveProtoUtils::MessageHasExactFields(const google::protobuf::Descriptor& msgDesc,
    std::initializer_list<int> fields)
{
    if (msgDesc.field_count() != static_cast<int>(fields.size()))
        return false;

    for (int field : fields)
    {
        if (msgDesc.FindFieldByNumber(field) == nullptr)
            return false;
    }

    return true;
}
