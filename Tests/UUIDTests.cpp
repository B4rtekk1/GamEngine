#include <Engine/Scene/Components/IdentityComponents.h>

int main() {
    using namespace Engine;

    if (NullUUID != 0) return 1;
    const UUID first = createUUID();
    const UUID second = createUUID();
    if (first == NullUUID || second == NullUUID || first == second) return 2;

    const UUID reserved = second + 1000;
    reserveUUID(reserved);
    const UUID afterReserve = createUUID();
    if (afterReserve <= reserved) return 3;

    NameComponent name;
    UUIDComponent uuid;
    ParentComponent parent;
    if (name.value != "GameObject" || uuid.value != NullUUID || parent.parent != NullUUID) return 4;
    name.value = "Child";
    uuid.value = afterReserve;
    parent.parent = first;
    if (name.value != "Child" || uuid.value != afterReserve || parent.parent != first) return 5;
    return 0;
}
