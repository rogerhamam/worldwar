# Networking for deterministic-lockstep multiplayer (see net/session.h).
# Kept as its own library rather than folded into ww_sim so that the simulation
# stays free of platform/socket code -- the headless runner, the tests and the
# campaign tooling all link ww_sim and none of them should drag in Winsock.
add_library(ww_net STATIC
    src/socket.cpp
    src/session.cpp
    src/upnp.cpp
)
target_include_directories(ww_net PUBLIC include)
# ww_sim for the Command type and its wire codec, and for SkirmishSettings --
# the handshake has to agree on exactly the settings the world is generated
# from, so it serialises the real struct rather than a parallel copy of it.
target_link_libraries(ww_net PUBLIC ww_sim)
if(WIN32)
    # ws2_32: Winsock. iphlpapi: adapter/routing queries used by local_address.
    target_link_libraries(ww_net PUBLIC ws2_32 iphlpapi)
endif()
