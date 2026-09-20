# firmware/components/ventModel/CMakeLists.txt — IDF proxy for drivers/ventModel
#
# §5c step 1 of design/integrateWindowPositionSensor.md, done in 2.12.0 with the
# dual control mode. Deliberately absent until now: an IDF component is compiled
# into every build, and until something in the firmware chose between two models
# the library was a tested reference with no consumer (design/ventModelContract.md).
#
# The library is the control LAW and nothing else: no ESP-IDF, no FreeRTOS, no
# project headers, by the contract's one hard rule. So this proxy has no
# REQUIRES at all — the same sources compile here, in the host unit tests
# (`pio test -e native` in drivers/ventModel) and in the offline replay
# (model/vent_step_replay.py). Anything that needs a queue, a clock or storage
# belongs on T6's side of the boundary.

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/ventModel/src/vent_model_stepped.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/ventModel/src/vent_model_graded.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../drivers/ventModel/src"
)

# Tier-1 hardening flags scoped to this component only (gh#13 idiom).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wformat=2
    -Wshadow
    -Wstack-usage=2200
    -Wlogical-op
    -Wstrict-overflow=2
    -Wnull-dereference
)
