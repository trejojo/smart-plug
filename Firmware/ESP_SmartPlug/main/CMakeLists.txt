idf_component_register(SRCS "main.c"
                    "board_profile.c"
                    "module_baseline_io.c"
                    "module_sdcard.c"
                    "module_tmp102.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES led_strip fatfs sdmmc vfs)