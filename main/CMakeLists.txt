# Uncomment exactly ONE of the following to select what to build/test:
#set(MAIN_FILE "test_hardware.c")
set(MAIN_FILE "realtime.c")
#set(MAIN_FILE "mainloop.c")

idf_component_register(SRCS "${MAIN_FILE}" "utils.c"
                    INCLUDE_DIRS ".")
