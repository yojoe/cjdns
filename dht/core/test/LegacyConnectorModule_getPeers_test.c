#include "LegacyConnectorModule_framework.c"

int main()
{
    const char* control =
        "d1:ad2:id20:abcdefghij01234567899:info_hash20:mnopqrstuvwxyz123456e1:q9:get_peers1:t2:aa1:y1:qe";

    struct sockaddr_in ipAddr;
    NetworkTools_getPeerAddress("\x7F\x00\x00\x01\x1E\xD3", 6, (struct sockaddr_storage*) &ipAddr);

    struct DHTModuleRegistry* registry = DHTModules_new();
    LegacyConnectorModuleInternal_setContext(
        &(struct LegacyConnectorModule_context) {
            .registry = registry,
            .myId = {20, "abcdefghij0123456789"},
            .whenToCallDHTPeriodic = 0
        }
    );

    struct LegacyConnectorModuleTest_context testContext;
    memset(&testContext, 0, sizeof(testContext));

    struct DHTModule receiver = {
        .name = "TestModule",
        .context = &testContext,
        .handleOutgoing = handleOutgoing
    };

    DHTModules_register(&receiver, registry);

    prepareFakeQuery();
    send_get_peers((struct sockaddr*) &ipAddr, -1,
                                         (unsigned char*) "aa", 2,
                                         (unsigned char*) "mnopqrstuvwxyz123456",
                                         0,
                                         0);

    printf("\n\nget_peers: %s\n\n", testContext.message);
    return memcmp(control, testContext.message, strlen(control));
}
