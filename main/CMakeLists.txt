set(embed "")
if(CONFIG_EMU_WEB)
    list(APPEND embed "web/pcfiles.js")
endif()

idf_component_register(SRCS "main.c" "settings.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES ${embed}
                       PRIV_REQUIRES emu ibmpc x86 nvs_flash esp_psram esp_timer)
