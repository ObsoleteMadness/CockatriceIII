# slirp - userspace TCP/IP stack backing the emulated Ethernet.
#
# Vendored legacy C from QEMU's slirp. It is not warning-clean and is not worth
# making so; the warnings are silenced here rather than in the global flags so
# that the emulator's own sources keep theirs.

add_library(slirp STATIC
    bootp.c
    cksum.c
    debug.c
    if.c
    ip_icmp.c
    ip_input.c
    ip_output.c
    mbuf.c
    misc.c
    queue.c
    sbuf.c
    slirp.c
    socket.c
    tcp_input.c
    tcp_output.c
    tcp_subr.c
    tcp_timer.c
    tftp.c
    udp.c
)

target_include_directories(slirp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(slirp PRIVATE /w)
else()
    target_compile_options(slirp PRIVATE -w)
endif()

# slirp_config.h already branches on _WIN32 for the Winsock spellings; it just
# needs the libraries themselves.
if(WIN32)
    target_link_libraries(slirp PUBLIC ws2_32 iphlpapi)
endif()
