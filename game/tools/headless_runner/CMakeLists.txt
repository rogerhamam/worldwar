add_executable(headless_runner main.cpp nettest.cpp)
# ww_net for the --nettest / --upnp modes: the lockstep protocol is only
# testable by actually running both peers over a real socket (see nettest.cpp).
target_link_libraries(headless_runner PRIVATE ww_sim ww_net)
