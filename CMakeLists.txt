cmake_minimum_required(VERSION 3.20)
project(kaybeestat C)

set(MODULE_NAME kaybeestat)
set(MODULE_VERSION 0.7)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")

execute_process(
    COMMAND uname -r
    OUTPUT_VARIABLE KERNEL_RELEASE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(KERNEL_DIR "/lib/modules/${KERNEL_RELEASE}/build")

# kbuild

add_custom_target(module ALL
    COMMAND make -C ${KERNEL_DIR} M=${CMAKE_CURRENT_SOURCE_DIR} modules
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building kernel module via kbuild"
)

add_custom_target(module-clean
    COMMAND make -C ${KERNEL_DIR} M=${CMAKE_CURRENT_SOURCE_DIR} clean
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Cleaning kernel module build artifacts"
)

add_custom_target(module-load
    COMMAND sudo insmod ${CMAKE_CURRENT_SOURCE_DIR}/${MODULE_NAME}.ko
    DEPENDS module
    COMMENT "Loading kernel module"
)

add_custom_target(module-unload
    COMMAND sudo rmmod ${MODULE_NAME}
    COMMENT "Unloading kernel module"
)

# dkms

add_custom_target(dkms-add
    COMMAND sudo dkms add ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Adding module to DKMS"
)

add_custom_target(dkms-build
    COMMAND sudo dkms build ${MODULE_NAME}/${MODULE_VERSION}
    COMMENT "Building module with DKMS"
)

add_custom_target(dkms-install
    COMMAND sudo dkms install ${MODULE_NAME}/${MODULE_VERSION}
    COMMENT "Installing module with DKMS"
)

add_custom_target(dkms-remove
    COMMAND sudo dkms remove ${MODULE_NAME}/${MODULE_VERSION} --all
    COMMENT "Removing module from DKMS"
)

add_custom_target(dkms-status
    COMMAND dkms status ${MODULE_NAME}
    COMMENT "Checking DKMS status"
)

# daemon

add_executable(kaybeestatd kaybeestatd.c)
target_compile_options(kaybeestatd PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes)

# install

install(TARGETS kaybeestatd DESTINATION /usr/local/bin)
install(FILES ${CMAKE_SOURCE_DIR}/99-kaybeestat.rules DESTINATION /etc/udev/rules.d)
install(FILES ${CMAKE_SOURCE_DIR}/kaybeestatd.service DESTINATION /etc/systemd/system)

install(CODE "execute_process(COMMAND sh -c \"getent group kaybeestat >/dev/null || groupadd kaybeestat\")")
install(CODE "execute_process(COMMAND udevadm control --reload-rules)")
install(CODE "execute_process(COMMAND systemctl daemon-reload)")
install(CODE "message(STATUS \"next (dkms): pacman -S dkms && doas dkms add ${CMAKE_SOURCE_DIR} && doas dkms build ${MODULE_NAME}/${MODULE_VERSION} && doas dkms install ${MODULE_NAME}/${MODULE_VERSION}\")")
install(CODE "message(STATUS \"next (manual): doas insmod ${CMAKE_SOURCE_DIR}/${MODULE_NAME}.ko\")")
install(CODE "message(STATUS \"then: doas systemctl enable --now kaybeestatd\")")
install(CODE "message(STATUS \"optional: doas usermod -aG kaybeestat $ENV{USER}\")")

# test state

add_custom_target(invalidate-test-state
    COMMAND ${CMAKE_COMMAND} -E remove -f ${CMAKE_SOURCE_DIR}/.test-state
    COMMENT "Invalidating test state"
)

# tests

add_executable(test_kaybeestat tests/kaybeestat/main.c)
target_include_directories(test_kaybeestat PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(test_kaybeestat PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
target_link_libraries(test_kaybeestat PRIVATE)
add_dependencies(test_kaybeestat invalidate-test-state)

list(APPEND CMAKE_MODULE_PATH "$ENV{HOME}/.config/cmake")
include(QEMUTest)

qemu_add_test(
    NAME kaybeestat_tests
    COMMAND test_kaybeestat
    ARCH x86_64
    DKMS
    KERNEL_MODULE ${CMAKE_SOURCE_DIR}/${MODULE_NAME}.ko
    MODULE_NAME ${MODULE_NAME}
)

qemu_add_run_target()
