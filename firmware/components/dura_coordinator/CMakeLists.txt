idf_component_register(
    SRCS "dura_coordinator.c"
    INCLUDE_DIRS "include"
    REQUIRES dura_meter dura_recipe dura_peers
)
